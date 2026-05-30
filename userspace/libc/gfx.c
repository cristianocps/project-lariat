#include "libc/gfx.h"
#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/fcntl.h"
#include "libc/sys/ioctl.h"
#include "libc/font8x16.h"

/* Framebuffer ABI (mirrors include/uapi.h, kept local to avoid pulling in the
 * kernel UAPI header which clashes with the libc headers). */
#ifndef MAP_SHARED
#define MAP_SHARED 0x01
#endif
#define FBIOGET_INFO 0x4600
struct fb_var_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint64_t size;
};

int gfx_open(gfx_t *g) {
    g->fd = open("/dev/fb0", O_RDWR);
    if (g->fd < 0) return -1;

    struct fb_var_info info;
    if (ioctl(g->fd, FBIOGET_INFO, (long)&info) < 0) { close(g->fd); return -1; }

    g->fb_bytes = info.size;
    uint32_t *fb = (uint32_t *)mmap(0, info.size, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, g->fd, 0);
    if ((long)fb <= 0) { close(g->fd); return -1; }

    g->screen.pix   = fb;
    g->screen.w     = (int)info.width;
    g->screen.h     = (int)info.height;
    g->screen.pitch = (int)(info.pitch / 4);

    g->back.w     = g->screen.w;
    g->back.h     = g->screen.h;
    g->back.pitch = g->screen.w;
    g->back.pix   = (uint32_t *)malloc((size_t)g->screen.w * g->screen.h * 4);
    if (!g->back.pix) { close(g->fd); return -1; }
    return 0;
}

void gfx_present(gfx_t *g) {
    /* Copy each row (back buffer is tightly packed; screen may be padded). */
    for (int y = 0; y < g->screen.h; y++) {
        memcpy(g->screen.pix + (size_t)y * g->screen.pitch,
               g->back.pix + (size_t)y * g->back.pitch,
               (size_t)g->screen.w * 4);
    }
}

void gfx_fill(surface_t *s, uint32_t color) {
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = s->pix + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) row[x] = color;
    }
}

void gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = s->pix + (size_t)(y + yy) * s->pitch + x;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
}

void gfx_frame(surface_t *s, int x, int y, int w, int h, uint32_t color) {
    gfx_rect(s, x, y, w, 1, color);
    gfx_rect(s, x, y + h - 1, w, 1, color);
    gfx_rect(s, x, y, 1, h, color);
    gfx_rect(s, x + w - 1, y, 1, h, color);
}

void gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src) {
    for (int y = 0; y < src->h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < src->w; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            dst->pix[(size_t)ty * dst->pitch + tx] =
                src->pix[(size_t)y * src->pitch + x];
        }
    }
}

void gfx_char(surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg) {
    unsigned uc = (unsigned char)c;
    const unsigned char *gl;
    if (uc < FONT_FIRST || uc > FONT_LAST) gl = font8x16[0];   /* space */
    else gl = font8x16[uc - FONT_FIRST];

    for (int row = 0; row < FONT_H; row++) {
        int ty = y + row;
        if (ty < 0 || ty >= s->h) continue;
        unsigned char bits = gl[row];
        for (int col = 0; col < FONT_W; col++) {
            int tx = x + col;
            if (tx < 0 || tx >= s->w) continue;
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            if (color == GFX_TRANSPARENT) continue;
            s->pix[(size_t)ty * s->pitch + tx] = color;
        }
    }
}

void gfx_text(surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg) {
    int cx = x;
    for (const char *p = str; *p; p++) {
        if (*p == '\n') { cx = x; y += FONT_H; continue; }
        gfx_char(s, cx, y, *p, fg, bg);
        cx += FONT_W;
    }
}
