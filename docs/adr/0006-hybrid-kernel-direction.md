# ADR-0006: Hybrid (macOS/XNU-style) kernel direction

- Status: Accepted
- Date: 2026-05-30

## Context

Lariat is a monolithic kernel: drivers, filesystems, the TCP/IP stack, and the
display backing all run in kernel space. We want to move toward a microkernel
"service" structure similar to macOS. macOS/XNU, however, is a *hybrid* kernel:
a Mach core (IPC ports, scheduling, VM) fused with BSD subsystems, with most
drivers still in kernel space (IOKit) and `launchd` supervising userspace
services. A pure microkernel (drivers/fs/net all in userspace servers) is far
more work and risk.

## Decision

Adopt a hybrid kernel direction, not a pure microkernel:

- **In-kernel core stays**: scheduler, VM (PMM/VMM), low-level drivers
  (IOKit-style), syscall entry, and a new first-class IPC primitive.
- **Userspace services**: introduce supervised userspace daemons over the IPC,
  migrating selected subsystems out incrementally. The **display + input
  server** is the first proof-of-concept migration.
- **Boundary**: a subsystem moves to userspace only when the isolation/
  extensibility benefit outweighs the IPC cost; performance-critical and
  low-level paths remain in-kernel.

Userspace drivers (with userspace DMA/IRQ forwarding) are explicitly out of
scope for this milestone but the IPC layer must not preclude them later.

## Consequences

- Requires a new IPC mechanism and a service manager (ADR-0007, Phase M).
- The display protocol (ADR-0005) is the first consumer of the IPC.
- Drivers remain in `drivers/` in-kernel for now (matches IOKit).
- `kernel/ipc/` is added to the layout for the IPC subsystem.
