/* edit - a minimal GUI text editor client for the Lariat window server.
 *
 * A reference Phase 3 app: connects over IPC, opens a window, and edits an
 * in-memory text buffer (optionally loading/saving a file given as argv[1]).
 * Keys: printable insert, Backspace deletes, Enter starts a new line, and the
 * Tab key (\t) saves when editing a named file. */

#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/fcntl.h"
#include "libc/ws.h"

#define FW 8
#define FH 16
#define ROWS 26
#define COLS 72

static char lines[ROWS][COLS];
static int  llen[ROWS];
static int  nlines = 1;
static int  cx, cy;
static const char *fname;

static void load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[4096];
    int n = (int)read(fd, buf, sizeof(buf));
    close(fd);
    int r = 0, c = 0;
    for (int i = 0; i < n && r < ROWS; i++) {
        if (buf[i] == '\n') { llen[r] = c; r++; c = 0; }
        else if (c < COLS - 1) lines[r][c++] = buf[i];
    }
    llen[r] = c;
    nlines = r + 1;
}

static void save(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    for (int r = 0; r < nlines; r++) {
        write(fd, lines[r], (size_t)llen[r]);
        write(fd, "\n", 1);
    }
    close(fd);
}

static void redraw(ws_conn_t *c, int win) {
    ws_fill(c, win, 0x00101620);
    char info[80];
    snprintf(info, sizeof(info), "%s  (Tab=save)  ln %d col %d",
             fname ? fname : "[no file]", cy + 1, cx + 1);
    ws_text(c, win, 4, 2, 0x0070c0ff, info);
    for (int r = 0; r < nlines; r++) {
        lines[r][llen[r]] = '\0';
        ws_text(c, win, 4, 22 + r * FH, 0x00d8e0ec, lines[r]);
    }
    ws_rect(c, win, 4 + cx * FW, 22 + cy * FH, 2, FH, 0x00ffd040);
    ws_present(c, win);
}

static void insert_char(int ch) {
    if (cy >= ROWS) return;
    if (ch == '\n') {
        if (nlines < ROWS) {
            for (int r = nlines; r > cy + 1; r--) {
                memcpy(lines[r], lines[r - 1], COLS);
                llen[r] = llen[r - 1];
            }
            int tail = llen[cy] - cx;
            memcpy(lines[cy + 1], lines[cy] + cx, (size_t)tail);
            llen[cy + 1] = tail;
            llen[cy] = cx;
            nlines++;
            cy++; cx = 0;
        }
    } else if (ch == '\b' || ch == 127) {
        if (cx > 0) {
            for (int i = cx - 1; i < llen[cy] - 1; i++) lines[cy][i] = lines[cy][i + 1];
            llen[cy]--; cx--;
        } else if (cy > 0) {
            int prev = llen[cy - 1];
            for (int i = 0; i < llen[cy] && prev + i < COLS - 1; i++)
                lines[cy - 1][prev + i] = lines[cy][i];
            llen[cy - 1] = prev + llen[cy];
            for (int r = cy; r < nlines - 1; r++) {
                memcpy(lines[r], lines[r + 1], COLS);
                llen[r] = llen[r + 1];
            }
            nlines--; cy--; cx = prev;
        }
    } else if (ch >= 32 && ch < 127) {
        if (llen[cy] < COLS - 1) {
            for (int i = llen[cy]; i > cx; i--) lines[cy][i] = lines[cy][i - 1];
            lines[cy][cx] = (char)ch;
            llen[cy]++; cx++;
        }
    }
}

int main(int argc, char **argv) {
    fname = (argc > 1) ? argv[1] : 0;
    for (int r = 0; r < ROWS; r++) llen[r] = 0;
    if (fname) load(fname);

    ws_conn_t c;
    if (ws_connect(&c) != 0) { printf("edit: no window server\n"); return 1; }
    int win = ws_create_window(&c, 200, 120, COLS * FW + 10, ROWS * FH + 30, "Editor");
    if (win < 0) { printf("edit: create window failed\n"); return 1; }
    redraw(&c, win);

    for (;;) {
        ws_event_t ev;
        if (!ws_wait_event(&c, &ev)) break;
        if (ev.op == WS_EV_CLOSE) break;
        if (ev.op == WS_EV_KEY && ev.b) {
            int ch = (int)ev.a;
            if (ch == '\t') { if (fname) save(fname); }
            else insert_char(ch);
            redraw(&c, win);
        }
    }
    ws_destroy_window(&c, win);
    return 0;
}
