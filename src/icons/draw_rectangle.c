// Minimal PNG writer for solid-color rectangle icons.
// Usage: draw_rectangle <hexcolor|transparent> [--size=N<=196] <filename.png>
//
// The rectangle keeps the same aspect ratio as the Ulanzi D200 button 14 tile:
// reference (196+196+50) x 196 = 442 x 196.
//
// We treat --size as the HEIGHT, and compute WIDTH via a proportional scale.
// Writes to the given path (if relative, it is resolved relative to the project root). Uses zlib for compression.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>

#include "fd_path.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static uint32_t crc_table[256];
static int crc_table_computed = 0;

static void make_crc_table(void) {
    uint32_t c;
    for (uint32_t n = 0; n < 256; n++) {
        c = n;
        for (int k = 0; k < 8; k++) {
            if (c & 1) c = 0xedb88320U ^ (c >> 1);
            else c = c >> 1;
        }
        crc_table[n] = c;
    }
    crc_table_computed = 1;
}

static uint32_t update_crc(uint32_t crc, const unsigned char *buf, size_t len) {
    uint32_t c = crc;
    if (!crc_table_computed) make_crc_table();
    for (size_t n = 0; n < len; n++) c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
    return c;
}

static uint32_t crc(const unsigned char *buf, size_t len) {
    return update_crc(0xffffffffL, buf, len) ^ 0xffffffffL;
}

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

static int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    if (strcmp(s, "transparent") == 0 || strcmp(s, "TRANSPARENT") == 0) {
        *r = *g = *b = 0;
        *a = 0;
        return 0;
    }
    if (strlen(s) != 6) return -1;
    int r8 = hexbyte(s[0], s[1]);
    int g8 = hexbyte(s[2], s[3]);
    int b8 = hexbyte(s[4], s[5]);
    if (r8 < 0 || g8 < 0 || b8 < 0) return -1;
    *r = (uint8_t)r8;
    *g = (uint8_t)g8;
    *b = (uint8_t)b8;
    *a = 255;
    return 0;
}

static void write_be32(FILE *f, uint32_t v) {
    unsigned char b[4];
    b[0] = (v >> 24) & 0xff;
    b[1] = (v >> 16) & 0xff;
    b[2] = (v >> 8) & 0xff;
    b[3] = v & 0xff;
    fwrite(b, 1, 4, f);
}

static int write_chunk(FILE *f, const char *type, const unsigned char *data, size_t len) {
    write_be32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (len > 0) fwrite(data, 1, len, f);
    uint8_t *crc_buf = malloc(len + 4);
    if (!crc_buf) return -1;
    memcpy(crc_buf, type, 4);
    if (len > 0) memcpy(crc_buf + 4, data, len);
    uint32_t c = crc(crc_buf, len + 4);
    free(crc_buf);
    write_be32(f, c);
    return 0;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int scaled_width_for_height(int h) {
    // Reference: 442 x 196 (button14 width x height)
    // width = round(h * 442 / 196)
    const int ref_w = 196 + 196 + 50; // 442
    const int ref_h = 196;
    int num = h * ref_w;
    int w = (num + ref_h / 2) / ref_h;
    if (w < 1) w = 1;
    if (w > ref_w) w = ref_w;
    return w;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <hexcolor|transparent> [--size=N<=196] <filename.png>\n", argv[0]);
        return 1;
    }
    const char *color_str = argv[1];
    int h = 196;
    const char *fname = NULL;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--size=", 7) == 0) {
            h = atoi(argv[i] + 7);
        } else {
            fname = argv[i];
        }
    }
    if (!fname) {
        fprintf(stderr, "Filename required.\n");
        return 1;
    }
    h = clamp_int(h, 1, 196);
    int w = scaled_width_for_height(h);

    uint8_t r, g, b, a;
    if (parse_color(color_str, &r, &g, &b, &a) != 0) {
        fprintf(stderr, "Invalid color: %s\n", color_str);
        return 1;
    }

    char path[PATH_MAX];
    if (fname[0] == '/') {
        fd_snprintf_checked(path, sizeof(path), "out(abs)", "%s", fname);
    } else {
        char root[PATH_MAX];
        if (fd_find_project_root(root, sizeof(root)) != 0) {
            fprintf(stderr, "Could not locate project root (set PROJECT_ROOT)\n");
            return 1;
        }
        if (fd_resolve_root_relative(root, fname, path, sizeof(path)) != 0) return 1;
    }
    if (fd_mkdir_p_parent(path) != 0) {
        perror("mkdir");
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("open output");
        return 1;
    }

    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    // IHDR
    unsigned char ihdr[13];
    ihdr[0] = (w >> 24) & 0xff;
    ihdr[1] = (w >> 16) & 0xff;
    ihdr[2] = (w >> 8) & 0xff;
    ihdr[3] = w & 0xff;
    ihdr[4] = (h >> 24) & 0xff;
    ihdr[5] = (h >> 16) & 0xff;
    ihdr[6] = (h >> 8) & 0xff;
    ihdr[7] = h & 0xff;
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // color type RGBA
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    write_chunk(f, "IHDR", ihdr, 13);

    // Raw image data
    size_t row_bytes = 1 + 4u * (size_t)w;
    size_t raw_len = row_bytes * (size_t)h;
    unsigned char *raw = malloc(raw_len);
    if (!raw) {
        fclose(f);
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    for (int y = 0; y < h; y++) {
        size_t off = (size_t)y * row_bytes;
        raw[off] = 0;
        for (int x = 0; x < w; x++) {
            size_t p = off + 1 + (size_t)x * 4;
            raw[p + 0] = r;
            raw[p + 1] = g;
            raw[p + 2] = b;
            raw[p + 3] = a;
        }
    }

    uLongf comp_bound = compressBound(raw_len);
    unsigned char *zbuf = malloc(comp_bound);
    if (!zbuf) {
        free(raw);
        fclose(f);
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    int zres = compress2(zbuf, &comp_bound, raw, raw_len, Z_BEST_COMPRESSION);
    if (zres != Z_OK) {
        free(raw);
        free(zbuf);
        fclose(f);
        fprintf(stderr, "compress2 failed\n");
        return 1;
    }

    write_chunk(f, "IDAT", zbuf, comp_bound);
    write_chunk(f, "IEND", NULL, 0);

    free(raw);
    free(zbuf);
    fclose(f);
    return 0;
}

