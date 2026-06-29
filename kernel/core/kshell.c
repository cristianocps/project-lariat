#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "string.h"
#include "timer.h"
#include "pmm.h"
#include "vfs.h"
#include "device.h"
#include "pci.h"
#include "ata.h"
#include "kshell.h"

#define KSHELL_MAX_CMDS 32
#define KSHELL_PROMPT   "lariat> "

struct kshell_cmd {
    const char *name;
    const char *help;
    kshell_fn   fn;
};

static struct kshell_cmd g_cmds[KSHELL_MAX_CMDS];
static int g_ncmds;

int kshell_register(const char *name, const char *help, kshell_fn handler) {
    if (g_ncmds >= KSHELL_MAX_CMDS) return -1;
    g_cmds[g_ncmds].name  = name;
    g_cmds[g_ncmds].help  = help;
    g_cmds[g_ncmds].fn    = handler;
    g_ncmds++;
    return 0;
}

void kshell_print(const char *s) {
    vga_print(s);
    serial_print(SERIAL_COM1, s);
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
            kshell_print("\n");
            return;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                kshell_print("\b \b");
            }
        } else if (c >= 32 && c < 127) {
            buf[i++] = (char)c;
            print_char((char)c);
        }
    }
    buf[i] = '\0';
}

/* -------------------------------------------------------------------------- *
 * Built-in commands
 * -------------------------------------------------------------------------- */

static void cmd_help(const char *args) {
    (void)args;
    kshell_print("Available commands:\n");
    for (int i = 0; i < g_ncmds; i++) {
        kshell_print("  ");
        kshell_print(g_cmds[i].name);
        kshell_print(" - ");
        kshell_print(g_cmds[i].help);
        kshell_print("\n");
    }
}

static void cmd_clear(const char *args) {
    (void)args;
    vga_clear();
}

static void cmd_reboot(const char *args) {
    (void)args;
    kshell_print("Rebooting...\n");
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) bad_idt = {0, 0};
    __asm__ __volatile__("lidt %0" :: "m"(bad_idt));
    __asm__ __volatile__("int $0x00");
}

static void cmd_ticks(const char *args) {
    (void)args;
    serial_printf(SERIAL_COM1, "Timer ticks: %llx\n", timer_get_ticks());
}

static void cmd_about(const char *args) {
    (void)args;
    kshell_print("Project Lariat - 64-bit Operating System\n");
    kshell_print("Built from scratch, no Linux/Unix dependencies\n");
    kshell_print("Target: x86_64 bare metal\n");
}

static void cmd_echo(const char *args) {
    while (*args == ' ') args++;
    kshell_print(args);
    kshell_print("\n");
}

static void cmd_meminfo(const char *args) {
    (void)args;
    uint64_t total = pmm_total_pages();
    uint64_t free  = pmm_get_free_count();
    uint64_t used  = total - free;
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

static void cmd_pci(const char *args) {
    (void)args;
    kshell_print("PCI devices:\n");
    device_list_class(DEV_CLASS_OTHER, list_pci_device, NULL);
}

static void cmd_ata(const char *args) {
    (void)args;
    kshell_print("ATA drives:\n");
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

static void cmd_devs(const char *args) {
    (void)args;
    kshell_print("Registered devices:\n");
    device_list_class(DEV_CLASS_NONE, list_all_device, NULL);
    device_list_class(DEV_CLASS_CHAR, list_all_device, NULL);
    device_list_class(DEV_CLASS_BLOCK, list_all_device, NULL);
    device_list_class(DEV_CLASS_NET, list_all_device, NULL);
    device_list_class(DEV_CLASS_OTHER, list_all_device, NULL);
}

static void cmd_ls(const char *args) {
    while (*args == ' ') args++;
    const char *path = (*args) ? args : "/";

    struct vfs_dir *dir = vfs_opendir(path);
    if (!dir) {
        kshell_print("Cannot open directory: ");
        kshell_print(path);
        kshell_print("\n");
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
        kshell_print("Usage: cat <file>\n");
        return;
    }

    struct vfs_file *f = vfs_open(args, O_RDONLY);
    if (!f) {
        kshell_print("Cannot open file: ");
        kshell_print(args);
        kshell_print("\n");
        return;
    }

    char buf[128];
    ssize_t n;
    while ((n = vfs_read(f, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        kshell_print(buf);
    }
    vfs_close(f);
}

static void register_builtins(void) {
    static int done;
    if (done) return;
    done = 1;
    kshell_register("help",    "Show this help message",    cmd_help);
    kshell_register("clear",   "Clear the screen",          cmd_clear);
    kshell_register("reboot",  "Reboot the system",         cmd_reboot);
    kshell_register("ticks",   "Show timer ticks",          cmd_ticks);
    kshell_register("meminfo", "Show memory stats",         cmd_meminfo);
    kshell_register("about",   "About Project Lariat",      cmd_about);
    kshell_register("echo",    "Echo text back",            cmd_echo);
    kshell_register("pci",     "List PCI devices",          cmd_pci);
    kshell_register("ata",     "List ATA drives",           cmd_ata);
    kshell_register("devs",    "List registered devices",   cmd_devs);
    kshell_register("ls",      "List directory contents",   cmd_ls);
    kshell_register("cat",     "Display file contents",     cmd_cat);
}

static kshell_fn lookup(const char *name) {
    for (int i = 0; i < g_ncmds; i++)
        if (strcmp(name, g_cmds[i].name) == 0)
            return g_cmds[i].fn;
    return NULL;
}

void kshell_thread(void *arg) {
    (void)arg;
    register_builtins();
    kshell_print("Interrupts enabled. Type 'help' for commands.\n\n");

    char buf[256];
    for (;;) {
        kshell_print(KSHELL_PROMPT);
        read_line(buf, sizeof(buf));
        if (buf[0] == '\0') continue;

        char *cmd = buf;
        char *args = buf;
        while (*args && *args != ' ') args++;
        if (*args == ' ') { *args = '\0'; args++; }

        kshell_fn fn = lookup(cmd);
        if (fn) {
            fn(args);
        } else {
            kshell_print("Unknown command: ");
            kshell_print(cmd);
            kshell_print("\nType 'help' for available commands.\n");
        }
    }
}
