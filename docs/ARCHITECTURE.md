# Project Lariat Architecture

Project Lariat is a 64-bit x86_64 operating system built from scratch. This
document gives the subsystem-level overview; see `LAYOUT.md` for where code
lives and `adr/` for the rationale behind major decisions.

## Boot & CPU bring-up

1. BIOS loads the 512-byte boot sector (`boot/boot.asm`) at `0x7C00`.
2. The bootloader queries the e820 memory map, loads the kernel via LBA reads,
   switches 16-bit real -> 32-bit protected -> 64-bit long mode, and jumps to
   `kmain()` at physical `0x100000`.
3. Early in `kmain`, the VMM builds a higher-half direct map of all physical RAM
   at `0xFFFF800000000000` (`phys_to_virt`/`virt_to_phys`).
4. ACPI (RSDP/XSDT/MADT) discovers CPU topology and the LAPIC/IOAPIC bases; the
   8259 PIC is disabled in favor of the Local + I/O APICs.
5. `smp_init()` brings each application processor online (INIT-SIPI-SIPI) with
   its own TSS and per-CPU SYSCALL/SYSRET MSRs.

## Memory

- **PMM** (`kernel/pmm.c`): physical frame allocator over a dynamic e820 bitmap.
- **VMM** (`kernel/vmm.c`): 4-level page tables, per-process address spaces,
  page-table deep clone for `fork()`, higher-half direct map.
- **User layout** (`include/process.h`): code at `0x40000000`, heap (`brk`) from
  `0x50000000`, mmap arena from `0x700000000`, stack near `0x7FC0000000`.

## Scheduling

Preemptive, multi-core round-robin (`kernel/sched.c`). Every CPU's LAPIC timer
drives a context switch against a single shared, `sched_lock`-protected ready
queue, so ring-3 threads load-balance and migrate across cores.

## Syscalls & processes

- SYSCALL/SYSRET fast path (`cpu/syscall.c`, `cpu/syscall_asm.asm`), Linux
  x86_64 numbering, negative-`errno` returns. Syscall numbers are the public ABI
  in `include/uapi/syscall.h`.
- `fork`/`clone`/`execve`/`wait4` with credentials (uid/gid/groups) per thread.
- ELF64 loader (`kernel/elf.c`) for `/bin` programs (embedded in the kernel
  image and/or loaded from a filesystem).

## Filesystems

VFS layer (`kernel/vfs.c`) presents a single Unix-like namespace (see
`adr/0013` and `FILESYSTEM.md`): ramfs is the immutable system root `/` (rebuilt
from the kernel image each boot), the writable ext4 data volume mounts at `/var`
(persistent state, package DB, homes), FAT32 is a legacy scratch mount at
`/mnt/legacy`, and procfs at `/proc`. A minimal devfs exposes `/dev/console`,
`/dev/fb0`, `/dev/input`. The backing device/fs-type is metadata, not part of
the path. `/etc` and `/home` are **firmlinks** (symlinks) into the `/var` data
volume, so system config and home directories persist in place — the VFS
follows symlinks during path resolution (depth-limited). A macOS-style
immutable-system + persistent-data-volume model.

## Drivers

PCI enumeration plus ATA/IDE (block), PS/2 keyboard + mouse and interrupt-driven
COM1 (input/char), Bochs/QEMU VBE framebuffer (video), and rtl8139 (net), routed
through the I/O APIC.

## Networking

In-kernel TCP/IP stack (`kernel/net/`): pbuf, ethernet, ARP, IPv4, ICMP, UDP,
TCP, and a BSD-style socket layer.

## Userspace

PID 1 `init` runs self-tests then a login loop; `login` authenticates against
`/etc/passwd`+`/etc/shadow` and drops privileges before exec'ing the user shell
(`/bin/sh`, with pipelines, redirection, and job control). A framebuffer
compositor (`gui`) provides an experimental desktop. A minimal in-tree libc
backs all bootstrap programs.

## Direction

The roadmap (`ROADMAP.md`, with rationale in `adr/`) evolves Lariat toward: a real-application toolchain
target with dynamic linking, a package manager, a hybrid (macOS/XNU-style)
kernel with first-class IPC and a service manager, an extensible desktop, and a
system settings subsystem.
