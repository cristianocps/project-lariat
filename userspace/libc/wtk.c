/* wtk - tiny widget toolkit implementation.  See wtk.h. */

#include "wtk.h"
#include "string.h"
#include "font8x16.h"

#define FW 8
#define FH 16

#define COL_LABELFG 0x00d8e0ec
#define COL_BTN     0x00384866
#define COL_BTNFG   0x00ffffff
#define COL_INPUT   0x00101418
#define COL_INPUTFG 0x0080e0a0
#define COL_BORDER  0x00586273

void wtk_init(wtk_window_t *w, ws_conn_t *conn, int win, int width, int height, uint32_t bg) {
    memset(w, 0, sizeof(*w));
    w->conn = conn; w->win = win; w->w = width; w->h = height; w->bg = bg;
}

static int add(wtk_window_t *w, wtk_kind_t kind, int x, int y, int bw, int bh) {
    if (w->nwidget >= WTK_MAX_WIDGETS) return -1;
    int id = w->nwidget++;
    wtk_widget_t *wd = &w->widgets[id];
    memset(wd, 0, sizeof(*wd));
    wd->kind = kind; wd->x = x; wd->y = y; wd->w = bw; wd->h = bh;
    return id;
}

int wtk_label(wtk_window_t *w, int x, int y, const char *text, uint32_t fg) {
    int id = add(w, WTK_LABEL, x, y, (int)strlen(text) * FW, FH);
    if (id < 0) return -1;
    strncpy(w->widgets[id].text, text, sizeof(w->widgets[id].text) - 1);
    w->widgets[id].fg = fg ? fg : COL_LABELFG;
    return id;
}

int wtk_button(wtk_window_t *w, int x, int y, int bw, int bh, const char *text,
               wtk_cb cb, void *user) {
    int id = add(w, WTK_BUTTON, x, y, bw, bh);
    if (id < 0) return -1;
    strncpy(w->widgets[id].text, text, sizeof(w->widgets[id].text) - 1);
    w->widgets[id].fg = COL_BTNFG; w->widgets[id].bg = COL_BTN;
    w->widgets[id].on_click = cb; w->widgets[id].user = user;
    return id;
}

int wtk_input(wtk_window_t *w, int x, int y, int bw, int bh) {
    int id = add(w, WTK_INPUT, x, y, bw, bh);
    if (id < 0) return -1;
    w->widgets[id].fg = COL_INPUTFG; w->widgets[id].bg = COL_INPUT;
    return id;
}

void wtk_set_text(wtk_window_t *w, int id, const char *text) {
    if (id < 0 || id >= w->nwidget) return;
    strncpy(w->widgets[id].text, text, sizeof(w->widgets[id].text) - 1);
    w->widgets[id].text[sizeof(w->widgets[id].text) - 1] = '\0';
}

const char *wtk_get_text(wtk_window_t *w, int id) {
    if (id < 0 || id >= w->nwidget) return "";
    return w->widgets[id].text;
}

void wtk_draw(wtk_window_t *w) {
    ws_fill(w->conn, w->win, w->bg);
    for (int i = 0; i < w->nwidget; i++) {
        wtk_widget_t *wd = &w->widgets[i];
        switch (wd->kind) {
        case WTK_LABEL:
            ws_text(w->conn, w->win, wd->x, wd->y, wd->fg, wd->text);
            break;
        case WTK_BUTTON:
            ws_rect(w->conn, w->win, wd->x, wd->y, wd->w, wd->h, wd->bg);
            ws_text(w->conn, w->win, wd->x + (wd->w - (int)strlen(wd->text) * FW) / 2,
                    wd->y + (wd->h - FH) / 2, wd->fg, wd->text);
            break;
        case WTK_INPUT:
            ws_rect(w->conn, w->win, wd->x, wd->y, wd->w, wd->h, wd->bg);
            ws_rect(w->conn, w->win, wd->x, wd->y, wd->w, 1, COL_BORDER);
            ws_text(w->conn, w->win, wd->x + 3, wd->y + (wd->h - FH) / 2, wd->fg, wd->text);
            break;
        }
    }
    ws_present(w->conn, w->win);
}

static int hit(wtk_widget_t *wd, int x, int y) {
    return x >= wd->x && x < wd->x + wd->w && y >= wd->y && y < wd->y + wd->h;
}

int wtk_handle(wtk_window_t *w, const ws_event_t *ev) {
    if (ev->op == WS_EV_MOUSE && ev->a /* button down */) {
        for (int i = 0; i < w->nwidget; i++) {
            wtk_widget_t *wd = &w->widgets[i];
            if (wd->kind == WTK_BUTTON && hit(wd, ev->x, ev->y)) {
                if (wd->on_click) wd->on_click(w, i, wd->user);
                return 1;
            }
            if (wd->kind == WTK_INPUT && hit(wd, ev->x, ev->y)) {
                for (int j = 0; j < w->nwidget; j++)
                    if (w->widgets[j].kind == WTK_INPUT) w->widgets[j].focus = 0;
                wd->focus = 1;
                return 1;
            }
        }
        return 0;
    }
    if (ev->op == WS_EV_KEY && ev->b /* pressed */) {
        for (int i = 0; i < w->nwidget; i++) {
            wtk_widget_t *wd = &w->widgets[i];
            if (wd->kind == WTK_INPUT && wd->focus) {
                int ch = (int)ev->a;
                size_t n = strlen(wd->text);
                if (ch == '\b' || ch == 127) { if (n) wd->text[n - 1] = '\0'; }
                else if (ch >= 32 && ch < 127 && n < sizeof(wd->text) - 1) {
                    wd->text[n] = (char)ch; wd->text[n + 1] = '\0';
                }
                return 1;
            }
        }
    }
    return 0;
}
