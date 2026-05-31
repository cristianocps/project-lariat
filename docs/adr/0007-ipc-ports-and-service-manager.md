# ADR-0007: IPC ports and the service manager

- Status: Accepted
- Date: 2026-05-30

## Context

A hybrid kernel (ADR-0006) needs (a) a first-class IPC mechanism for userspace
services and (b) a supervisor that launches and restarts them. Today IPC is
limited to pipes, signals, and TCP/UDP sockets (no AF_UNIX), and PID 1 only runs
a login loop with no service supervision or name lookup.

## Decision

**IPC ("ports")**: add a Mach-port-like, capability-based message-passing
primitive — bidirectional message queues with send/receive rights, small inline
messages plus shared-memory (page-grant) for bulk transfer, and a bootstrap/name
registry so clients look services up by name. It is bootstrapped on a new
AF_UNIX socket family (extending `kernel/net/socket.c`) plus `MAP_SHARED` mmap,
with room to grow into a dedicated kernel IPC object.

**Service manager (launchd-like)**: evolve PID 1 (`userspace/init.c`) into a
supervisor that reads service definitions from `/etc/services.d/*.conf`,
starts/stops/restarts/watches daemons, sets up their environment, owns the IPC
bootstrap registry, and supports on-demand launch. The login session and the
`windowserver` become managed services.

## Consequences

- Adds `kernel/ipc/` and AF_UNIX support; depends on shared-memory mmap and the
  expanded syscall set (Phase 0).
- Unifies the "init toward services" idea from Phase 4 into Phase M.
- Service definitions live on the persistent root (ADR-0004) under `/etc`.
- The `windowserver` (ADR-0005) is the first managed service and first IPC
  consumer.
