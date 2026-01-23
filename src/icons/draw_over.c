// draw_over.c: overlay top image onto bottom image with alpha blending.
// Usage: draw_over <top.png> <bottom.png>
// Resizes top.png to bottom.png dimensions (bilinear) and writes the result back to bottom.png.
// No external libs (pure zlib + minimal PNG reader/writer).

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <zlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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
    return crc_update(0xffffffffu, buf, len) ^ 0xffffffffu;
}

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void write_be32(FILE *f, uint32_t v) {
    unsigned char b[4] = { (v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff };
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

// --- PNG decode (bit depth 8 only; color types: 6 RGBA, 2 RGB, 3 palette) ---
typedef struct {
    uint32_t w, h;
    unsigned char *rgba; // w*h*4
} Image;

static void image_free(Image *img) {
    free(img->rgba);
    img->rgba = NULL;
    img->w = img->h = 0;
}

static int png_unfilter(uint8_t *out, const uint8_t *scan, uint32_t w, uint32_t h, uint32_t bpp) {
    uint32_t stride = bpp * w;
    const uint8_t *prev = NULL;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = scan + y * (1 + stride);
        uint8_t type = row[0];
        const uint8_t *dat = row + 1;
        uint8_t *dst = out + y * stride;
        switch (type) {
            case 0: // None
                memcpy(dst, dat, stride);
                break;
            case 1: // Sub
                for (uint32_t x = 0; x < stride; x++) {
                    uint8_t left = (x >= bpp) ? dst[x - bpp] : 0;
                    dst[x] = (uint8_t)(dat[x] + left);
                }
                break;
            case 2: // Up
                for (uint32_t x = 0; x < stride; x++) {
                    uint8_t up = prev ? prev[x] : 0;
                    dst[x] = (uint8_t)(dat[x] + up);
                }
                break;
            case 3: // Average
                for (uint32_t x = 0; x < stride; x++) {
                    uint8_t left = (x >= bpp) ? dst[x - bpp] : 0;
                    uint8_t up = prev ? prev[x] : 0;
                    dst[x] = (uint8_t)(dat[x] + ((left + up) >> 1));
                }
                break;
            case 4: // Paeth
                for (uint32_t x = 0; x < stride; x++) {
                    uint8_t left = (x >= bpp) ? dst[x - bpp] : 0;
                    uint8_t up = prev ? prev[x] : 0;
                    uint8_t up_left = (prev && x >= bpp) ? prev[x - bpp] : 0;
                    int p = (int)left + (int)up - (int)up_left;
                    int pa = abs(p - (int)left);
                    int pb = abs(p - (int)up);
                    int pc = abs(p - (int)up_left);
                    uint8_t pr = (pa <= pb && pa <= pc) ? left : ((pb <= pc) ? up : up_left);
                    dst[x] = (uint8_t)(dat[x] + pr);
                }
                break;
            default:
                return -1;
        }
        prev = dst;
    }
    return 0;
}

static int load_png_rgba(const char *path, Image *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        fclose(f);
        return -1;
    }

    uint32_t w = 0, h = 0;
    uint8_t bit_depth = 0, color_type = 0;
    unsigned char palette[256][4];
    int palette_size = 0;
    for (int i = 0; i < 256; i++) { palette[i][0] = palette[i][1] = palette[i][2] = 0; palette[i][3] = 255; }

    unsigned char *idat = NULL;
    size_t idat_len = 0;

    for (;;) {
        unsigned char lenb[4], type[4];
        if (fread(lenb, 1, 4, f) != 4) break;
        uint32_t len = read_be32(lenb);
        if (fread(type, 1, 4, f) != 4) { free(idat); fclose(f); return -1; }
        unsigned char *buf = NULL;
        if (len) {
            buf = malloc(len);
            if (!buf || fread(buf, 1, len, f) != len) { free(buf); free(idat); fclose(f); return -1; }
        }
        fseek(f, 4, SEEK_CUR); // CRC

        if (memcmp(type, "IHDR", 4) == 0) {
            if (len < 13) { free(buf); free(idat); fclose(f); return -1; }
            w = read_be32(buf);
            h = read_be32(buf + 4);
            bit_depth = buf[8];
            color_type = buf[9];
            if (bit_depth != 8) { free(buf); free(idat); fclose(f); return -1; }
            if (!(color_type == 6 || color_type == 2 || color_type == 3)) { free(buf); free(idat); fclose(f); return -1; }
        } else if (memcmp(type, "PLTE", 4) == 0) {
            if (len % 3 != 0 || len / 3 > 256) { free(buf); free(idat); fclose(f); return -1; }
            palette_size = (int)(len / 3);
            for (int i = 0; i < palette_size; i++) {
                palette[i][0] = buf[3 * i + 0];
                palette[i][1] = buf[3 * i + 1];
                palette[i][2] = buf[3 * i + 2];
                palette[i][3] = 255;
            }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            int entries = (int)len;
            if (entries > 256) entries = 256;
            for (int i = 0; i < entries; i++) palette[i][3] = buf[i];
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

    if (!w || !h || !idat) { free(idat); return -1; }
    if (color_type == 3 && palette_size == 0) { free(idat); return -1; }

    uint32_t bpp = (color_type == 6) ? 4 : (color_type == 2 ? 3 : 1);
    size_t scan_cap = (size_t)(1 + bpp * w) * h;
    unsigned char *scan = malloc(scan_cap);
    if (!scan) { free(idat); return -1; }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) { free(idat); free(scan); return -1; }
    zs.next_in = idat;
    zs.avail_in = (unsigned int)idat_len;
    zs.next_out = scan;
    zs.avail_out = (unsigned int)scan_cap;
    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    free(idat);
    if (ret != Z_STREAM_END) { free(scan); return -1; }

    unsigned char *raw = malloc((size_t)bpp * w * h);
    if (!raw) { free(scan); return -1; }
    if (png_unfilter(raw, scan, w, h, bpp) != 0) { free(scan); free(raw); return -1; }
    free(scan);

    unsigned char *rgba = malloc((size_t)w * h * 4);
    if (!rgba) { free(raw); return -1; }

    if (color_type == 6) {
        memcpy(rgba, raw, (size_t)w * h * 4);
    } else if (color_type == 2) {
        size_t in = 0, outp = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            rgba[outp++] = raw[in++];
            rgba[outp++] = raw[in++];
            rgba[outp++] = raw[in++];
            rgba[outp++] = 255;
        }
    } else { // palette
        size_t outp = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            unsigned char idx = raw[i];
            if (idx >= (unsigned char)palette_size) {
                rgba[outp++] = 0;
                rgba[outp++] = 0;
                rgba[outp++] = 0;
                rgba[outp++] = 0;
            } else {
                rgba[outp++] = palette[idx][0];
                rgba[outp++] = palette[idx][1];
                rgba[outp++] = palette[idx][2];
                rgba[outp++] = palette[idx][3];
            }
        }
    }
    free(raw);

    out->w = w;
    out->h = h;
    out->rgba = rgba;
    return 0;
}

static int save_png_rgba(const char *path, const unsigned char *rgba, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    unsigned char ihdr[13];
    ihdr[0] = (w >> 24) & 0xff; ihdr[1] = (w >> 16) & 0xff; ihdr[2] = (w >> 8) & 0xff; ihdr[3] = w & 0xff;
    ihdr[4] = (h >> 24) & 0xff; ihdr[5] = (h >> 16) & 0xff; ihdr[6] = (h >> 8) & 0xff; ihdr[7] = h & 0xff;
    ihdr[8] = 8;
    ihdr[9] = 6; // RGBA
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (write_chunk(f, "IHDR", ihdr, 13) != 0) { fclose(f); return -1; }

    size_t row_bytes = 1 + (size_t)w * 4;
    size_t raw_len = row_bytes * h;
    unsigned char *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (uint32_t y = 0; y < h; y++) {
        size_t off = (size_t)y * row_bytes;
        raw[off] = 0; // no filter
        memcpy(raw + off + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }

    uLongf bound = compressBound(raw_len);
    unsigned char *zbuf = malloc(bound);
    if (!zbuf) { free(raw); fclose(f); return -1; }
    int zres = compress2(zbuf, &bound, raw, raw_len, Z_BEST_COMPRESSION);
    free(raw);
    if (zres != Z_OK) { free(zbuf); fclose(f); return -1; }

    int ok = (write_chunk(f, "IDAT", zbuf, (size_t)bound) == 0) && (write_chunk(f, "IEND", NULL, 0) == 0);
    free(zbuf);
    fclose(f);
    return ok ? 0 : -1;
}

// --- image ops ---
static uint8_t *resize_rgba_bilinear(const uint8_t *src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) {
        uint8_t *copy = malloc((size_t)dw * dh * 4);
        if (copy) memcpy(copy, src, (size_t)dw * dh * 4);
        return copy;
    }
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;

    double sx = (double)sw / (double)dw;
    double sy = (double)sh / (double)dh;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            double src_x = x * sx;
            double src_y = y * sy;
            int x0 = (int)src_x;
            int y0 = (int)src_y;
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
            double fx = src_x - x0;
            double fy = src_y - y0;

            const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * 4;
            const uint8_t *p01 = src + ((size_t)y0 * sw + x1) * 4;
            const uint8_t *p10 = src + ((size_t)y1 * sw + x0) * 4;
            const uint8_t *p11 = src + ((size_t)y1 * sw + x1) * 4;
            uint8_t *out = dst + ((size_t)y * dw + x) * 4;

            for (int c = 0; c < 4; c++) {
                double v00 = p00[c];
                double v01 = p01[c];
                double v10 = p10[c];
                double v11 = p11[c];
                double v0 = v00 * (1.0 - fx) + v01 * fx;
                double v1 = v10 * (1.0 - fx) + v11 * fx;
                double v = v0 * (1.0 - fy) + v1 * fy;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                out[c] = (uint8_t)(v + 0.5);
            }
        }
    }
    return dst;
}

static void composite_top_over_bottom(uint8_t *bottom_rgba, const uint8_t *top_rgba, uint32_t w, uint32_t h) {
    size_t pixels = (size_t)w * h;
    for (size_t i = 0; i < pixels; i++) {
        uint8_t *b = bottom_rgba + i * 4;
        const uint8_t *t = top_rgba + i * 4;

        uint32_t ta = t[3];
        if (ta == 0) continue;
        uint32_t ba = b[3];
        if (ba == 0 && ta == 255) {
            b[0] = t[0]; b[1] = t[1]; b[2] = t[2]; b[3] = 255;
            continue;
        }

        uint32_t out_a = ta + (ba * (255 - ta) + 127) / 255;
        if (out_a == 0) {
            b[0] = b[1] = b[2] = b[3] = 0;
            continue;
        }

        uint32_t bt = (ba * (255 - ta) + 127) / 255;
        uint32_t pr = t[0] * ta + b[0] * bt;
        uint32_t pg = t[1] * ta + b[1] * bt;
        uint32_t pb = t[2] * ta + b[2] * bt;

        b[0] = (uint8_t)((pr + out_a / 2) / out_a);
        b[1] = (uint8_t)((pg + out_a / 2) / out_a);
        b[2] = (uint8_t)((pb + out_a / 2) / out_a);
        b[3] = (uint8_t)out_a;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <top.png> <bottom.png>\n", argv[0]);
        return 1;
    }
    const char *top_path = argv[1];
    const char *bottom_path = argv[2];

    Image top = {0}, bottom = {0};
    if (load_png_rgba(top_path, &top) != 0) {
        fprintf(stderr, "Failed to read top PNG: %s\n", top_path);
        return 1;
    }
    if (load_png_rgba(bottom_path, &bottom) != 0) {
        fprintf(stderr, "Failed to read bottom PNG: %s\n", bottom_path);
        image_free(&top);
        return 1;
    }

    if (bottom.w == 0 || bottom.h == 0) {
        fprintf(stderr, "Invalid bottom image\n");
        image_free(&top);
        image_free(&bottom);
        return 1;
    }

    uint8_t *top_resized = resize_rgba_bilinear(top.rgba, (int)top.w, (int)top.h, (int)bottom.w, (int)bottom.h);
    if (!top_resized) {
        fprintf(stderr, "Out of memory\n");
        image_free(&top);
        image_free(&bottom);
        return 1;
    }

    composite_top_over_bottom(bottom.rgba, top_resized, bottom.w, bottom.h);
    free(top_resized);

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", bottom_path);
    if (save_png_rgba(tmp_path, bottom.rgba, bottom.w, bottom.h) != 0) {
        fprintf(stderr, "Failed to write output PNG\n");
        remove(tmp_path);
        image_free(&top);
        image_free(&bottom);
        return 1;
    }
    if (rename(tmp_path, bottom_path) != 0) {
        fprintf(stderr, "Failed to replace bottom file: %s\n", strerror(errno));
        remove(tmp_path);
        image_free(&top);
        image_free(&bottom);
        return 1;
    }

    image_free(&top);
    image_free(&bottom);
    return 0;
}
