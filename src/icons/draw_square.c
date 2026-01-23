// Minimal PNG writer for solid-color square icons.
// Usage: draw_square <hexcolor|transparent> [--size=N] <filename.png>
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
            if (c & 1)
                c = 0xedb88320U ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[n] = c;
    }
    crc_table_computed = 1;
}

static uint32_t update_crc(uint32_t crc, const unsigned char *buf, size_t len) {
    uint32_t c = crc;
    if (!crc_table_computed)
        make_crc_table();
    for (size_t n = 0; n < len; n++) {
        c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
    }
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <hexcolor|transparent> [--size=N<=196] <filename.png>\n", argv[0]);
        return 1;
    }
    const char *color_str = argv[1];
    int size = 196;
    const char *fname = NULL;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--size=", 7) == 0) {
            size = atoi(argv[i] + 7);
        } else {
            fname = argv[i];
        }
    }
    if (!fname) {
        fprintf(stderr, "Filename required.\n");
        return 1;
    }
    if (size < 1) size = 1;
    if (size > 196) size = 196;
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

    // PNG signature
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    // IHDR
    unsigned char ihdr[13];
    ihdr[0] = (size >> 24) & 0xff;
    ihdr[1] = (size >> 16) & 0xff;
    ihdr[2] = (size >> 8) & 0xff;
    ihdr[3] = size & 0xff;
    ihdr[4] = ihdr[0];
    ihdr[5] = ihdr[1];
    ihdr[6] = ihdr[2];
    ihdr[7] = ihdr[3];
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // color type RGBA
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    write_chunk(f, "IHDR", ihdr, 13);

    // Prepare raw image data (per-row filter byte + RGBA)
    size_t row_bytes = 1 + 4 * size;
    size_t raw_len = row_bytes * size;
    unsigned char *raw = malloc(raw_len);
    if (!raw) {
        fclose(f);
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    for (int y = 0; y < size; y++) {
        size_t offset = y * row_bytes;
        raw[offset] = 0; // filter None
        for (int x = 0; x < size; x++) {
            size_t p = offset + 1 + x * 4;
            raw[p + 0] = r;
            raw[p + 1] = g;
            raw[p + 2] = b;
            raw[p + 3] = a;
        }
    }

    // Compress with zlib (default deflate)
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
