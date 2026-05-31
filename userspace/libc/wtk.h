#ifndef LIBC_WTK_H
#define LIBC_WTK_H

/* wtk - a tiny widget toolkit for Lariat GUI clients, built on the window
 * server client library (ws.h).  Provides labels, buttons, and single-line
 * text inputs in a simple retained model with mouse/keyboard event dispatch. */

#include <stdint.h>
#include "ws.h"

typedef enum { WTK_LABEL, WTK_BUTTON, WTK_INPUT } wtk_kind_t;

struct wtk_window;
typedef void (*wtk_cb)(struct wtk_window *w, int widget_id, void *user);

typedef struct {
    wtk_kind_t kind;
    int        x, y, w, h;
    char       text[64];
    uint32_t   fg, bg;
    wtk_cb     on_click;
    void      *user;
    int        focus;          /* WTK_INPUT: has keyboard focus */
} wtk_widget_t;

#define WTK_MAX_WIDGETS 48

typedef struct wtk_window {
    ws_conn_t   *conn;
    int          win;
    int          w, h;
    uint32_t     bg;
    wtk_widget_t widgets[WTK_MAX_WIDGETS];
    int          nwidget;
} wtk_window_t;

void wtk_init(wtk_window_t *w, ws_conn_t *conn, int win, int width, int height, uint32_t bg);

int  wtk_label (wtk_window_t *w, int x, int y, const char *text, uint32_t fg);
int  wtk_button(wtk_window_t *w, int x, int y, int bw, int bh, const char *text,
                wtk_cb cb, void *user);
int  wtk_input (wtk_window_t *w, int x, int y, int bw, int bh);

void wtk_set_text(wtk_window_t *w, int id, const char *text);
const char *wtk_get_text(wtk_window_t *w, int id);

void wtk_draw(wtk_window_t *w);
/* Dispatch one ws event (mouse/key). Returns 1 if it caused a redraw need. */
int  wtk_handle(wtk_window_t *w, const ws_event_t *ev);

#endif /* LIBC_WTK_H */
