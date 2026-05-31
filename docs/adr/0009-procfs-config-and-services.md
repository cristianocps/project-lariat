# ADR-0009: procfs, /etc configuration, and init services

- Status: Accepted
- Date: 2026-05-30

## Context

Phase 4 needs runtime-configurable system settings. Before this, `uname`
strings were hardcoded in `cpu/syscall.c`, there was no `/proc`/`/sys`, no
`/etc` configuration beyond the account databases, and PID 1 only ran a bare
login loop with no service supervision.

## Decision

**procfs** (`kernel/fs/procfs.c`): a minimal synthetic filesystem mounted at
`/proc`, backed by per-file generators (and optional setters). Exposes
`/proc/version`, `/proc/hostname` (rw), `/proc/uptime`, `/proc/meminfo`,
`/proc/net/info`, and `/proc/sys/kernel/{hostname,timer_hz}`. The live hostname
is a kernel tunable (`sys_hostname()`/`sys_set_hostname()`) that `uname`'s
`nodename` now reads, settable through `/proc`.

**/etc configuration**: `kernel/kernel.c` seeds factory defaults onto the
persistent root (ADR-0004) if absent and restores them into the ramfs `/etc`
on boot: `/etc/hostname`, `/etc/lariat.conf` (key=value: hostname, timezone,
motd), `/etc/fstab`, `/etc/profile`. The persisted hostname is applied to the
kernel tunable at boot. (The persistent copy of `lariat.conf` uses a dot-free
8.3 name because the simple FAT32 driver does not store long names.)

**init toward services** (`userspace/init.c`): PID 1 gains `load_config()`
(prints the motd from `/etc/lariat.conf`) and a small service supervisor that
reads `/etc/services.d/<name>.conf` files (`exec=`, `respawn=`), starts them at
boot, and respawns `respawn=1` services when they exit. The console login
session is the first managed (respawn) service, so the old bespoke login loop
becomes a general supervised-service mechanism.

## Consequences

- `uname` is now data-driven for `nodename`; other fields remain static.
- procfs is read-mostly; it does not yet expose per-process directories.
- The service supervisor is intentionally minimal (no dependencies, ordering,
  or socket activation); it converges with the Phase M launchd-like direction
  and can host the `windowserver` and network daemons as service definitions.
- Settings UIs (GUI Settings app) write through `/etc` + `/proc`.
