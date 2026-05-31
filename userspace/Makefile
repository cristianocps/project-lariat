CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy

CFLAGS = -m64 -ffreestanding -O2 -Wall -Wextra -nostdlib -nostartfiles \
         -fno-builtin -fno-exceptions -fno-stack-protector -nodefaultlibs \
         -fno-pie -fno-pic -mno-sse -mno-sse2 -mno-red-zone \
         -I. -I../include -I../include/uapi \
         -ffunction-sections -fdata-sections

LDFLAGS = -m elf_x86_64 -T linker.ld -no-pie --gc-sections

# Programs that link against crt0 + libc.
PROGS = sh ls cat echo hello httpget echosrv echocli \
        true false clear sleep mkdir rmdir rm cp mv wc grep head tail ps kill \
        id whoami login su passwd useradd userdel gui lpkg calc edit term settings lcc
PROG_ELVES = $(addsuffix .elf,$(PROGS))

# libc objects shared by all crt0-based programs.
LIBC_OBJS = libc/crt0.o libc/string.o libc/stdlib.o libc/stdio.o libc/errno.o \
            libc/env.o libc/pthread.o libc/clone.o libc/pwd.o libc/gfx.o \
            libc/ipc.o libc/ws.o libc/wtk.o

all: init.elf $(PROG_ELVES)

# --- libc ---
libc/crt0.o: libc/crt0.S
	$(AS) -f elf64 $< -o $@

libc/clone.o: libc/clone.S
	$(AS) -f elf64 $< -o $@

libc/%.o: libc/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- crt0-based programs ($prog.c + libc) ---
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.elf: %.o $(LIBC_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(LIBC_OBJS) $<

# --- init: keeps its own minimal _start (init.S) ---
init_asm.o: init.S
	$(AS) -f elf64 $< -o $@

init_c.o: init.c
	$(CC) $(CFLAGS) -c $< -o $@

init.elf: init_asm.o init_c.o libc/errno.o linker.ld
	$(LD) $(LDFLAGS) -o $@ init_asm.o init_c.o libc/errno.o

clean:
	rm -f *.o libc/*.o *.elf *.bin
