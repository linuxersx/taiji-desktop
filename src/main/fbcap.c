// SPDX-License-Identifier: GPL-3.0-or-later
/* Read the Linux framebuffer and write a small BMP thumbnail.
 *
 * Wayland sessions frequently lack a usable WAYLAND_DISPLAY in the daemon that
 * triggers the overview, and grim then fails silently.  The framebuffer is the
 * frame that is actually being scanned out, so it is the most reliable source
 * for "what the user was just looking at".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint8_t channel(uint32_t value, const struct fb_bitfield *f)
{
    if (!f->length) return 0;
    uint32_t mask = (1u << f->length) - 1u;
    uint32_t c = (value >> f->offset) & mask;
    return (uint8_t)((c * 255u) / mask);
}

static uint32_t raw_pixel(const uint8_t *row, int x, int bpp)
{
    if (bpp == 16) {
        uint16_t v;
        memcpy(&v, row + (size_t)x * 2, sizeof(v));
        return v;
    }
    if (bpp == 24) {
        return (uint32_t)row[(size_t)x * 3] |
               ((uint32_t)row[(size_t)x * 3 + 1] << 8) |
               ((uint32_t)row[(size_t)x * 3 + 2] << 16);
    }
    uint32_t v;
    memcpy(&v, row + (size_t)x * 4, sizeof(v));
    return v;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : NULL;
    int max_w = argc > 2 ? atoi(argv[2]) : 480;
    if (!out || !*out) {
        fprintf(stderr, "usage: taiji-fbcap <output.bmp> [max-width]\n");
        return 2;
    }
    if (max_w < 64) max_w = 64;

    const char *dev = getenv("TAIJI_FB");
    if (!dev || !*dev) dev = "/dev/fb0";

    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "taiji-fbcap: open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fi) != 0) {
        fprintf(stderr, "taiji-fbcap: ioctl: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    int w = (int)vi.xres;
    int h = (int)vi.yres;
    int bpp = (int)vi.bits_per_pixel;
    size_t line = fi.line_length ? fi.line_length : (size_t)w * (size_t)((bpp + 7) / 8);
    if (w <= 0 || h <= 0 || (bpp != 16 && bpp != 24 && bpp != 32)) {
        fprintf(stderr, "taiji-fbcap: unsupported mode %dx%d %dbpp\n", w, h, bpp);
        close(fd);
        return 1;
    }

    size_t map_size = fi.smem_len ? fi.smem_len
                                 : line * (size_t)(vi.yres_virtual ? (int)vi.yres_virtual : h);
    size_t bytes_pp = (size_t)((bpp + 7) / 8);
    size_t base = (size_t)vi.yoffset * line + (size_t)vi.xoffset * bytes_pp;
    if (base >= map_size) base = 0;
    if (base + line * (size_t)h > map_size)
        h = (int)((map_size - base) / line);
    if (h <= 0) {
        fprintf(stderr, "taiji-fbcap: framebuffer window is outside memory\n");
        close(fd);
        return 1;
    }

    uint8_t *map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "taiji-fbcap: mmap: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    int step = (w + max_w - 1) / max_w;
    if (step < 1) step = 1;
    int out_w = w / step;
    int out_h = h / step;
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    size_t row_size = ((size_t)out_w * 3 + 3u) & ~(size_t)3u;
    size_t data_size = row_size * (size_t)out_h;
    uint8_t *rows = calloc(1, data_size);
    if (!rows) {
        fprintf(stderr, "taiji-fbcap: out of memory\n");
        munmap(map, map_size);
        close(fd);
        return 1;
    }

    long lit = 0;
    long lit_threshold = (long)out_w * (long)out_h / 1000;
    if (lit_threshold < 4) lit_threshold = 4;

    /* BMP rows are stored bottom-up. */
    for (int oy = 0; oy < out_h; oy++) {
        int sy = h - 1 - oy * step;
        const uint8_t *src = map + base + (size_t)sy * line;
        uint8_t *dst = rows + (size_t)oy * row_size;
        for (int ox = 0; ox < out_w; ox++) {
            int sx = ox * step;
            uint32_t raw = raw_pixel(src, sx, bpp);
            uint8_t r = channel(raw, &vi.red);
            uint8_t g = channel(raw, &vi.green);
            uint8_t b = channel(raw, &vi.blue);
            if (vi.red.length == 0 && vi.green.length == 0 && vi.blue.length == 0) {
                /* Some drivers leave the bitfields empty; assume common layouts. */
                if (bpp == 16) {
                    r = (uint8_t)(((raw >> 11) & 0x1F) * 255 / 31);
                    g = (uint8_t)(((raw >> 5) & 0x3F) * 255 / 63);
                    b = (uint8_t)((raw & 0x1F) * 255 / 31);
                } else {
                    r = (uint8_t)((raw >> 16) & 0xFF);
                    g = (uint8_t)((raw >> 8) & 0xFF);
                    b = (uint8_t)(raw & 0xFF);
                }
            }
            dst[ox * 3] = b;      /* BMP stores BGR */
            dst[ox * 3 + 1] = g;
            dst[ox * 3 + 2] = r;
            if (r + g + b > 24) lit++;
        }
    }

    munmap(map, map_size);
    close(fd);

    if (lit < lit_threshold) {
        fprintf(stderr, "taiji-fbcap: %s looks blank (%ld lit pixels)\n", dev, lit);
        free(rows);
        return 1;
    }

    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "taiji-fbcap: open %s: %s\n", out, strerror(errno));
        free(rows);
        return 1;
    }
    uint8_t header[54];
    memset(header, 0, sizeof(header));
    uint32_t file_size = 54u + (uint32_t)data_size;
    header[0] = 'B';
    header[1] = 'M';
    put_le32(header + 2, file_size);
    put_le32(header + 10, 54);
    put_le32(header + 14, 40);
    put_le32(header + 18, (uint32_t)out_w);
    put_le32(header + 22, (uint32_t)out_h);
    put_le16(header + 26, 1);
    put_le16(header + 28, 24);
    put_le32(header + 34, (uint32_t)data_size);
    put_le32(header + 38, 2835);
    put_le32(header + 42, 2835);

    int ok = fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
             fwrite(rows, 1, data_size, f) == data_size;
    if (fclose(f) != 0) ok = 0;
    free(rows);
    if (!ok) {
        fprintf(stderr, "taiji-fbcap: write %s failed\n", out);
        unlink(out);
        return 1;
    }
    return 0;
}
