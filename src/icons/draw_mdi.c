// Render an MDI SVG tinted to a given color and composite it centered onto the given PNG.
// Depends on cairo + librsvg + pkg-config for compilation.
// Usage: draw_mdi <mdi:name|name> <hexcolor> [--size=N<=196] <filename.png>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <zlib.h>
#include <cairo.h>
#include <librsvg/rsvg.h>
#include <stdarg.h>

#include "fd_path.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Avoid truncation warnings by routing formatting through checked wrappers.
static void die_snprintf(const char *label) {
    fprintf(stderr, "Error: buffer too small for %s\n", label);
    exit(1);
}

static void snprintf_checked(char *dst, size_t cap, const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) die_snprintf(label);
}

// Silence intentional unused warnings for static helpers kept for future refactors.
#if defined(__GNUC__) || defined(__clang__)
#define FD_UNUSED __attribute__((unused))
#else
#define FD_UNUSED
#endif

// CRC helpers
static uint32_t crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xedb88320U ^ (c >> 1) : (c >> 1);
        }
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static uint32_t crc_update(uint32_t c, const unsigned char *buf, size_t len) {
    if (!crc_ready) crc_init();
    for (size_t i = 0; i < len; i++) {
        c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    }
    return c;
}
static uint32_t crc_calc(const unsigned char *buf, size_t len) {
    return crc_update(0xffffffffL, buf, len) ^ 0xffffffffL;
}

static void write_be32(FILE *f, uint32_t v) {
    unsigned char b[4] = { (v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff };
    fwrite(b,1,4,f);
}

static int write_chunk(FILE *f, const char *type, const unsigned char *data, size_t len) {
    write_be32(f, (uint32_t)len);
    fwrite(type,1,4,f);
    if (len>0) fwrite(data,1,len,f);
    size_t crc_len = len + 4;
    unsigned char *buf = malloc(crc_len);
    if (!buf) return -1;
    memcpy(buf, type, 4);
    if (len>0) memcpy(buf+4, data, len);
    uint32_t c = crc_calc(buf, crc_len);
    free(buf);
    write_be32(f, c);
    return 0;
}

static FD_UNUSED int save_png_raw(const char *path, const unsigned char *raw, uint32_t width, uint32_t height) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("write png"); return -1; }
    const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    fwrite(sig,1,8,f);
    unsigned char ihdr[13];
    ihdr[0]=(width>>24)&0xff; ihdr[1]=(width>>16)&0xff; ihdr[2]=(width>>8)&0xff; ihdr[3]=width&0xff;
    ihdr[4]=(height>>24)&0xff; ihdr[5]=(height>>16)&0xff; ihdr[6]=(height>>8)&0xff; ihdr[7]=height&0xff;
    ihdr[8]=8; ihdr[9]=6; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    write_chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (1 + 4*width) * height;
    uLongf comp_bound = compressBound(raw_len);
    unsigned char *zbuf = malloc(comp_bound);
    if (!zbuf) { fclose(f); return -1; }
    int zres = compress2(zbuf, &comp_bound, raw, raw_len, Z_BEST_COMPRESSION);
    if (zres != Z_OK) { free(zbuf); fclose(f); return -1; }
    write_chunk(f, "IDAT", zbuf, comp_bound);
    write_chunk(f, "IEND", NULL, 0);
    free(zbuf);
    fclose(f);
    return 0;
}

// static const char *base_name(const char *path) {
//     const char *slash = strrchr(path, '/');
//     return slash ? slash + 1 : path;
// }

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

static int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (strlen(s) != 6) return -1;
    int r8 = hexbyte(s[0], s[1]);
    int g8 = hexbyte(s[2], s[3]);
    int b8 = hexbyte(s[4], s[5]);
    if (r8 < 0 || g8 < 0 || b8 < 0) return -1;
    *r = (uint8_t)r8; *g = (uint8_t)g8; *b = (uint8_t)b8;
    return 0;
}

static void colorize_surface(cairo_surface_t *surf, uint8_t r, uint8_t g, uint8_t b) {
    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    int width = cairo_image_surface_get_width(surf);
    int height = cairo_image_surface_get_height(surf);
    for (int y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(data + y * stride);
        for (int x = 0; x < width; x++) {
            uint8_t *p = (uint8_t *)&row[x];
            uint8_t a = p[3];
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = a;
        }
    }
    cairo_surface_mark_dirty(surf);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <mdi:name|name> <hexcolor> [--size=N<=196] <filename.png>\n", argv[0]);
        return 1;
    }

    const char *icon_spec = argv[1];
    const char *color_str = argv[2];
    int size = 196;
    const char *fname = NULL;
    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--size=", 7) == 0) {
            size = atoi(argv[i] + 7);
        } else {
            fname = argv[i];
        }
    }
    if (!fname) { fprintf(stderr, "Filename required.\n"); return 1; }
    if (size < 1) size = 1;
    if (size > 196) size = 196;

    uint8_t r,g,b;
    if (parse_color(color_str, &r, &g, &b) != 0) {
        fprintf(stderr, "Invalid color: %s\n", color_str);
        return 1;
    }

    const char *name = icon_spec;
    if (strncmp(icon_spec, "mdi:", 4) == 0) name = icon_spec + 4;

    char root[PATH_MAX];
    if (fd_find_project_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "Could not locate project root (set PROJECT_ROOT)\n");
        return 1;
    }

    char svg_path[PATH_MAX];
    snprintf_checked(svg_path, sizeof(svg_path), "svg_path", "%s/mdi/%s.svg", root, name);
    struct stat st;
    if (stat(svg_path, &st) != 0) {
        perror("svg not found");
        return 1;
    }

    char png_path[PATH_MAX];
    if (fname[0] == '/') snprintf_checked(png_path, sizeof(png_path), "png_path(abs)", "%s", fname);
    else snprintf_checked(png_path, sizeof(png_path), "png_path(root_rel)", "%s/%s", root, fname);
    if (fd_mkdir_p_parent(png_path) != 0) {
        perror("mkdir");
        return 1;
    }

    cairo_surface_t *target = cairo_image_surface_create_from_png(png_path);
    if (cairo_surface_status(target) != CAIRO_STATUS_SUCCESS) {
        // create blank 196x196 if missing
        target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 196, 196);
    }
    int tw = cairo_image_surface_get_width(target);
    int th = cairo_image_surface_get_height(target);

    cairo_surface_t *overlay = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(overlay);

    RsvgHandle *handle = rsvg_handle_new_from_file(svg_path, NULL);
    if (!handle) { fprintf(stderr,"Failed to load SVG\n"); cairo_destroy(cr); cairo_surface_destroy(overlay); cairo_surface_destroy(target); return 1; }

    RsvgRectangle viewport = {0, 0, (double)size, (double)size};
    if (!rsvg_handle_render_document(handle, cr, &viewport, NULL)) {
        fprintf(stderr,"Failed to render SVG\n");
        g_object_unref(handle);
        cairo_destroy(cr);
        cairo_surface_destroy(overlay);
        cairo_surface_destroy(target);
        return 1;
    }
    g_object_unref(handle);
    cairo_destroy(cr);

    // Colorize overlay
    colorize_surface(overlay, r, g, b);

    // // Save debug overlay (tinted icon only)
    // const char *base = base_name(png_path);
    // char debug_path[PATH_MAX];
    // snprintf_checked(debug_path, sizeof(debug_path), "debug_path", "%s.debug.png", png_path);
    // cairo_surface_write_to_png(overlay, debug_path);

    // Composite centered
    cairo_t *ct = cairo_create(target);
    cairo_set_source_surface(ct, overlay, (tw - size) / 2.0, (th - size) / 2.0);
    cairo_paint(ct);
    cairo_destroy(ct);

    // Write via cairo (handles premultiplied format)
    cairo_surface_flush(target);
    cairo_surface_write_to_png(target, png_path);

    cairo_surface_destroy(overlay);
    cairo_surface_destroy(target);
    return 0;
}
