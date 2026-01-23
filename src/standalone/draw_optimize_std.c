// Minimal PNG optimizer (standalone): quantize to <=256 colors and rewrite as indexed PNG with zlib compression.
// Usage: draw_optimize_std [-c N<=256] <path/to/file.png|directory>
// Writes to <file>_opt.png alongside the input. No stdout on success.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

#define DEFAULT_COLORS 64
#define PATH_BUF 2048

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

// PNG reader (expects 8-bit RGBA)
typedef struct {
    uint32_t width, height;
    unsigned char *pixels; // RGBA pixels, size width*height*4
} PngRaw;

static int read_be32u(const unsigned char *p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

static int png_unfilter(uint8_t *dest, const uint8_t *src, uint32_t h, uint32_t bpp_bytes, uint32_t stride_bytes) {
    const uint8_t *prev = NULL;
    for (uint32_t y=0; y<h; y++) {
        const uint8_t *row = src + y*(1 + stride_bytes);
        uint8_t type = row[0];
        const uint8_t *dat = row + 1;
        uint8_t *out = dest + y*stride_bytes;
        switch (type) {
            case 0: // None
                memcpy(out, dat, stride_bytes);
                break;
            case 1: // Sub
                for (uint32_t x=0; x<stride_bytes; x++) {
                    uint8_t left = (x>=bpp_bytes) ? out[x-bpp_bytes] : 0;
                    out[x] = (uint8_t)(dat[x] + left);
                }
                break;
            case 2: // Up
                for (uint32_t x=0; x<stride_bytes; x++) {
                    uint8_t up = prev ? prev[x] : 0;
                    out[x] = (uint8_t)(dat[x] + up);
                }
                break;
            case 3: // Average
                for (uint32_t x=0; x<stride_bytes; x++) {
                    uint8_t left = (x>=bpp_bytes) ? out[x-bpp_bytes] : 0;
                    uint8_t up = prev ? prev[x] : 0;
                    out[x] = (uint8_t)(dat[x] + ((left + up) >> 1));
                }
                break;
            case 4: // Paeth
                for (uint32_t x=0; x<stride_bytes; x++) {
                    uint8_t left = (x>=bpp_bytes) ? out[x-bpp_bytes] : 0;
                    uint8_t up = prev ? prev[x] : 0;
                    uint8_t up_left = (prev && x>=bpp_bytes) ? prev[x-bpp_bytes] : 0;
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
    uint32_t w=0,h=0; int ok=0; uint8_t color_type=0, bit_depth=0;
    unsigned char palette[256][4]; int palette_size=0;
    for (int i=0;i<256;i++){ palette[i][0]=0; palette[i][1]=0; palette[i][2]=0; palette[i][3]=255; }
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
            bit_depth = buf[8];
            color_type = buf[9];
            if (bit_depth!=8) { free(buf); free(idat); fclose(f); return -1; } // require 8-bit depth
            if (!(color_type==6 || color_type==2 || color_type==3)) { free(buf); free(idat); fclose(f); return -1; } // allow RGBA/RGB/palette
            ok=1;
        } else if (memcmp(type,"PLTE",4)==0) {
            if (len % 3 != 0 || len/3 > 256) { free(buf); free(idat); fclose(f); return -1; }
            palette_size = (int)(len/3);
            for (int i=0;i<palette_size;i++) {
                palette[i][0]=buf[3*i+0];
                palette[i][1]=buf[3*i+1];
                palette[i][2]=buf[3*i+2];
                palette[i][3]=255;
            }
        } else if (memcmp(type,"tRNS",4)==0) {
            int entries = (int)len;
            if (entries > 256) entries = 256;
            for (int i=0;i<entries;i++) palette[i][3] = buf[i];
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
    if (color_type==3 && palette_size==0) { free(idat); return -1; }
    uint32_t bpp_bytes = (color_type==6) ? 4 : (color_type==2 ? 3 : 1);
    // stride in bytes per row (for indexed with bit_depth<8, this handles packing)
    uint32_t stride_bytes = (color_type==3 && bit_depth < 8)
        ? (uint32_t)((w * bit_depth + 7) / 8)
        : bpp_bytes * w;
    size_t scan_cap = (size_t)(1 + stride_bytes) * h;
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
    size_t rgba_sz = (size_t)w * h * 4;
    unsigned char *raw = malloc((size_t)stride_bytes * h);
    if (!raw) { free(scan); return -1; }
    if (png_unfilter(raw, scan, h, bpp_bytes, stride_bytes)!=0) { free(scan); free(raw); return -1; }
    free(scan);

    unsigned char *pixels = malloc(rgba_sz);
    if (!pixels) { free(raw); return -1; }
    if (color_type==6) { // RGBA
        memcpy(pixels, raw, rgba_sz);
    } else if (color_type==2) { // RGB -> RGBA
        size_t pos = 0;
        size_t outp = 0;
        while (pos < (size_t)w * h * 3) {
            pixels[outp++] = raw[pos++];
            pixels[outp++] = raw[pos++];
            pixels[outp++] = raw[pos++];
            pixels[outp++] = 255;
        }
    } else { // palette -> RGBA, unpack bits if needed
        size_t outp = 0;
        if (bit_depth==8) {
            for (size_t i=0;i<(size_t)w*h;i++) {
                unsigned char idx = raw[i];
                if (idx >= (unsigned char)palette_size) {
                    pixels[outp++] = 0; pixels[outp++] = 0; pixels[outp++] = 0; pixels[outp++] = 0;
                } else {
                    pixels[outp++] = palette[idx][0];
                    pixels[outp++] = palette[idx][1];
                    pixels[outp++] = palette[idx][2];
                    pixels[outp++] = palette[idx][3];
                }
            }
        } else { // bit_depth 1/2/4
            size_t total = (size_t)w * h;
            int bits = bit_depth;
            for (size_t i=0;i<total;i++) {
                size_t byte_idx = (i * bits) / 8;
                int shift = 8 - bits - (int)((i * bits) % 8);
                unsigned char idx = (raw[byte_idx] >> shift) & ((1<<bits)-1);
                if (idx >= (unsigned char)palette_size) {
                    pixels[outp++] = 0; pixels[outp++] = 0; pixels[outp++] = 0; pixels[outp++] = 0;
                } else {
                    pixels[outp++] = palette[idx][0];
                    pixels[outp++] = palette[idx][1];
                    pixels[outp++] = palette[idx][2];
                    pixels[outp++] = palette[idx][3];
                }
            }
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
    m->keys=NULL; m->counts=NULL; m->cap=0; m->size=0;
}
static int hist_inc(HistMap *m, uint32_t key) {
    size_t mask = m->cap - 1;
    size_t idx = mix32(key) & mask;
    while (1) {
        uint32_t cur = m->keys[idx];
        if (cur==KEY_EMPTY) {
            m->keys[idx] = key;
            m->counts[idx] = 1;
            m->size++;
            return 0;
        } else if (cur==key) {
            m->counts[idx]++;
            return 0;
        }
        idx = (idx + 1) & mask;
    }
    return -1;
}

static uint32_t pack_rgba(uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
    return ((uint32_t)r<<24)|((uint32_t)g<<16)|((uint32_t)b<<8)|a;
}

static size_t hist_to_array(HistMap *m, ColorEntry **out) {
    *out = NULL;
    if (m->size==0) return 0;
    ColorEntry *arr = malloc(m->size * sizeof(ColorEntry));
    if (!arr) return 0;
    size_t pos=0;
    for (size_t i=0;i<m->cap;i++) {
        uint32_t key = m->keys[i];
        if (key!=KEY_EMPTY) {
            arr[pos].r = (key>>24)&0xff;
            arr[pos].g = (key>>16)&0xff;
            arr[pos].b = (key>>8)&0xff;
            arr[pos].a = key&0xff;
            arr[pos].count = m->counts[i];
            pos++;
        }
    }
    return pos;
}

static int cmp_count_desc(const void *a, const void *b) {
    const ColorEntry *x=a, *y=b;
    if (x->count < y->count) return 1;
    if (x->count > y->count) return -1;
    return 0;
}

static int nearest_palette(const ColorEntry *pal, int pal_sz, uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
    int best = 0;
    uint32_t best_dist = 0xffffffffu;
    for (int i=0;i<pal_sz;i++) {
        int dr = (int)pal[i].r - (int)r;
        int dg = (int)pal[i].g - (int)g;
        int db = (int)pal[i].b - (int)b;
        int da = (int)pal[i].a - (int)a;
        uint32_t dist = (uint32_t)(dr*dr + dg*dg + db*db + da*da);
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    return best;
}

// PNG write (indexed)
static int save_png_indexed(const char *path, const uint8_t *idx, uint32_t w, uint32_t h, const ColorEntry *pal, int pal_sz) {
    FILE *f = fopen(path,"wb");
    if (!f) return -1;
    const unsigned char sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    fwrite(sig,1,8,f);

    // IHDR
    unsigned char ihdr[13];
    ihdr[0]=(w>>24)&0xff; ihdr[1]=(w>>16)&0xff; ihdr[2]=(w>>8)&0xff; ihdr[3]=w&0xff;
    ihdr[4]=(h>>24)&0xff; ihdr[5]=(h>>16)&0xff; ihdr[6]=(h>>8)&0xff; ihdr[7]=h&0xff;
    ihdr[8]=3; // indexed
    ihdr[9]=0; // compression
    ihdr[10]=0; // filter
    ihdr[11]=0; // interlace
    ihdr[12]=0;
    write_chunk(f,"IHDR",ihdr,13);

    // PLTE
    unsigned char *plte = malloc(3 * pal_sz);
    if (!plte) { fclose(f); return -1; }
    for (int i=0;i<pal_sz;i++) {
        plte[3*i+0]=pal[i].r;
        plte[3*i+1]=pal[i].g;
        plte[3*i+2]=pal[i].b;
    }
    write_chunk(f,"PLTE", plte, 3*pal_sz);
    free(plte);

    // tRNS
    unsigned char *trns = malloc(pal_sz);
    if (!trns) { fclose(f); return -1; }
    for (int i=0;i<pal_sz;i++) trns[i] = pal[i].a;
    write_chunk(f,"tRNS", trns, pal_sz);
    free(trns);

    // IDAT
    size_t stride = w;
    size_t scan_len = (stride + 1) * h;
    unsigned char *scan = malloc(scan_len);
    if (!scan) { fclose(f); return -1; }
    size_t pos=0;
    for (uint32_t y=0;y<h;y++) {
        scan[pos++] = 0; // no filter
        memcpy(scan+pos, idx + y*stride, stride);
        pos += stride;
    }
    uLongf comp_bound = compressBound(scan_len);
    unsigned char *comp = malloc(comp_bound);
    if (!comp) { free(scan); fclose(f); return -1; }
    uLongf comp_len = comp_bound;
    if (compress2(comp, &comp_len, scan, scan_len, Z_BEST_COMPRESSION)!=Z_OK) {
        free(scan); free(comp); fclose(f); return -1;
    }
    write_chunk(f,"IDAT", comp, comp_len);
    free(scan); free(comp);

    // IEND
    write_chunk(f,"IEND", NULL, 0);
    fclose(f);
    return 0;
}

static int ends_with_png(const char *fname) {
    const char *dot = strrchr(fname, '.');
    return (dot && (strcasecmp(dot, ".png")==0));
}

static int fallback_shell(const char *fname, int color_limit) {
    const char *helper = "standalone/draw_optimize_std.sh";
    if (access(helper, X_OK)!=0) return 1;
    char cmd[PATH_BUF + 128];
    int n = snprintf(cmd, sizeof(cmd), "%s -c %d \"%s\"", helper, color_limit, fname);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return 1;
    int rc = system(cmd);
    return (rc==0) ? 0 : 1;
}
static int optimize_file(const char *fname, int color_limit);
static int process_target(const char *target, int color_limit);
static int process_dir(const char *dirpath, int color_limit) {
    DIR *d = opendir(dirpath);
    if (!d) {
        fprintf(stderr, "Cannot open dir: %s\n", dirpath);
        return 1;
    }
    struct dirent *ent;
    int failures = 0;
    char path[PATH_BUF];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name,".")==0 || strcmp(ent->d_name,"..")==0) continue;
        size_t need = strlen(dirpath)+1+strlen(ent->d_name)+1;
        if (need >= PATH_BUF) { fprintf(stderr, "Skip (path too long): %s/%s\n", dirpath, ent->d_name); failures++; continue; }
        size_t base_len = strlen(dirpath);
        size_t name_len = strlen(ent->d_name);
        memcpy(path, dirpath, base_len);
        path[base_len] = '/';
        memcpy(path + base_len + 1, ent->d_name, name_len);
        path[base_len + 1 + name_len] = '\0';
        if (process_target(path, color_limit)!=0) failures++;
    }
    closedir(d);
    return failures==0 ? 0 : 1;
}

static int process_target(const char *target, int color_limit) {
    struct stat st;
    if (stat(target, &st)!=0) {
        fprintf(stderr, "Not found: %s\n", target);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        return process_dir(target, color_limit);
    } else if (S_ISREG(st.st_mode)) {
        return optimize_file(target, color_limit);
    }
    fprintf(stderr, "Skip (not file/dir): %s\n", target);
    return 1;
}

static int optimize_file(const char *fname, int color_limit) {
    if (!ends_with_png(fname)) {
        fprintf(stderr, "Skip (not png): %s\n", fname);
        return 0;
    }
    size_t len = strlen(fname);
    if (len + 4 + 1 >= PATH_BUF) {
        fprintf(stderr, "Skip (path too long): %s\n", fname);
        return 1;
    }
    const char *dot = strrchr(fname, '.');
    if (!dot) {
        fprintf(stderr, "Skip (no extension): %s\n", fname);
        return 1;
    }
    size_t stem_len = (size_t)(dot - fname);
    if (stem_len + 4 + strlen(dot) + 1 >= PATH_BUF) {
        fprintf(stderr, "Skip (out path too long): %s\n", fname);
        return 1;
    }

    char in_path[PATH_BUF];
    char out_path[PATH_BUF];
    snprintf(in_path, sizeof(in_path), "%s", fname);
    memcpy(out_path, fname, stem_len);
    memcpy(out_path + stem_len, "_opt", 4);
    strcpy(out_path + stem_len + 4, dot);

    PngRaw png;
    if (load_png_rgba(in_path,&png)!=0) {
        if (fallback_shell(in_path, color_limit)==0) return 0;
        fprintf(stderr, "Failed to load PNG: %s\n", in_path);
        return 1;
    }

    size_t pixels = (size_t)png.width * png.height;
    int seen_white = 0;
    HistMap hist;
    size_t cap = 1;
    while (cap < pixels*2) cap <<=1;
    if (cap < 1024) cap = 1024;
    if (hist_init(&hist, cap)!=0) { free(png.pixels); return 1; }

    for (uint32_t y=0;y<png.height;y++) {
        const unsigned char *px = png.pixels + y*(4*png.width);
        for (uint32_t x=0;x<png.width;x++) {
            uint8_t r=px[0],g=px[1],b=px[2],a=px[3];
            px +=4;
            uint32_t key = pack_rgba(r,g,b,a);
            hist_inc(&hist, key);
            if (!seen_white && r==255 && g==255 && b==255 && a==255) seen_white = 1;
        }
    }

    ColorEntry *colors=NULL;
    size_t color_count = hist_to_array(&hist, &colors);
    hist_free(&hist);
    if (!colors || color_count==0) {
        free(colors); free(png.pixels);
        if (fallback_shell(in_path, color_limit)==0) return 0;
        fprintf(stderr, "Empty palette: %s\n", in_path);
        return 1;
    }
    qsort(colors, color_count, sizeof(ColorEntry), cmp_count_desc);
    int pal_sz = (color_count < (size_t)color_limit) ? (int)color_count : color_limit;

    ColorEntry *palette = malloc(pal_sz * sizeof(ColorEntry));
    if (!palette) { free(colors); free(png.pixels); return 1; }
    for (int i=0;i<pal_sz;i++) {
        palette[i]=colors[i];
        palette[i].a = (palette[i].a==0) ? 0 : 255;
    }
    if (seen_white) {
        int has_white = 0;
        for (int i=0;i<pal_sz;i++) {
            if (palette[i].r==255 && palette[i].g==255 && palette[i].b==255 && palette[i].a==255) {
                has_white = 1; break;
            }
        }
        if (!has_white && pal_sz>0) {
            palette[pal_sz-1].r = 255;
            palette[pal_sz-1].g = 255;
            palette[pal_sz-1].b = 255;
            palette[pal_sz-1].a = 255;
            palette[pal_sz-1].count = 1;
        }
    }

    uint8_t *idxbuf = malloc(pixels);
    if (!idxbuf) { free(colors); free(palette); free(png.pixels); fprintf(stderr, "OOM idxbuf: %s\n", in_path); return 1; }
    size_t pos=0;
    for (uint32_t y=0;y<png.height;y++) {
        const unsigned char *px = png.pixels + y*(4*png.width);
        for (uint32_t x=0;x<png.width;x++) {
            uint8_t r=px[0],g=px[1],b=px[2],a=px[3];
            px +=4;
            int idx = nearest_palette(palette, pal_sz, r,g,b,a);
            idxbuf[pos++] = (uint8_t)idx;
        }
    }

    int res = save_png_indexed(out_path, idxbuf, png.width, png.height, palette, pal_sz);
    free(idxbuf);
    free(palette);
    free(colors);
    free(png.pixels);
    if (res==0) {
        fprintf(stdout, "Optimized %s -> %s\n", in_path, out_path);
        return 0;
    }
    fprintf(stderr, "Failed to save: %s\n", out_path);
    return 1;
}

int main(int argc, char **argv) {
    int color_limit = DEFAULT_COLORS;
    const char *target = NULL;
    for (int i=1;i<argc;i++) {
        if ((strcmp(argv[i],"-c")==0 || strcmp(argv[i],"--color")==0) && i+1<argc) {
            color_limit = atoi(argv[++i]);
        } else if (strncmp(argv[i],"--color=",8)==0) {
            color_limit = atoi(argv[i]+8);
        } else {
            target = argv[i];
        }
    }
    if (!target) return 1;
    if (color_limit < 1) color_limit = 1;
    if (color_limit > 256) color_limit = 256;

    return process_target(target, color_limit);
}
