#ifndef _LARIAT_WSPROTO_H
#define _LARIAT_WSPROTO_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Lariat window-server protocol (Phase 3, riding on Phase M IPC ports).
 *
 * The compositor ("windowserver", userspace/gui.c) registers a well-known
 * bootstrap port (WS_BOOTSTRAP) and receives request datagrams from clients.
 * Each client first creates its own reply/event port named "ws.ev.<pid>" and
 * CONNECTs, passing that name; the server addresses input events and replies
 * to that port.  All messages fit in one LIPC datagram (<= LIPC_MSG_MAX).
 *
 * Bulk pixels are pushed as fixed 32x32 XRGB tiles (WS_TILE): a 32x32 tile is
 * exactly 4096 bytes, which matches LIPC_MSG_MAX, so the tile header is sent
 * as a separate small message immediately followed by the raw tile payload.
 * Simple primitives (fill/rect/text) avoid per-pixel traffic entirely.
 * -------------------------------------------------------------------------- */

#define WS_BOOTSTRAP   "lariat.wm"
#define WS_EVPORT_FMT  "ws.ev.%d"      /* per-client event port (pid) */
#define WS_TITLE_MAX   28
#define WS_TEXT_MAX    64
#define WS_TILE_DIM    16              /* max tile is WS_TILE_DIM x WS_TILE_DIM */

/* Client -> server request opcodes. */
enum {
    WS_CONNECT      = 1,   /* establish a session; payload: ws_connect */
    WS_CREATE_WIN   = 2,   /* create a window;       payload: ws_create */
    WS_FILL         = 3,   /* fill window bg;         a=color */
    WS_RECT         = 4,   /* draw rect; x,y,w,h, a=color */
    WS_TEXT         = 5,   /* draw text; x,y, a=color, str */
    WS_TILE         = 6,   /* blit pixels at x,y (w<=16,h<=16): ws_msg_t then
                            * w*h XRGB pixels appended in the same datagram */
    WS_PRESENT      = 7,   /* mark the window dirty / request composite */
    WS_DESTROY_WIN  = 8,   /* destroy a window */
    WS_TITLE        = 9,   /* set window title; str */
};

/* Server -> client event opcodes (delivered to the client's event port). */
enum {
    WS_EV_CONNECTED = 100, /* a=client_id */
    WS_EV_CREATED   = 101, /* win_id assigned; a=win_id */
    WS_EV_KEY       = 102, /* a=keycode, b=pressed */
    WS_EV_MOUSE     = 103, /* x,y window-local; a=buttons */
    WS_EV_CLOSE     = 104, /* the user closed win_id */
    WS_EV_FOCUS     = 105, /* a=1 gained focus, 0 lost */
};

/* One fixed-layout control message.  `str` carries titles / draw text. */
typedef struct {
    uint32_t op;
    uint32_t client_id;
    uint32_t win_id;
    int32_t  x, y, w, h;
    uint32_t a, b, c, d;
    char     str[WS_TITLE_MAX > WS_TEXT_MAX ? WS_TITLE_MAX : WS_TEXT_MAX];
} ws_msg_t;

/* CONNECT payload reuses ws_msg_t with str = the client's event port name. */

#endif /* _LARIAT_WSPROTO_H */
