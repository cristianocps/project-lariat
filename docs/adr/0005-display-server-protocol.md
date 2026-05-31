# ADR-0005: Display server protocol for the desktop

- Status: Accepted
- Date: 2026-05-30

## Context

The desktop is a single monolithic program (`userspace/gui.c`) that owns
`/dev/fb0`, composites a hardcoded set of windows, and emulates a terminal by
piping to `/bin/sh`. There is no way for an independent application to create a
window; the window count and layout are compile-time constants. We want
multiple installable GUI apps (calculator, text editor) to coexist.

## Decision

Introduce a display-server protocol (`include/uapi/wsproto.h`). The compositor
(`userspace/gui.c`) owns `/dev/fb0` and `/dev/input`, registers a well-known
bootstrap IPC port (`lariat.wm`), and serves clients over the Phase M IPC
ports. As-built protocol:

- **Transport**: Phase M named IPC ports (not AF_UNIX, which is unimplemented).
  A client creates its own event port `ws.ev.<pid>`, sends `WS_CONNECT` with
  that name, and receives a `WS_EV_CONNECTED` reply carrying its client id.
- **Requests** (`ws_msg_t`, one datagram each): `WS_CREATE_WIN`,
  `WS_FILL`/`WS_RECT`/`WS_TEXT` primitives, `WS_TILE` (a <=16x16 XRGB pixel
  block appended to the message header — the "submit-buffer" path within the
  4 KiB datagram limit), `WS_PRESENT`, `WS_TITLE`, `WS_DESTROY_WIN`.
- **Events** (server -> client event port): `WS_EV_CREATED`, `WS_EV_KEY`,
  `WS_EV_MOUSE` (window-local), `WS_EV_CLOSE`, `WS_EV_FOCUS`.

Client surfaces are server-side (the server keeps each window's pixels and
applies the client's draw ops), so no cross-process shared memory is required
yet; bulk pixels use `WS_TILE`. A widget toolkit (`userspace/libc/wtk`) and a
client library (`userspace/libc/ws`) sit on top so apps do not marshal the wire
format by hand. The compositor keeps a built-in terminal so the desktop is
usable with no clients, and a panel launcher spawns IPC client apps.

## Consequences

- The hardcoded `NWIN`/`draw_order` model is replaced by a dynamic window list
  keyed by client id, with move/raise/close/z-order and a launcher (Phase 3).
- Depends on Phase M IPC ports (Phase 0 mmap not yet needed; server-side
  surfaces avoid shared memory for now — a future `WS_TILE` upgrade to
  `MAP_SHARED` can remove per-tile copies).
- Input flows kernel `/dev/input` -> `windowserver` -> focused client.
- The handshake (`WS_CONNECT`/`WS_CREATE_WIN`) is covered by the init
  self-test ("ws protocol: ok").
