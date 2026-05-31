#ifndef LIBC_WS_H
#define LIBC_WS_H

/* Client library for the Lariat window server (Phase 3).  Speaks the protocol
 * in include/uapi/wsproto.h over Phase M IPC ports.  See userspace/gui.c. */

#include <stdint.h>
#include "wsproto.h"

typedef struct {
    int      srv;          /* port to the server (send)   */
    int      ev;           /* our event port (recv)       */
    uint32_t client_id;
    char     evname[32];
} ws_conn_t;

typedef struct {
    uint32_t op;           /* WS_EV_* */
    uint32_t win_id;
    int32_t  x, y;
    uint32_t a, b;
} ws_event_t;

/* Connect to the window server. Returns 0 on success, -1 if no server. */
int  ws_connect(ws_conn_t *c);
void ws_disconnect(ws_conn_t *c);

/* Create a window; returns a win_id (>=0) or -1. */
int  ws_create_window(ws_conn_t *c, int x, int y, int w, int h, const char *title);
void ws_destroy_window(ws_conn_t *c, int win);

/* Drawing primitives (window-local coordinates, content area). */
void ws_fill(ws_conn_t *c, int win, uint32_t color);
void ws_rect(ws_conn_t *c, int win, int x, int y, int w, int h, uint32_t color);
void ws_text(ws_conn_t *c, int win, int x, int y, uint32_t color, const char *s);
void ws_tile(ws_conn_t *c, int win, int x, int y, int w, int h, const uint32_t *px);
void ws_present(ws_conn_t *c, int win);
void ws_set_title(ws_conn_t *c, int win, const char *title);

/* Poll one input/control event. Returns 1 if an event was read, else 0. */
int  ws_poll_event(ws_conn_t *c, ws_event_t *ev);
/* Block until an event arrives. Returns 1, or 0 on error. */
int  ws_wait_event(ws_conn_t *c, ws_event_t *ev);

#endif /* LIBC_WS_H */
