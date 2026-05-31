/* gui - the Lariat window server / compositor ("windowserver").
 *
 * Owns /dev/fb0 and /dev/input, composites a desktop (wallpaper + panel +
 * launcher), manages a dynamic list of windows with move/raise/close and
 * z-order, and serves GUI clients over Phase M IPC ports using the protocol in
 * include/uapi/wsproto.h.  A built-in terminal window (wired to /bin/sh over
 * pipes) is always present so the desktop is usable with no clients; the panel
 * launcher spawns IPC client apps (calculator, editor) that draw via the
 * protocol.  See docs/adr/0005-display-server-protocol.md and ADR-0007.
 */

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
#include "libc/ws.h"
#include "lipc.h"

#define SCRW 1024
#define SCRH 768
#define PANEL_H 26
#define TB_H    20            /* window title-bar height */
#define CLOSE_SZ 14

/* Palette (XRGB). */
#define C_WALL   0x00204a6e
#define C_PANEL  0x00181c24
#define C_PANELT 0x00d8e0ec
#define C_BTN    0x002e3850
#define C_BTNH   0x00405a8a
#define C_WINBG  0x00202830
#define C_WINBDR 0x00586273
#define C_TBAR   0x00305a8a
#define C_TBARF  0x00407ec0   /* focused title bar */
#define C_TBART  0x00ffffff
#define C_CLOSE  0x00c04040
#define C_TERMBG 0x00101418
#define C_TERMFG 0x0048e08a

enum { WK_FREE = 0, WK_TERM, WK_DEMO, WK_CLIENT };

typedef struct {
    int       kind;
    uint32_t  win_id;        /* global window id                 */
    int       client;        /* index into clients[] or -1       */
    int       x, y, w, h;
    char      title[WS_TITLE_MAX + 4];
    surface_t surf;          /* full-window pixels (title + content) */
} window_t;

typedef struct {
    int      used;
    uint32_t id;
    int      evport;         /* port to send events to this client */
} client_t;

#define MAXWIN 16
#define MAXCLIENT 16
static window_t wins[MAXWIN];
static client_t clients[MAXCLIENT];
static int      order[MAXWIN];   /* z-order: indices back..front */
static int      norder;
static uint32_t g_next_win = 1, g_next_client = 1;
static int      bootstrap = -1;

/* --------------------------------------------------------------------------
 * Built-in terminal window state (singleton, owned by the first window).
 * -------------------------------------------------------------------------- */
#define TCOLS 64
#define TROWS 21
static char  term_grid[TROWS][TCOLS];
static int   term_cx, term_cy;
static char  line_buf[256];
static int   line_len;
static int   sh_in = -1, sh_out = -1;
static int   term_win = -1;      /* index of the terminal window */

static void term_clear(void) {
    for (int r = 0; r < TROWS; r++)
        for (int c = 0; c < TCOLS; c++) term_grid[r][c] = ' ';
    term_cx = term_cy = 0;
}
static void term_scroll(void) {
    for (int r = 1; r < TROWS; r++) memcpy(term_grid[r - 1], term_grid[r], TCOLS);
    for (int c = 0; c < TCOLS; c++) term_grid[TROWS - 1][c] = ' ';
    term_cy = TROWS - 1;
}
static void term_putc(char ch) {
    if (ch == '\n') { term_cx = 0; if (++term_cy >= TROWS) term_scroll(); return; }
    if (ch == '\r') { term_cx = 0; return; }
    if (ch == '\b') { if (term_cx > 0) term_cx--; return; }
    if (ch == '\t') { term_cx = (term_cx + 8) & ~7; if (term_cx >= TCOLS) { term_cx = 0; if (++term_cy >= TROWS) term_scroll(); } return; }
    if (ch < 32) return;
    if (term_cx >= TCOLS) { term_cx = 0; if (++term_cy >= TROWS) term_scroll(); }
    term_grid[term_cy][term_cx++] = ch;
}
static void term_puts(const char *s, int n) { for (int i = 0; i < n; i++) term_putc(s[i]); }

static int term_spawn_shell(void) {
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

/* --------------------------------------------------------------------------
 * Window list / z-order helpers
 * -------------------------------------------------------------------------- */
static int alloc_window(int kind, int cli, int x, int y, int w, int h, const char *title) {
    for (int i = 0; i < MAXWIN; i++) {
        if (wins[i].kind == WK_FREE) {
            window_t *win = &wins[i];
            win->kind = kind; win->client = cli;
            win->win_id = g_next_win++;
            win->x = x; win->y = y; win->w = w; win->h = h;
            win->title[0] = '\0';
            if (title) { strncpy(win->title, title, sizeof(win->title) - 1); }
            win->surf.w = w; win->surf.h = h; win->surf.pitch = w;
            win->surf.pix = malloc((size_t)w * h * 4);
            if (!win->surf.pix) { win->kind = WK_FREE; return -1; }
            gfx_fill(&win->surf, C_WINBG);
            order[norder++] = i;
            return i;
        }
    }
    return -1;
}

static void order_remove(int idx) {
    int pos = -1;
    for (int i = 0; i < norder; i++) if (order[i] == idx) pos = i;
    if (pos < 0) return;
    for (int i = pos; i < norder - 1; i++) order[i] = order[i + 1];
    norder--;
}

static void free_window(int idx) {
    if (idx < 0 || wins[idx].kind == WK_FREE) return;
    if (wins[idx].surf.pix) free(wins[idx].surf.pix);
    wins[idx].surf.pix = NULL;
    wins[idx].kind = WK_FREE;
    order_remove(idx);
}

static void raise_window(int idx) {
    order_remove(idx);
    order[norder++] = idx;
}
static int focused_window(void) { return norder ? order[norder - 1] : -1; }

static int find_window_by_id(int cli, uint32_t win_id) {
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].kind == WK_CLIENT && wins[i].client == cli && wins[i].win_id == win_id)
            return i;
    return -1;
}

/* Sub-surface covering a window's content area (below the title bar). */
static surface_t content_of(window_t *w) {
    surface_t s;
    s.pix = w->surf.pix + (size_t)TB_H * w->surf.pitch;
    s.w = w->w;
    s.h = w->h - TB_H;
    s.pitch = w->surf.pitch;
    return s;
}

/* --------------------------------------------------------------------------
 * IPC: client session + protocol dispatch
 * -------------------------------------------------------------------------- */
static void send_ev(int cli, uint32_t op, uint32_t win_id, int x, int y,
                    uint32_t a, uint32_t b) {
    if (cli < 0 || cli >= MAXCLIENT || !clients[cli].used || clients[cli].evport < 0)
        return;
    ws_msg_t m;
    memset(&m, 0, sizeof(m));
    m.op = op; m.win_id = win_id; m.x = x; m.y = y; m.a = a; m.b = b;
    port_send(clients[cli].evport, &m, sizeof(m));
}

static int client_for_id(uint32_t id) {
    for (int i = 0; i < MAXCLIENT; i++)
        if (clients[i].used && clients[i].id == id) return i;
    return -1;
}

static void tile_blit(window_t *w, int dx, int dy, int tw, int th, const uint32_t *px) {
    surface_t c = content_of(w);
    for (int y = 0; y < th; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= c.h) continue;
        for (int x = 0; x < tw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= c.w) continue;
            c.pix[(size_t)ty * c.pitch + tx] = px[(size_t)y * tw + x];
        }
    }
}

/* Handle one client request datagram (rbuf, n bytes). */
static void handle_request(char *rbuf, long n) {
    if (n < (long)sizeof(uint32_t)) return;
    ws_msg_t *m = (ws_msg_t *)rbuf;

    switch (m->op) {
    case WS_CONNECT: {
        for (int i = 0; i < MAXCLIENT; i++) {
            if (!clients[i].used) {
                clients[i].used = 1;
                clients[i].id = g_next_client++;
                m->str[sizeof(m->str) - 1] = '\0';
                clients[i].evport = port_open(m->str);
                send_ev(i, WS_EV_CONNECTED, 0, 0, 0, clients[i].id, 0);
                return;
            }
        }
        return;
    }
    case WS_CREATE_WIN: {
        int cli = client_for_id(m->client_id);
        if (cli < 0) return;
        int w = m->w > 0 ? m->w : 200, h = m->h > 0 ? m->h : 150;
        int idx = alloc_window(WK_CLIENT, cli, m->x, m->y, w, h + TB_H, m->str);
        if (idx < 0) {
            /* Always reply so the client never blocks forever; a==0 (win_id is
             * never 0) signals failure. */
            send_ev(cli, WS_EV_CREATED, 0, 0, 0, 0, 0);
            return;
        }
        raise_window(idx);
        send_ev(cli, WS_EV_CREATED, wins[idx].win_id, 0, 0, wins[idx].win_id, 0);
        return;
    }
    default: break;
    }

    /* Remaining ops target an existing window owned by the client. */
    int cli = client_for_id(m->client_id);
    if (cli < 0) return;
    int idx = find_window_by_id(cli, m->win_id);
    if (idx < 0) return;
    window_t *w = &wins[idx];
    surface_t c = content_of(w);

    switch (m->op) {
    case WS_FILL:    gfx_fill(&c, m->a); break;
    case WS_RECT:    gfx_rect(&c, m->x, m->y, m->w, m->h, m->a); break;
    case WS_TEXT:    m->str[sizeof(m->str) - 1] = '\0';
                     gfx_text(&c, m->x, m->y, m->str, m->a, GFX_TRANSPARENT); break;
    case WS_TILE: {
        int tw = m->w, th = m->h;
        if (tw > 0 && th > 0 && tw <= WS_TILE_DIM && th <= WS_TILE_DIM &&
            n >= (long)(sizeof(ws_msg_t) + (long)tw * th * 4))
            tile_blit(w, m->x, m->y, tw, th, (uint32_t *)(rbuf + sizeof(ws_msg_t)));
        break;
    }
    case WS_TITLE:   m->str[sizeof(m->str) - 1] = '\0';
                     strncpy(w->title, m->str, sizeof(w->title) - 1); break;
    case WS_DESTROY_WIN: free_window(idx); break;
    case WS_PRESENT: default: break;
    }
}

static void drain_ipc(void) {
    if (bootstrap < 0) return;
    char rbuf[sizeof(ws_msg_t) + WS_TILE_DIM * WS_TILE_DIM * 4];
    for (;;) {
        long n = port_recv(bootstrap, rbuf, sizeof(rbuf), 1 /*nonblock*/);
        if (n <= 0) break;
        handle_request(rbuf, n);
    }
}

/* --------------------------------------------------------------------------
 * Rendering
 * -------------------------------------------------------------------------- */
static void term_render(window_t *w, int focused) {
    int ox = 4, oy = TB_H + 2;
    gfx_rect(&w->surf, ox - 2, oy - 2, TCOLS * FONT_W + 4, TROWS * FONT_H + 4, C_TERMBG);
    for (int r = 0; r < TROWS; r++)
        for (int c = 0; c < TCOLS; c++) {
            char ch = term_grid[r][c];
            if (ch != ' ')
                gfx_char(&w->surf, ox + c * FONT_W, oy + r * FONT_H, ch, C_TERMFG, GFX_TRANSPARENT);
        }
    if (focused)
        gfx_rect(&w->surf, ox + term_cx * FONT_W, oy + term_cy * FONT_H, FONT_W, FONT_H, 0x0080c060);
}

static void demo_render(window_t *w, int frame) {
    surface_t c = content_of(w);
    int cw = c.w - 8, ch = c.h - 8, ox = 4, oy = 4;
    for (int y = 0; y < ch; y++) {
        uint32_t col = rgb((uint8_t)(40 + y * 120 / (ch ? ch : 1)),
                           (uint8_t)(20 + ((frame * 2) & 0x7f)),
                           (uint8_t)(120 + y * 100 / (ch ? ch : 1)));
        gfx_rect(&c, ox, oy + y, cw, 1, col);
    }
    int span = cw - 30 > 1 ? cw - 30 : 1;
    int bx = (frame * 3) % span;
    if ((frame / span) & 1) bx = span - bx;
    gfx_rect(&c, ox + bx, oy + ch / 2 - 12, 24, 24, 0x00ffd040);
    gfx_text(&c, ox + 8, oy + 6, "Lariat GUI", 0x00ffffff, GFX_TRANSPARENT);
}

/* Draw a window's chrome (title bar + close box + border) over its surface. */
static void render_chrome(window_t *w, int focused) {
    gfx_rect(&w->surf, 0, 0, w->w, TB_H, focused ? C_TBARF : C_TBAR);
    gfx_text(&w->surf, 6, 2, w->title, C_TBART, GFX_TRANSPARENT);
    gfx_rect(&w->surf, w->w - CLOSE_SZ - 4, 3, CLOSE_SZ, CLOSE_SZ, C_CLOSE);
    gfx_text(&w->surf, w->w - CLOSE_SZ - 1, 2, "x", 0x00ffffff, GFX_TRANSPARENT);
    gfx_frame(&w->surf, 0, 0, w->w, w->h, C_WINBDR);
}

/* --------------------------------------------------------------------------
 * Launcher panel
 * -------------------------------------------------------------------------- */
typedef struct { const char *label; int x, w; const char *exec; } button_t;
static button_t buttons[] = {
    { "Calc",  0, 56, "/usr/bin/calc" },
    { "Edit",  0, 56, "/usr/bin/edit" },
    { "Setup", 0, 64, "/bin/settings" },
    { "Term",  0, 56, NULL },     /* NULL exec = raise built-in terminal */
};
#define NBUTTON ((int)(sizeof(buttons) / sizeof(buttons[0])))

static void layout_buttons(void) {
    int x = 130;
    for (int i = 0; i < NBUTTON; i++) { buttons[i].x = x; x += buttons[i].w + 6; }
}

static void spawn(const char *path) {
    int pid = fork();
    if (pid == 0) {
        char *av[] = { (char *)path, 0 };
        execve(path, av, environ);
        _exit(127);
    }
}

static void launcher_click(int mx, int my) {
    if (my >= PANEL_H) return;
    for (int i = 0; i < NBUTTON; i++) {
        if (mx >= buttons[i].x && mx < buttons[i].x + buttons[i].w) {
            if (buttons[i].exec) spawn(buttons[i].exec);
            else if (term_win >= 0) raise_window(term_win);
            return;
        }
    }
}

static const char *cursor_img[] = {
    "X           ","XX          ","X.X         ","X..X        ",
    "X...X       ","X....X      ","X.....X     ","X......X    ",
    "X.......X   ","X........X  ","X.....XXXXX ","X..X..X     ",
    "X.X X..X    ","XX  X..X    ","X    X..X   ","     XXXX   ",
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
 * Main
 * -------------------------------------------------------------------------- */
int main(void) {
    gfx_t g;
    if (gfx_open(&g) != 0) { printf("gui: cannot open /dev/fb0\n"); return 1; }
    int infd = open("/dev/input", O_RDONLY);
    if (infd < 0) { printf("gui: cannot open /dev/input\n"); return 1; }

    bootstrap = port_create(WS_BOOTSTRAP);   /* may be -1 if already taken */
    layout_buttons();

    /* Built-in terminal + demo windows. */
    int tw = TCOLS * FONT_W + 12, th = TROWS * FONT_H + TB_H + 10;
    term_win = alloc_window(WK_TERM, -1, 60, 70, tw, th, "Terminal - /bin/sh");
    alloc_window(WK_DEMO, -1, 620, 150, 320, 240, "Demo");
    term_clear();
    term_spawn_shell();

    int mx = SCRW / 2, my = SCRH / 2;
    int dragging = -1, drag_dx = 0, drag_dy = 0;
    int frame = 0;

    for (;;) {
        struct pollfd pfd[2];
        pfd[0].fd = infd;   pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = sh_out; pfd[1].events = POLLIN; pfd[1].revents = 0;
        poll(pfd, 2, 0);

        if (pfd[1].revents & POLLIN) {
            char buf[512];
            int n = (int)read(sh_out, buf, sizeof(buf));
            if (n > 0) term_puts(buf, n);
        }

        drain_ipc();

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
                    } else {
                        int f = focused_window();
                        if (f >= 0 && wins[f].kind == WK_CLIENT) {
                            window_t *w = &wins[f];
                            if (mx >= w->x && mx < w->x + w->w &&
                                my >= w->y + TB_H && my < w->y + w->h)
                                send_ev(w->client, WS_EV_MOUSE, w->win_id,
                                        mx - w->x, my - w->y - TB_H, 0, 0);
                        }
                    }
                } else if (e->type == EV_KEY && e->code == BTN_LEFT) {
                    if (e->value) {
                        launcher_click(mx, my);
                        for (int k = norder - 1; k >= 0; k--) {
                            int idx = order[k];
                            window_t *w = &wins[idx];
                            if (mx >= w->x && mx < w->x + w->w &&
                                my >= w->y && my < w->y + w->h) {
                                raise_window(idx);
                                /* close box? */
                                if (my < w->y + TB_H &&
                                    mx >= w->x + w->w - CLOSE_SZ - 4 &&
                                    mx <  w->x + w->w - 4) {
                                    if (w->kind == WK_CLIENT)
                                        send_ev(w->client, WS_EV_CLOSE, w->win_id, 0, 0, 0, 0);
                                    if (w->kind != WK_TERM) free_window(idx);
                                } else if (my < w->y + TB_H) {
                                    dragging = idx; drag_dx = mx - w->x; drag_dy = my - w->y;
                                } else if (w->kind == WK_CLIENT) {
                                    send_ev(w->client, WS_EV_MOUSE, w->win_id,
                                            mx - w->x, my - w->y - TB_H, 1, 0);
                                }
                                break;
                            }
                        }
                    } else {
                        dragging = -1;
                    }
                } else if (e->type == EV_KEY && e->code < 0x100) {
                    int f = focused_window();
                    if (f < 0) continue;
                    if (wins[f].kind == WK_TERM) {
                        char ch = (char)e->code;
                        if (ch == '\n' || ch == '\r') {
                            term_putc('\n');
                            if (sh_in >= 0) { write(sh_in, line_buf, line_len); write(sh_in, "\n", 1); }
                            line_len = 0;
                        } else if (ch == '\b' || ch == 127) {
                            if (line_len > 0) { line_len--; term_putc('\b'); }
                        } else if (ch >= 32) {
                            if (line_len < (int)sizeof(line_buf) - 1) line_buf[line_len++] = ch;
                            term_putc(ch);
                        }
                    } else if (wins[f].kind == WK_CLIENT) {
                        send_ev(wins[f].client, WS_EV_KEY, wins[f].win_id, 0, 0, e->code, 1);
                    }
                }
            }
        }

        /* Composite. */
        gfx_fill(&g.back, C_WALL);
        gfx_rect(&g.back, 0, 0, SCRW, PANEL_H, C_PANEL);
        gfx_text(&g.back, 8, 5, "Lariat", C_PANELT, GFX_TRANSPARENT);
        for (int i = 0; i < NBUTTON; i++) {
            int hot = (mx >= buttons[i].x && mx < buttons[i].x + buttons[i].w && my < PANEL_H);
            gfx_rect(&g.back, buttons[i].x, 3, buttons[i].w, PANEL_H - 6, hot ? C_BTNH : C_BTN);
            gfx_text(&g.back, buttons[i].x + 8, 5, buttons[i].label, C_PANELT, GFX_TRANSPARENT);
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long secs = ts.tv_sec;
        int hh = (int)(secs / 3600) % 100, mm = (int)((secs / 60) % 60), ss = (int)(secs % 60);
        char clk[16] = "up 00:00:00";
        clk[3] = '0' + hh / 10; clk[4] = '0' + hh % 10;
        clk[6] = '0' + mm / 10; clk[7] = '0' + mm % 10;
        clk[9] = '0' + ss / 10; clk[10] = '0' + ss % 10;
        gfx_text(&g.back, SCRW - 8 - (int)strlen(clk) * FONT_W, 5, clk, C_PANELT, GFX_TRANSPARENT);

        for (int k = 0; k < norder; k++) {
            int idx = order[k];
            window_t *w = &wins[idx];
            int focused = (idx == focused_window());
            if (w->kind == WK_TERM)      term_render(w, focused);
            else if (w->kind == WK_DEMO) demo_render(w, frame);
            /* WK_CLIENT content is painted by the client via the protocol. */
            render_chrome(w, focused);
            gfx_blit(&g.back, w->x, w->y, &w->surf);
        }

        draw_cursor(&g.back, mx, my);
        gfx_present(&g);
        frame++;
    }
    return 0;
}
