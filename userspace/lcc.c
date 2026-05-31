/* lcc - the Lariat C-subset compiler.
 *
 * A small, self-contained, single-pass compiler that runs natively on Lariat
 * and turns a C-subset source file into a runnable static x86_64 ELF (no
 * external assembler or linker).  It is the Phase 5 "build a program on-device"
 * milestone: a concrete, working step toward self-hosting while a full native
 * GCC/binutils bootstrap (see toolchain/) remains gated on a large host build.
 *
 * Supported language (a strict subset of C):
 *   int main(void) {            // signature is parsed leniently up to '{'
 *       int x = <expr>;         // declare an int local
 *       x = <expr>;             // assign
 *       write("literal");       // write a string literal to stdout (fd 1)
 *       return <expr>;          // exit(expr); ends the program
 *   }                           // implicit return 0 at end of body
 *   // line comments are supported
 *
 *   <expr> : <term> (('+'|'-') <term>)*
 *   <term> : <factor> (('*'|'/'|'%') <factor>)*
 *   <factor>: INT | IDENT | '(' <expr> ')' | '-' <factor>
 *
 * Codegen is stack-based: each expression leaves its value in %rax, using the
 * machine stack for the left operand of a binary op.  Locals live in a writable
 * region addressed as [%r15 + slot*8]; %r15 is loaded with that region's
 * absolute virtual address in the prologue.
 *
 * Usage: lcc <source.c> <output>
 */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/sys/stat.h"

#define BASE       0x40000000ULL
#define EHDR_SZ    64
#define PHDR_SZ    56
#define CODE_OFF   (EHDR_SZ + PHDR_SZ)

#define MAX_CODE   65536
#define MAX_STR    8192
#define MAX_LOCALS 64
#define MAX_FIX    512

static unsigned char code[MAX_CODE];
static int           code_len;

static char          strbuf[MAX_STR];   /* concatenated string literals */
static int           strbuf_len;

static char          local_names[MAX_LOCALS][32];
static int           nlocals;

/* Fixups patched once the final layout (string/locals vaddrs) is known. */
enum { FIX_LOCALS_BASE, FIX_STR };
typedef struct { int code_off; int kind; int str_off; } fixup_t;
static fixup_t fixups[MAX_FIX];
static int     nfix;

/* --- source / lexer ------------------------------------------------------- */
static char  src[MAX_CODE];
static int   src_len, pos;

enum { T_EOF, T_INT, T_IDENT, T_NUM, T_STR, T_RETURN, T_WRITE, T_PUNCT };
typedef struct {
    int  kind;
    long num;
    char text[64];   /* ident text or string contents */
    int  slen;       /* string length for T_STR */
    char punct;
} token_t;
static token_t tok;

static void die(const char *msg) {
    fputs("lcc: ", STDERR_FILENO);
    fputs(msg, STDERR_FILENO);
    fputs("\n", STDERR_FILENO);
    _exit(1);
}

static void skip_ws(void) {
    for (;;) {
        while (pos < src_len && (src[pos] == ' ' || src[pos] == '\t' ||
                                 src[pos] == '\n' || src[pos] == '\r'))
            pos++;
        if (pos + 1 < src_len && src[pos] == '/' && src[pos + 1] == '/') {
            while (pos < src_len && src[pos] != '\n') pos++;
            continue;
        }
        break;
    }
}

static int is_id0(char c) { return (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); }
static int is_id(char c)  { return is_id0(c) || (c >= '0' && c <= '9'); }

static void next(void) {
    skip_ws();
    if (pos >= src_len) { tok.kind = T_EOF; return; }
    char c = src[pos];
    if (c >= '0' && c <= '9') {
        long v = 0;
        while (pos < src_len && src[pos] >= '0' && src[pos] <= '9')
            v = v * 10 + (src[pos++] - '0');
        tok.kind = T_NUM; tok.num = v; return;
    }
    if (is_id0(c)) {
        int n = 0;
        while (pos < src_len && is_id(src[pos]) && n < 63) tok.text[n++] = src[pos++];
        tok.text[n] = '\0';
        if (strcmp(tok.text, "int") == 0)         tok.kind = T_INT;
        else if (strcmp(tok.text, "return") == 0) tok.kind = T_RETURN;
        else if (strcmp(tok.text, "write") == 0)  tok.kind = T_WRITE;
        else                                      tok.kind = T_IDENT;
        return;
    }
    if (c == '"') {
        pos++;
        int n = 0;
        while (pos < src_len && src[pos] != '"' && n < 63) {
            char ch = src[pos++];
            if (ch == '\\' && pos < src_len) {
                char e = src[pos++];
                if (e == 'n') ch = '\n';
                else if (e == 't') ch = '\t';
                else ch = e;
            }
            tok.text[n++] = ch;
        }
        if (pos >= src_len || src[pos] != '"') die("unterminated string");
        pos++;
        tok.text[n] = '\0'; tok.slen = n; tok.kind = T_STR; return;
    }
    tok.kind = T_PUNCT; tok.punct = c; pos++;
}

static void expect_punct(char p) {
    if (tok.kind != T_PUNCT || tok.punct != p) die("syntax error: expected punctuation");
    next();
}

/* --- code emitter --------------------------------------------------------- */
static void emit(unsigned char b) {
    if (code_len >= MAX_CODE) die("program too large");
    code[code_len++] = b;
}
static void emit_imm32(unsigned v) { for (int i = 0; i < 4; i++) emit((v >> (i * 8)) & 0xff); }
static void emit_imm64(unsigned long long v) { for (int i = 0; i < 8; i++) emit((v >> (i * 8)) & 0xff); }

static void add_fixup(int kind, int str_off) {
    if (nfix >= MAX_FIX) die("too many fixups");
    fixups[nfix].code_off = code_len;   /* points at the imm to patch */
    fixups[nfix].kind = kind;
    fixups[nfix].str_off = str_off;
    nfix++;
}

/* mov rax, imm64 */
static void emit_mov_rax_imm(unsigned long long v) { emit(0x48); emit(0xB8); emit_imm64(v); }
/* mov rax, [r15+disp32] */
static void emit_load_local(int slot) { emit(0x49); emit(0x8B); emit(0x87); emit_imm32((unsigned)(slot * 8)); }
/* mov [r15+disp32], rax */
static void emit_store_local(int slot) { emit(0x49); emit(0x89); emit(0x87); emit_imm32((unsigned)(slot * 8)); }
static void emit_push_rax(void) { emit(0x50); }
static void emit_pop_rax(void)  { emit(0x58); }

/* --- locals --------------------------------------------------------------- */
static int local_slot(const char *name) {
    for (int i = 0; i < nlocals; i++)
        if (strcmp(local_names[i], name) == 0) return i;
    return -1;
}
static int local_declare(const char *name) {
    if (local_slot(name) >= 0) die("redeclared variable");
    if (nlocals >= MAX_LOCALS) die("too many locals");
    strncpy(local_names[nlocals], name, sizeof(local_names[0]) - 1);
    return nlocals++;
}

/* --- expression codegen --------------------------------------------------- */
static void gen_expr(void);

static void gen_factor(void) {
    if (tok.kind == T_NUM) { emit_mov_rax_imm((unsigned long long)tok.num); next(); return; }
    if (tok.kind == T_IDENT) {
        int s = local_slot(tok.text);
        if (s < 0) die("use of undeclared variable");
        emit_load_local(s); next(); return;
    }
    if (tok.kind == T_PUNCT && tok.punct == '(') {
        next(); gen_expr(); expect_punct(')'); return;
    }
    if (tok.kind == T_PUNCT && tok.punct == '-') {
        next(); gen_factor();
        emit(0x48); emit(0xF7); emit(0xD8);   /* neg rax */
        return;
    }
    die("syntax error in expression");
}

static void gen_term(void) {
    gen_factor();
    while (tok.kind == T_PUNCT && (tok.punct == '*' || tok.punct == '/' || tok.punct == '%')) {
        char op = tok.punct; next();
        emit_push_rax();        /* save left */
        gen_factor();           /* right -> rax */
        emit(0x48); emit(0x89); emit(0xC1);   /* mov rcx, rax (right) */
        emit_pop_rax();         /* left -> rax */
        if (op == '*') { emit(0x48); emit(0x0F); emit(0xAF); emit(0xC1); }   /* imul rax, rcx */
        else {
            emit(0x48); emit(0x99);            /* cqo */
            emit(0x48); emit(0xF7); emit(0xF9);/* idiv rcx */
            if (op == '%') { emit(0x48); emit(0x89); emit(0xD0); }           /* mov rax, rdx */
        }
    }
}

static void gen_expr(void) {
    gen_term();
    while (tok.kind == T_PUNCT && (tok.punct == '+' || tok.punct == '-')) {
        char op = tok.punct; next();
        emit_push_rax();
        gen_term();
        emit(0x48); emit(0x89); emit(0xC1);   /* mov rcx, rax (right) */
        emit_pop_rax();                        /* left -> rax */
        if (op == '+') { emit(0x48); emit(0x01); emit(0xC8); }   /* add rax, rcx */
        else           { emit(0x48); emit(0x29); emit(0xC8); }   /* sub rax, rcx */
    }
}

/* --- statement codegen ---------------------------------------------------- */
static int str_intern(const char *s, int len) {
    if (strbuf_len + len > MAX_STR) die("string pool overflow");
    int off = strbuf_len;
    memcpy(strbuf + off, s, len);
    strbuf_len += len;
    return off;
}

static void gen_write(void) {
    next();                       /* consume 'write' */
    expect_punct('(');
    if (tok.kind != T_STR) die("write() expects a string literal");
    int off = str_intern(tok.text, tok.slen);
    int len = tok.slen;
    next();
    expect_punct(')');
    expect_punct(';');
    /* write(1, str, len) */
    emit(0xB8); emit_imm32(1);                /* mov eax, 1 (SYS_write) */
    emit(0xBF); emit_imm32(1);                /* mov edi, 1 (stdout)    */
    emit(0x48); emit(0xBE); add_fixup(FIX_STR, off); emit_imm64(0); /* mov rsi, <str addr> */
    emit(0xBA); emit_imm32((unsigned)len);    /* mov edx, len           */
    emit(0x0F); emit(0x05);                   /* syscall                */
}

static void gen_return(void) {
    next();                       /* consume 'return' */
    gen_expr();                   /* value -> rax */
    expect_punct(';');
    emit(0x48); emit(0x89); emit(0xC7);       /* mov rdi, rax */
    emit(0xB8); emit_imm32(60);               /* mov eax, 60 (SYS_exit) */
    emit(0x0F); emit(0x05);                   /* syscall */
}

static void gen_stmt(void) {
    if (tok.kind == T_INT) {
        next();
        if (tok.kind != T_IDENT) die("expected variable name after int");
        int slot = local_declare(tok.text);
        next();
        expect_punct('=');
        gen_expr();
        expect_punct(';');
        emit_store_local(slot);
        return;
    }
    if (tok.kind == T_RETURN) { gen_return(); return; }
    if (tok.kind == T_WRITE)  { gen_write();  return; }
    if (tok.kind == T_IDENT) {
        int slot = local_slot(tok.text);
        if (slot < 0) die("assignment to undeclared variable");
        next();
        expect_punct('=');
        gen_expr();
        expect_punct(';');
        emit_store_local(slot);
        return;
    }
    if (tok.kind == T_PUNCT && tok.punct == ';') { next(); return; }
    die("unexpected statement");
}

/* --- ELF writer ----------------------------------------------------------- */
static void put32(unsigned char *p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (v >> (i*8)) & 0xff; }
static void put64(unsigned char *p, unsigned long long v) { for (int i = 0; i < 8; i++) p[i] = (v >> (i*8)) & 0xff; }

int main(int argc, char **argv) {
    if (argc < 3) { fputs("usage: lcc <source.c> <output>\n", STDERR_FILENO); return 2; }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) die("cannot open source");
    src_len = 0;
    for (;;) {
        if (src_len + 1 >= (int)sizeof(src)) break;
        long n = read(fd, src + src_len, sizeof(src) - 1 - src_len);
        if (n <= 0) break;
        src_len += (int)n;
    }
    close(fd);

    /* Prologue: mov r15, <locals base> (patched later). */
    emit(0x49); emit(0xBF); add_fixup(FIX_LOCALS_BASE, 0); emit_imm64(0);

    /* Parse leniently up to the function body's '{'. */
    next();
    while (tok.kind != T_EOF && !(tok.kind == T_PUNCT && tok.punct == '{')) next();
    if (tok.kind != T_PUNCT) die("missing function body '{'");
    next();   /* consume '{' */

    while (!(tok.kind == T_PUNCT && tok.punct == '}')) {
        if (tok.kind == T_EOF) die("missing closing '}'");
        gen_stmt();
    }

    /* Implicit return 0. */
    emit_mov_rax_imm(0);
    emit(0x48); emit(0x89); emit(0xC7);       /* mov rdi, rax */
    emit(0xB8); emit_imm32(60);               /* mov eax, 60  */
    emit(0x0F); emit(0x05);                   /* syscall      */

    /* Final layout. */
    int str_off_file = CODE_OFF + code_len;
    int locals_off_file = (str_off_file + strbuf_len + 7) & ~7;
    int locals_bytes = nlocals * 8;
    int total = locals_off_file + locals_bytes;

    unsigned long long code_vaddr   = BASE + CODE_OFF;
    unsigned long long str_vaddr    = BASE + str_off_file;
    unsigned long long locals_vaddr = BASE + locals_off_file;

    /* Apply fixups. */
    for (int i = 0; i < nfix; i++) {
        unsigned long long val = (fixups[i].kind == FIX_LOCALS_BASE)
                               ? locals_vaddr
                               : str_vaddr + (unsigned)fixups[i].str_off;
        put64(&code[fixups[i].code_off], val);
    }

    /* Assemble the file image. */
    static unsigned char out[CODE_OFF + MAX_CODE + MAX_STR + MAX_LOCALS * 8 + 16];
    if (total > (int)sizeof(out)) die("output too large");
    for (int i = 0; i < total; i++) out[i] = 0;

    /* ELF header. */
    unsigned char *e = out;
    e[0] = 0x7f; e[1] = 'E'; e[2] = 'L'; e[3] = 'F';
    e[4] = 2;    /* ELFCLASS64 */
    e[5] = 1;    /* ELFDATA2LSB */
    e[6] = 1;    /* EV_CURRENT */
    /* e_ident[7..15] = 0 (SysV ABI) */
    e[16] = 2; e[17] = 0;               /* e_type = ET_EXEC */
    e[18] = 0x3e; e[19] = 0;            /* e_machine = EM_X86_64 (62) */
    put32(e + 20, 1);                   /* e_version */
    put64(e + 24, code_vaddr);          /* e_entry */
    put64(e + 32, EHDR_SZ);             /* e_phoff */
    put64(e + 40, 0);                   /* e_shoff */
    put32(e + 48, 0);                   /* e_flags */
    e[52] = EHDR_SZ; e[53] = 0;         /* e_ehsize = 64 */
    e[54] = PHDR_SZ; e[55] = 0;         /* e_phentsize = 56 */
    e[56] = 1; e[57] = 0;               /* e_phnum = 1 */
    e[58] = 0; e[59] = 0;               /* e_shentsize */
    e[60] = 0; e[61] = 0;               /* e_shnum */
    e[62] = 0; e[63] = 0;               /* e_shstrndx */

    /* Program header (one RWX PT_LOAD covering the whole image). */
    unsigned char *p = out + EHDR_SZ;
    put32(p + 0, 1);                    /* p_type = PT_LOAD */
    put32(p + 4, 0x7);                  /* p_flags = R|W|X */
    put64(p + 8, 0);                    /* p_offset */
    put64(p + 16, BASE);                /* p_vaddr */
    put64(p + 24, BASE);                /* p_paddr */
    put64(p + 32, (unsigned long long)total);  /* p_filesz */
    put64(p + 40, (unsigned long long)total);  /* p_memsz */
    put64(p + 48, 0x1000);              /* p_align */

    /* Code + strings. */
    for (int i = 0; i < code_len; i++) out[CODE_OFF + i] = code[i];
    for (int i = 0; i < strbuf_len; i++) out[str_off_file + i] = (unsigned char)strbuf[i];

    int ofd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (ofd < 0) die("cannot create output");
    if (write(ofd, out, total) != total) { close(ofd); die("short write"); }
    close(ofd);
    chmod(argv[2], 0755);
    return 0;
}
