#include "vga.h"
#include "serial.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "ports.h"
#include "device.h"
#include "driver.h"
#include "pci.h"
#include "module.h"
#include "kapi.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "vfs.h"
#include "block.h"
#include "ata.h"
#include "fat32.h"
#include "ext4.h"
#include "procfs.h"
#include "sched.h"
#include "gdt.h"
#include "syscall.h"
#include "process.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "smp.h"
#include "net.h"
#include "gfx.h"
#include "ipc.h"
#include "world.h"
#include "smptest.h"
#include "kshell.h"

static void print(const char *str) {
    vga_print(str);
    serial_print(SERIAL_COM1, str);
}

void kmain(void) {
    vga_init();
    serial_init(SERIAL_COM1);

    print("\n================================\n");
    print("  Project Lariat 64-bit Kernel  \n");
    print("================================\n\n");

    /* Memory management must come first: build the direct map (physmap) so the
     * heap and everything downstream can use phys_to_virt().
     *
     * We deliberately do NOT identity-map all of RAM: the boot loader's low
     * 4MB identity map covers the kernel image, its stack and the PMM bitmap,
     * and everything else is reached through the physmap.  Identity-mapping all
     * RAM would collide with the user address space (e.g. USER_CODE_START at
     * 1GB) once the machine has >= 1GB of RAM. */
    vmm_init();
    vmm_build_physmap();

    /* Initialize kernel subsystems */
    kapi_heap_init((void *)0x200000, 0x100000);
    module_init();
    device_init();
    driver_init();

    block_init();
    ata_init();

    vfs_init();
    ipc_init();
    console_init();
    ramfs_init();
    fat32_init();
    ext4_init();
    procfs_init();

    /* Bring up the filesystem world: mount the core namespace (ramfs /, ext4
     * /var, procfs /proc), lay down the multi-user world (/bin, /etc + /home
     * firmlinks, accounts, config), then bring up the remaining fstab volumes
     * once the /etc firmlink is readable.  See kernel/core/world.c. */
    world_mount_core();
    world_setup();
    if (!world_is_rescue()) world_mount_fstab();

    gdt_init();
    tss_init();

    pic_init();
    idt_init();
    syscall_init();
    timer_init(100);
    clock_init();
    keyboard_init();

    /* Discover CPUs / APIC topology and enable the local APIC + IO-APIC. */
    acpi_init();
    lapic_init();
    ioapic_init();

    pci_init();
    driver_probe_all();

    /* Bring up application processors (no-op on a single-CPU machine).  This
     * also registers the LAPIC timer/IPI handlers on the shared IDT. */
    smp_init();

    /* Switch the whole system from the legacy 8259 PIC/PIT to the APIC:
     *   - route the keyboard (ISA IRQ1 -> GSI1) through the IO-APIC to the BSP,
     *   - mask both PICs entirely,
     *   - acknowledge all interrupts at the local APIC (idt apic mode),
     *   - drive the BSP's preemptive scheduler from its local APIC timer
     *     instead of the PIT. */
    uint8_t bsp_id = (uint8_t)lapic_id();
    ioapic_route(1, 33, bsp_id);
    /* Route COM1 (IRQ4 -> GSI4) to vector 36 so serial input is interrupt
     * driven; this keeps a piped/serial console from dropping bytes. */
    ioapic_route(4, 36, bsp_id);
    /* Route the PS/2 mouse (IRQ12 -> GSI12) to vector 44 for the GUI. */
    ioapic_route(12, 44, bsp_id);
    keyboard_register_serial();
    serial_enable_rx_interrupt(SERIAL_COM1);
    pic_disable();
    idt_set_apic_mode(1);
    lapic_timer_init(VEC_LAPIC_TIMER, 10000000);

    __asm__ __volatile__("sti");

    /* M11 GUI bring-up: unified input queue, framebuffer, PS/2 mouse. */
    input_init();
    bochs_vbe_init();
    mouse_init();

    /* SMP bring-up self-tests (see kernel/core/smptest.c). */
    smp_demo();
    smp_stress();

    /* Start scheduler */
    scheduler_init();

    /* Bring up networking: core (loopback + RX dispatch thread) then the NIC. */
    net_init();
    rtl8139_init();

    /* Create the init process.  It loads through the ELF path inside its own
     * address space (see thread_trampoline), so .bss is mapped and a proper
     * argv/envp stack is built. */
    serial_print(SERIAL_COM1, "[KERNEL] Loading userspace init (/init)...\n");
    struct thread *init_proc = process_create_user("/init");
    if (!init_proc) {
        serial_print(SERIAL_COM1, "[KERNEL] Failed to create user process, falling back to kernel shell\n");
        thread_create((void (*)(void *))kshell_thread, NULL);
    } else {
        /* init adopts orphaned processes (PID 1 semantics). */
        sched_set_reaper(init_proc);
    }

    /* Yield to the init process (cooperative scheduler) */
    thread_yield();

    /* Idle loop: also drain any SMP work so the BSP participates. */
    while (1) {
        smp_run_pending_work();
        __asm__ __volatile__("sti; hlt");
    }
}
