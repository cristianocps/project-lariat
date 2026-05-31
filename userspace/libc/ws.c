/* Lariat window-server client library.  See ws.h and include/uapi/wsproto.h. */

#include "ws.h"
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "lipc.h"

static void msg_init(ws_msg_t *m, uint32_t op, const ws_conn_t *c, int win) {
    memset(m, 0, sizeof(*m));
    m->op = op;
    m->client_id = c->client_id;
    m->win_id = (uint32_t)win;
}

static int recv_event(ws_conn_t *c, ws_event_t *out, int nonblock) {
    ws_msg_t m;
    long n = port_recv(c->ev, &m, sizeof(m), nonblock);
    if (n < (long)sizeof(uint32_t)) return 0;
    out->op = m.op;
    out->win_id = m.win_id;
    out->x = m.x;
    out->y = m.y;
    out->a = m.a;
    out->b = m.b;
    return 1;
}

int ws_connect(ws_conn_t *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->evname, sizeof(c->evname), WS_EVPORT_FMT, getpid());
    c->ev = port_create(c->evname);
    if (c->ev < 0) return -1;
    c->srv = port_open(WS_BOOTSTRAP);
    if (c->srv < 0) return -1;

    ws_msg_t m;
    msg_init(&m, WS_CONNECT, c, 0);
    strncpy(m.str, c->evname, sizeof(m.str) - 1);
    if (port_send(c->srv, &m, sizeof(m)) < 0) return -1;

    /* Await the CONNECTED reply that carries our client id. */
    for (;;) {
        ws_event_t ev;
        if (!recv_event(c, &ev, 0)) return -1;
        if (ev.op == WS_EV_CONNECTED) { c->client_id = ev.a; return 0; }
    }
}

void ws_disconnect(ws_conn_t *c) { (void)c; }

int ws_create_window(ws_conn_t *c, int x, int y, int w, int h, const char *title) {
    ws_msg_t m;
    msg_init(&m, WS_CREATE_WIN, c, 0);
    m.x = x; m.y = y; m.w = w; m.h = h;
    if (title) strncpy(m.str, title, sizeof(m.str) - 1);
    if (port_send(c->srv, &m, sizeof(m)) < 0) return -1;
    for (;;) {
        ws_event_t ev;
        if (!recv_event(c, &ev, 0)) return -1;
        if (ev.op == WS_EV_CREATED) return ev.a ? (int)ev.a : -1;
    }
}

void ws_destroy_window(ws_conn_t *c, int win) {
    ws_msg_t m;
    msg_init(&m, WS_DESTROY_WIN, c, win);
    port_send(c->srv, &m, sizeof(m));
}

void ws_fill(ws_conn_t *c, int win, uint32_t color) {
    ws_msg_t m;
    msg_init(&m, WS_FILL, c, win);
    m.a = color;
    port_send(c->srv, &m, sizeof(m));
}

void ws_rect(ws_conn_t *c, int win, int x, int y, int w, int h, uint32_t color) {
    ws_msg_t m;
    msg_init(&m, WS_RECT, c, win);
    m.x = x; m.y = y; m.w = w; m.h = h; m.a = color;
    port_send(c->srv, &m, sizeof(m));
}

void ws_text(ws_conn_t *c, int win, int x, int y, uint32_t color, const char *s) {
    ws_msg_t m;
    msg_init(&m, WS_TEXT, c, win);
    m.x = x; m.y = y; m.a = color;
    if (s) strncpy(m.str, s, sizeof(m.str) - 1);
    port_send(c->srv, &m, sizeof(m));
}

void ws_tile(ws_conn_t *c, int win, int x, int y, int w, int h, const uint32_t *px) {
    if (w > WS_TILE_DIM || h > WS_TILE_DIM) return;
    /* ws_msg_t header followed by w*h XRGB pixels in one datagram. */
    char buf[sizeof(ws_msg_t) + WS_TILE_DIM * WS_TILE_DIM * 4];
    ws_msg_t *m = (ws_msg_t *)buf;
    msg_init(m, WS_TILE, c, win);
    m->x = x; m->y = y; m->w = w; m->h = h;
    unsigned long bytes = (unsigned long)w * (unsigned long)h * 4;
    memcpy(buf + sizeof(ws_msg_t), px, bytes);
    port_send(c->srv, buf, sizeof(ws_msg_t) + bytes);
}

void ws_present(ws_conn_t *c, int win) {
    ws_msg_t m;
    msg_init(&m, WS_PRESENT, c, win);
    port_send(c->srv, &m, sizeof(m));
}

void ws_set_title(ws_conn_t *c, int win, const char *title) {
    ws_msg_t m;
    msg_init(&m, WS_TITLE, c, win);
    if (title) strncpy(m.str, title, sizeof(m.str) - 1);
    port_send(c->srv, &m, sizeof(m));
}

int ws_poll_event(ws_conn_t *c, ws_event_t *ev) { return recv_event(c, ev, 1); }
int ws_wait_event(ws_conn_t *c, ws_event_t *ev) { return recv_event(c, ev, 0); }
