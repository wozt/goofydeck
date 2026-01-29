// Normalize an existing PNG:
// - crop center to largest square
// - resize to a fixed square size (default 128x128; configurable below)
// Output is an RGBA PNG.
//
// Usage: draw_normalize <input.png> <output.png>

#include <png.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Quick knob: output icon size (square). Keep <= 196 to match device constraints.
static int g_normalize_target_size = 100;

// Quick knobs: optional pre-quantization dithering (Floyd–Steinberg style) on RGB.
// This is meant to reduce banding before later palette quantization (draw_optimize).
// 0 disables dithering.
static int g_normalize_fs_dither_enable = 1;
// Per-channel quantization bits for dithering (2..8). 6 is a decent default.
static int g_normalize_fs_dither_bits = 3;

static void die_errno(const char *msg) {
    fprintf(stderr, "Error: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die_errno("malloc");
    return p;
}

static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) die_errno("calloc");
    return p;
}

static uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static int read_png_rgba(const char *path, uint8_t **out_rgba, int *out_w, int *out_h) {
    *out_rgba = NULL;
    *out_w = 0;
    *out_h = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    png_uint_32 w = 0, h = 0;
    int bit_depth = 0, color_type = 0, interlace = 0, compression = 0, filter = 0;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, &interlace, &compression, &filter);

    // Convert to 8-bit RGBA.
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER); // RGB -> RGBA

    png_read_update_info(png, info);
    png_size_t rowbytes = png_get_rowbytes(png, info);

    uint8_t *buf = xmalloc(rowbytes * h);
    png_bytep *rows = xmalloc(sizeof(png_bytep) * h);
    for (png_uint_32 y = 0; y < h; y++) rows[y] = buf + (size_t)y * rowbytes;

    png_read_image(png, rows);
    png_read_end(png, NULL);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    // Ensure rowbytes matches w*4; libpng may align but for RGBA it should be exactly.
    // If it doesn't, repack.
    if ((png_uint_32)rowbytes != w * 4) {
        uint8_t *packed = xmalloc((size_t)w * h * 4);
        for (png_uint_32 y = 0; y < h; y++) {
            memcpy(packed + (size_t)y * w * 4, buf + (size_t)y * rowbytes, (size_t)w * 4);
        }
        free(buf);
        buf = packed;
    }

    *out_rgba = buf;
    *out_w = (int)w;
    *out_h = (int)h;
    return 0;
}

static int write_png_rgba(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    // External icons may come from URLs; prefer smaller output while keeping full color fidelity.
    png_set_compression_level(png, Z_BEST_COMPRESSION);
    png_set_filter(png, 0, PNG_ALL_FILTERS);

    png_bytep *rows = xmalloc(sizeof(png_bytep) * (size_t)h);
    for (int y = 0; y < h; y++) rows[y] = (png_bytep)(rgba + (size_t)y * (size_t)w * 4);
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

static void crop_center_square(const uint8_t *src, int sw, int sh, uint8_t **out, int *out_w, int *out_h) {
    int s = sw < sh ? sw : sh;
    int x0 = (sw - s) / 2;
    int y0 = (sh - s) / 2;
    uint8_t *sq = xmalloc((size_t)s * (size_t)s * 4);
    for (int y = 0; y < s; y++) {
        const uint8_t *row = src + ((size_t)(y0 + y) * (size_t)sw + (size_t)x0) * 4;
        memcpy(sq + (size_t)y * (size_t)s * 4, row, (size_t)s * 4);
    }
    *out = sq;
    *out_w = s;
    *out_h = s;
}

static void resize_bilinear_rgba(const uint8_t *src, int sw, int sh, uint8_t **out, int *out_w, int *out_h, int dw, int dh) {
    uint8_t *dst = xcalloc((size_t)dw * (size_t)dh, 4);
    for (int y = 0; y < dh; y++) {
        float gy = (dh == 1) ? 0.0f : ((float)y * (float)(sh - 1)) / (float)(dh - 1);
        int y0 = (int)gy;
        int y1 = (y0 + 1 < sh) ? (y0 + 1) : y0;
        float fy = gy - (float)y0;
        for (int x = 0; x < dw; x++) {
            float gx = (dw == 1) ? 0.0f : ((float)x * (float)(sw - 1)) / (float)(dw - 1);
            int x0 = (int)gx;
            int x1 = (x0 + 1 < sw) ? (x0 + 1) : x0;
            float fx = gx - (float)x0;

            const uint8_t *p00 = src + ((size_t)y0 * (size_t)sw + (size_t)x0) * 4;
            const uint8_t *p10 = src + ((size_t)y0 * (size_t)sw + (size_t)x1) * 4;
            const uint8_t *p01 = src + ((size_t)y1 * (size_t)sw + (size_t)x0) * 4;
            const uint8_t *p11 = src + ((size_t)y1 * (size_t)sw + (size_t)x1) * 4;

            float w00 = (1.0f - fx) * (1.0f - fy);
            float w10 = fx * (1.0f - fy);
            float w01 = (1.0f - fx) * fy;
            float w11 = fx * fy;

            uint8_t *d = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4;
            for (int c = 0; c < 4; c++) {
                float v = w00 * (float)p00[c] + w10 * (float)p10[c] + w01 * (float)p01[c] + w11 * (float)p11[c];
                d[c] = clamp_u8((int)(v + 0.5f));
            }
        }
    }
    *out = dst;
    *out_w = dw;
    *out_h = dh;
}

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t quantize_u8_bits(uint8_t v, int bits) {
    bits = clamp_i(bits, 1, 8);
    int levels = 1 << bits; // 2..256
    if (levels <= 1) return v;
    // Map 0..255 -> 0..levels-1 with rounding, then back to 0..255.
    int q = (int)((v * (levels - 1) + 127) / 255);
    int out = (int)((q * 255 + (levels - 2) / 2) / (levels - 1));
    return clamp_u8(out);
}

// Apply Floyd–Steinberg error diffusion to RGB channels only, quantizing to N bits per channel.
// Alpha is left unchanged. Fully transparent pixels do not diffuse error.
static void fs_dither_rgb_inplace(uint8_t *rgba, int w, int h, int bits) {
    if (!rgba || w <= 0 || h <= 0) return;
    bits = clamp_i(bits, 2, 8);

    // Fixed-point errors with denominator 16 (matches FS weights).
    size_t row_n = (size_t)w + 2;
    int *er_cur = xcalloc(row_n, sizeof(int));
    int *eg_cur = xcalloc(row_n, sizeof(int));
    int *eb_cur = xcalloc(row_n, sizeof(int));
    int *er_nxt = xcalloc(row_n, sizeof(int));
    int *eg_nxt = xcalloc(row_n, sizeof(int));
    int *eb_nxt = xcalloc(row_n, sizeof(int));

    for (int y = 0; y < h; y++) {
        uint8_t *row = rgba + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + (size_t)x * 4;
            uint8_t a = p[3];

            int r = (int)p[0] + (er_cur[x + 1] / 16);
            int g = (int)p[1] + (eg_cur[x + 1] / 16);
            int b = (int)p[2] + (eb_cur[x + 1] / 16);
            r = clamp_i(r, 0, 255);
            g = clamp_i(g, 0, 255);
            b = clamp_i(b, 0, 255);

            uint8_t rq = quantize_u8_bits((uint8_t)r, bits);
            uint8_t gq = quantize_u8_bits((uint8_t)g, bits);
            uint8_t bq = quantize_u8_bits((uint8_t)b, bits);

            p[0] = rq;
            p[1] = gq;
            p[2] = bq;

            if (a == 0) continue;

            int dr = r - (int)rq;
            int dg = g - (int)gq;
            int db = b - (int)bq;

            // Right (x+1): 7/16
            er_cur[x + 2] += dr * 7;
            eg_cur[x + 2] += dg * 7;
            eb_cur[x + 2] += db * 7;

            // Down-left (x-1): 3/16
            er_nxt[x + 0] += dr * 3;
            eg_nxt[x + 0] += dg * 3;
            eb_nxt[x + 0] += db * 3;

            // Down (x): 5/16
            er_nxt[x + 1] += dr * 5;
            eg_nxt[x + 1] += dg * 5;
            eb_nxt[x + 1] += db * 5;

            // Down-right (x+1): 1/16
            er_nxt[x + 2] += dr * 1;
            eg_nxt[x + 2] += dg * 1;
            eb_nxt[x + 2] += db * 1;
        }

        // Next line: swap and clear.
        int *tmp;
        tmp = er_cur; er_cur = er_nxt; er_nxt = tmp;
        tmp = eg_cur; eg_cur = eg_nxt; eg_nxt = tmp;
        tmp = eb_cur; eb_cur = eb_nxt; eb_nxt = tmp;
        memset(er_nxt, 0, row_n * sizeof(int));
        memset(eg_nxt, 0, row_n * sizeof(int));
        memset(eb_nxt, 0, row_n * sizeof(int));
    }

    free(er_cur); free(eg_cur); free(eb_cur);
    free(er_nxt); free(eg_nxt); free(eb_nxt);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.png> <output.png>\n", argv[0]);
        return 2;
    }
    const char *in = argv[1];
    const char *out = argv[2];

    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (read_png_rgba(in, &rgba, &w, &h) != 0) {
        fprintf(stderr, "Error: failed to read PNG: %s\n", in);
        return 1;
    }

    uint8_t *sq = NULL;
    int sw = 0, sh = 0;
    crop_center_square(rgba, w, h, &sq, &sw, &sh);
    free(rgba);

    int target = g_normalize_target_size;
    if (target < 1) target = 1;
    if (target > 196) target = 196;

    // Always output a target x target icon (upscale or downscale) for consistent composition.
    uint8_t *final = NULL;
    int fw = 0, fh = 0;
    if (sw == target) {
        final = sq;
        fw = sw;
        fh = sh;
    } else {
        resize_bilinear_rgba(sq, sw, sh, &final, &fw, &fh, target, target);
        free(sq);
    }

    if (g_normalize_fs_dither_enable) {
        fs_dither_rgb_inplace(final, fw, fh, g_normalize_fs_dither_bits);
    }

    if (write_png_rgba(out, final, fw, fh) != 0) {
        fprintf(stderr, "Error: failed to write PNG: %s\n", out);
        free(final);
        return 1;
    }
    free(final);
    return 0;
}
