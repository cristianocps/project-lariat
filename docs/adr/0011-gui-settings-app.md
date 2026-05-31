# ADR-0011: GUI Settings app

- Status: Accepted
- Date: 2026-05-30

## Context

Phase 4 calls for a graphical system-settings application covering hostname,
users, network, date/time, and display. The pieces it needs already exist: the
windowserver protocol and `ws`/`wtk` client libraries (ADR-0005), procfs
tunables (ADR-0009), and the account tools (ADR-0010).

## Decision

`userspace/settings.c` is a windowserver client (embedded as `/bin/settings`,
launched from the panel's "Setup" button) with five tabs implemented by
rebuilding the `wtk` widget list on demand:

- **Host** - prefilled from `/etc/hostname`; Apply writes `/etc/hostname`,
  persists via `etc_sync`, and applies live through `/proc/sys/kernel/hostname`.
- **Users** - lists `/etc/passwd` accounts with per-row Delete, plus an input +
  Add; both shell out to `/bin/useradd` / `/bin/userdel`.
- **Net** - read-only render of `/proc/net/info`.
- **Time** - read-only `/proc/uptime` and `/proc/version` (RTC set is not yet
  supported).
- **Disp** - display/window information.

Two windowserver robustness fixes landed alongside it:

1. **Always reply to `WS_CREATE_WIN`.** Previously the server `return`ed without
   a reply if window allocation failed, hanging the client forever in
   `ws_create_window`. It now sends `WS_EV_CREATED` with `win_id == 0` (window
   ids start at 1) to signal failure, and the client maps that to `-1`.
2. **Stop the init self-test from squatting the bootstrap port.** The headless
   windowserver-protocol self-test created the real `WS_BOOTSTRAP` port
   ("lariat.wm"). Because PID 1 runs forever and named ports have no destructor,
   this permanently prevented the real `gui` from claiming the name, so *no* GUI
   client could ever connect. The self-test now uses a dedicated
   `ws.selftest.wm` name.

## Consequences

- The Settings app is a regular ws/wtk client; it requires the desktop to run as
  root for the Users tab (useradd/userdel enforce uid 0 themselves).
- Window allocation failures now fail fast and visibly instead of hanging.
- Setting the wall clock and richer network configuration remain future work.
