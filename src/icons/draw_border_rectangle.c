// draw_border_rectangle.c: draw a filled rounded rectangle onto an existing RGBA PNG.
// This is the wide-tile (button 14) counterpart to draw_border (square).
//
// Usage: draw_border_rectangle <hexcolor|transparent> [--size=H<=196] [--radius=R<=50] <filename.png>
//
// Notes:
// - --size specifies HEIGHT (H). Width is derived by a product rule from the reference wide-tile size:
//   reference is (196 + 196 + 50) x 196 = 442 x 196, so W = round(H * 442 / 196).
// - Operates in place. If filename is relative, it is resolved relative to the project root.

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "fd_path.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_H 196
#define REF_W 442
#define REF_H 196

// --- CRC helpers (PNG) ---
static uint32_t crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320U ^ (c >> 1) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static uint32_t crc_update(uint32_t c, const unsigned char *buf, size_t len) {
    if (!crc_ready) crc_init();
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    return c;
}
static uint32_t crc_calc(const unsigned char *buf, size_t len) {
    return crc_update(0xffffffffL, buf, len) ^ 0xffffffffL;
}
static void write_be32(FILE *f, uint32_t v) {
    unsigned char b[4] = {(v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff};
    fwrite(b, 1, 4, f);
}
static int write_chunk(FILE *f, const char *type, const unsigned char *data, size_t len) {
    write_be32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    unsigned char *buf = malloc(len + 4);
    if (!buf) return -1;
    memcpy(buf, type, 4);
    if (len) memcpy(buf + 4, data, len);
    uint32_t c = crc_calc(buf, len + 4);
    free(buf);
    write_be32(f, c);
    return 0;
}

static int read_be32(const unsigned char *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int hexbyte(char h, char l) {
    int v = 0;
    if (h >= '0' && h <= '9') v = (h - '0') << 4;
    else if (h >= 'A' && h <= 'F') v = (h - 'A' + 10) << 4;
    else if (h >= 'a' && h <= 'f') v = (h - 'a' + 10) << 4;
    else return -1;
    if (l >= '0' && l <= '9') v |= (l - '0');
    else if (l >= 'A' && l <= 'F') v |= (l - 'A' + 10);
    else if (l >= 'a' && l <= 'f') v |= (l - 'a' + 10);
    else return -1;
    return v;
}

static int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b, int *is_transparent) {
    if (strcasecmp(s, "transparent") == 0) {
        *is_transparent = 1;
        *r = *g = *b = 0;
        return 0;
    }
    *is_transparent = 0;
    if (!s || strlen(s) != 6) return -1;
    int r8 = hexbyte(s[0], s[1]);
    int g8 = hexbyte(s[2], s[3]);
    int b8 = hexbyte(s[4], s[5]);
    if (r8 < 0 || g8 < 0 || b8 < 0) return -1;
    *r = (uint8_t)r8;
    *g = (uint8_t)g8;
    *b = (uint8_t)b8;
    return 0;
}

typedef struct {
    uint32_t width, height;
    unsigned char *data; // scanlines (filter byte + RGBA pixels)
    size_t data_len;
} PngRaw;

static int load_png_raw(const char *path, PngRaw *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        fclose(f);
        return -1;
    }

    uint32_t width = 0, height = 0;
    unsigned char *idat = NULL;
    size_t idat_len = 0;
    for (;;) {
        unsigned char lenb[4], type[4];
        if (fread(lenb, 1, 4, f) != 4) break;
        uint32_t len = (uint32_t)read_be32(lenb);
        if (fread(type, 1, 4, f) != 4) { free(idat); fclose(f); return -1; }
        unsigned char *buf = NULL;
        if (len) {
            buf = malloc(len);
            if (!buf || fread(buf, 1, len, f) != len) { free(buf); free(idat); fclose(f); return -1; }
        }
        fseek(f, 4, SEEK_CUR); // CRC

        if (memcmp(type, "IHDR", 4) == 0) {
            width = (uint32_t)read_be32(buf);
            height = (uint32_t)read_be32(buf + 4);
            if (buf[8] != 8 || buf[9] != 6) { // 8-bit RGBA
                free(buf);
                free(idat);
                fclose(f);
                return -1;
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            unsigned char *nb = realloc(idat, idat_len + len);
            if (!nb) { free(buf); free(idat); fclose(f); return -1; }
            memcpy(nb + idat_len, buf, len);
            idat = nb;
            idat_len += len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            free(buf);
            break;
        }
        free(buf);
    }
    fclose(f);

    if (!width || !height || !idat) { free(idat); return -1; }

    size_t raw_cap = (size_t)(1 + 4 * width) * (size_t)height;
    unsigned char *raw = malloc(raw_cap);
    if (!raw) { free(idat); return -1; }
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK) { free(idat); free(raw); return -1; }
    strm.next_in = idat;
    strm.avail_in = (uInt)idat_len;
    strm.next_out = raw;
    strm.avail_out = (uInt)raw_cap;
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    free(idat);
    if (ret != Z_STREAM_END) { free(raw); return -1; }

    out->width = width;
    out->height = height;
    out->data = raw;
    out->data_len = raw_cap;
    return 0;
}

static int save_png_raw(const char *path, const unsigned char *raw, uint32_t width, uint32_t height) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    unsigned char ihdr[13];
    ihdr[0] = (width >> 24) & 0xff;
    ihdr[1] = (width >> 16) & 0xff;
    ihdr[2] = (width >> 8) & 0xff;
    ihdr[3] = width & 0xff;
    ihdr[4] = (height >> 24) & 0xff;
    ihdr[5] = (height >> 16) & 0xff;
    ihdr[6] = (height >> 8) & 0xff;
    ihdr[7] = height & 0xff;
    ihdr[8] = 8;
    ihdr[9] = 6; // RGBA
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    if (write_chunk(f, "IHDR", ihdr, 13) != 0) { fclose(f); return -1; }

    size_t raw_len = (size_t)(1 + 4 * width) * (size_t)height;
    uLongf comp_bound = compressBound(raw_len);
    unsigned char *zbuf = malloc(comp_bound);
    if (!zbuf) { fclose(f); return -1; }
    if (compress2(zbuf, &comp_bound, raw, raw_len, Z_BEST_COMPRESSION) != Z_OK) {
        free(zbuf);
        fclose(f);
        return -1;
    }
    int ok = 0;
    ok |= write_chunk(f, "IDAT", zbuf, (size_t)comp_bound);
    ok |= write_chunk(f, "IEND", NULL, 0);
    free(zbuf);
    fclose(f);
    return ok ? -1 : 0;
}

static int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int round_div(int num, int den) {
    if (den == 0) return 0;
    if (num >= 0) return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}

static int wide_w_from_h(int h) { return round_div(h * REF_W, REF_H); }

static void blend_rounded_rect(unsigned char *raw, uint32_t img_w, uint32_t img_h, int rect_h, int radius_percent,
                               uint8_t r, uint8_t g, uint8_t b, int is_transparent) {
    int w = (int)img_w;
    int h = (int)img_h;
    int rect_w = wide_w_from_h(rect_h);

    rect_h = clamp_i(rect_h, 1, h);
    rect_w = clamp_i(rect_w, 1, w);

    int rad_px = (rect_h * radius_percent) / 100;
    int max_rad = (rect_h < rect_w ? rect_h : rect_w) / 2;
    rad_px = clamp_i(rad_px, 0, max_rad);

    int start_x = (w - rect_w) / 2;
    int start_y = (h - rect_h) / 2;

    int rad2 = rad_px * rad_px;
    int inner_w = rect_w - 2 * rad_px;
    int inner_h = rect_h - 2 * rad_px;
    if (inner_w < 0) inner_w = 0;
    if (inner_h < 0) inner_h = 0;

    size_t stride = 1 + 4 * (size_t)w;
    for (int y = start_y; y < start_y + rect_h; y++) {
        if (y < 0 || y >= h) continue;
        unsigned char *row = raw + (size_t)y * stride;
        for (int x = start_x; x < start_x + rect_w; x++) {
            if (x < 0 || x >= w) continue;
            int lx = x - start_x;
            int ly = y - start_y;

            int inside = 0;
            if (lx >= rad_px && lx < rad_px + inner_w && ly >= rad_px && ly < rad_px + inner_h) {
                inside = 1;
            } else if (rad_px > 0) {
                int cx;
                if (lx < rad_px) cx = rad_px;
                else if (lx >= rad_px + inner_w) cx = rad_px + inner_w - 1;
                else cx = lx;

                int cy;
                if (ly < rad_px) cy = rad_px;
                else if (ly >= rad_px + inner_h) cy = rad_px + inner_h - 1;
                else cy = ly;

                int dx = lx - cx;
                int dy = ly - cy;
                if (dx * dx + dy * dy <= rad2) inside = 1;
            } else {
                inside = 1;
            }
            if (!inside) continue;

            uint8_t *px = row + (1 + (size_t)x * 4);
            if (is_transparent) {
                px[0] = px[1] = px[2] = 0;
                px[3] = 0;
                continue;
            }
            px[0] = r;
            px[1] = g;
            px[2] = b;
            px[3] = 255;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <hexcolor|transparent> [--size=H<=196] [--radius=R<=50] <filename.png>\n", argv[0]);
        return 1;
    }

    const char *color_str = argv[1];
    int rect_h = 196;
    int radius = 12;
    const char *fname = NULL;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--size=", 7) == 0) rect_h = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "--radius=", 9) == 0) radius = atoi(argv[i] + 9);
        else fname = argv[i];
    }
    if (!fname) {
        fprintf(stderr, "Error: filename required\n");
        return 1;
    }
    rect_h = clamp_i(rect_h, 1, MAX_H);
    radius = clamp_i(radius, 0, 50);

    uint8_t r = 0, g = 0, b = 0;
    int is_transparent = 0;
    if (parse_color(color_str, &r, &g, &b, &is_transparent) != 0) {
        fprintf(stderr, "Error: invalid color '%s'\n", color_str);
        return 1;
    }

    char root[PATH_MAX];
    if (fd_find_project_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "Could not locate project root (set PROJECT_ROOT)\n");
        return 1;
    }

    char path[PATH_MAX];
    if (fd_resolve_root_relative(root, fname, path, sizeof(path)) != 0) {
        fprintf(stderr, "Error: bad filename\n");
        return 1;
    }

    PngRaw png;
    if (load_png_raw(path, &png) != 0) {
        fprintf(stderr, "Error: failed to load PNG: %s\n", path);
        return 1;
    }

    blend_rounded_rect(png.data, png.width, png.height, rect_h, radius, r, g, b, is_transparent);

    int rc = save_png_raw(path, png.data, png.width, png.height);
    free(png.data);
    if (rc != 0) {
        fprintf(stderr, "Error: failed to write PNG: %s\n", path);
        return 1;
    }
    return 0;
}

