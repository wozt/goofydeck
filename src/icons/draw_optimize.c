// Minimal PNG optimizer: quantize to <=256 colors and rewrite as indexed PNG with zlib compression.
// Usage: draw_optimize [-d] [-c N<=256|-c=N] <filename.png>
// Operates on the given path in place (if relative, it is resolved relative to the project root). No stdout on success.

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

#define MAX_SIZE 196
#define MAX_WIDE_W 442
#define MAX_WIDE_H 196
#define DEFAULT_COLORS 64

// CRC helpers
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
    unsigned char b[4] = { (v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff };
    fwrite(b,1,4,f);
}
static int write_chunk(FILE *f, const char *type, const unsigned char *data, size_t len) {
    write_be32(f, (uint32_t)len);
    fwrite(type,1,4,f);
    if (len>0) fwrite(data,1,len,f);
    size_t c_len = len + 4;
    unsigned char *buf = malloc(c_len);
    if (!buf) return -1;
    memcpy(buf, type, 4);
    if (len>0) memcpy(buf+4, data, len);
    uint32_t c = crc_calc(buf, c_len);
    free(buf);
    write_be32(f, c);
    return 0;
}

// PNG reader (expects 8-bit RGB/RGBA)
typedef struct {
    uint32_t width, height;
    unsigned char *pixels; // RGBA pixels, size width*height*4
} PngRaw;

static int read_be32u(const unsigned char *p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

static int png_unfilter(uint8_t *dest, const uint8_t *src, uint32_t w, uint32_t h, uint32_t bpp) {
    uint32_t stride = bpp * w;
    const uint8_t *prev = NULL;
    for (uint32_t y=0; y<h; y++) {
        const uint8_t *row = src + y*(1 + stride);
        uint8_t type = row[0];
        const uint8_t *dat = row + 1;
        uint8_t *out = dest + y*stride;
        switch (type) {
            case 0: // None
                memcpy(out, dat, stride);
                break;
            case 1: // Sub
                for (uint32_t x=0; x<stride; x++) {
                    uint8_t left = (x>=bpp) ? out[x-bpp] : 0;
                    out[x] = (uint8_t)(dat[x] + left);
                }
                break;
            case 2: // Up
                for (uint32_t x=0; x<stride; x++) {
                    uint8_t up = prev ? prev[x] : 0;
                    out[x] = (uint8_t)(dat[x] + up);
                }
                break;
            case 3: // Average
                for (uint32_t x=0; x<stride; x++) {
                    uint8_t left = (x>=bpp) ? out[x-bpp] : 0;
                    uint8_t up = prev ? prev[x] : 0;
                    out[x] = (uint8_t)(dat[x] + ((left + up) >> 1));
                }
                break;
            case 4: // Paeth
                for (uint32_t x=0; x<stride; x++) {
                    uint8_t left = (x>=bpp) ? out[x-bpp] : 0;
                    uint8_t up = prev ? prev[x] : 0;
                    uint8_t up_left = (prev && x>=bpp) ? prev[x-bpp] : 0;
                    int p = (int)left + (int)up - (int)up_left;
                    int pa = abs(p - (int)left);
                    int pb = abs(p - (int)up);
                    int pc = abs(p - (int)up_left);
                    uint8_t pr;
                    if (pa <= pb && pa <= pc) pr = left;
                    else if (pb <= pc) pr = up;
                    else pr = up_left;
                    out[x] = (uint8_t)(dat[x] + pr);
                }
                break;
            default:
                return -1;
        }
        prev = out;
    }
    return 0;
}

static int load_png_rgba(const char *path, PngRaw *out) {
    memset(out,0,sizeof(*out));
    FILE *f = fopen(path,"rb");
    if (!f) return -1;
    unsigned char sig[8];
    if (fread(sig,1,8,f)!=8 || memcmp(sig,"\x89PNG\r\n\x1a\n",8)!=0) { fclose(f); return -1; }
    uint32_t w=0,h=0; int ok=0;
    uint8_t color_type = 0;
    unsigned char *idat=NULL; size_t idat_len=0;
    while (1) {
        unsigned char lenb[4], type[4];
        if (fread(lenb,1,4,f)!=4) break;
        uint32_t len = read_be32u(lenb);
        if (fread(type,1,4,f)!=4) { free(idat); fclose(f); return -1; }
        unsigned char *buf = NULL;
        if (len>0) {
            buf = malloc(len);
            if (!buf || fread(buf,1,len,f)!=len) { free(buf); free(idat); fclose(f); return -1; }
        }
        fseek(f,4,SEEK_CUR); // skip CRC
        if (memcmp(type,"IHDR",4)==0) {
            w = read_be32u(buf); h = read_be32u(buf+4);
            color_type = buf[9];
            if (buf[8]!=8 || !(color_type==6 || color_type==2)) { free(buf); free(idat); fclose(f); return -1; } // 8-bit RGB/RGBA
            ok=1;
        } else if (memcmp(type,"IDAT",4)==0) {
            unsigned char *nb = realloc(idat, idat_len + len);
            if (!nb) { free(buf); free(idat); fclose(f); return -1; }
            memcpy(nb+idat_len, buf, len);
            idat = nb; idat_len += len;
        } else if (memcmp(type,"IEND",4)==0) {
            free(buf);
            break;
        }
        free(buf);
    }
    fclose(f);
    if (!ok || w==0 || h==0 || !idat) { free(idat); return -1; }
    uint32_t bpp = (color_type == 6) ? 4u : 3u;
    size_t scan_cap = (size_t)(1 + bpp*w) * h;
    unsigned char *scan = malloc(scan_cap);
    if (!scan) { free(idat); return -1; }
    z_stream zs; memset(&zs,0,sizeof(zs));
    if (inflateInit(&zs)!=Z_OK) { free(idat); free(scan); return -1; }
    zs.next_in = idat; zs.avail_in = idat_len;
    zs.next_out = scan; zs.avail_out = scan_cap;
    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    size_t scan_len = scan_cap - zs.avail_out;
    free(idat);
    if (ret != Z_STREAM_END || scan_len != scan_cap) { free(scan); return -1; }
    unsigned char *raw = malloc((size_t)w * h * bpp);
    if (!raw) { free(scan); return -1; }
    if (png_unfilter(raw, scan, w, h, bpp)!=0) { free(scan); free(raw); return -1; }
    free(scan);
    unsigned char *pixels = malloc((size_t)w * h * 4);
    if (!pixels) { free(raw); return -1; }
    if (bpp == 4) {
        memcpy(pixels, raw, (size_t)w * h * 4);
    } else {
        for (size_t i = 0, j = 0; i < (size_t)w * h; i++) {
            pixels[j++] = raw[i*3 + 0];
            pixels[j++] = raw[i*3 + 1];
            pixels[j++] = raw[i*3 + 2];
            pixels[j++] = 255;
        }
    }
    free(raw);
    out->width = w; out->height = h; out->pixels = pixels;
    return 0;
}

// Palette quantization (popularity + nearest mapping)
typedef struct { uint32_t key; uint32_t count; } HistEntry;
typedef struct { uint8_t r,g,b,a; uint32_t count; } ColorEntry;

// simple open addressing hash for histogram
typedef struct {
    uint32_t *keys;
    uint32_t *counts;
    size_t cap;
    size_t size;
} HistMap;

static const uint32_t KEY_EMPTY = 0xffffffffu;

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

static int hist_init(HistMap *m, size_t cap) {
    m->cap = cap;
    m->size = 0;
    m->keys = malloc(cap * sizeof(uint32_t));
    m->counts = calloc(cap, sizeof(uint32_t));
    if (!m->keys || !m->counts) return -1;
    for (size_t i=0;i<cap;i++) m->keys[i] = KEY_EMPTY;
    return 0;
}
static void hist_free(HistMap *m) {
    free(m->keys); free(m->counts);
    m->keys=NULL; m->counts=NULL; m->cap=m->size=0;
}
static void hist_inc(HistMap *m, uint32_t key) {
    size_t cap = m->cap;
    size_t idx = mix32(key) % cap;
    for (;;) {
        if (m->keys[idx]==KEY_EMPTY) {
            m->keys[idx]= key;
            m->counts[idx]=1;
            m->size++;
            return;
        }
        if (m->keys[idx]==key) { m->counts[idx]++; return; }
        idx = (idx+1)%cap;
    }
}

static size_t hist_to_array(const HistMap *m, ColorEntry **out) {
    ColorEntry *arr = malloc(m->size * sizeof(ColorEntry));
    if (!arr) return 0;
    size_t j=0;
    for (size_t i=0;i<m->cap;i++) {
        if (m->keys[i]!=KEY_EMPTY) {
            uint32_t key = m->keys[i];
            ColorEntry ce;
            ce.r = (key>>24)&0xff;
            ce.g = (key>>16)&0xff;
            ce.b = (key>>8)&0xff;
            ce.a = key&0xff;
            ce.count = m->counts[i];
            arr[j++] = ce;
        }
    }
    *out = arr;
    return j;
}

static int cmp_count_desc(const void *a, const void *b) {
    const ColorEntry *ca = (const ColorEntry*)a;
    const ColorEntry *cb = (const ColorEntry*)b;
    if (ca->count < cb->count) return 1;
    if (ca->count > cb->count) return -1;
    return 0;
}

static uint32_t pack_rgba(uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    return ((uint32_t)r<<24)|((uint32_t)g<<16)|((uint32_t)b<<8)|a;
}

static int nearest_palette(const ColorEntry *pal, int pal_sz, uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
    int best = 0;
    int best_d = 1<<30;
    for (int i=0;i<pal_sz;i++) {
        int dr = (int)pal[i].r - (int)r;
        int dg = (int)pal[i].g - (int)g;
        int db = (int)pal[i].b - (int)b;
        int da = (int)pal[i].a - (int)a;
        int d = dr*dr + dg*dg + db*db + da*da;
        if (d < best_d) { best_d = d; best = i; if (d==0) break; }
    }
    return best;
}

// PNG write indexed
static int save_png_indexed(const char *path, const uint8_t *idx, uint32_t w, uint32_t h,
                            const ColorEntry *pal, int pal_sz) {
    FILE *f = fopen(path,"wb");
    if (!f) return -1;
    const unsigned char sig[8]={137,80,78,71,13,10,26,10};
    fwrite(sig,1,8,f);
    unsigned char ihdr[13];
    ihdr[0]=(w>>24)&0xff; ihdr[1]=(w>>16)&0xff; ihdr[2]=(w>>8)&0xff; ihdr[3]=w&0xff;
    ihdr[4]=(h>>24)&0xff; ihdr[5]=(h>>16)&0xff; ihdr[6]=(h>>8)&0xff; ihdr[7]=h&0xff;
    int bit_depth = 8;
    if (pal_sz <= 2) bit_depth = 1;
    else if (pal_sz <= 4) bit_depth = 2;
    else if (pal_sz <= 16) bit_depth = 4;
    ihdr[8]=bit_depth; ihdr[9]=3; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    write_chunk(f,"IHDR",ihdr,13);

    unsigned char *plte = malloc(pal_sz*3);
    unsigned char *trns = malloc(pal_sz);
    if (!plte || !trns) { free(plte); free(trns); fclose(f); return -1; }
    for (int i=0;i<pal_sz;i++) {
        plte[3*i+0]=pal[i].r;
        plte[3*i+1]=pal[i].g;
        plte[3*i+2]=pal[i].b;
        trns[i]=pal[i].a;
    }
    int trns_len = pal_sz;
    while (trns_len>0 && trns[trns_len-1]==255) trns_len--;
    write_chunk(f,"PLTE",plte,pal_sz*3);
    if (trns_len>0) write_chunk(f,"tRNS",trns,trns_len);
    free(plte); free(trns);

    size_t row_bits = (size_t)w * bit_depth;
    size_t row_bytes = (row_bits + 7) / 8;
    size_t raw_len = (1 + row_bytes)*h;
    unsigned char *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (uint32_t y=0;y<h;y++) {
        size_t off = y*(1+row_bytes);
        raw[off]=0;
        const uint8_t *row = idx + y*w;
        if (bit_depth==8) {
            memcpy(raw+off+1, row, row_bytes);
        } else {
            unsigned char *dst = raw+off+1;
            size_t bit_pos = 0;
            unsigned char cur = 0;
            unsigned shift_step = (unsigned)bit_depth;
            for (uint32_t x=0;x<w;x++) {
                cur <<= shift_step;
                cur |= (row[x] & ((1u<<shift_step)-1));
                bit_pos += shift_step;
                if (bit_pos == 8) {
                    *dst++ = cur;
                    cur = 0;
                    bit_pos = 0;
                }
            }
            if (bit_pos>0) {
                cur <<= (8 - bit_pos);
                *dst++ = cur;
            }
        }
    }
    uLongf comp_bound = compressBound(raw_len);
    unsigned char *zbuf = malloc(comp_bound);
    if (!zbuf) { free(raw); fclose(f); return -1; }

    const char *size_mode = getenv("DRAW_OPT_SIZE"); // set to use best-of-three (slower, maybe smaller)
    if (size_mode && size_mode[0]!=0) {
        unsigned char *best = malloc(comp_bound);
        if (!best) { free(raw); free(zbuf); fclose(f); return -1; }
        size_t best_len = comp_bound+1;
        int strategies[] = { Z_DEFAULT_STRATEGY, Z_RLE, Z_FILTERED };
        for (size_t si=0; si<sizeof(strategies)/sizeof(strategies[0]); si++) {
            z_stream zs; memset(&zs,0,sizeof(zs));
            if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15, 8, strategies[si]) != Z_OK) continue;
            zs.next_in = raw;
            zs.avail_in = raw_len;
            zs.next_out = zbuf;
            zs.avail_out = comp_bound;
            int ret = deflate(&zs, Z_FINISH);
            size_t written = comp_bound - zs.avail_out;
            deflateEnd(&zs);
            if (ret == Z_STREAM_END && written < best_len) {
                best_len = written;
                memcpy(best, zbuf, written);
            }
        }
        free(raw);
        free(zbuf);
        if (best_len==comp_bound+1) { free(best); fclose(f); return -1; }
        write_chunk(f,"IDAT",best,best_len);
        free(best);
    } else {
        z_stream zs; memset(&zs,0,sizeof(zs));
        if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) { free(raw); free(zbuf); fclose(f); return -1; }
        zs.next_in = raw;
        zs.avail_in = raw_len;
        zs.next_out = zbuf;
        zs.avail_out = comp_bound;
        int ret = deflate(&zs, Z_FINISH);
        size_t written = comp_bound - zs.avail_out;
        deflateEnd(&zs);
        free(raw);
        if (ret != Z_STREAM_END) { free(zbuf); fclose(f); return -1; }
        write_chunk(f,"IDAT",zbuf,written);
        free(zbuf);
    }
    write_chunk(f,"IEND",NULL,0);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    int color_limit = DEFAULT_COLORS;
    const char *fname = NULL;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dither") == 0) {
            // Dithering is handled by draw_normalize in this project.
            // Keep this flag for compatibility with scripts.
            continue;
        } else if ((strcmp(argv[i],"-c")==0 || strcmp(argv[i],"--color")==0) && i+1<argc) {
            color_limit = atoi(argv[++i]);
        } else if (strncmp(argv[i], "-c=", 3) == 0) {
            color_limit = atoi(argv[i] + 3);
        } else if (strncmp(argv[i],"--color=",8)==0) {
            color_limit = atoi(argv[i]+8);
        } else {
            fname = argv[i];
        }
    }
    if (!fname) {
        return 1;
    }
    if (color_limit < 1) color_limit = 1;
    if (color_limit > 256) color_limit = 256;
    if (!strstr(fname,".png")) return 1;
    char path[PATH_MAX];
    if (fname[0] == '/') {
        fd_snprintf_checked(path, sizeof(path), "target(abs)", "%s", fname);
    } else {
        char root[PATH_MAX];
        if (fd_find_project_root(root, sizeof(root)) != 0) {
            fprintf(stderr, "Could not locate project root (set PROJECT_ROOT)\n");
            return 1;
        }
        if (fd_resolve_root_relative(root, fname, path, sizeof(path)) != 0) return 1;
    }
    PngRaw png;
    if (load_png_rgba(path,&png)!=0) return 1;
    if (png.width < 1 || png.height < 1) { free(png.pixels); return 1; }
    // Classic icons: square up to 196x196
    // Button 14 (wide tile): allow rectangles up to 442x196
    if (!((png.width == png.height && png.width <= MAX_SIZE) ||
          (png.width <= MAX_WIDE_W && png.height <= MAX_WIDE_H))) {
        free(png.pixels);
        return 1;
    }

    size_t pixels = (size_t)png.width * png.height;
    int seen_white = 0;
    HistMap hist;
    size_t cap = 1;
    while (cap < pixels*2) cap <<=1;
    if (cap < 1024) cap = 1024;
    if (hist_init(&hist, cap)!=0) { free(png.pixels); return 1; }

    // Build histogram
    for (uint32_t y=0;y<png.height;y++) {
        const unsigned char *px = png.pixels + y*(4*png.width);
        for (uint32_t x=0;x<png.width;x++) {
            uint8_t r=px[0],g=px[1],b=px[2],a=px[3];
            px +=4;
            // // Important: for fully transparent pixels, discard RGB to avoid "color bleed" in
            // // indexed PNG palettes (tRNS). This preserves transparency while making the
            // // transparent color stable (RGB=0).
            // if (a == 0) { r = 0; g = 0; b = 0; }
            uint32_t key = pack_rgba(r,g,b,a);
            hist_inc(&hist, key);
            if (!seen_white && r==255 && g==255 && b==255 && a==255) seen_white = 1;
        }
    }

    ColorEntry *colors=NULL;
    size_t color_count = hist_to_array(&hist, &colors);
    hist_free(&hist);
    if (!colors || color_count==0) { free(colors); free(png.pixels); return 1; }
    qsort(colors, color_count, sizeof(ColorEntry), cmp_count_desc);
    int pal_sz = (color_count < (size_t)color_limit) ? (int)color_count : color_limit;

    // Palette
    ColorEntry *palette = malloc(pal_sz * sizeof(ColorEntry));
    if (!palette) { free(colors); free(png.pixels); return 1; }
    for (int i=0;i<pal_sz;i++) {
        palette[i]=colors[i];
        palette[i].a = (palette[i].a==0) ? 0 : 255; // normalize alpha to 0/255
    }
    // Ensure pure white remains if present in the source
    if (seen_white) {
        int has_white = 0;
        for (int i=0;i<pal_sz;i++) {
            if (palette[i].r==255 && palette[i].g==255 && palette[i].b==255 && palette[i].a==255) {
                has_white = 1; break;
            }
        }
        if (!has_white) {
            palette[pal_sz-1].r = 255;
            palette[pal_sz-1].g = 255;
            palette[pal_sz-1].b = 255;
            palette[pal_sz-1].a = 255;
            palette[pal_sz-1].count = 1;
        }
    }

    uint8_t *idxbuf = malloc(pixels);
    if (!idxbuf) { free(colors); free(palette); free(png.pixels); return 1; }
    size_t pos=0;
    for (uint32_t y=0;y<png.height;y++) {
        const unsigned char *px = png.pixels + y*(4*png.width);
        for (uint32_t x=0;x<png.width;x++) {
            uint8_t r=px[0],g=px[1],b=px[2],a=px[3];
            px +=4;
            // if (a == 0) { r = 0; g = 0; b = 0; }
            int idx = nearest_palette(palette, pal_sz, r,g,b,a);
            idxbuf[pos++] = (uint8_t)idx;
        }
    }

    int res = save_png_indexed(path, idxbuf, png.width, png.height, palette, pal_sz);
    free(idxbuf);
    free(palette);
    free(colors);
    free(png.pixels);
    return res==0 ? 0 : 1;
}
