#ifndef LIBC_GFX_H
#define LIBC_GFX_H

#include <stdint.h>
#include <stddef.h>

/* A drawable surface: 32bpp ARGB/XRGB pixels, `pitch` pixels per row. */
typedef struct {
    uint32_t *pix;
    int       w, h;
    int       pitch;   /* in pixels */
} surface_t;

/* Framebuffer context (the real screen) plus an off-screen back buffer that all
 * compositing draws into; gfx_present() copies the back buffer to the screen. */
typedef struct {
    int       fd;
    surface_t screen;   /* mmapped /dev/fb0 */
    surface_t back;     /* off-screen buffer */
    uint64_t  fb_bytes;
} gfx_t;

/* RGB helper. */
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Sentinel "background" colour meaning "leave existing pixels untouched"
 * (used to draw text over arbitrary content). */
#define GFX_TRANSPARENT 0xFF000001u

int  gfx_open(gfx_t *g);
void gfx_present(gfx_t *g);

void gfx_fill(surface_t *s, uint32_t color);
void gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t color);
void gfx_frame(surface_t *s, int x, int y, int w, int h, uint32_t color);
void gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src);
void gfx_char(surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg);
void gfx_text(surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg);

#endif /* LIBC_GFX_H */
