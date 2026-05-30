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
#include "sched.h"
#include "gdt.h"
#include "syscall.h"
#include "process.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "smp.h"
#include "net.h"
#include "crypt_lite.h"
#include "gfx.h"

/* Embedded userspace init binary */
extern uint8_t _userspace_init_start[];
extern uint8_t _userspace_init_end[];

/* setuid-root helpers written into the ramfs at boot (see m10_setup). */
extern uint8_t _prog_su_start[];
extern uint8_t _prog_su_end[];
extern uint8_t _prog_passwd_start[];
extern uint8_t _prog_passwd_end[];

#define PROMPT "lariat> "

static void shell_thread(void *arg);
void smp_demo(void);
void smp_stress(void);

/* --------------------------------------------------------------------------
 * M10: lay down the multi-user world at boot.
 *
 * Creates /etc/passwd + /etc/shadow (with crypt-lite hashes for the default
 * accounts) and installs setuid-root /bin/su and /bin/passwd into the ramfs so
 * the set-user-ID-on-exec path has real, owned, mode-bearing inodes to act on.
 * -------------------------------------------------------------------------- */
static void m10_str_append(char *dst, size_t cap, size_t *off, const char *s) {
    while (*s && *off + 1 < cap) dst[(*off)++] = *s++;
    dst[*off] = '\0';
}

static void m10_write_file(const char *path, const void *data, size_t len,
                           uint32_t mode, uint32_t uid, uint32_t gid) {
    struct vfs_file *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (!f) {
        serial_printf(SERIAL_COM1, "[M10] failed to create %s\n", path);
        return;
    }
    if (len) vfs_write(f, data, len);
    if (f->inode) {
        f->inode->mode = mode;
        f->inode->uid = uid;
        f->inode->gid = gid;
    }
    vfs_close(f);
}

static void m10_setup(void) {
    vfs_mkdir("/etc", 0755);
    vfs_mkdir("/bin", 0755);
    vfs_mkdir("/home", 0755);
    vfs_mkdir("/home/root", 0755);
    vfs_mkdir("/home/user", 0755);

    /* Default accounts: root (uid 0, password "root") and an unprivileged
     * user (uid 1000, password "user"). */
    static const char passwd_txt[] =
        "root:x:0:0:root:/home/root:/bin/sh\n"
        "user:x:1000:1000:user:/home/user:/bin/sh\n";
    m10_write_file("/etc/passwd", passwd_txt, sizeof(passwd_txt) - 1,
                   S_IFREG | 0644, 0, 0);

    char hroot[64], huser[64];
    crypt_lite("root", "rt", hroot, sizeof(hroot));
    crypt_lite("user", "us", huser, sizeof(huser));

    char shadow[256];
    size_t off = 0;
    m10_str_append(shadow, sizeof(shadow), &off, "root:");
    m10_str_append(shadow, sizeof(shadow), &off, hroot);
    m10_str_append(shadow, sizeof(shadow), &off, ":\nuser:");
    m10_str_append(shadow, sizeof(shadow), &off, huser);
    m10_str_append(shadow, sizeof(shadow), &off, ":\n");
    m10_write_file("/etc/shadow", shadow, off, S_IFREG | 0600, 0, 0);

    /* setuid-root helper binaries. */
    m10_write_file("/bin/su", _prog_su_start,
                   (size_t)(_prog_su_end - _prog_su_start),
                   S_IFREG | S_ISUID | 0755, 0, 0);
    m10_write_file("/bin/passwd", _prog_passwd_start,
                   (size_t)(_prog_passwd_end - _prog_passwd_start),
                   S_IFREG | S_ISUID | 0755, 0, 0);

    serial_print(SERIAL_COM1, "[M10] multi-user world initialised\n");
}

static void print(const char *str) {
    vga_print(str);
    serial_print(SERIAL_COM1, str);
}

static void print_char(char c) {
    vga_putchar(c);
    serial_putc(SERIAL_COM1, c);
}

static void read_line(char *buf, size_t max_len) {
    size_t i = 0;
    while (i < max_len - 1) {
        int c = keyboard_getc();
        if (c == 0) continue;

        if (c == '\n') {
            buf[i] = '\0';
            print("\n");
            return;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                print("\b \b");
            }
        } else if (c >= 32 && c < 127) {
            buf[i++] = (char)c;
            print_char((char)c);
        }
    }
    buf[i] = '\0';
}

static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void cmd_help(void) {
    print("Available commands:\n");
    print("  help     - Show this help message\n");
    print("  clear    - Clear the screen\n");
    print("  reboot   - Reboot the system\n");
    print("  ticks    - Show timer ticks\n");
    print("  meminfo  - Show memory stats\n");
    print("  echo     - Echo text back\n");
    print("  about    - About Project Lariat\n");
    print("  pci      - List PCI devices\n");
    print("  ata      - List ATA drives\n");
    print("  devs     - List registered devices\n");
    print("  ls       - List directory contents\n");
    print("  cat      - Display file contents\n");
}

static void cmd_ls(const char *args) {
    while (*args == ' ') args++;
    const char *path = (*args) ? args : "/";

    struct vfs_dir *dir = vfs_opendir(path);
    if (!dir) {
        print("Cannot open directory: ");
        print(path);
        print("\n");
        return;
    }

    struct vfs_dir_entry entry;
    while (vfs_readdir(dir, &entry)) {
        serial_printf(SERIAL_COM1, "  %s%s\n",
            entry.name,
            (entry.mode & S_IFDIR) ? "/" : "");
    }
    vfs_closedir(dir);
}

static void cmd_cat(const char *args) {
    while (*args == ' ') args++;
    if (!*args) {
        print("Usage: cat <file>\n");
        return;
    }

    struct vfs_file *f = vfs_open(args, O_RDONLY);
    if (!f) {
        print("Cannot open file: ");
        print(args);
        print("\n");
        return;
    }

    char buf[128];
    ssize_t n;
    while ((n = vfs_read(f, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        print(buf);
    }
    vfs_close(f);
}

static void cmd_clear(void) {
    vga_clear();
}

static void cmd_reboot(void) {
    print("Rebooting...\n");
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) bad_idt = {0, 0};
    __asm__ __volatile__("lidt %0" :: "m"(bad_idt));
    __asm__ __volatile__("int $0x00");
}

static void cmd_ticks(void) {
    serial_printf(SERIAL_COM1, "Timer ticks: %llx\n", timer_get_ticks());
}

static void cmd_about(void) {
    print("Project Lariat - 64-bit Operating System\n");
    print("Built from scratch, no Linux/Unix dependencies\n");
    print("Target: x86_64 bare metal\n");
}

static void cmd_echo(const char *args) {
    while (*args == ' ') args++;
    print(args);
    print("\n");
}

static void cmd_meminfo(void) {
    uint64_t total = pmm_total_pages();
    uint64_t free = pmm_get_free_count();
    uint64_t used = total - free;
    serial_printf(SERIAL_COM1,
        "Memory: %d MB total, %d MB used, %d MB free\n"
        "Pages:  %d total, %d used, %d free\n",
        (int)(total * 4096 / (1024 * 1024)),
        (int)(used * 4096 / (1024 * 1024)),
        (int)(free * 4096 / (1024 * 1024)),
        (int)total, (int)used, (int)free);
}

static void list_pci_device(device_t *dev, void *ctx) {
    (void)ctx;
    pci_dev_t *pci = device_pci(dev);
    if (!pci) return;
    serial_printf(SERIAL_COM1,
        "  %s: %04x:%04x class=%02x.%02x.%02x rev=%02x IRQ=%d\n",
        dev->name, pci->vendor_id, pci->device_id,
        pci->class_code, pci->subclass, pci->prog_if,
        pci->revision, pci->int_line);
}

static void cmd_pci(void) {
    print("PCI devices:\n");
    device_list_class(DEV_CLASS_OTHER, list_pci_device, NULL);
}

static void cmd_ata(void) {
    print("ATA drives:\n");
    for (int i = 0; i < 4; i++) {
        ata_dev_t *dev = ata_get_device(i);
        if (!dev) continue;
        serial_printf(SERIAL_COM1,
            "  %s: %s, %d sectors (%d MB)\n",
            (i == 0) ? "hda" : (i == 1) ? "hdb" : (i == 2) ? "hdc" : "hdd",
            dev->model, (int)dev->sectors, (int)(dev->sectors / 2048));
    }
}

static void list_all_device(device_t *dev, void *ctx) {
    (void)ctx;
    serial_printf(SERIAL_COM1, "  %s (class=%d, drv=%s)\n",
        dev->name, dev->class,
        dev->driver ? dev->driver->name : "none");
}

static void cmd_devs(void) {
    print("Registered devices:\n");
    device_list_class(DEV_CLASS_NONE, list_all_device, NULL);
    device_list_class(DEV_CLASS_CHAR, list_all_device, NULL);
    device_list_class(DEV_CLASS_BLOCK, list_all_device, NULL);
    device_list_class(DEV_CLASS_NET, list_all_device, NULL);
    device_list_class(DEV_CLASS_OTHER, list_all_device, NULL);
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
    console_init();
    ramfs_init();
    fat32_init();
    ext4_init();

    /* Mount filesystems */
    vfs_mount("ramfs", NULL, "/");
    vfs_mkdir("/disk", 0755);
    vfs_mount("fat32", "hdb", "/disk");
    vfs_mkdir("/ext4", 0755);
    vfs_mount("ext4", "hdc", "/ext4");

    /* Create a test file in ramfs for userspace I/O testing */
    struct vfs_file *testf = vfs_open("/hello.txt", O_CREAT | O_WRONLY);
    if (testf) {
        const char *msg = "Hello from ramfs!\n";
        vfs_write(testf, msg, 18);
        vfs_close(testf);
    }

    /* M10: users, /etc/passwd + /etc/shadow, setuid helpers. */
    m10_setup();

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
        thread_create((void (*)(void *))shell_thread, NULL);
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

/* --------------------------------------------------------------------------
 * SMP demo: schedule parallel jobs across all online cores
 * -------------------------------------------------------------------------- */
static spinlock_t smp_demo_lock = SPINLOCK_INIT;

static void smp_demo_job(void *arg) {
    int id = (int)(long)arg;
    uint64_t flags = spin_lock_irqsave(&smp_demo_lock);
    serial_printf(SERIAL_COM1, "[SMP] job %d ran on lapic id %d\n",
                  id, (int)lapic_id());
    spin_unlock_irqrestore(&smp_demo_lock, flags);
}

void smp_demo(void) {
    if (smp_cpu_count() <= 1) {
        serial_print(SERIAL_COM1, "[SMP] demo skipped (1 CPU)\n");
        return;
    }
    serial_printf(SERIAL_COM1, "[SMP] dispatching 24 jobs across %d CPUs\n",
                  smp_cpu_count());
    for (int i = 0; i < 24; i++) {
        smp_enqueue_work(smp_demo_job, (void *)(long)i);
    }
    /* Give the APs a chance to grab their share before the BSP drains the rest,
     * so the work is visibly distributed across cores. */
    for (volatile int spin = 0; spin < 2000000; spin++) {
        __asm__ __volatile__("pause");
    }
    smp_run_pending_work();
}

/* --------------------------------------------------------------------------
 * SMP stress: hammer the PMM concurrently from every core and verify that the
 * free-page count returns to its starting value (catches lost pages and
 * pmm_lock corruption).  Each allocation is touched through the direct map,
 * which also validates physmap access from the application processors.
 * -------------------------------------------------------------------------- */
#define PMM_STRESS_ITERS 4000
static volatile int pmm_stress_remaining;

static void pmm_stress_job(void *arg) {
    (void)arg;
    for (int i = 0; i < PMM_STRESS_ITERS; i++) {
        uint64_t p = pmm_alloc_page();
        if (p) {
            *(volatile uint64_t *)phys_to_virt(p) = p ^ 0xA5A5A5A5ULL;
            pmm_free_page(p);
        }
    }
    __sync_fetch_and_sub(&pmm_stress_remaining, 1);
}

void smp_stress(void) {
    uint64_t before = pmm_get_free_count();
    int jobs = (int)smp_cpu_count() * 4;
    if (jobs < 4) jobs = 4;

    pmm_stress_remaining = jobs;
    for (int i = 0; i < jobs; i++) {
        smp_enqueue_work(pmm_stress_job, 0);
    }
    /* BSP joins in, then waits for every job to finish. */
    while (pmm_stress_remaining > 0) {
        smp_run_pending_work();
        __asm__ __volatile__("pause");
    }

    uint64_t after = pmm_get_free_count();
    serial_printf(SERIAL_COM1,
                  "[STRESS] pmm %d jobs x %d alloc/free across %d core(s): "
                  "free before=%d after=%d %s\n",
                  jobs, PMM_STRESS_ITERS, (int)smp_cpu_count(),
                  (int)before, (int)after,
                  before == after ? "OK" : "*** LEAK/CORRUPTION ***");
}

/* --------------------------------------------------------------------------
 * Shell thread
 * -------------------------------------------------------------------------- */
static void shell_thread(void *arg) {
    (void)arg;
    print("Interrupts enabled. Type 'help' for commands.\n\n");

    char buf[256];
    while (1) {
        print(PROMPT);
        read_line(buf, sizeof(buf));

        if (buf[0] == '\0') continue;

        char *cmd = buf;
        char *args = buf;
        while (*args && *args != ' ') args++;
        if (*args == ' ') {
            *args = '\0';
            args++;
        }

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "clear") == 0) {
            cmd_clear();
        } else if (strcmp(cmd, "reboot") == 0) {
            cmd_reboot();
        } else if (strcmp(cmd, "ticks") == 0) {
            cmd_ticks();
        } else if (strcmp(cmd, "meminfo") == 0) {
            cmd_meminfo();
        } else if (strcmp(cmd, "about") == 0) {
            cmd_about();
        } else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(args);
        } else if (strcmp(cmd, "pci") == 0) {
            cmd_pci();
        } else if (strcmp(cmd, "ata") == 0) {
            cmd_ata();
        } else if (strcmp(cmd, "devs") == 0) {
            cmd_devs();
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls(args);
        } else if (strcmp(cmd, "cat") == 0) {
            cmd_cat(args);
        } else {
            print("Unknown command: ");
            print(cmd);
            print("\nType 'help' for available commands.\n");
        }
    }
}
