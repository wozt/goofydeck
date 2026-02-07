// Render an SVG file into a wide-tile PNG (button 14) with optional tint.
//
// Usage: draw_svg_rectangle <path.svg> <hexcolor|transparent|keep>
//        [--height=H<=196] [--size=N<=196] [--offset=x,y] [--brightness=1..200] <output.png>
//
// - keep: keep original SVG colors (no tinting), preserves alpha.
// - transparent: render the SVG as an alpha "punch" (shape becomes transparent) using the same mask logic as draw_mdi.
//
// Canvas is W x H where H is --height (default 196) and W = round(H * 442 / 196).

#include <cairo/cairo.h>
#include <librsvg/rsvg.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define REF_W 442
#define REF_H 196

static int round_div(int num, int den) {
    if (den == 0) return 0;
    if (num >= 0) return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}
static int wide_w_from_h(int h) { return round_div(h * REF_W, REF_H); }

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

static int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b, int *is_transparent, int *is_keep) {
    *is_transparent = 0;
    *is_keep = 0;
    if (strcasecmp(s, "transparent") == 0) { *is_transparent = 1; return 0; }
    if (strcasecmp(s, "keep") == 0) { *is_keep = 1; return 0; }
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

static void apply_brightness(cairo_surface_t *surf, int brightness_percent) {
    if (!surf) return;
    if (brightness_percent == 100) return;
    if (brightness_percent < 1) brightness_percent = 1;
    if (brightness_percent > 200) brightness_percent = 200;

    cairo_surface_flush(surf);
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    int stride = cairo_image_surface_get_stride(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    float mul = (float)brightness_percent / 100.0f;

    for (int y = 0; y < h; y++) {
        uint8_t *row = data + y * stride;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + x * 4; // BGRA
            int bb = (int)(p[0] * mul);
            int gg = (int)(p[1] * mul);
            int rr = (int)(p[2] * mul);
            p[0] = (uint8_t)(bb > 255 ? 255 : bb);
            p[1] = (uint8_t)(gg > 255 ? 255 : gg);
            p[2] = (uint8_t)(rr > 255 ? 255 : rr);
        }
    }
    cairo_surface_mark_dirty(surf);
}

static void colorize_surface_like_mdi(cairo_surface_t *surf, uint8_t r, uint8_t g, uint8_t b, int is_transparent) {
    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    int width = cairo_image_surface_get_width(surf);
    int height = cairo_image_surface_get_height(surf);

    for (int y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(data + y * stride);
        for (int x = 0; x < width; x++) {
            uint8_t *p = (uint8_t *)&row[x]; // BGRA
            uint8_t b_src = p[0];
            uint8_t g_src = p[1];
            uint8_t r_src = p[2];
            uint8_t a_src = p[3];
            if (a_src == 0) continue;

            float gray = (r_src + g_src + b_src) / (3.0f * 255.0f);
            if (is_transparent) {
                if (gray < 0.5f) {
                    p[0] = p[1] = p[2] = 0;
                    p[3] = 255;
                } else {
                    p[0] = p[1] = p[2] = 0;
                    p[3] = 0;
                }
            } else {
                if (gray < 0.5f) {
                    p[0] = b;
                    p[1] = g;
                    p[2] = r;
                    p[3] = a_src;
                } else {
                    p[0] = 255;
                    p[1] = 255;
                    p[2] = 255;
                    p[3] = a_src;
                }
            }
        }
    }
    cairo_surface_mark_dirty(surf);
}

static int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <path.svg> <hexcolor|transparent|keep> [--height=H<=196] [--size=N<=196] [--offset=x,y] [--brightness=1..200] <output.png>\n",
                argv[0]);
        return 2;
    }

    const char *svg_path = argv[1];
    const char *color_s = argv[2];
    const char *out_png = argv[argc - 1];

    int canvas_h = 196;
    int icon_size = 196;
    int offx = 0, offy = 0;
    int brightness = 100;

    for (int i = 3; i < argc - 1; i++) {
        if (strncmp(argv[i], "--height=", 9) == 0) canvas_h = atoi(argv[i] + 9);
        else if (strncmp(argv[i], "--size=", 7) == 0) icon_size = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "--offset=", 9) == 0) {
            if (sscanf(argv[i] + 9, "%d,%d", &offx, &offy) != 2) return 2;
        } else if (strncmp(argv[i], "--brightness=", 13) == 0) {
            brightness = atoi(argv[i] + 13);
        }
    }

    canvas_h = clamp_i(canvas_h, 1, 196);
    int canvas_w = wide_w_from_h(canvas_h);
    if (canvas_w < 1) canvas_w = 1;

    icon_size = clamp_i(icon_size, 1, 196);
    brightness = clamp_i(brightness, 1, 200);

    uint8_t cr = 255, cg = 255, cb = 255;
    int is_transparent = 0, is_keep = 0;
    if (parse_color(color_s, &cr, &cg, &cb, &is_transparent, &is_keep) != 0) {
        fprintf(stderr, "Error: invalid color: %s\n", color_s);
        return 2;
    }

    GError *err = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_file(svg_path, &err);
    if (!handle) {
        fprintf(stderr, "Error: failed to load SVG: %s\n", err ? err->message : "unknown");
        if (err) g_error_free(err);
        return 1;
    }

    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvas_w, canvas_h);
    cairo_t *cr_dst = cairo_create(dst);
    cairo_set_source_rgba(cr_dst, 0, 0, 0, 0);
    cairo_set_operator(cr_dst, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr_dst);
    cairo_set_operator(cr_dst, CAIRO_OPERATOR_OVER);

    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, icon_size, icon_size);
    cairo_t *cr_tmp = cairo_create(tmp);
    cairo_set_source_rgba(cr_tmp, 0, 0, 0, 0);
    cairo_set_operator(cr_tmp, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr_tmp);
    cairo_set_operator(cr_tmp, CAIRO_OPERATOR_OVER);

    RsvgDimensionData dim;
    rsvg_handle_get_dimensions(handle, &dim);
    double sx = 1.0, sy = 1.0;
    if (dim.width > 0 && dim.height > 0) {
        sx = (double)icon_size / (double)dim.width;
        sy = (double)icon_size / (double)dim.height;
    }
    double s = sx < sy ? sx : sy;
    cairo_scale(cr_tmp, s, s);

    if (!rsvg_handle_render_cairo(handle, cr_tmp)) {
        fprintf(stderr, "Error: failed to render SVG\n");
        cairo_destroy(cr_tmp);
        cairo_surface_destroy(tmp);
        cairo_destroy(cr_dst);
        cairo_surface_destroy(dst);
        g_object_unref(handle);
        return 1;
    }
    cairo_destroy(cr_tmp);
    g_object_unref(handle);

    if (!is_keep) {
        cairo_surface_flush(tmp);
        colorize_surface_like_mdi(tmp, cr, cg, cb, is_transparent);
    }
    apply_brightness(tmp, brightness);

    int x = (canvas_w - icon_size) / 2 + offx;
    int y = (canvas_h - icon_size) / 2 + offy;
    cairo_set_source_surface(cr_dst, tmp, x, y);
    if (is_transparent && !is_keep) {
        cairo_set_operator(cr_dst, CAIRO_OPERATOR_DEST_OUT);
        cairo_paint(cr_dst);
    } else {
        cairo_set_operator(cr_dst, CAIRO_OPERATOR_OVER);
        cairo_paint(cr_dst);
    }

    cairo_destroy(cr_dst);
    cairo_surface_destroy(tmp);

    cairo_status_t st = cairo_surface_write_to_png(dst, out_png);
    cairo_surface_destroy(dst);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: write_to_png failed: %s\n", cairo_status_to_string(st));
        return 1;
    }
    return 0;
}

