# Project Lariat

A 64-bit operating system built from scratch for the x86_64 architecture, with a
preemptive scheduler, a Linux-leaning POSIX syscall surface, and symmetric
multiprocessing (SMP) bring-up.

> Documentation lives in [`docs/`](docs/): see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
> for the subsystem overview, [`docs/LAYOUT.md`](docs/LAYOUT.md) for the repo
> layout and conventions, and [`docs/adr/`](docs/adr/) for the Architecture
> Decision Records behind the evolution roadmap.

## Architecture

- **Bootloader**: Custom 16-bit real-mode boot sector written in NASM assembly
- **Transition**: 16-bit real mode → 32-bit protected mode → 64-bit long mode
- **Kernel**: 64-bit C kernel loaded and executed at physical `0x100000`
  (`-mcmodel=large`), with an assembly entry point
- **Memory**:
  - PMM: physical memory manager with a dynamically sized e820 bitmap (no fixed
    RAM ceiling; the low 2 MB is reserved for boot structures and the AP
    trampoline)
  - **Direct map (physmap)**: all physical RAM is linearly mapped into the
    higher half at `0xFFFF800000000000`, accessed through `phys_to_virt()` /
    `virt_to_phys()`. The kernel no longer dereferences raw physical addresses,
    which unlocks RAM above 4 GB.
  - VMM: 4-level page tables with per-process user address spaces, page-table
    deep clone for `fork()`, and recursive teardown on exit
- **Output**: VGA text mode driver (80×25, 16 colors) + serial COM1 logging
- **Interrupts**: IDT with Local APIC + I/O APIC (the legacy 8259 PIC is
  disabled); per-CPU LAPIC timers; IPIs for TLB shootdown
- **Scheduling**: **Preemptive, multi-core** round-robin scheduler. Every CPU's
  LAPIC timer drives a context switch against a single shared ready queue, so
  threads are load-balanced across all online cores. The running thread is
  per-CPU; the executing CPU is identified by its LAPIC id (not GS), so the BSP
  and APs share one scheduling path
- **SMP**: ACPI/MADT topology discovery, AP trampoline + INIT/SIPI bring-up,
  per-CPU TSS, per-CPU SYSCALL/SYSRET MSRs, and **application processors that
  schedule ring-3 threads** (a thread can migrate between cores). A
  lock-protected cross-core work queue also runs kernel jobs in parallel
- **Userspace**: Ring-3 processes via SYSCALL/SYSRET, an ELF64 loader, and a
  Linux x86_64-numbered syscall ABI with negative-`errno` returns. PID 1 `init`
  runs self-tests then a **login loop** (`/bin/login` authenticates against
  `/etc/passwd`+`/etc/shadow` and drops privileges before exec'ing the user
  shell). Ships an interactive **shell** (`/bin/sh`, with pipelines `|`,
  redirection `>`/`>>`/`<`, `;` sequencing, background `&`, and job control)
  plus coreutils-style `/bin` programs, networking demos, and a framebuffer
  desktop compositor (`gui`), backed by a minimal in-tree **libc**
- **Filesystems**: VFS layer with ramfs, FAT32, and ext4 support
- **Drivers**: ATA/IDE block driver (with >4 GB DMA bounce buffers), PCI bus scan,
  PS/2 keyboard and **interrupt-driven COM1 serial input**, both routed through
  the I/O APIC into a shared TTY ring buffer

## Project Structure

```
project-lariat/
├── boot/
│   ├── boot.asm          # 512-byte boot sector: disk load, e820, paging, long mode entry
│   └── ap_trampoline.asm # 16-bit→long-mode trampoline for application processors
├── cpu/
│   ├── gdt.c / gdt_asm.asm      # GDT + per-CPU TSS descriptors
│   ├── idt.c / idt_asm.asm      # IDT, ISR/IRQ stubs, PIC- vs APIC-mode EOI
│   ├── pic.c                    # 8259 PIC remapping / full disable
│   ├── timer.c                  # Tick counter + LAPIC-driven scheduler tick
│   ├── syscall.c / syscall_asm.asm  # SYSCALL/SYSRET setup and handlers
│   └── context.asm              # switch_thread() assembly context switch
├── drivers/                     # Drivers grouped by class (see docs/LAYOUT.md)
│   ├── block/ata.c              # ATA/IDE block device driver
│   ├── char/serial.c            # COM1 serial output + RX interrupt enable
│   ├── input/keyboard.c         # PS/2 keyboard + COM1 RX IRQ → shared TTY ring buffer
│   ├── input/mouse.c            # PS/2 mouse → /dev/input
│   ├── net/rtl8139.c            # RTL8139 NIC driver
│   └── video/bochs_vbe.c        # Bochs/QEMU VBE framebuffer → /dev/fb0
├── kernel/
│   ├── entry.asm                # 64-bit kernel entry: BSS zeroing, stack setup
│   ├── kernel.c                 # Kernel main (kmain)
│   ├── acpi.c                   # ACPI RSDP/RSDT/XSDT/MADT parsing
│   ├── lapic.c                  # Local APIC driver (timer, IPIs)
│   ├── ioapic.c                 # I/O APIC driver (interrupt routing)
│   ├── smp.c                    # AP bring-up, per-CPU data, cross-core work queue, TLB shootdown
│   ├── vga.c                    # VGA text mode driver
│   ├── device.c / driver.c      # Device + driver frameworks
│   ├── pci.c                    # PCI bus enumeration
│   ├── module.c                 # Kernel module loader (stubs)
│   ├── kapi.c                   # Kernel API: kmalloc/kfree, ioremap, spinlocks, IRQ API
│   ├── pmm.c                    # Physical memory manager (dynamic e820 bitmap, SMP-locked)
│   ├── vmm.c                    # Virtual memory manager + direct map + fork clone/teardown
│   ├── vfs.c                    # Virtual filesystem layer
│   ├── block.c                  # Block device abstraction
│   ├── sched.c                  # Preemptive thread scheduler (sched_lock protected)
│   ├── process.c                # User process creation and fork()
│   ├── elf.c                    # ELF64 loader (execve) + embedded /bin program registry
│   ├── console.c                # /dev/console character device (stdin/stdout/stderr backend)
│   ├── fd.c                     # File descriptor table + ref-counting
│   ├── fork_return.asm          # fork child first-entry trampoline (iretq to ring 3)
│   ├── enter_userspace.asm      # iretq trampoline into ring 3
│   └── fs/
│       ├── ramfs.c              # RAM-based filesystem
│       ├── fat32.c              # FAT32 filesystem driver
│       └── ext4.c               # ext4 filesystem driver (read-only, basic)
├── include/
│   ├── *.h                      # Kernel-internal headers
│   └── uapi/                    # Public ABI headers (syscall.h, uapi.h) shared with userspace
├── docs/                        # ARCHITECTURE.md, LAYOUT.md, adr/ decision records
├── userspace/
│   ├── init.c / init.S          # PID 1: runs POSIX self-tests, then execs /bin/sh
│   ├── sh.c                     # Interactive shell (built-ins + fork/execve of /bin)
│   ├── ls.c / cat.c / echo.c / hello.c  # /bin programs (embedded in the kernel image)
│   ├── libc/                    # Minimal libc: crt0.S, string, stdlib (malloc),
│   │                            #   stdio (printf/read_line), unistd, fcntl, errno
│   ├── Makefile
│   └── linker.ld
├── linker.ld             # x86_64 linker script (kernel at 0x100000)
└── Makefile              # Build system
```

## Boot Process

1. **BIOS** loads the boot sector to `0x7C00`
2. **Bootloader** queries the e820 memory map and loads the kernel to `0x7E00`
   using **LBA extended reads** (INT 13h AH=42h) in 16 KB chunks — a 512 KB load
   window so the (now multi-program) kernel image is no longer capped at the old
   127-sector CHS limit
3. Switches to **32-bit protected mode**
4. Copies the kernel from `0x7E00` to `0x100000` (1 MB) using `rep movsd`
5. Sets up **identity paging** with 2 MB huge pages
6. Enables **PAE**, **long mode** (EFER.LME), and **paging**
7. Far jump to **64-bit long mode**, calls `kmain()` at `0x100000`
8. Early in `kmain`, the VMM builds the **higher-half direct map** of physical RAM
   at `0xFFFF800000000000` and switches `phys_to_virt()` over to it

## Interrupts & SMP Bring-up

1. `acpi_init()` parses the RSDP → RSDT/XSDT → MADT to enumerate CPUs/LAPIC IDs
   and locate the LAPIC and I/O APIC MMIO bases
2. `lapic_init()` maps and enables the Local APIC (via the direct map)
3. `ioapic_init()` maps the I/O APIC and masks all redirection entries
4. `smp_init()` installs the AP trampoline at `0x8000` and brings each
   application processor up with an INIT–SIPI–SIPI sequence; each AP loads the
   shared GDT/IDT, its own TSS, programs its SYSCALL/SYSRET MSRs, claims its
   idle thread, and arms its LAPIC timer
5. The system switches off the 8259 PIC entirely: the keyboard (ISA IRQ 1) is
   routed through the I/O APIC to the BSP, and the preemptive scheduler is driven
   by each CPU's **LAPIC timer** instead of the PIT

Every core (BSP + APs) runs the same preemptive scheduler against a shared,
`sched_lock`-protected ready queue, so application processors pick up and run
ring-3 threads and the load is naturally balanced. `sched_lock` is held across
the context switch (the switched-in thread releases it), which makes thread
migration between cores race-free without needing `swapgs`. The lock-protected
cross-core work queue additionally executes kernel jobs in parallel.

## Build Requirements

- `nasm` (Netwide Assembler)
- `gcc` with x86_64 support
- `ld` (GNU linker)
- `objcopy`
- `qemu-system-x86_64` (for testing)

## Build

```bash
make all
```

This produces `build/lariat.bin` — a raw bootable disk image containing the boot
sector and kernel.

## Run

```bash
# With display (VNC/SDL window)
make run

# Headless (serial output in terminal)
make run-headless

# With GDB debug server (port 1234)
make debug
```

To exercise SMP, pass multiple CPUs to QEMU (e.g. `-smp 4`). The kernel detects
the topology via ACPI and brings every core online.

## Current Features

- [x] Custom bootloader (no GRUB/multiboot dependency)
- [x] 16-bit → 32-bit → 64-bit mode transition
- [x] PAE paging with 2 MB huge pages + 4 KB page-table management
- [x] Physical memory manager with a dynamic e820 bitmap (no RAM ceiling)
- [x] Higher-half direct map of physical RAM (`phys_to_virt`/`virt_to_phys`), >4 GB capable
- [x] Real `ioremap`, SMP-safe PMM, multi-page `kfree`
- [x] VGA text mode driver + serial logging
- [x] ACPI (RSDP/XSDT/MADT) topology discovery
- [x] Local APIC + I/O APIC drivers; 8259 PIC disabled
- [x] **Preemptive, multi-core** scheduler: every CPU's LAPIC timer schedules
      ring-3 threads off a shared ready queue (threads migrate across cores)
- [x] SMP: AP trampoline + INIT/SIPI bring-up, per-CPU TSS, per-CPU SYSCALL MSRs,
      APs scheduling ring-3 threads, cross-core work queue
- [x] TLB shootdown IPIs for shared kernel-mapping changes
- [x] PS/2 keyboard driver (lock-protected, I/O APIC routed)
- [x] PCI bus enumeration, ATA/IDE block driver (>4 GB DMA bounce buffers)
- [x] VFS layer with ramfs, FAT32, and ext4 (basic) support
- [x] GDT with ring-0/ring-3 segments + per-CPU TSS
- [x] SYSCALL/SYSRET system calls, Linux x86_64 numbering, negative-`errno` returns
- [x] ELF64 loader; `execve` (from embedded `/bin` images or disk, `.bss` mapped),
      `fork()`, `waitpid()` with resource cleanup + orphan reparenting
- [x] `/dev/console`; fds 0/1/2 in the fd table with ref-counting on `fork`/`dup`;
      **interrupt-driven serial input** so a piped/serial console is reliable
- [x] Syscalls: `dup`/`dup2`, `stat`/`fstat`, `getdents64`, `mkdir`/`rmdir`/`unlink`,
      `pipe`, `nanosleep`, anonymous `mmap`/`brk`, `getpid`/`getppid`,
      `getcwd`/`chdir`, `fcntl` (`F_DUPFD`/`F_GETFL`/`F_SETFL`), `uname`
- [x] Signals: `kill`/`sigaction`/`sigreturn` with delivery on syscall return
- [x] **Userland**: PID 1 `init` runs POSIX self-tests then execs an interactive
      **shell** (`/bin/sh`, with `cd`/`pwd`/`help`/`exit` built-ins and per-process
      cwd) which forks+execs `/bin` programs (`ls`, `cat`, `echo`, `hello`)
- [x] **Minimal libc**: crt0 (argc/argv/envp), string, `malloc`/`free` (brk),
      `printf`/`snprintf`/`read_line`

## Known Limitations / Open Work

- **Single shared run queue.** All cores pull from one `sched_lock`-protected
  ready queue. This load-balances naturally but serializes scheduling decisions;
  per-CPU run queues with work-stealing would scale better under heavy load.
- **`current_thread()` reads the LAPIC ID** on every call (including the syscall
  fast path) instead of using a `swapgs` per-CPU base. Correct and GS-state
  independent, but an `swapgs`/`KERNEL_GS_BASE` scheme would be cheaper.
- **TLB shootdown is coarse** (full CR3 reload on the target CPU) rather than
  per-page `invlpg`.
- **ext4 is read-only**; FAT32/ramfs cleanup paths are incomplete.
- **`execve` leaks the previous image's frames.** Re-exec overwrites PTEs without
  freeing the old user pages (they are reclaimed only at process exit). Bounded
  and harmless for the small programs here, but a proper per-exec address-space
  teardown is still wanted.
- **Console input dropped if flooded during boot.** Bytes piped to the serial
  console *before* the shell is ready can overrun the UART (the kernel isn't
  servicing IRQ4 yet). Interactive typing and post-boot input are reliable;
  scripted tests should send input after the prompt appears.
- The userspace `malloc`/`free` is a bump allocator (a real free-list/`mmap`
  allocator is planned in the evolution roadmap).
- Stubs remain in `module.c` (symbol tables) and the PCI `request_irq` path is
  unused (the generic IRQ API still routes through the now-disabled PIC).

## Next Steps

- **Scale out SMP scheduling**: per-CPU run queues with work-stealing and a
  `swapgs`-based per-CPU `current` to replace the shared queue + LAPIC-id lookup.
- **Grow the userland further**: shell pipelines/redirection and job control,
  more programs, `poll`/`select`, environment variables, and a fuller libc.
- **Tighten `execve`**: free the old address space on re-exec (no per-exec leak).
- **ext4 write support** and filesystem resource cleanup.
- Re-enable build warnings as errors (`-Werror`) once the tree is clean.

## Development History

Earlier sessions resolved several foundational bugs; the full roadmap (higher-half
direct map, preemption, POSIX expansion, and SMP) is now implemented:

- **`fork()` child never executed** — `scheduler_tick()` re-enqueued the running
  thread and orphaned the rest of the ready queue, and `thread_yield()` re-queued
  the idle thread. Fixed by leaving the running thread untouched on tick and
  excluding the idle thread from re-enqueue.
- **`read()` returned garbage in userspace** — the SYSCALL return path used the
  callee-saved `r12` as scratch and never restored the user's `r15`. Fixed by
  saving `rax` on the kernel stack and restoring the real `r15` before `sysret`.
- **Process cleanup, preemption, POSIX, and SMP** were subsequently added per the
  project roadmap (see commit history).
