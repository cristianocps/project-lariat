/* calc - a GUI calculator client for the Lariat window server.
 *
 * A reference Phase 3 app: connects to the windowserver over IPC, builds its
 * UI with the wtk toolkit, and evaluates a left-to-right integer expression. */

#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/ws.h"
#include "libc/wtk.h"

static long acc;
static long entry;
static int  have_entry;
static char pending;          /* 0 or one of + - * / */
static char display[32];
static int  disp_id;

static void show_num(long v) {
    int i = 0, neg = (v < 0);
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    char tmp[24];
    if (!u) tmp[i++] = '0';
    while (u) { tmp[i++] = '0' + (int)(u % 10); u /= 10; }
    int j = 0;
    if (neg) display[j++] = '-';
    while (i > 0) display[j++] = tmp[--i];
    display[j] = '\0';
}

static void apply_pending(void) {
    if (!pending) { acc = entry; return; }
    switch (pending) {
    case '+': acc += entry; break;
    case '-': acc -= entry; break;
    case '*': acc *= entry; break;
    case '/': acc = entry ? acc / entry : 0; break;
    }
}

static wtk_window_t W;

static void on_key(wtk_window_t *w, int id, void *user) {
    (void)id;
    char k = ((const char *)user)[0];
    if (k >= '0' && k <= '9') {
        entry = entry * 10 + (k - '0');
        have_entry = 1;
        show_num(entry);
    } else if (k == 'C') {
        acc = entry = 0; pending = 0; have_entry = 0;
        strcpy(display, "0");
    } else if (k == '=') {
        apply_pending();
        pending = 0; entry = acc; have_entry = 0;
        show_num(acc);
    } else { /* operator */
        if (have_entry || pending == 0) apply_pending();
        pending = k; entry = 0; have_entry = 0;
        show_num(acc);
    }
    wtk_set_text(w, disp_id, display);
    wtk_draw(w);
}

int main(void) {
    ws_conn_t c;
    if (ws_connect(&c) != 0) { printf("calc: no window server\n"); return 1; }
    int win = ws_create_window(&c, 360, 140, 188, 232, "Calculator");
    if (win < 0) { printf("calc: create window failed\n"); return 1; }

    strcpy(display, "0");
    wtk_init(&W, &c, win, 188, 232, 0x00202830);
    disp_id = wtk_label(&W, 12, 10, "0", 0x0080e0a0);

    /* 4x4 keypad. */
    static const char *rows[4] = { "789/", "456*", "123-", "0C=+" };
    static char keys[16];   /* keep label storage alive for callbacks */
    int n = 0;
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) {
            char ch = rows[r][col];
            keys[n] = ch;
            char lbl[2] = { ch, 0 };
            wtk_button(&W, 12 + col * 42, 40 + r * 42, 38, 38, lbl, on_key, &keys[n]);
            n++;
        }
    wtk_draw(&W);

    for (;;) {
        ws_event_t ev;
        if (!ws_wait_event(&c, &ev)) break;
        if (ev.op == WS_EV_CLOSE) break;
        if (wtk_handle(&W, &ev)) { /* on_key already redrew */ }
    }
    ws_destroy_window(&c, win);
    return 0;
}
