/* Bochs / QEMU stdvga (BGA) linear-framebuffer driver.
 *
 * Programs the Bochs DISPI registers for a 1024x768x32 linear framebuffer and
 * exposes the LFB to userspace as /dev/fb0 (ioctl FBIOGET_INFO + mmap).  The
 * framebuffer's physical base is taken from BAR0 of the PCI display adapter
 * (vendor 0x1234, device 0x1111). */

#include "gfx.h"
#include "vfs.h"
#include "pci.h"
#include "ports.h"
#include "mm.h"
#include "vmm.h"
#include "sched.h"
#include "kapi.h"
#include "serial.h"
#include "uapi.h"
#include "errno.h"
#include <string.h>

/* DISPI I/O ports and register indices. */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_BANK        5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9

#define VBE_DISPI_DISABLED    0x00
#define VBE_DISPI_ENABLED     0x01
#define VBE_DISPI_LFB_ENABLED 0x40

#define FB_WIDTH  1024
#define FB_HEIGHT 768
#define FB_BPP    32

static uint64_t fb_phys;
static uint32_t fb_pitch;
static uint32_t fb_width, fb_height, fb_bpp;
static volatile uint32_t *fb_kvirt;   /* kernel mapping for early clear */

static void bga_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t bga_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bga_set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    bga_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    bga_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
}

/* --------------------------------------------------------------------------
 * /dev/fb0 file operations
 * -------------------------------------------------------------------------- */
static int fb_ioctl(struct vfs_file *file, unsigned long req, unsigned long arg) {
    (void)file;
    if (req == FBIOGET_INFO) {
        struct fb_var_info *info = (struct fb_var_info *)(uintptr_t)arg;
        if (!info) return -EINVAL;
        info->width  = fb_width;
        info->height = fb_height;
        info->bpp    = fb_bpp;
        info->pitch  = fb_pitch;
        info->size   = (uint64_t)fb_pitch * fb_height;
        return 0;
    }
    return -ENOTTY;
}

static int fb_mmap(struct vfs_file *file, uint64_t user_va, size_t length,
                   uint64_t prot) {
    (void)file; (void)prot;
    struct thread *t = current_thread();
    if (!t || !t->cr3) return -ENOMEM;

    uint64_t total = (uint64_t)fb_pitch * fb_height;
    if (length > total) length = total;
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(t->cr3);

    for (uint64_t i = 0; i < pages; i++) {
        if (vmm_map_page_in(pml4, user_va + i * PAGE_SIZE,
                            fb_phys + i * PAGE_SIZE,
                            PT_USER | PT_PRESENT | PT_WRITABLE) < 0)
            return -ENOMEM;
    }
    return 0;
}

static ssize_t fb_write(struct vfs_file *file, const void *buf, size_t count) {
    (void)file;
    uint64_t total = (uint64_t)fb_pitch * fb_height;
    if ((uint64_t)file->pos >= total) return 0;
    if (file->pos + count > total) count = total - file->pos;
    memcpy((void *)((uint8_t *)fb_kvirt + file->pos), buf, count);
    file->pos += count;
    return (ssize_t)count;
}

static off_t fb_lseek(struct vfs_file *file, off_t off, int whence) {
    uint64_t total = (uint64_t)fb_pitch * fb_height;
    if (whence == 0) file->pos = off;
    else if (whence == 1) file->pos += off;
    else if (whence == 2) file->pos = (off_t)total + off;
    if (file->pos < 0) file->pos = 0;
    return file->pos;
}

static struct vfs_file_ops fb_fops = {
    .write = fb_write,
    .lseek = fb_lseek,
    .ioctl = fb_ioctl,
    .mmap  = fb_mmap,
};

static struct vfs_inode fb_inode;

/* --------------------------------------------------------------------------
 * Bring-up
 * -------------------------------------------------------------------------- */
int bochs_vbe_init(void) {
    /* Locate the display adapter and read BAR0 (the LFB). */
    int found = 0;
    for (uint16_t slot = 0; slot < 32 && !found; slot++) {
        uint16_t v = pci_vendor(0, slot, 0);
        if (v == 0xFFFF) continue;
        uint16_t d = pci_device(0, slot, 0);
        uint8_t cls = pci_class(0, slot, 0);
        /* Bochs/QEMU stdvga is 1234:1111; also accept any VGA display class. */
        if ((v == 0x1234 && d == 0x1111) || cls == PCI_CLASS_DISPLAY) {
            uint32_t bar0 = pci_bar(0, slot, 0, 0);
            if (pci_bar_is_mem(bar0)) {
                fb_phys = pci_bar_mem_addr(bar0);
                pci_enable_bus_mastering(0, slot, 0);
                found = 1;
            }
        }
    }
    if (!found) {
        serial_print(SERIAL_COM1, "[FB] no Bochs/VGA display adapter found\n");
        return -ENODEV;
    }

    /* Confirm the DISPI interface is present. */
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        serial_printf(SERIAL_COM1, "[FB] DISPI id=%x unexpected, trying anyway\n", id);
    }

    bga_set_mode(FB_WIDTH, FB_HEIGHT, FB_BPP);

    fb_width  = FB_WIDTH;
    fb_height = FB_HEIGHT;
    fb_bpp    = FB_BPP;
    fb_pitch  = FB_WIDTH * (FB_BPP / 8);

    /* Map the LFB for the kernel (used to paint an initial background). */
    fb_kvirt = (volatile uint32_t *)ioremap(fb_phys, (uint64_t)fb_pitch * fb_height);

    /* Paint a dark blue background so a bare boot shows the mode took effect. */
    uint32_t pixels = fb_width * fb_height;
    for (uint32_t i = 0; i < pixels; i++) fb_kvirt[i] = 0x00102038;

    /* Register /dev/fb0. */
    memset(&fb_inode, 0, sizeof(fb_inode));
    fb_inode.mode = S_IFCHR | 0666;
    fb_inode.f_ops = &fb_fops;
    fb_inode.size = (uint32_t)((uint64_t)fb_pitch * fb_height);
    vfs_devfs_register("fb0", &fb_inode);

    serial_printf(SERIAL_COM1,
                  "[FB] %dx%dx%d pitch=%d phys=%x -> /dev/fb0\n",
                  fb_width, fb_height, fb_bpp, fb_pitch, (uint32_t)fb_phys);
    return 0;
}
