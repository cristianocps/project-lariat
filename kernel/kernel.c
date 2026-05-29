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
#include "vfs.h"
#include "block.h"
#include "ata.h"
#include "fat32.h"
#include "ext4.h"
#include "sched.h"

#define PROMPT "lariat> "

static void shell_thread(void *arg);

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

    /* Initialize kernel subsystems */
    kapi_heap_init((void *)0x200000, 0x100000);
    module_init();
    device_init();
    driver_init();

    vmm_init();
    vmm_expand_identity_mapping();

    block_init();
    ata_init();

    vfs_init();
    ramfs_init();
    fat32_init();
    ext4_init();

    /* Mount filesystems */
    vfs_mount("ramfs", NULL, "/");
    vfs_mkdir("/disk", 0755);
    vfs_mount("fat32", "hdb", "/disk");
    vfs_mkdir("/ext4", 0755);
    vfs_mount("ext4", "hdc", "/ext4");

    pic_init();
    idt_init();
    timer_init(100);
    keyboard_init();

    pci_init();
    driver_probe_all();

    pic_clear_mask(0);
    pic_clear_mask(1);

    __asm__ __volatile__("sti");

    /* Start scheduler */
    scheduler_init();
    thread_create((void (*)(void *))shell_thread, NULL);

    /* Idle loop */
    while (1) {
        __asm__ __volatile__("hlt");
    }
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
