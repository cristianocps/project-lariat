# Project Lariat - 64-bit Build System
NASM    = nasm
CC      = gcc
LD      = ld
OBJCOPY = objcopy
QEMU    = LD_LIBRARY_PATH=/tmp/qemu-extract/usr/lib/x86_64-linux-gnu QEMU_MODULE_DIR=/tmp/qemu-extract/usr/lib/x86_64-linux-gnu/qemu $(HOME)/.local/bin/qemu-system-x86_64

CFLAGS  = -m64 -ffreestanding -O2 -Wall -Wextra -nostdlib -nostartfiles \
          -fno-builtin -fno-exceptions -fno-stack-protector -nodefaultlibs \
          -mcmodel=large -fno-pie -fno-pic -mno-sse -mno-sse2 -mno-red-zone -fcf-protection=none -Iinclude

QEMU_FLAGS = -L /tmp/qemu-extract/usr/share/qemu -L /tmp/qemu-extract/usr/share/seabios

BOOT_SRC   = boot/boot.asm
LINKER     = linker.ld

BUILD_DIR  = build
BOOT_BIN   = $(BUILD_DIR)/boot.bin
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
OS_BIN     = $(BUILD_DIR)/lariat.bin
OS_ISO     = $(BUILD_DIR)/lariat.iso

# Kernel assembly sources
KERNEL_ASM = kernel/entry.asm cpu/idt_asm.asm kernel/context.asm

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
             kernel/fs/ramfs.c \
             kernel/fs/fat32.c \
             kernel/fs/ext4.c \
             drivers/serial.c \
             drivers/keyboard.c \
             drivers/ata.c \
             cpu/pic.c \
             cpu/timer.c \
             cpu/idt.c

KERNEL_OBJS = $(patsubst %.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASM)) \
              $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_C))

.PHONY: all clean run run-headless debug iso dirs

all: dirs $(OS_BIN)

dirs:
	@mkdir -p $(BUILD_DIR)/kernel $(BUILD_DIR)/kernel/fs $(BUILD_DIR)/drivers $(BUILD_DIR)/cpu

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

# C objects
$(BUILD_DIR)/kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/cpu/%.o: cpu/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/fs/%.o: kernel/fs/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Kernel (flat binary)
$(KERNEL_BIN): $(KERNEL_OBJS) $(LINKER)
	$(LD) -m elf_x86_64 -T $(LINKER) -no-pie -o $(BUILD_DIR)/kernel.elf $(KERNEL_OBJS)
	$(OBJCOPY) -O binary $(BUILD_DIR)/kernel.elf $(KERNEL_BIN)

# Combine boot sector + kernel into OS image
$(OS_BIN): $(BOOT_BIN) $(KERNEL_BIN)
	cat $(BOOT_BIN) $(KERNEL_BIN) > $(OS_BIN)
	# Pad to at least 64KB for floppy/ISO compatibility
	dd if=/dev/zero bs=1 count=1 seek=65535 of=$(OS_BIN) status=none

run: $(OS_BIN)
	$(QEMU) $(QEMU_FLAGS) \
		-drive format=raw,file=$(OS_BIN),if=ide,media=disk,bus=0,unit=0 \
		-drive format=raw,file=disk.img,if=ide,media=disk,bus=0,unit=1 \
		-drive format=raw,file=ext4.img,if=ide,media=disk,bus=1,unit=0 \
		-m 32 -net none

run-headless: $(OS_BIN)
	$(QEMU) $(QEMU_FLAGS) \
		-drive format=raw,file=$(OS_BIN),if=ide,media=disk,bus=0,unit=0 \
		-drive format=raw,file=disk.img,if=ide,media=disk,bus=0,unit=1 \
		-drive format=raw,file=ext4.img,if=ide,media=disk,bus=1,unit=0 \
		-m 32 -net none -nographic -monitor none

debug: $(OS_BIN)
	$(QEMU) $(QEMU_FLAGS) -drive format=raw,file=$(OS_BIN) -m 32 -net none -s -S

iso: $(OS_BIN)
	mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(OS_BIN) $(BUILD_DIR)/iso/boot/lariat.bin
	printf 'set timeout=0\nset default=0\n\nmenuentry "Project Lariat" {\n    multiboot /boot/lariat.bin\n    boot\n}\n' > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(OS_ISO) $(BUILD_DIR)/iso 2>/dev/null || \
		echo "grub-mkrescue failed (may need xorriso)"

clean:
	rm -rf $(BUILD_DIR)
