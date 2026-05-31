/* term - a GUI terminal client for the Lariat window server.
 *
 * A reference Phase 3 app: connects over IPC, opens a window, spawns /bin/sh
 * over pipes, renders the shell output into a text grid, and forwards typed
 * lines.  (The windowserver also hosts a built-in terminal; this proves the
 * same can be done as an ordinary IPC client.) */

#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/poll.h"
#include "libc/ws.h"

#define FW 8
#define FH 16
#define TCOLS 64
#define TROWS 24

static char grid[TROWS][TCOLS];
static int  cx, cy;
static char line[256];
static int  llen;
static int  sh_in = -1, sh_out = -1;

static void cls(void) { for (int r = 0; r < TROWS; r++) for (int c = 0; c < TCOLS; c++) grid[r][c] = ' '; cx = cy = 0; }
static void scroll1(void) {
    for (int r = 1; r < TROWS; r++) memcpy(grid[r - 1], grid[r], TCOLS);
    for (int c = 0; c < TCOLS; c++) grid[TROWS - 1][c] = ' ';
    cy = TROWS - 1;
}
static void putc1(char ch) {
    if (ch == '\n') { cx = 0; if (++cy >= TROWS) scroll1(); return; }
    if (ch == '\r') { cx = 0; return; }
    if (ch == '\b') { if (cx > 0) cx--; return; }
    if (ch < 32) return;
    if (cx >= TCOLS) { cx = 0; if (++cy >= TROWS) scroll1(); }
    grid[cy][cx++] = ch;
}

static int spawn_shell(void) {
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) return -1;
    int pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        char *av[] = { "/bin/sh", 0 };
        execve("/bin/sh", av, environ);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    sh_in = in[1]; sh_out = out[0];
    return pid;
}

static void redraw(ws_conn_t *c, int win) {
    ws_fill(c, win, 0x00101418);
    char tmp[TCOLS + 1];
    for (int r = 0; r < TROWS; r++) {
        int n = 0;
        for (int col = 0; col < TCOLS; col++) tmp[n++] = grid[r][col];
        tmp[n] = '\0';
        ws_text(c, win, 4, 2 + r * FH, 0x0048e08a, tmp);
    }
    ws_rect(c, win, 4 + cx * FW, 2 + cy * FH, FW, FH, 0x0080c060);
    ws_present(c, win);
}

int main(void) {
    ws_conn_t c;
    if (ws_connect(&c) != 0) { printf("term: no window server\n"); return 1; }
    int win = ws_create_window(&c, 100, 100, TCOLS * FW + 10, TROWS * FH + 6, "Terminal");
    if (win < 0) { printf("term: create window failed\n"); return 1; }
    cls();
    spawn_shell();
    redraw(&c, win);

    for (;;) {
        struct pollfd pfd[1];
        pfd[0].fd = sh_out; pfd[0].events = POLLIN; pfd[0].revents = 0;
        poll(pfd, 1, 0);
        int dirty = 0;
        if (pfd[0].revents & POLLIN) {
            char buf[256];
            int n = (int)read(sh_out, buf, sizeof(buf));
            for (int i = 0; i < n; i++) putc1(buf[i]);
            if (n > 0) dirty = 1;
        }
        ws_event_t ev;
        while (ws_poll_event(&c, &ev)) {
            if (ev.op == WS_EV_CLOSE) { ws_destroy_window(&c, win); return 0; }
            if (ev.op == WS_EV_KEY && ev.b) {
                char ch = (char)ev.a;
                if (ch == '\n' || ch == '\r') {
                    putc1('\n');
                    if (sh_in >= 0) { write(sh_in, line, (size_t)llen); write(sh_in, "\n", 1); }
                    llen = 0;
                } else if (ch == '\b' || ch == 127) {
                    if (llen > 0) { llen--; putc1('\b'); }
                } else if (ch >= 32) {
                    if (llen < (int)sizeof(line) - 1) line[llen++] = ch;
                    putc1(ch);
                }
                dirty = 1;
            }
        }
        if (dirty) redraw(&c, win);
    }
}
