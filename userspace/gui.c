/* gui - the Lariat window server / compositor.
 *
 * Owns /dev/fb0, composites a desktop (wallpaper + panel), a set of windows,
 * and a mouse cursor, and routes /dev/input events.  One window is a graphical
 * terminal: it spawns /bin/sh as a child connected by pipes (the "client"),
 * renders the shell's output into a text grid, and forwards typed lines to the
 * shell's stdin.  A second window is a small animated demo. */

#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/stdio.h"
#include "libc/fcntl.h"
#include "libc/poll.h"
#include "libc/time.h"
#include "libc/gfx.h"
#include "libc/font8x16.h"
#include "libc/input.h"

#define SCRW 1024
#define SCRH 768
#define PANEL_H 26
#define TB_H    20            /* window title-bar height */

/* Palette (XRGB). */
#define C_WALL   0x00204a6e
#define C_PANEL  0x00181c24
#define C_PANELT 0x00d8e0ec
#define C_WINBG  0x00202830
#define C_WINBDR 0x00586273
#define C_TBAR   0x00305a8a
#define C_TBARF  0x00407ec0   /* focused title bar */
#define C_TBART  0x00ffffff
#define C_TERMBG 0x00101418
#define C_TERMFG 0x0048e08a

typedef struct {
    int       x, y, w, h;
    char      title[32];
    surface_t surf;          /* full-window pixels (incl. title bar/border) */
} window_t;

/* --------------------------------------------------------------------------
 * Terminal window state
 * -------------------------------------------------------------------------- */
#define TCOLS 64
#define TROWS 21
static char  term_grid[TROWS][TCOLS];
static int   term_cx, term_cy;
static char  line_buf[256];
static int   line_len;
static int   sh_in = -1, sh_out = -1;

static void term_clear(void) {
    for (int r = 0; r < TROWS; r++)
        for (int c = 0; c < TCOLS; c++) term_grid[r][c] = ' ';
    term_cx = term_cy = 0;
}

static void term_scroll(void) {
    for (int r = 1; r < TROWS; r++)
        memcpy(term_grid[r - 1], term_grid[r], TCOLS);
    for (int c = 0; c < TCOLS; c++) term_grid[TROWS - 1][c] = ' ';
    term_cy = TROWS - 1;
}

static void term_putc(char ch) {
    if (ch == '\n') {
        term_cx = 0;
        if (++term_cy >= TROWS) term_scroll();
        return;
    }
    if (ch == '\r') { term_cx = 0; return; }
    if (ch == '\b') { if (term_cx > 0) term_cx--; return; }
    if (ch == '\t') { term_cx = (term_cx + 8) & ~7; if (term_cx >= TCOLS) { term_cx = 0; if (++term_cy >= TROWS) term_scroll(); } return; }
    if (ch < 32) return;

    if (term_cx >= TCOLS) { term_cx = 0; if (++term_cy >= TROWS) term_scroll(); }
    term_grid[term_cy][term_cx++] = ch;
}

static void term_puts(const char *s, int n) {
    for (int i = 0; i < n; i++) term_putc(s[i]);
}

/* Spawn /bin/sh wired to pipes; returns child pid or -1. */
static int term_spawn_shell(void) {
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) return -1;
    int pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        char *av[] = { "/bin/sh", 0 };
        execve("/bin/sh", av, environ);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    sh_in = in[1];
    sh_out = out[0];
    return pid;
}

/* Render the terminal grid into its window surface. */
static void term_render(window_t *w, int focused) {
    gfx_fill(&w->surf, C_WINBG);
    gfx_rect(&w->surf, 0, 0, w->w, TB_H, focused ? C_TBARF : C_TBAR);
    gfx_text(&w->surf, 6, 2, w->title, C_TBART, GFX_TRANSPARENT);
    gfx_frame(&w->surf, 0, 0, w->w, w->h, C_WINBDR);

    int ox = 4, oy = TB_H + 2;
    gfx_rect(&w->surf, ox - 2, oy - 2, TCOLS * FONT_W + 4, TROWS * FONT_H + 4, C_TERMBG);
    for (int r = 0; r < TROWS; r++) {
        for (int c = 0; c < TCOLS; c++) {
            char ch = term_grid[r][c];
            if (ch != ' ')
                gfx_char(&w->surf, ox + c * FONT_W, oy + r * FONT_H, ch,
                         C_TERMFG, GFX_TRANSPARENT);
        }
    }
    /* Text cursor (block) when focused. */
    if (focused)
        gfx_rect(&w->surf, ox + term_cx * FONT_W, oy + term_cy * FONT_H,
                 FONT_W, FONT_H, 0x0080c060);
}

/* --------------------------------------------------------------------------
 * Demo window
 * -------------------------------------------------------------------------- */
static void demo_render(window_t *w, int focused, int frame) {
    gfx_fill(&w->surf, C_WINBG);
    gfx_rect(&w->surf, 0, 0, w->w, TB_H, focused ? C_TBARF : C_TBAR);
    gfx_text(&w->surf, 6, 2, w->title, C_TBART, GFX_TRANSPARENT);
    gfx_frame(&w->surf, 0, 0, w->w, w->h, C_WINBDR);

    int cw = w->w - 8, ch = w->h - TB_H - 8;
    int ox = 4, oy = TB_H + 4;
    /* Colour gradient backdrop. */
    for (int y = 0; y < ch; y++) {
        uint32_t col = rgb((uint8_t)(40 + y * 120 / ch),
                           (uint8_t)(20 + ((frame * 2) & 0x7f)),
                           (uint8_t)(120 + y * 100 / ch));
        gfx_rect(&w->surf, ox, oy + y, cw, 1, col);
    }
    /* A box bouncing horizontally. */
    int bx = (frame * 3) % (cw - 30);
    if ((frame / (cw - 30)) & 1) bx = (cw - 30) - bx;
    gfx_rect(&w->surf, ox + bx, oy + ch / 2 - 12, 24, 24, 0x00ffd040);
    gfx_text(&w->surf, ox + 8, oy + 6, "Lariat GUI", 0x00ffffff, GFX_TRANSPARENT);
}

/* --------------------------------------------------------------------------
 * Mouse cursor
 * -------------------------------------------------------------------------- */
static const char *cursor_img[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     XXXX   ",
};

static void draw_cursor(surface_t *s, int mx, int my) {
    for (int r = 0; r < 16; r++) {
        const char *row = cursor_img[r];
        for (int c = 0; row[c]; c++) {
            uint32_t col;
            if (row[c] == 'X') col = 0x00000000;
            else if (row[c] == '.') col = 0x00ffffff;
            else continue;
            int px = mx + c, py = my + r;
            if (px >= 0 && px < s->w && py >= 0 && py < s->h)
                s->pix[(size_t)py * s->pitch + px] = col;
        }
    }
}

/* --------------------------------------------------------------------------
 * Compositor
 * -------------------------------------------------------------------------- */
#define NWIN 2
static window_t wins[NWIN];
static int draw_order[NWIN] = { 1, 0 };   /* back..front; terminal on top */

static void raise_window(int idx) {
    int pos = -1;
    for (int i = 0; i < NWIN; i++) if (draw_order[i] == idx) pos = i;
    if (pos < 0) return;
    for (int i = pos; i < NWIN - 1; i++) draw_order[i] = draw_order[i + 1];
    draw_order[NWIN - 1] = idx;
}

static int focused_window(void) { return draw_order[NWIN - 1]; }

int main(void) {
    gfx_t g;
    if (gfx_open(&g) != 0) {
        printf("gui: cannot open /dev/fb0\n");
        return 1;
    }

    int infd = open("/dev/input", O_RDONLY);
    if (infd < 0) { printf("gui: cannot open /dev/input\n"); return 1; }

    /* Terminal window. */
    wins[0].x = 60; wins[0].y = 70;
    wins[0].w = TCOLS * FONT_W + 12;
    wins[0].h = TROWS * FONT_H + TB_H + 10;
    strcpy(wins[0].title, "Terminal - /bin/sh");
    wins[0].surf.w = wins[0].w; wins[0].surf.h = wins[0].h;
    wins[0].surf.pitch = wins[0].w;
    wins[0].surf.pix = malloc((size_t)wins[0].w * wins[0].h * 4);

    /* Demo window. */
    wins[1].x = 620; wins[1].y = 150;
    wins[1].w = 320; wins[1].h = 240;
    strcpy(wins[1].title, "Demo");
    wins[1].surf.w = wins[1].w; wins[1].surf.h = wins[1].h;
    wins[1].surf.pitch = wins[1].w;
    wins[1].surf.pix = malloc((size_t)wins[1].w * wins[1].h * 4);

    term_clear();
    term_spawn_shell();

    int mx = SCRW / 2, my = SCRH / 2;
    int dragging = -1, drag_dx = 0, drag_dy = 0;
    int frame = 0;

    for (;;) {
        /* Non-blocking poll: the compositor runs a continuous render loop,
         * draining input and shell output whenever either is ready.  (A blocking
         * timeout is intentionally avoided: the scheduler is cooperative and has
         * no usable periodic clock to drive poll/nanosleep timeouts.) */
        struct pollfd pfd[2];
        pfd[0].fd = infd;   pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = sh_out; pfd[1].events = POLLIN; pfd[1].revents = 0;
        poll(pfd, 2, 0);

        /* Drain shell output into the terminal grid. */
        if (pfd[1].revents & POLLIN) {
            char buf[512];
            int n = (int)read(sh_out, buf, sizeof(buf));
            if (n > 0) term_puts(buf, n);
        }

        /* Drain input events. */
        if (pfd[0].revents & POLLIN) {
            struct input_event evs[32];
            int n = (int)read(infd, evs, sizeof(evs));
            int cnt = n / (int)sizeof(struct input_event);
            for (int i = 0; i < cnt; i++) {
                struct input_event *e = &evs[i];
                if (e->type == EV_REL) {
                    if (e->code == REL_X) mx += e->value;
                    else if (e->code == REL_Y) my += e->value;
                    if (mx < 0) mx = 0;
                    if (mx >= SCRW) mx = SCRW - 1;
                    if (my < 0) my = 0;
                    if (my >= SCRH) my = SCRH - 1;
                    if (dragging >= 0) {
                        wins[dragging].x = mx - drag_dx;
                        wins[dragging].y = my - drag_dy;
                    }
                } else if (e->type == EV_KEY && e->code == BTN_LEFT) {
                    if (e->value) {
                        /* Press: focus/raise the window under the cursor; begin
                         * a drag if the title bar was hit. */
                        for (int k = NWIN - 1; k >= 0; k--) {
                            int idx = draw_order[k];
                            window_t *w = &wins[idx];
                            if (mx >= w->x && mx < w->x + w->w &&
                                my >= w->y && my < w->y + w->h) {
                                raise_window(idx);
                                if (my < w->y + TB_H) {
                                    dragging = idx;
                                    drag_dx = mx - w->x;
                                    drag_dy = my - w->y;
                                }
                                break;
                            }
                        }
                    } else {
                        dragging = -1;
                    }
                } else if (e->type == EV_KEY && e->code < 0x100) {
                    /* Keyboard -> focused terminal (local line editing). */
                    if (focused_window() == 0) {
                        char ch = (char)e->code;
                        if (ch == '\n' || ch == '\r') {
                            term_putc('\n');
                            if (sh_in >= 0) {
                                write(sh_in, line_buf, line_len);
                                write(sh_in, "\n", 1);
                            }
                            line_len = 0;
                        } else if (ch == '\b' || ch == 127) {
                            if (line_len > 0) { line_len--; term_putc('\b'); }
                        } else if (ch >= 32) {
                            if (line_len < (int)sizeof(line_buf) - 1)
                                line_buf[line_len++] = ch;
                            term_putc(ch);
                        }
                    }
                }
            }
        }

        /* Composite the frame. */
        gfx_fill(&g.back, C_WALL);
        gfx_rect(&g.back, 0, 0, SCRW, PANEL_H, C_PANEL);
        gfx_text(&g.back, 8, 5, "Lariat Desktop", C_PANELT, GFX_TRANSPARENT);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long secs = ts.tv_sec;
        int hh = (int)(secs / 3600) % 100, mm = (int)((secs / 60) % 60),
            ss = (int)(secs % 60);
        char clk[16] = "up 00:00:00";
        clk[3] = '0' + hh / 10; clk[4] = '0' + hh % 10;
        clk[6] = '0' + mm / 10; clk[7] = '0' + mm % 10;
        clk[9] = '0' + ss / 10; clk[10] = '0' + ss % 10;
        gfx_text(&g.back, SCRW - 8 - (int)strlen(clk) * FONT_W, 5, clk,
                 C_PANELT, GFX_TRANSPARENT);

        for (int k = 0; k < NWIN; k++) {
            int idx = draw_order[k];
            int focused = (idx == focused_window());
            if (idx == 0) term_render(&wins[0], focused);
            else          demo_render(&wins[idx], focused, frame);
            gfx_blit(&g.back, wins[idx].x, wins[idx].y, &wins[idx].surf);
        }

        draw_cursor(&g.back, mx, my);
        gfx_present(&g);
        frame++;
    }
    return 0;
}
