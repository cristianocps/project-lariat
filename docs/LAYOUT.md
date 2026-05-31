# Repository Layout & Conventions

This document defines where code lives in Project Lariat and the conventions new
contributions should follow. It is the source of truth referenced by ADR-0006
(hybrid kernel direction) and Phase A of the evolution roadmap.

## Top-level map

```
project-lariat/
├── boot/              # Real-mode boot sector + AP trampoline (NASM)
├── cpu/               # Arch/CPU core: GDT, IDT, PIC, timer, SYSCALL entry
├── kernel/            # Kernel C/asm, grouped by subsystem (see below)
│   ├── fs/            # Filesystems: ramfs, fat32, ext4, procfs
│   └── net/           # TCP/IP stack: pbuf, eth, arp, ipv4, icmp, udp, tcp, socket
├── drivers/           # Device drivers, grouped by class
│   ├── block/         # Block devices (ATA/IDE)
│   ├── char/          # Character devices (serial/COM1)
│   ├── input/         # Input devices (PS/2 keyboard, mouse)
│   ├── net/           # NIC drivers (rtl8139)
│   └── video/         # Framebuffer drivers (Bochs/QEMU VBE)
├── include/           # Kernel-internal headers
│   └── uapi/          # PUBLIC ABI headers (shared with userspace + sysroot)
├── userspace/         # Ring-3 programs
│   └── libc/          # In-tree minimal libc (bootstrap programs)
├── sysroot/           # Generated cross-toolchain sysroot (Phase 1; gitignored)
├── toolchain/         # Toolchain build scripts (Phase 1/5)
├── pkg/               # Package manager format spec + sample package recipes
├── docs/              # Architecture docs + ADRs
│   └── adr/           # Architecture Decision Records
├── linker.ld          # Kernel linker script (load at 0x100000)
└── Makefile           # Build system
```

## Header policy: `include/` vs `include/uapi/`

The header tree is split to seed the cross-toolchain sysroot:

- `include/uapi/` — the **public ABI**: syscall numbers (`syscall.h`), shared
  kernel/user structs and constants (`uapi.h`: input events, framebuffer info,
  ioctl numbers). These are the only kernel headers a userspace program or the
  ported libc (Phase 1) is allowed to depend on. Both the kernel Makefile and
  the userspace Makefile add `-Iinclude/uapi`.
- `include/` (top level) — **kernel-internal** headers (`pmm.h`, `vmm.h`,
  `sched.h`, `smp.h`, `vfs.h`, driver/device frameworks, ...). Userspace must
  not include these.

Rule of thumb: if a definition crosses the syscall boundary (is part of the
contract between kernel and userspace), it belongs in `include/uapi/`. Otherwise
it stays in `include/`.

## Driver placement

Drivers live under `drivers/<class>/`. Pick the class by the device's primary
interface: `block`, `char`, `input`, `net`, `video`. A new class gets a new
subdirectory plus a pattern rule in the Makefile (`$(BUILD_DIR)/drivers/<class>/%.o`)
and an entry in the `dirs:` target.

## Kernel subsystem grouping (target convention)

New kernel code should be filed by subsystem. `kernel/fs/` and `kernel/net/`
already follow this. As files are touched, prefer migrating them toward:

- `kernel/mm/`    — physical/virtual memory (`pmm`, `vmm`, `kapi`)
- `kernel/sched/` — scheduler, process, fork
- `kernel/fs/`    — filesystems + VFS
- `kernel/net/`   — networking
- `kernel/dev/`   — device/driver/module frameworks, PCI
- `kernel/ipc/`   — IPC ports + service bootstrap (Phase M)
- `kernel/core/`  — kmain, ACPI, SMP, LAPIC/IOAPIC

Moves are mechanical: update the `KERNEL_C` list and `dirs:`/pattern rules in
the Makefile, then rebuild. Because all headers resolve via `-Iinclude`, moving
`.c` files does not change include resolution. Do one subsystem per change and
verify the build after each move.

## Userspace program placement (target convention)

- `userspace/`            — coreutils-style `/bin` programs and the libc.
- system daemons (init, login, future `windowserver`, service manager) and GUI
  apps follow as the Phase M/Phase 3 work lands.

## Build artifacts

`build/`, `*.img`, `*.elf`, `*.o`, `*.bin`, `*.deb`, and `.local_libs/` are all
gitignored. Never commit build output, disk images, or locally fetched
dependencies.
