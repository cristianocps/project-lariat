# Project Lariat - 64-bit Build System
NASM    = nasm
CC      = gcc
LD      = ld
OBJCOPY = objcopy
# Portable QEMU runtime lives in-repo (persists across reboots, unlike /tmp).
# Override QEMU_RT to point elsewhere if needed.
QEMU_LOCAL := $(HOME)/.local/bin/qemu-system-x86_64
QEMU_RT    := $(CURDIR)/.local_libs/extract

ifneq ($(wildcard $(QEMU_LOCAL)),)
    # Portable runtime present: use bundled libs and BIOS.
    QEMU       := LD_LIBRARY_PATH=$(QEMU_RT)/usr/lib/x86_64-linux-gnu QEMU_MODULE_DIR=$(QEMU_RT)/usr/lib/x86_64-linux-gnu/qemu $(QEMU_LOCAL)
    QEMU_FLAGS := -L $(QEMU_RT)/usr/share/qemu -L $(QEMU_RT)/usr/share/seabios
else
    # Fall back to the system QEMU and its default firmware search paths.
    QEMU       := qemu-system-x86_64
    QEMU_FLAGS :=
endif

CFLAGS  = -m64 -ffreestanding -Os -Wall -Wextra -nostdlib -nostartfiles \
          -fno-builtin -fno-exceptions -fno-stack-protector -nodefaultlibs \
          -mcmodel=large -fno-pie -fno-pic -mno-sse -mno-sse2 -mno-red-zone \
          -fcf-protection=none -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -Iinclude -Iinclude/uapi \
          -MMD -MP


BOOT_SRC   = boot/boot.asm
LINKER     = linker.ld

BUILD_DIR  = build
BOOT_BIN   = $(BUILD_DIR)/boot.bin
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
OS_BIN     = $(BUILD_DIR)/lariat.bin
OS_ISO     = $(BUILD_DIR)/lariat.iso

# Kernel assembly sources
KERNEL_ASM = kernel/entry.asm cpu/idt_asm.asm kernel/context.asm cpu/gdt_asm.asm cpu/syscall_asm.asm kernel/enter_userspace.asm kernel/fork_return.asm

# Kernel C sources
KERNEL_C   = kernel/kernel.c \
             kernel/vga.c \
             kernel/device.c \
             kernel/driver.c \
             kernel/pci.c \
             kernel/module.c \
             kernel/kapi.c \
             kernel/pmm.c \
             kernel/vmm.c \
             kernel/vfs.c \
             kernel/block.c \
             kernel/sched.c \
             kernel/process.c \
             kernel/fd.c \
             kernel/console.c \
             kernel/pipe.c \
             kernel/elf.c \
             kernel/acpi.c \
             kernel/lapic.c \
             kernel/ioapic.c \
             kernel/smp.c \
             kernel/fs/ramfs.c \
             kernel/fs/fat32.c \
             kernel/fs/ext4.c \
             kernel/fs/procfs.c \
             kernel/net/pbuf.c \
             kernel/net/net.c \
             kernel/net/eth.c \
             kernel/net/arp.c \
             kernel/net/ipv4.c \
             kernel/net/icmp.c \
             kernel/net/udp.c \
             kernel/net/tcp.c \
             kernel/net/socket.c \
             kernel/ipc/port.c \
             kernel/fork_return_log.c \
             kernel/input.c \
             kernel/core/world.c \
             kernel/core/smptest.c \
             kernel/core/kshell.c \
             drivers/char/serial.c \
             drivers/input/keyboard.c \
             drivers/input/mouse.c \
             drivers/block/ata.c \
             drivers/video/bochs_vbe.c \
             drivers/net/rtl8139.c \
             cpu/pic.c \
             cpu/timer.c \
             cpu/idt.c \
             cpu/gdt.c \
             cpu/syscall.c

KERNEL_OBJS = $(patsubst %.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASM)) \
              $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_C))

.PHONY: all clean run run-headless run-gui run-desktop debug iso dirs disks sysroot
.DEFAULT_GOAL := all

all: dirs $(OS_BIN)

dirs:
	@mkdir -p $(BUILD_DIR)/kernel $(BUILD_DIR)/kernel/core $(BUILD_DIR)/kernel/fs $(BUILD_DIR)/kernel/net $(BUILD_DIR)/kernel/ipc $(BUILD_DIR)/drivers $(BUILD_DIR)/drivers/net $(BUILD_DIR)/drivers/video $(BUILD_DIR)/drivers/block $(BUILD_DIR)/drivers/input $(BUILD_DIR)/drivers/char $(BUILD_DIR)/cpu

# Boot sector (flat binary, 512 bytes)
$(BOOT_BIN): $(BOOT_SRC)
	$(NASM) -f bin $(BOOT_SRC) -o $(BOOT_BIN)

# Assembly objects
$(BUILD_DIR)/kernel/entry.o: kernel/entry.asm
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/cpu/idt_asm.o: cpu/idt_asm.asm
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/context.o: kernel/context.asm
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/cpu/gdt_asm.o: cpu/gdt_asm.asm
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/cpu/syscall_asm.o: cpu/syscall_asm.asm
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/enter_userspace.o: kernel/enter_userspace.asm
	$(NASM) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/fork_return.o: kernel/fork_return.asm
	$(NASM) -f elf64 $< -o $@

# C objects
$(BUILD_DIR)/kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/net/%.o: drivers/net/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/video/%.o: drivers/video/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/block/%.o: drivers/block/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/input/%.o: drivers/input/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/char/%.o: drivers/char/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/cpu/%.o: cpu/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/core/%.o: kernel/core/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/fs/%.o: kernel/fs/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/net/%.o: kernel/net/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/ipc/%.o: kernel/ipc/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build all userspace programs (init + /bin programs) as ELF images.
.PHONY: userspace-build
userspace-build:
	$(MAKE) -C userspace

userspace/init.elf userspace/sh.elf userspace/ls.elf userspace/cat.elf \
userspace/echo.elf userspace/hello.elf userspace/httpget.elf \
userspace/echosrv.elf userspace/echocli.elf \
userspace/true.elf userspace/false.elf userspace/clear.elf userspace/sleep.elf \
userspace/mkdir.elf userspace/rmdir.elf userspace/rm.elf userspace/cp.elf \
userspace/mv.elf userspace/wc.elf userspace/grep.elf userspace/head.elf \
userspace/tail.elf userspace/ps.elf userspace/kill.elf \
userspace/id.elf userspace/whoami.elf userspace/login.elf userspace/su.elf \
userspace/passwd.elf userspace/useradd.elf userspace/userdel.elf \
userspace/settings.elf userspace/lcc.elf userspace/gui.elf userspace/lpkg.elf: userspace-build

# Embedded /bin programs, each turned into a kernel object with its own symbols.
PROG_OBJS = $(BUILD_DIR)/prog_sh.o $(BUILD_DIR)/prog_ls.o $(BUILD_DIR)/prog_cat.o \
            $(BUILD_DIR)/prog_echo.o $(BUILD_DIR)/prog_hello.o \
            $(BUILD_DIR)/prog_httpget.o $(BUILD_DIR)/prog_echosrv.o \
            $(BUILD_DIR)/prog_echocli.o \
            $(BUILD_DIR)/prog_true.o $(BUILD_DIR)/prog_false.o \
            $(BUILD_DIR)/prog_clear.o $(BUILD_DIR)/prog_sleep.o \
            $(BUILD_DIR)/prog_mkdir.o $(BUILD_DIR)/prog_rmdir.o \
            $(BUILD_DIR)/prog_rm.o $(BUILD_DIR)/prog_cp.o \
            $(BUILD_DIR)/prog_mv.o $(BUILD_DIR)/prog_wc.o \
            $(BUILD_DIR)/prog_grep.o $(BUILD_DIR)/prog_head.o \
            $(BUILD_DIR)/prog_tail.o $(BUILD_DIR)/prog_ps.o \
            $(BUILD_DIR)/prog_kill.o $(BUILD_DIR)/prog_id.o \
            $(BUILD_DIR)/prog_whoami.o $(BUILD_DIR)/prog_login.o \
            $(BUILD_DIR)/prog_su.o $(BUILD_DIR)/prog_passwd.o \
            $(BUILD_DIR)/prog_useradd.o $(BUILD_DIR)/prog_userdel.o \
            $(BUILD_DIR)/prog_settings.o $(BUILD_DIR)/prog_lcc.o \
            $(BUILD_DIR)/prog_gui.o $(BUILD_DIR)/prog_lpkg.o

# Embed the init ELF (loaded through the ELF loader so .bss is mapped).
$(BUILD_DIR)/userspace_init.o: userspace/init.elf
	@mkdir -p $(BUILD_DIR)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 $< $@ \
		--redefine-sym _binary_userspace_init_elf_start=_userspace_init_start \
		--redefine-sym _binary_userspace_init_elf_end=_userspace_init_end \
		--redefine-sym _binary_userspace_init_elf_size=_userspace_init_size

# Embed each /bin program ELF (symbols _prog_<name>_start/_end).
$(BUILD_DIR)/prog_%.o: userspace/%.elf
	@mkdir -p $(BUILD_DIR)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 $< $@ \
		--redefine-sym _binary_userspace_$*_elf_start=_prog_$*_start \
		--redefine-sym _binary_userspace_$*_elf_end=_prog_$*_end \
		--redefine-sym _binary_userspace_$*_elf_size=_prog_$*_size

# AP trampoline (flat binary, embedded into the kernel)
$(BUILD_DIR)/ap_trampoline.bin: boot/ap_trampoline.asm
	@mkdir -p $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

$(BUILD_DIR)/ap_trampoline.o: $(BUILD_DIR)/ap_trampoline.bin
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 $< $@ \
		--redefine-sym _binary_build_ap_trampoline_bin_start=_ap_trampoline_start \
		--redefine-sym _binary_build_ap_trampoline_bin_end=_ap_trampoline_end \
		--redefine-sym _binary_build_ap_trampoline_bin_size=_ap_trampoline_size

# Kernel (flat binary)
$(KERNEL_BIN): $(KERNEL_OBJS) $(BUILD_DIR)/userspace_init.o $(PROG_OBJS) $(BUILD_DIR)/ap_trampoline.o $(LINKER)
	$(LD) -m elf_x86_64 -T $(LINKER) -no-pie -o $(BUILD_DIR)/kernel.elf $(KERNEL_OBJS) $(BUILD_DIR)/userspace_init.o $(PROG_OBJS) $(BUILD_DIR)/ap_trampoline.o
	$(OBJCOPY) -O binary $(BUILD_DIR)/kernel.elf $(KERNEL_BIN)

# Combine boot sector + kernel into OS image
$(OS_BIN): $(BOOT_BIN) $(KERNEL_BIN)
	cat $(BOOT_BIN) $(KERNEL_BIN) > $(OS_BIN)
	# Pad to 1 MB (2048 sectors) so the bootloader's 1024-sector LBA load window
	# never reads past EOF, even as the kernel image grows.
	dd if=/dev/zero bs=1 count=1 seek=1048575 of=$(OS_BIN) status=none conv=notrunc

# Networking: QEMU user-mode (slirp) gives the guest NAT to the host/internet.
# hostfwd maps host port 5555 -> guest 5555 so host tools can reach guest servers.
NET_FLAGS = -netdev user,id=n0,hostfwd=tcp::5555-:5555 -device rtl8139,netdev=n0,romfile=

run: $(OS_BIN)
	$(QEMU) $(QEMU_FLAGS) \
		-drive format=raw,file=$(OS_BIN),if=ide,media=disk,bus=0,unit=0 \
		-drive format=raw,file=disk.img,if=ide,media=disk,bus=0,unit=1 \
		-drive id=ext4d,format=raw,file=ext4.img,if=none,cache=writethrough \
		-device ide-hd,drive=ext4d,bus=ide.1,unit=0 \
		-m 1024 $(NET_FLAGS)

run-headless: $(OS_BIN)
	$(QEMU) $(QEMU_FLAGS) \
		-drive format=raw,file=$(OS_BIN),if=ide,media=disk,bus=0,unit=0 \
		-drive format=raw,file=disk.img,if=ide,media=disk,bus=0,unit=1 \
		-drive id=ext4d,format=raw,file=ext4.img,if=none,cache=writethrough \
		-device ide-hd,drive=ext4d,bus=ide.1,unit=0 \
		-m 1024 $(NET_FLAGS) -nographic -monitor none

# GUI run: Bochs/std VGA framebuffer with a QMP monitor on a unix socket so the
# desktop can be screendumped headlessly (make run-gui; then screendump via the
# monitor socket at /tmp/lariat-mon.sock).
run-gui: $(OS_BIN)
	$(QEMU) $(QEMU_FLAGS) \
		-drive format=raw,file=$(OS_BIN),if=ide,media=disk,bus=0,unit=0 \
		-drive format=raw,file=disk.img,if=ide,media=disk,bus=0,unit=1 \
		-drive id=ext4d,format=raw,file=ext4.img,if=none,cache=writethrough \
		-device ide-hd,drive=ext4d,bus=ide.1,unit=0 \
		-m 1024 $(NET_FLAGS) -vga std -display none \
		-monitor unix:/tmp/lariat-mon.sock,server,nowait -serial stdio

# Interactive desktop: framebuffer shown over VNC (display :0 -> localhost:5900)
# while the text console (login prompt) is on the serial line in your terminal.
# After boot: log in on the terminal as root/root, type `gui`, then drive the
# desktop with the mouse/keyboard in your VNC viewer.
run-desktop: $(OS_BIN)
	@echo ">>> Connect a VNC viewer to localhost:5900 (display :0)."
	@echo ">>> In THIS terminal: log in as root / root, then type 'gui'."
	$(QEMU) $(QEMU_FLAGS) \
		-drive format=raw,file=$(OS_BIN),if=ide,media=disk,bus=0,unit=0 \
		-drive format=raw,file=disk.img,if=ide,media=disk,bus=0,unit=1 \
		-drive id=ext4d,format=raw,file=ext4.img,if=none,cache=writethrough \
		-device ide-hd,drive=ext4d,bus=ide.1,unit=0 \
		-m 1024 $(NET_FLAGS) -vga std -vnc :0 -serial mon:stdio

debug: $(OS_BIN)
	$(QEMU) $(QEMU_FLAGS) -drive format=raw,file=$(OS_BIN) -m 32 -net none -s -S

iso: $(OS_BIN)
	mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(OS_BIN) $(BUILD_DIR)/iso/boot/lariat.bin
	printf 'set timeout=0\nset default=0\n\nmenuentry "Project Lariat" {\n    multiboot /boot/lariat.bin\n    boot\n}\n' > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(OS_ISO) $(BUILD_DIR)/iso 2>/dev/null || \
		echo "grub-mkrescue failed (may need xorriso)"

# Create the FAT32 (/disk) and ext4 (/ext4) data images if missing.
# Disk images are build artifacts (gitignored); see scripts/mkdisk.sh.
disks:
	./scripts/mkdisk.sh

# Seed the x86_64-lariat cross-toolchain sysroot from the public ABI headers.
# Full toolchain build is driven by toolchain/*.sh (see toolchain/README.md).
sysroot:
	./toolchain/make-sysroot.sh

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C userspace clean

# Auto-generated header dependencies (-MMD), so a header change rebuilds its
# dependents (prevents stale objects with mismatched struct layouts).  Placed
# last so it never overrides the default goal (all).
-include $(KERNEL_OBJS:.o=.d)
