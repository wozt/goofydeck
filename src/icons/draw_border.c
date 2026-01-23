// Minimal PNG overlay: draw a filled rounded square onto an existing RGBA PNG.
// Expects 8-bit RGBA, non-interlaced. Uses zlib for compression.
// Usage: draw_border <hexcolor> [--size=N<=196] [--radius=R<=50] <filename.png>
// Reads/writes the given path in place (if relative, it is resolved relative to the project root). No external libs.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <zlib.h>

#include "fd_path.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// CRC
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

static int hexbyte(char h, char l) {
    int v=0;
    if (h>='0'&&h<='9') v=(h-'0')<<4;
    else if (h>='A'&&h<='F') v=(h-'A'+10)<<4;
    else if (h>='a'&&h<='f') v=(h-'a'+10)<<4;
    else return -1;
    if (l>='0'&&l<='9') v|=(l-'0');
    else if (l>='A'&&l<='F') v|=(l-'A'+10);
    else if (l>='a'&&l<='f') v|=(l-'a'+10);
    else return -1;
    return v;
}

static int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b, int *is_transparent) {
    // Vérifier si c'est "transparent"
    if (strcasecmp(s, "transparent") == 0) {
        *is_transparent = 1;
        return 0;
    }
    
    // Sinon parser comme couleur hex
    *is_transparent = 0;
    if (strlen(s)!=6) return -1;
    int r8=hexbyte(s[0],s[1]);
    int g8=hexbyte(s[2],s[3]);
    int b8=hexbyte(s[4],s[5]);
    if (r8<0||g8<0||b8<0) return -1;
    *r=(uint8_t)r8; *g=(uint8_t)g8; *b=(uint8_t)b8;
    return 0;
}

// Minimal PNG reader
typedef struct {
    uint32_t width, height;
    unsigned char *data; // raw scanlines with filter byte per row (as stored)
    size_t data_len;
} PngRaw;

static int read_be32(const unsigned char *p) {
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

static int load_png_raw(const char *path, PngRaw *out) {
    memset(out,0,sizeof(*out));
    FILE *f=fopen(path,"rb");
    if (!f) { perror("open input"); return -1; }
    unsigned char sig[8];
    if (fread(sig,1,8,f)!=8 || memcmp(sig,"\x89PNG\r\n\x1a\n",8)!=0) {
        fclose(f); fprintf(stderr,"Not a PNG or bad signature\n"); return -1;
    }
    uint32_t width=0,height=0;
    unsigned char *idat=NULL; size_t idat_len=0;
    while (1) {
        unsigned char lenb[4], type[4];
        if (fread(lenb,1,4,f)!=4) break;
        uint32_t len = read_be32(lenb);
        if (fread(type,1,4,f)!=4) { free(idat); fclose(f); return -1; }
        unsigned char *buf = NULL;
        if (len>0) {
            buf = malloc(len);
            if (!buf || fread(buf,1,len,f)!=len) { free(buf); free(idat); fclose(f); return -1; }
        }
        fseek(f,4,SEEK_CUR); // skip CRC
        if (memcmp(type,"IHDR",4)==0) {
            width = read_be32(buf);
            height = read_be32(buf+4);
            if (buf[8]!=8 || buf[9]!=6) { free(buf); free(idat); fclose(f); fprintf(stderr,"Unsupported PNG format\n"); return -1; }
        } else if (memcmp(type,"IDAT",4)==0) {
            unsigned char *newbuf = realloc(idat, idat_len + len);
            if (!newbuf) { free(buf); free(idat); fclose(f); return -1; }
            memcpy(newbuf+idat_len, buf, len);
            idat = newbuf; idat_len += len;
        } else if (memcmp(type,"IEND",4)==0) {
            free(buf);
            break;
        }
        free(buf);
    }
    fclose(f);
    if (width==0||height==0||!idat) { free(idat); return -1; }

    // Inflate IDAT
    size_t raw_cap = (1 + 4*width)*height;
    unsigned char *raw = malloc(raw_cap);
    if (!raw) { free(idat); return -1; }
    z_stream strm;
    memset(&strm,0,sizeof(strm));
    int ret = inflateInit(&strm);
    if (ret != Z_OK) { free(idat); free(raw); return -1; }
    strm.next_in = idat;
    strm.avail_in = idat_len;
    strm.next_out = raw;
    strm.avail_out = raw_cap;
    ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    size_t raw_len = raw_cap - strm.avail_out;
    free(idat);
    if (ret != Z_STREAM_END) { free(raw); return -1; }

    out->width = width;
    out->height = height;
    out->data = raw;
    out->data_len = raw_len;
    return 0;
}

static int save_png_raw(const char *path, const unsigned char *raw, uint32_t width, uint32_t height) {
    FILE *f = fopen(path,"wb");
    if (!f) { perror("open output"); return -1; }
    const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    fwrite(sig,1,8,f);
    unsigned char ihdr[13];
    ihdr[0]=(width>>24)&0xff; ihdr[1]=(width>>16)&0xff; ihdr[2]=(width>>8)&0xff; ihdr[3]=width&0xff;
    ihdr[4]=(height>>24)&0xff; ihdr[5]=(height>>16)&0xff; ihdr[6]=(height>>8)&0xff; ihdr[7]=height&0xff;
    ihdr[8]=8; ihdr[9]=6; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    write_chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (1 + 4*width)*height;
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

static void blend_overlay(unsigned char *raw, uint32_t w_u, uint32_t h_u, uint32_t size_u, uint32_t radius_u, uint8_t r, uint8_t g, uint8_t b, int is_transparent) {
    int w = (int)w_u;
    int h = (int)h_u;
    int size = (int)size_u;
    int radius = (int)radius_u;

    int rad_px = (size * radius) / 100;
    int start_x = (w - size) / 2;
    int start_y = (h - size) / 2;
    int rad2 = rad_px * rad_px;
    int inner_w = size - 2 * rad_px;
    int inner_h = size - 2 * rad_px;
    size_t stride = 1 + 4 * (size_t)w;
    for (int y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            if (x < start_x || x >= start_x + size || y < start_y || y >= start_y + size) continue;
            int lx = x - start_x;
            int ly = y - start_y;
            int inside = 0;
            if (lx >= rad_px && lx < rad_px + inner_w && ly >= rad_px && ly < rad_px + inner_h) {
                inside = 1;
            } else {
                int cx, cy;
                if (lx < rad_px) cx = rad_px;
                else if (lx >= rad_px + inner_w) cx = rad_px + inner_w - 1;
                else cx = lx;
                if (ly < rad_px) cy = rad_px;
                else if (ly >= rad_px + inner_h) cy = rad_px + inner_h - 1;
                else cy = ly;
                int dx = lx - cx;
                int dy = ly - cy;
                if (dx*dx + dy*dy <= rad2) inside = 1;
            }
            if (!inside) continue;
            size_t p = 1 + (size_t)x * 4;
            uint8_t *px = row + p;
            
            // Si transparent, rendre les pixels complètement transparents (effacer la zone)
            if (is_transparent) {
                px[0] = px[1] = px[2] = 0;  // RGB = 0 (peu importe)
                px[3] = 0;                    // Alpha = 0 (totalement transparent)
                continue;
            }
            
            // Sinon faire le blending normal
            uint8_t dst_a = px[3];
            uint8_t src_a = 255;
            // alpha blend
            uint8_t out_a = src_a + dst_a - (src_a*dst_a)/255;
            if (out_a==0) { px[0]=px[1]=px[2]=px[3]=0; continue; }
            px[0] = (uint8_t)((r*src_a + px[0]*dst_a*(255-src_a)/255)/out_a);
            px[1] = (uint8_t)((g*src_a + px[1]*dst_a*(255-src_a)/255)/out_a);
            px[2] = (uint8_t)((b*src_a + px[2]*dst_a*(255-src_a)/255)/out_a);
            px[3] = out_a;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,"Usage: %s <hexcolor|transparent> [--size=N<=196] [--radius=R<=50] <filename.png>\n", argv[0]);
        return 1;
    }
    const char *color_str = argv[1];
    int size = 196;
    int radius = 0;
    const char *fname = NULL;
    for (int i=2;i<argc;i++) {
        if (strncmp(argv[i],"--size=",7)==0) size = atoi(argv[i]+7);
        else if (strncmp(argv[i],"--radius=",9)==0) radius = atoi(argv[i]+9);
        else fname = argv[i];
    }
    if (!fname) { fprintf(stderr,"Filename required.\n"); return 1; }
    if (size < 1) size = 1;
    if (size > 196) size = 196;
    if (radius < 0) radius = 0;
    if (radius > 50) radius = 50;
    uint8_t r,g,b;
    int is_transparent = 0;
    if (parse_color(color_str,&r,&g,&b,&is_transparent)!=0) { 
        fprintf(stderr,"Invalid color %s (expected 6-digit hex or 'transparent')\n", color_str); 
        return 1; 
    }

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
    if (load_png_raw(path,&png)!=0) { fprintf(stderr,"Failed to read %s (ensure it was generated by draw_square)\n", path); return 1; }
    if (png.width!=png.height || png.width<1 || png.width>196) { free(png.data); fprintf(stderr,"Unsupported dimensions\n"); return 1; }
    blend_overlay(png.data, png.width, png.height, (uint32_t)size, (uint32_t)radius, r,g,b,is_transparent);
    if (save_png_raw(path, png.data, png.width, png.height)!=0) { free(png.data); fprintf(stderr,"Failed to write output\n"); return 1; }
    free(png.data);
    printf("Updated %s\n", path);
    return 0;
}
