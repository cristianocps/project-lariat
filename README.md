# Project Lariat

A 64-bit operating system built from scratch for the x86_64 architecture.

## Architecture

- **Bootloader**: Custom 16-bit real-mode boot sector written in NASM assembly
- **Transition**: 16-bit real mode → 32-bit protected mode → 64-bit long mode
- **Kernel**: 64-bit C kernel with assembly entry point
- **Memory**: Identity-mapped first 4MB using 2MB huge pages
- **Output**: VGA text mode driver (80×25, 16 colors)

## Project Structure

```
project-lariat/
├── boot/
│   └── boot.asm          # 512-byte boot sector: disk load, paging, long mode entry
├── kernel/
│   ├── entry.asm         # 64-bit kernel entry: BSS zeroing, stack setup
│   ├── kernel.c          # Kernel main
│   └── vga.c             # VGA text mode driver
├── include/
│   └── vga.h             # VGA driver header
├── linker.ld             # x86_64 linker script (kernel at 0x100000)
└── Makefile              # Build system
```

## Boot Process

1. **BIOS** loads the boot sector to `0x7C00`
2. **Bootloader** loads the kernel to `0x7E00` using BIOS int 0x13
3. Switches to **32-bit protected mode**
4. Copies kernel from `0x7E00` to `0x100000` (1MB)
5. Sets up **identity paging** with 2MB huge pages (first 4MB)
6. Enables **PAE**, **long mode** (EFER.LME), and **paging**
7. Far jump to **64-bit long mode**
8. Calls `kmain()` at `0x100000`

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

This produces `build/lariat.bin` — a raw bootable disk image.

## Run

```bash
# With display (VNC/SDL window)
make run

# Headless (serial + VGA in terminal)
make run-headless

# With GDB debug server (port 1234)
make debug
```

## Current Features

- [x] Custom bootloader (no GRUB/multiboot dependency)
- [x] 16-bit → 32-bit → 64-bit mode transition
- [x] PAE paging with 2MB huge pages
- [x] 64-bit long mode execution
- [x] VGA text mode driver
- [x] BSS section zeroing at startup
- [x] Interrupt-safe halt loop

## Next Steps

- Interrupt Descriptor Table (IDT) and interrupt handling
- PIC/APIC timer
- Keyboard input driver
- Physical memory manager
- Virtual memory allocator
- Process/task scheduler
- Disk I/O and filesystem
- System calls API
- User-mode programs and shell
