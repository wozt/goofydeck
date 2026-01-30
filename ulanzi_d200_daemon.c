// Ulanzi D200 device manager daemon
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <hidapi/hidapi.h>
#include <zlib.h>
#include <inttypes.h>
#include <png.h>
#include <setjmp.h>

// Silence intentional unused warnings for static helpers kept for future refactors.
#if defined(__GNUC__) || defined(__clang__)
#define FD_UNUSED __attribute__((unused))
#else
#define FD_UNUSED
#endif

#define VID 0x2207
#define PID 0x0019
#define PACKET_SIZE 1024
#define HEADER0 0x7c
#define HEADER1 0x7c
#define SOCK_PATH "/tmp/ulanzi_device.sock"

static int running = 1;
// static const int MAX_PADDING_RETRIES = 4096; // max bytes to pad before force-patch
static const int MAX_PADDING_RETRIES = 1024; // max bytes to pad before force-patch in fast mode
static uint64_t TOTAL_BYTES_PATCHED = 0;
static const int KEEPALIVE_INTERVAL = 24;  // seconds
static uint64_t TOTAL_BYTES_SENT = 0;
static int g_debug = 0;
static int g_fast_nopad = 0;
// 0 = short fixed-width status line, 1 = legacy verbose format
static int g_sendzip_log_legacy = 0;

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

static hid_device *open_device(void) {
    return hid_open(VID, PID, NULL);
}

static int write_packet(hid_device *dev, const uint8_t *packet, size_t len) {
    if (!dev) return -1;
    uint8_t buf_with_report[PACKET_SIZE + 1];
    buf_with_report[0] = 0x00;
    memcpy(buf_with_report + 1, packet, len);
    int res = hid_write(dev, buf_with_report, len + 1);
    if (res < 0) {
        res = hid_write(dev, packet, len);
    }
    return res;
}

// Function declarations for in-memory PNG writing
static void png_write_data_to_memory(png_structp png_ptr, png_bytep data, png_size_t length);
static void png_flush_data_to_memory(png_structp png_ptr);
static int rgba_to_png_memory(const uint8_t *rgba, int w, int h, uint8_t **png_data, size_t *png_len);
static FD_UNUSED int write_png_rgba(const char *path, const uint8_t *data, int w, int h);

// Implementation of write_png_rgba
static int write_png_rgba(const char *path, const uint8_t *data, int w, int h) {
    FILE *fp=fopen(path,"wb"); if(!fp) return -1;
    png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    if(!png){fclose(fp);return -1;}
    png_infop info=png_create_info_struct(png);
    if(!info){png_destroy_write_struct(&png,NULL);fclose(fp);return -1;}
    if(setjmp(png_jmpbuf(png))){png_destroy_write_struct(&png,&info);fclose(fp);return -1;}
    png_init_io(png,fp);
    png_set_compression_level(png, Z_BEST_SPEED);
    png_set_filter(png, 0, PNG_ALL_FILTERS);
    png_set_IHDR(png,info,w,h,8,PNG_COLOR_TYPE_RGBA,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_BASE,PNG_FILTER_TYPE_BASE);
    png_bytep *rows=malloc(sizeof(png_bytep)*h); if(!rows){png_destroy_write_struct(&png,&info);fclose(fp);return -1;}
    for(int y=0;y<h;y++) rows[y]=(png_bytep)(data + (size_t)y*w*4);
    png_set_rows(png,info,rows);
    png_write_png(png,info,PNG_TRANSFORM_IDENTITY,NULL);
    free(rows);
    png_destroy_write_struct(&png,&info);
    fclose(fp);
    return 0;
}

static void build_packet(uint16_t command, const uint8_t *data, size_t data_len, size_t total_len, uint8_t *out) {
    memset(out, 0, PACKET_SIZE);
    out[0] = HEADER0;
    out[1] = HEADER1;
    out[2] = (command >> 8) & 0xff;
    out[3] = command & 0xff;
    out[4] = (uint8_t)(total_len & 0xff);
    out[5] = (uint8_t)((total_len >> 8) & 0xff);
    out[6] = (uint8_t)((total_len >> 16) & 0xff);
    out[7] = (uint8_t)((total_len >> 24) & 0xff);
    if (data && data_len > 0) {
        memcpy(out + 8, data, data_len > (PACKET_SIZE - 8) ? (PACKET_SIZE - 8) : data_len);
    }
}

static int send_command(hid_device *dev, uint16_t cmd, const uint8_t *data, size_t len) {
    if (!dev) return -1;
    uint8_t packet[PACKET_SIZE];
    build_packet(cmd, data, len, len, packet);
    return write_packet(dev, packet, PACKET_SIZE);
}

static int has_invalid_bytes(const uint8_t *buf, size_t len) {
    for (size_t i = 1016; i < len; i += 1024) {
        if (buf[i] == 0x00 || buf[i] == 0x7c) return 1;
    }
    return 0;
}

static size_t patch_invalid_bytes(uint8_t *buf, size_t len) {
    size_t patched_count = 0;
    for (size_t i = 1016; i < len; i += 1024) {
        if (buf[i] == 0x00 || buf[i] == 0x7c) { buf[i] = 0x11; patched_count++; }
        // sleep(0);
    }
    return patched_count;
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *prepare_zip_buffer(const uint8_t *buf, size_t len, size_t *out_len, int *did_patch, int *pad_used, size_t *patched_count) {
    *did_patch = 0;
    *pad_used = 0;
    *patched_count = 0;
    int start_pad = g_fast_nopad ? MAX_PADDING_RETRIES : 0;
    for (int pad = start_pad; pad <= MAX_PADDING_RETRIES; pad++) {
        size_t nlen = len + (size_t)pad;
        uint8_t *tmp = malloc(nlen);
        if (!tmp) return NULL;
        memcpy(tmp, buf, len);
        if (pad > 0) memset(tmp + len, 0x00, pad);
        if (!has_invalid_bytes(tmp, nlen)) {
            if (pad > 0 && g_debug) {
                time_t t=time(NULL); struct tm *tm=localtime(&t); char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",tm);
                // fprintf(stderr, "\r[%s] padding %d byte(s) resolved invalid bytes\033[K", ts, pad);
                fflush(stderr);
            }
            *pad_used = pad;
            *out_len = nlen;
            return tmp;
        }
        if (pad == MAX_PADDING_RETRIES) {
            *patched_count = patch_invalid_bytes(tmp, nlen);
            if (*patched_count > 0) *did_patch = 1;
            *pad_used = pad;
            *out_len = nlen;
            return tmp;
        }
        free(tmp);
    }
    return NULL;
}

// --- simple dynamic buffer helpers ---
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b) {
    b->len = 0;
    b->cap = 4096;
    b->data = malloc(b->cap);
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int buf_reserve(Buf *b, size_t add) {
    if (b->len + add <= b->cap) return 0;
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < b->len + add) newcap *= 2;
    uint8_t *p = realloc(b->data, newcap);
    if (!p) return -1;
    b->data = p;
    b->cap = newcap;
    return 0;
}

static int buf_write(Buf *b, const void *data, size_t len) {
    if (buf_reserve(b, len) != 0) return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static int buf_write_u16(Buf *b, uint16_t v) {
    uint8_t tmp[2] = { (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff) };
    return buf_write(b, tmp, 2);
}

static int buf_write_u32(Buf *b, uint32_t v) {
    uint8_t tmp[4] = {
        (uint8_t)(v & 0xff),
        (uint8_t)((v >> 8) & 0xff),
        (uint8_t)((v >> 16) & 0xff),
        (uint8_t)((v >> 24) & 0xff)
    };
    return buf_write(b, tmp, 4);
}

// --- ZIP writer (store only, no compression) ---
typedef struct {
    char *name;
    uint32_t crc32;
    uint32_t size;
    uint32_t comp_size;
    uint16_t method;
    uint32_t offset;
} ZipEntry;

typedef struct {
    Buf buf;
    ZipEntry *entries;
    size_t count;
    size_t cap;
} ZipWriter;

static void zw_init(ZipWriter *zw) {
    buf_init(&zw->buf);
    zw->entries = NULL;
    zw->count = 0;
    zw->cap = 0;
}

static void zw_free(ZipWriter *zw) {
    for (size_t i = 0; i < zw->count; i++) free(zw->entries[i].name);
    free(zw->entries);
    buf_free(&zw->buf);
}

static void human_bytes(uint64_t bytes, double *val, const char **unit) {
    static const char *units[] = {"B","KB","MB","GB","TB"};
    int idx = 0;
    double v = (double)bytes;
    while (v >= 1024.0 && idx < 4) { v /= 1024.0; idx++; }
    *val = v;
    *unit = units[idx];
}

static void log_sendzip(size_t len, int pad, int patched, size_t patched_count) {
    (void)patched;
    TOTAL_BYTES_SENT += len;
    TOTAL_BYTES_PATCHED += patched_count;
    time_t t=time(NULL);
    struct tm *tm=localtime(&t);
    char ts[32];
    if (g_sendzip_log_legacy) {
        double hval; const char *hunit;
        human_bytes(TOTAL_BYTES_SENT, &hval, &hunit);
        strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",tm);
        fprintf(stderr, "\r[%s] sendzip %zu bytes (pad=%d, patched=%zu, total_patched=%" PRIu64 ") total=%.2f%s\033[K",
                ts, len, pad, patched_count, TOTAL_BYTES_PATCHED, hval, hunit);
    } else {
        // Short, fixed-width format to avoid "blinking" in the console.
        // Example:
        //   01/27|00:10:28 zip=001.4KB total=17.78MB
        //   01/27|00:10:28 zip=00852-B total=17.78MB
        strftime(ts, sizeof(ts), "%m/%d|%H:%M:%S", tm);

        char zbuf[32];
        if (len < 1024) {
            // Bytes-only special format, fixed width.
            snprintf(zbuf, sizeof(zbuf), "%05zu-B", len);
        } else {
            double kb = (double)len / 1024.0;
            // Fixed width: 5 chars with leading zeros + 1 decimal (e.g. 001.4)
            // Clamp to 999.9 to keep width stable.
            if (kb > 999.9) kb = 999.9;
            snprintf(zbuf, sizeof(zbuf), "%05.1fKB", kb);
        }

        double hval; const char *hunit;
        human_bytes(TOTAL_BYTES_SENT, &hval, &hunit);
        fprintf(stderr, "\r%s zip=%s total=%.2f%s\033[K", ts, zbuf, hval, hunit);
    }
    fflush(stderr);
}

static int zw_add_entry(ZipWriter *zw, const char *name, const uint8_t *data, size_t size) {
    if (!name) return -1;
    if (zw->count >= zw->cap) {
        size_t nc = zw->cap ? zw->cap * 2 : 16;
        ZipEntry *tmp = realloc(zw->entries, nc * sizeof(ZipEntry));
        if (!tmp) return -1;
        zw->entries = tmp;
        zw->cap = nc;
    }
    uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);
    crc = (uint32_t)crc32(crc, data, size);
    uint32_t offset = (uint32_t)zw->buf.len;
    uint16_t name_len = (uint16_t)strlen(name);
    // local file header
    buf_write_u32(&zw->buf, 0x04034b50);
    buf_write_u16(&zw->buf, 20); // version needed
    buf_write_u16(&zw->buf, 0);  // flags
    buf_write_u16(&zw->buf, 0);  // method store only
    buf_write_u16(&zw->buf, 0);  // mtime
    buf_write_u16(&zw->buf, 0);  // mdate
    buf_write_u32(&zw->buf, crc);
    buf_write_u32(&zw->buf, (uint32_t)size);
    buf_write_u32(&zw->buf, (uint32_t)size);
    buf_write_u16(&zw->buf, name_len);
    buf_write_u16(&zw->buf, 0); // extra len
    buf_write(&zw->buf, name, name_len);
    buf_write(&zw->buf, data, size);

    zw->entries[zw->count].name = strdup(name);
    zw->entries[zw->count].crc32 = crc;
    zw->entries[zw->count].size = (uint32_t)size;
    zw->entries[zw->count].comp_size = (uint32_t)size;
    zw->entries[zw->count].method = 0;
    zw->entries[zw->count].offset = offset;
    zw->count++;
    return 0;
}

static int zw_finalize(ZipWriter *zw, uint8_t **out, size_t *out_len) {
    uint32_t central_offset = (uint32_t)zw->buf.len;
    for (size_t i = 0; i < zw->count; i++) {
        ZipEntry *e = &zw->entries[i];
        uint16_t name_len = (uint16_t)strlen(e->name);
        buf_write_u32(&zw->buf, 0x02014b50); // central header
        buf_write_u16(&zw->buf, 20); // version made by
        buf_write_u16(&zw->buf, 20); // version needed
        buf_write_u16(&zw->buf, 0);  // flags
        buf_write_u16(&zw->buf, e->method);  // method
        buf_write_u16(&zw->buf, 0);  // mtime
        buf_write_u16(&zw->buf, 0);  // mdate
        buf_write_u32(&zw->buf, e->crc32);
        buf_write_u32(&zw->buf, e->comp_size);
        buf_write_u32(&zw->buf, e->size);
        buf_write_u16(&zw->buf, name_len);
        buf_write_u16(&zw->buf, 0); // extra len
        buf_write_u16(&zw->buf, 0); // comment len
        buf_write_u16(&zw->buf, 0); // disk start
        buf_write_u16(&zw->buf, 0); // int attrs
        buf_write_u32(&zw->buf, 0); // ext attrs
        buf_write_u32(&zw->buf, e->offset);
        buf_write(&zw->buf, e->name, name_len);
    }
    uint32_t central_size = (uint32_t)(zw->buf.len - central_offset);
    // EOCD
    buf_write_u32(&zw->buf, 0x06054b50);
    buf_write_u16(&zw->buf, 0); // disk
    buf_write_u16(&zw->buf, 0); // start disk
    buf_write_u16(&zw->buf, (uint16_t)zw->count);
    buf_write_u16(&zw->buf, (uint16_t)zw->count);
    buf_write_u32(&zw->buf, central_size);
    buf_write_u32(&zw->buf, central_offset);
    buf_write_u16(&zw->buf, 0); // comment len

    *out = zw->buf.data;
    *out_len = zw->buf.len;
    // transfer ownership of buf; don't free in zw_free
    zw->buf.data = NULL;
    zw->buf.len = zw->buf.cap = 0;
    return 0;
}

typedef struct {
    char *name;
    const uint8_t *data;
    size_t size;
} ZipInEntry;

static int zip_parse_local_entries(const uint8_t *buf, size_t len, ZipInEntry **out_entries, size_t *out_count) {
    *out_entries = NULL;
    *out_count = 0;
    if (!buf || len < 30) return -1;

    size_t cap = 0;
    size_t count = 0;
    ZipInEntry *entries = NULL;

    size_t off = 0;
    while (off + 30 <= len) {
        uint32_t sig = rd_le32(buf + off);
        if (sig != 0x04034b50u) break; // stop at central dir / EOCD

        uint16_t flags = rd_le16(buf + off + 6);
        uint16_t method = rd_le16(buf + off + 8);
        uint32_t comp_size = rd_le32(buf + off + 18);
        uint16_t name_len = rd_le16(buf + off + 26);
        uint16_t extra_len = rd_le16(buf + off + 28);

        if (flags != 0) return -1;    // data descriptor etc not supported here
        if (method != 0) return -1;   // only store-only supported here
        if (off + 30 + (size_t)name_len + (size_t)extra_len > len) return -1;

        const uint8_t *namep = buf + off + 30;
        size_t data_off = off + 30 + (size_t)name_len + (size_t)extra_len;
        if (data_off + (size_t)comp_size > len) return -1;

        char *name = malloc((size_t)name_len + 1);
        if (!name) return -1;
        memcpy(name, namep, name_len);
        name[name_len] = '\0';

        if (count >= cap) {
            size_t nc = cap ? cap * 2 : 16;
            ZipInEntry *tmp = realloc(entries, nc * sizeof(ZipInEntry));
            if (!tmp) { free(name); return -1; }
            entries = tmp;
            cap = nc;
        }
        entries[count].name = name;
        entries[count].data = buf + data_off;
        entries[count].size = (size_t)comp_size;
        count++;

        off = data_off + (size_t)comp_size;
    }

    if (count == 0) {
        free(entries);
        return -1;
    }
    *out_entries = entries;
    *out_count = count;
    return 0;
}

static void zip_free_entries(ZipInEntry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) free(entries[i].name);
    free(entries);
}

static int build_zip_from_zipfile_with_dummy(const uint8_t *in_buf, size_t in_len, size_t dummy_len,
                                             uint8_t **out_buf, size_t *out_len) {
    ZipInEntry *entries = NULL;
    size_t count = 0;
    if (zip_parse_local_entries(in_buf, in_len, &entries, &count) != 0) return -1;

    ZipWriter zw;
    zw_init(&zw);
    if (dummy_len > 0) {
        uint8_t *dummy = malloc(dummy_len);
        if (!dummy) { zip_free_entries(entries, count); zw_free(&zw); return -1; }
        memset(dummy, 0x01, dummy_len); // avoid 0x00/0x7c
        zw_add_entry(&zw, "dummy.txt", dummy, dummy_len);
        free(dummy);
    }
    for (size_t i = 0; i < count; i++) {
        zw_add_entry(&zw, entries[i].name, entries[i].data, entries[i].size);
    }
    zip_free_entries(entries, count);

    uint8_t *zipbuf = NULL;
    size_t ziplen = 0;
    zw_finalize(&zw, &zipbuf, &ziplen);
    zw_free(&zw);
    *out_buf = zipbuf;
    *out_len = ziplen;
    return 0;
}

static int send_zip_buffer_cmd(hid_device *dev, const uint8_t *buf, size_t sz, uint16_t cmd, int pad_used, size_t patched_count) {
    uint8_t packet[PACKET_SIZE];
    size_t first_len = PACKET_SIZE - 8;
    build_packet(cmd, buf, first_len, sz, packet);
    // first packet is header + 1016 data; we don't patch here because the problematic offsets start at the next packet.
    if (write_packet(dev, packet, PACKET_SIZE) < 0) { return -1; }
    size_t offset = first_len;
    while (offset < sz) {
        size_t chunk = sz - offset;
        if (chunk > PACKET_SIZE) chunk = PACKET_SIZE;
        uint8_t tmp[PACKET_SIZE];
        memset(tmp, 0, PACKET_SIZE);
        memcpy(tmp, buf + offset, chunk);
        if (write_packet(dev, tmp, PACKET_SIZE) < 0) { return -1; }
        offset += chunk;
    }
    log_sendzip(sz, pad_used, patched_count > 0, patched_count);
    return 0;
}

static int send_zip(hid_device *dev, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);

    // Rebuild ZIP in-memory with dummy.txt first (same technique as build_zip_from_icons).
    // This can shift invalid bytes even if they occur in the original manifest/header area.
    uint8_t *zipbuf = NULL;
    size_t ziplen = 0;
    int pad_used = 0;
    size_t patched_count = 0;
    for (int pad = 0; pad <= MAX_PADDING_RETRIES; pad++) {
        if (build_zip_from_zipfile_with_dummy(buf, (size_t)sz, (size_t)pad, &zipbuf, &ziplen) != 0 || !zipbuf) {
            if (zipbuf) free(zipbuf);
            zipbuf = NULL;
            continue;
        }
        if (!has_invalid_bytes(zipbuf, ziplen)) {
            pad_used = pad;
            break;
        }
        if (pad == MAX_PADDING_RETRIES) {
            patched_count = patch_invalid_bytes(zipbuf, ziplen);
            pad_used = pad;
            break;
        }
        free(zipbuf);
        zipbuf = NULL;
    }

    if (!zipbuf) {
        // Fallback to legacy external padding if the ZIP couldn't be parsed/rebuilt.
        size_t out_len = 0;
        int did_patch = 0;
        int legacy_pad = 0;
        size_t legacy_patched = 0;
        uint8_t *prepared = prepare_zip_buffer(buf, (size_t)sz, &out_len, &did_patch, &legacy_pad, &legacy_patched);
        free(buf);
        if (!prepared) return -1;
        int res = send_zip_buffer_cmd(dev, prepared, out_len, 0x0001, legacy_pad, legacy_patched);
        free(prepared);
        return res;
    }

    free(buf);
    int res = send_zip_buffer_cmd(dev, zipbuf, ziplen, 0x0001, pad_used, patched_count);
    free(zipbuf);
    return res;
}

static int send_zip_buffer(hid_device *dev, const uint8_t *buf, size_t sz, int pad_used, size_t patched_count) {
    return send_zip_buffer_cmd(dev, buf, sz, 0x0001, pad_used, patched_count);
}

// --- ZIP build from directory (icons + manifest) ---
typedef struct {
    int btn_index; // 0-based button index
    char *path;
    char *name;
    char *label;
    uint8_t *data;
    size_t data_len;
} IconItem;

static char *basename_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    return strdup(base);
}

static FD_UNUSED char *label_from_name(const char *name) {
    const char *dot = strrchr(name, '.');
    size_t len = dot ? (size_t)(dot - name) : strlen(name);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, name, len);
    out[len] = '\0';
    return out;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->len = 0;
    sb->cap = 1024;
    sb->data = malloc(sb->cap);
    if (sb->data) sb->data[0] = '\0';
}

static void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static int sb_reserve(StrBuf *sb, size_t add) {
    if (sb->len + add + 1 <= sb->cap) return 0;
    size_t nc = sb->cap ? sb->cap : 1024;
    while (nc < sb->len + add + 1) nc *= 2;
    char *p = realloc(sb->data, nc);
    if (!p) return -1;
    sb->data = p;
    sb->cap = nc;
    return 0;
}

static int sb_append(StrBuf *sb, const char *s) {
    size_t l = strlen(s);
    if (sb_reserve(sb, l) != 0) return -1;
    memcpy(sb->data + sb->len, s, l);
    sb->len += l;
    sb->data[sb->len] = '\0';
    return 0;
}

static int sb_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(tmp)) {
        char *dyn = malloc((size_t)n + 1);
        if (!dyn) return -1;
        va_start(ap, fmt);
        vsnprintf(dyn, (size_t)n + 1, fmt, ap);
        va_end(ap);
        int r = sb_append(sb, dyn);
        free(dyn);
        return r;
    }
    return sb_append(sb, tmp);
}

static int sb_append_json_string(StrBuf *sb, const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *tmp = malloc(n + 1);
    if (!tmp) return -1;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '"') tmp[w++] = s[i];
    }
    tmp[w] = '\0';
    int r = sb_appendf(sb, "\"%s\"", tmp);
    free(tmp);
    return r;
}

static int build_manifest(const IconItem *items, size_t count, StrBuf *out) {
    sb_init(out);
    sb_append(out, "{");
    for (size_t i = 0; i < count; i++) {
        int idx = items[i].btn_index;
        int row = idx / 5;
        int col = idx % 5;
        if (i > 0) sb_append(out, ",");
        sb_appendf(out, "\"%d_%d\":{\"State\":0,\"ViewParam\":[{\"Icon\":\"icons/%s\",\"Text\":", col, row, items[i].name);
        sb_append_json_string(out, items[i].label ? items[i].label : "");
        sb_append(out, "}]}");
    }
    sb_append(out, "}");
    return 0;
}

static int build_zip_with_dummy(const IconItem *items, size_t count, size_t dummy_len, uint8_t **out_buf, size_t *out_len) {
    if (count == 0) return -1;
    ZipWriter zw;
    zw_init(&zw);
    if (dummy_len > 0) {
        uint8_t *dummy = malloc(dummy_len);
        if (!dummy) { zw_free(&zw); return -1; }
        memset(dummy, 0x01, dummy_len); // avoid 0x00/0x7c
        zw_add_entry(&zw, "dummy.txt", dummy, dummy_len);
        free(dummy);
    }
    StrBuf manifest;
    build_manifest(items, count, &manifest);
    zw_add_entry(&zw, "manifest.json", (const uint8_t *)manifest.data, manifest.len);
    for (size_t i = 0; i < count; i++) {
        const uint8_t *buf = NULL;
        size_t sz = 0;
        uint8_t *alloc = NULL;
        if (items[i].data && items[i].data_len > 0) {
            buf = items[i].data;
            sz = items[i].data_len;
        } else if (items[i].path) {
            FILE *f = fopen(items[i].path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long lsz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (lsz <= 0) { fclose(f); continue; }
            alloc = malloc((size_t)lsz);
            if (!alloc) { fclose(f); continue; }
            if (fread(alloc, 1, (size_t)lsz, f) != (size_t)lsz) { free(alloc); fclose(f); continue; }
            fclose(f);
            buf = alloc;
            sz = (size_t)lsz;
        } else {
            continue;
        }
        char icon_name[512];
        snprintf(icon_name, sizeof(icon_name), "icons/%s", items[i].name);
        zw_add_entry(&zw, icon_name, buf, sz);
        if (alloc) free(alloc);
    }
    uint8_t *zipbuf = NULL;
    size_t ziplen = 0;
    zw_finalize(&zw, &zipbuf, &ziplen);
    zw_free(&zw);
    sb_free(&manifest);
    *out_buf = zipbuf;
    *out_len = ziplen;
    return 0;
}

static int build_zip_from_rgba_icons(const IconItem *items, size_t count, uint8_t **out_buf, size_t *out_len, int *pad_used, size_t *patched_count);

static int build_zip_from_icons(const IconItem *items, size_t count, uint8_t **out_buf, size_t *out_len, int *pad_used, size_t *patched_count) {
    *pad_used = 0;
    *patched_count = 0;
    if (count == 0) return -1;
    for (int pad = 0; pad <= MAX_PADDING_RETRIES; pad++) {
        uint8_t *zipbuf = NULL; size_t ziplen = 0;
        if (build_zip_with_dummy(items, count, (size_t)pad, &zipbuf, &ziplen) != 0 || !zipbuf) {
            if (zipbuf) free(zipbuf);
            continue;
        }
        if (!has_invalid_bytes(zipbuf, ziplen)) {
            *out_buf = zipbuf;
            *out_len = ziplen;
            *pad_used = pad;
            if (pad > 0 && g_debug) {
                time_t t=time(NULL); struct tm *tm=localtime(&t); char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",tm);
                // fprintf(stderr, "\r[%s] padding %d byte(s) resolved invalid bytes\033[K", ts, pad);
                fflush(stderr);
            }
            return 0;
        }
        if (pad == MAX_PADDING_RETRIES) {
            *patched_count = patch_invalid_bytes(zipbuf, ziplen);
            *out_buf = zipbuf;
            *out_len = ziplen;
            *pad_used = pad;
            return 0;
        }
        free(zipbuf);
    }
    return -1;
}

static int send_partial_update(hid_device *dev, const IconItem *items, size_t count) {
    uint8_t *zipbuf=NULL; size_t ziplen=0; int pad_used=0; size_t patched=0;
    if (build_zip_from_icons(items, count, &zipbuf, &ziplen, &pad_used, &patched)!=0 || !zipbuf) return -1;
    int res = send_zip_buffer_cmd(dev, zipbuf, ziplen, 0x000d, pad_used, patched);
    free(zipbuf);
    return res;
}

static void trim_line(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[len-1] = '\0';
        len--;
    }
}

static void notify_rb_event(int *rb_fd, const char *msg) {
    if (!rb_fd || *rb_fd < 0 || !msg) return;
    if (write(*rb_fd, msg, strlen(msg)) < 0) {
        close(*rb_fd);
        *rb_fd = -1;
    }
}

static int make_listen_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    unlink(SOCK_PATH);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 5) < 0) { perror("listen"); exit(1); }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    hid_init();
    int debug = getenv("ULANZI_DEBUG") ? 1 : 0;
    g_debug = debug;
    if (getenv("ULANZI_FAST_NOPAD")) g_fast_nopad = 1;
    hid_device *dev = NULL;
    double next_reconnect = 0.0;
    {
        dev = open_device();
        if (dev) {
            hid_set_nonblocking(dev, 0);
        } else {
            fprintf(stderr, "Unable to open device (will retry)\n");
        }
    }

    int listen_fd = make_listen_socket();
    printf("ulanzi_d200_daemon listening on %s\n", SOCK_PATH);

    int rb_fd = -1;
    double down_time[14] = {0};
    int hold_emitted[14] = {0};
    int longhold_emitted[14] = {0};
    int tap_pending[14] = {0};
    const double HOLD_THRESHOLD = 0.75;     // seconds
    const double LONGHOLD_THRESHOLD = 5.0;  // seconds
    const double TAP_THRESHOLD = 0.02;      // seconds
    time_t last_keepalive = time(NULL);

    // Remember last "small window" state so keep-alive does not force mode=1 (CLOCK).
    // Legacy: mode 0=STATS, 1=CLOCK, 2=BACKGROUND
    int sw_mode = 1;
    int sw_cpu = 0;
    int sw_mem = 0;
    int sw_gpu = 0;

    while (running) {
        // Auto-reconnect to HID device if it disappeared (USB reset / unplug).
        if (!dev) {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double now = (double)ts_now.tv_sec + (double)ts_now.tv_nsec / 1e9;
            if (now >= next_reconnect) {
                dev = open_device();
                if (dev) {
                    hid_set_nonblocking(dev, 0);
                    memset(down_time, 0, sizeof(down_time));
                    memset(hold_emitted, 0, sizeof(hold_emitted));
                    memset(longhold_emitted, 0, sizeof(longhold_emitted));
                    memset(tap_pending, 0, sizeof(tap_pending));
                    last_keepalive = time(NULL);
                    if (debug) fprintf(stderr, "[debug] Reconnected to HID device\n");
                    notify_rb_event(&rb_fd, "evt connected\n");
                } else {
                    next_reconnect = now + 0.5;
                }
            }
        }

        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0) {
            int cflags = fcntl(cfd, F_GETFL, 0);
            if (cflags >= 0) fcntl(cfd, F_SETFL, cflags & ~O_NONBLOCK); // blocking for command handling
            char line[2048];
            ssize_t n = read(cfd, line, sizeof(line) - 1);
            if (n > 0) {
                line[n] = '\0';
                trim_line(line);

                // ping is a daemon health/status check. It must work even if the USB HID device is missing.
                // This is used by paging_daemon to detect device reconnect and resync state.
                if (strncmp(line, "ping", 4) == 0) {
                    if (dev) write(cfd, "ok\n", 3);
                    else write(cfd, "err no_device\n", 14);
                    goto cmd_done;
                }

                // If the USB device is disconnected, only allow read-buttons subscription to stay open.
                // Other commands require an active HID device.
                if (!dev && strncmp(line, "read-buttons", 12) != 0) {
                    write(cfd, "err no_device\n", 14);
                    goto cmd_done;
                }
	                if (strncmp(line, "set-brightness ", 15) == 0) {
	                    int v = atoi(line + 15);
	                    if (v < 0) v = 0;
	                    if (v > 100) v = 100;
	                    char payload[16]; snprintf(payload, sizeof(payload), "%d", v);
                    if (send_command(dev, 0x000a, (uint8_t *)payload, strlen(payload)) >= 0)
                        write(cfd, "ok\n", 3);
                    else write(cfd, "err\n", 4);
                } else if (strncmp(line, "set-small-window ", 17) == 0) {
                    int mode=1,cpu=0,mem=0,gpu=0;
                    char timestr[32]="00:00:00";
                    sscanf(line+17, "%d %d %d %31s %d", &mode,&cpu,&mem,timestr,&gpu);
                    // Persist the requested state (even if time_str is synthetic) for future keep-alive.
                    sw_mode = mode;
                    sw_cpu = cpu;
                    sw_mem = mem;
                    sw_gpu = gpu;
                    char payload[64];
                    snprintf(payload,sizeof(payload),"%d|%d|%d|%s|%d",mode,cpu,mem,timestr,gpu);
                    if (send_command(dev,0x0006,(uint8_t*)payload,strlen(payload))>=0)
                        write(cfd,"ok\n",3);
                    else write(cfd,"err\n",4);
                } else if (strncmp(line, "set-label-style ", 16)==0) {
                    char *path=line+16; while (*path==' ') path++;
                    FILE *f=fopen(path,"rb");
                    if (!f) { write(cfd,"err\n",4); }
                    else {
                        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
                        if (sz<=0||sz>4096) { fclose(f); write(cfd,"err\n",4); }
                        else {
                            char *buf=malloc((size_t)sz+1);
                            fread(buf,1,(size_t)sz,f); buf[sz]='\0'; fclose(f);
                            int res=send_command(dev,0x000b,(uint8_t*)buf,strlen(buf));
                            free(buf);
                            if (res>=0) write(cfd,"ok\n",3); else write(cfd,"err\n",4);
                        }
                    }
                } else if (strncmp(line,"set-buttons ",12)==0) {
                    char *path=line+12; while(*path==' ') path++;
                    if (send_zip(dev,path)==0) write(cfd,"ok\n",3);
                    else { perror("send_zip"); write(cfd,"err\n",4); }
                } else if (strncmp(line,"set-buttons-explicit-14",23)==0) {
                    char *p = line + 23;
                    char *argv[64];
                    int argc = 0;
                    char *labels[14] = {0};
                    while (*p && argc < 64) {
                        while (*p==' ') p++;
                        if (!*p) break;
                        argv[argc++] = p;
                        while (*p && *p!=' ') p++;
                        if (*p) { *p='\0'; p++; }
                    }
                    for (int i=0;i<argc;i++) {
                        if (strncmp(argv[i],"--label-",8)==0) {
                            int idx = atoi(argv[i]+8) - 1;
                            if (idx >=0 && idx < 14) {
                                char *eq = strchr(argv[i],'=');
                                if (eq && eq[1]) {
                                    free(labels[idx]);
                                    labels[idx] = strdup(eq+1);
                                }
                            }
                        }
                    }
                    IconItem items[14]; memset(items, 0, sizeof(items));
                    size_t icount=0;
                    for (int i=0;i<argc;i++) {
                        if (strncmp(argv[i],"--button-",9)==0) {
                            int idx = atoi(argv[i]+9) - 1;
                            if (idx < 0 || idx >=14) continue;
                            char *eq = strchr(argv[i],'=');
                            if (!eq || !eq[1]) continue;
                            const char *path = eq+1;
                            struct stat st;
                            if (stat(path,&st)!=0 || !S_ISREG(st.st_mode)) continue;
                            items[icount].btn_index = idx;
                            items[icount].path = strdup(path);
                            items[icount].name = basename_dup(path);
                            if (idx == 13) {
                                items[icount].label = strdup(""); // ignore label for 14
                            } else if (labels[idx]) {
                                items[icount].label = strdup(labels[idx]);
                            } else {
                                items[icount].label = strdup("");
                            }
                            icount++;
                        }
                    }
                    if (icount==0) { write(cfd,"err\n",4); }
                    else {
                        uint8_t *zipbuf=NULL; size_t ziplen=0; int pad_used=0; size_t patched=0;
                        if (build_zip_from_icons(items, icount, &zipbuf, &ziplen, &pad_used, &patched)==0 && zipbuf) {
                            int res = send_zip_buffer_cmd(dev, zipbuf, ziplen, 0x0001, pad_used, patched);
                            free(zipbuf);
                            if (res==0) write(cfd,"ok\n",3);
                            else write(cfd,"err\n",4);
                        } else {
                            write(cfd,"err\n",4);
                        }
                    }
                    for (size_t i=0;i<icount;i++) {
                        free(items[i].path);
                        free(items[i].name);
                        free(items[i].label);
                        if (items[i].data) free(items[i].data);
                    }
                    for (int i=0;i<14;i++) free(labels[i]);
                } else if (strncmp(line,"set-buttons-explicit",20)==0) {
                    char *p = line + 20;
                    char *argv[64];
                    int argc = 0;
                    char *labels[13] = {0};
                    while (*p && argc < 64) {
                        while (*p==' ') p++;
                        if (!*p) break;
                        argv[argc++] = p;
                        while (*p && *p!=' ') p++;
                        if (*p) { *p='\0'; p++; }
                    }
                    // first pass: collect labels
                    for (int i=0;i<argc;i++) {
                        if (strncmp(argv[i],"--label-",8)==0) {
                            int idx = atoi(argv[i]+8) - 1;
                            if (idx >=0 && idx < 13) {
                                char *eq = strchr(argv[i],'=');
                                if (eq && eq[1]) {
                                    free(labels[idx]);
                                    labels[idx] = strdup(eq+1);
                                }
                            }
                        }
                    }
                    IconItem items[13]; memset(items, 0, sizeof(items));
                    size_t icount=0;
                    for (int i=0;i<argc;i++) {
                        if (strncmp(argv[i],"--button-",9)==0) {
                            int idx = atoi(argv[i]+9) - 1;
                            if (idx < 0 || idx >=13) continue;
                            char *eq = strchr(argv[i],'=');
                            if (!eq || !eq[1]) continue;
                            const char *path = eq+1;
                            struct stat st;
                            if (stat(path,&st)!=0 || !S_ISREG(st.st_mode)) continue;
                            items[icount].btn_index = idx;
                            items[icount].path = strdup(path);
                            items[icount].name = basename_dup(path);
                            if (labels[idx]) {
                                items[icount].label = strdup(labels[idx]);
                            } else {
                                items[icount].label = strdup("");
                            }
                            icount++;
                        }
                    }
                    if (icount==0) { write(cfd,"err\n",4); }
                    else {
                        uint8_t *zipbuf=NULL; size_t ziplen=0;
                        int pad_used=0; size_t patched=0;
                        if (build_zip_from_icons(items, icount, &zipbuf, &ziplen, &pad_used, &patched)==0 && zipbuf) {
                            int res = send_zip_buffer(dev, zipbuf, ziplen, pad_used, patched);
                            free(zipbuf);
                            if (res==0) write(cfd,"ok\n",3);
                            else write(cfd,"err\n",4);
                        } else {
                            write(cfd,"err\n",4);
                        }
                    }
                    for (size_t i=0;i<icount;i++) {
                        free(items[i].path);
                        free(items[i].name);
                        free(items[i].label);
                    }
                    for (int i=0;i<13;i++) free(labels[i]);
                } else if (strncmp(line,"set-partial-explicit",20)==0) {
                    char *p = line + 20;
                    char *argv[64];
                    int argc = 0;
                    char *labels[13] = {0};
                    while (*p && argc < 64) {
                        while (*p==' ') p++;
                        if (!*p) break;
                        argv[argc++] = p;
                        while (*p && *p!=' ') p++;
                        if (*p) { *p='\0'; p++; }
                    }
                    for (int i=0;i<argc;i++) {
                        if (strncmp(argv[i],"--label-",8)==0) {
                            int idx = atoi(argv[i]+8) - 1;
                            if (idx >=0 && idx < 13) {
                                char *eq = strchr(argv[i],'=');
                                if (eq && eq[1]) {
                                    free(labels[idx]);
                                    labels[idx] = strdup(eq+1);
                                }
                            }
                        }
                    }
                    IconItem items[13]; memset(items, 0, sizeof(items));
                    size_t icount=0;
                    for (int i=0;i<argc;i++) {
                        if (strncmp(argv[i],"--button-",9)==0) {
                            int idx = atoi(argv[i]+9) - 1;
                            if (idx < 0 || idx >=13) continue;
                            char *eq = strchr(argv[i],'=');
                            if (!eq || !eq[1]) continue;
                            const char *path = eq+1;
                            struct stat st;
                            if (stat(path,&st)!=0 || !S_ISREG(st.st_mode)) continue;
                            items[icount].btn_index = idx;
                            items[icount].path = strdup(path);
                            items[icount].name = basename_dup(path);
                            if (labels[idx]) items[icount].label = strdup(labels[idx]);
                            else items[icount].label = strdup("");
                            icount++;
                        }
                    }
                    if (icount==0) { write(cfd,"err\n",4); }
                    else {
                        int res = send_partial_update(dev, items, icount);
                        if (res==0) write(cfd,"ok\n",3); else write(cfd,"err\n",4);
                    }
                    for (size_t i=0;i<icount;i++) {
                        free(items[i].path);
                        free(items[i].name);
                        free(items[i].label);
                        if (items[i].data) free(items[i].data);
                    }
                    for (int i=0;i<13;i++) free(labels[i]);
                } else if (strncmp(line,"read-buttons",12)==0) {
                    rb_fd = cfd;
                    write(cfd,"ok\n",3);
                    cfd = -1; // keep open
                } else {
                    write(cfd,"unknown\n",8);
                }
cmd_done:
            }
            if (cfd != -1) close(cfd);
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                perror("accept");
                break;
            }
        }

        // stream button events to read-buttons subscriber (only when device is connected)
        if (rb_fd >= 0 && dev) {
            uint8_t buf[PACKET_SIZE];
            int r = hid_read_timeout(dev, buf, sizeof(buf), 50);
            if (r > 0 && buf[0]==HEADER0 && buf[1]==HEADER1) {
                uint16_t cmd=((uint16_t)buf[2]<<8)|buf[3];
                if (cmd==0x0101 || cmd==0x0102) {
                    if (debug) fprintf(stderr, "[dbg] packet cmd=0x%04x len=%d\n", cmd, r);
                    // Legacy python exposes this as ButtonPress.state (first byte of data[8:12]).
                    // For button index 13 (the small window/clock/background button), this value
                    // changes when the user cycles modes on-device. Track it so keep-alive can
                    // preserve the current mode instead of forcing CLOCK.
                    int pkt_state = (int)buf[8];
                    int idx=buf[9];
                    if (idx < 0 || idx >= 14) continue;
                    if (idx == 13 && pkt_state >= 0 && pkt_state <= 2) {
                        sw_mode = pkt_state;
                    }
                    uint8_t raw_press = buf[11];
                    int pressed = (raw_press == 0x01);
                    int release_evt = (raw_press != 0x01);
                    if (idx == 13) {
                        // Special: first raw_press 0x01 -> TAP; second raw_press 0x01 -> RELEASE
                        if (raw_press == 0x01 && down_time[idx] == 0) {
                            pressed = 1;
                            release_evt = 0;
                        } else if (raw_press == 0x01 && down_time[idx] > 0) {
                            pressed = 0;
                            release_evt = 1;
                        } else {
                            pressed = 0;
                            release_evt = 0;
                        }
                    }
                    if (debug) fprintf(stderr, "[dbg] idx=%d raw_press=0x%02x pressed=%d release_evt=%d\n", idx, raw_press, pressed, release_evt);
                    struct timespec ts_now;
                    clock_gettime(CLOCK_MONOTONIC, &ts_now);
                    double now = (double)ts_now.tv_sec + (double)ts_now.tv_nsec / 1e9;
                    if (pressed) {
                        if (down_time[idx] == 0) {
                            down_time[idx] = now;
                            hold_emitted[idx] = 0;
                            longhold_emitted[idx] = 0;
                            tap_pending[idx] = 1;
                            if (debug) fprintf(stderr, "[dbg] press start idx=%d t=%.3f\n", idx+1, now);
                            if (idx == 13) {
                                char out[64];
        snprintf(out, sizeof(out), "button %d TAP\n", idx+1);
        if (write(rb_fd, out, strlen(out)) < 0) { close(rb_fd); rb_fd = -1; }
                            }
                        }
                    } else if (release_evt) {
                        char out[200];
                        double held = 0.0;
                        if (down_time[idx] > 0) held = now - down_time[idx];
                        if (debug) fprintf(stderr, "[dbg] release idx=%d held=%.3f\n", idx+1, held);
                        if (idx == 13) {
                            snprintf(out, sizeof(out), "button %d RELEASED\n", idx+1);
                        } else {
                            // Only emit TAP on release if it was a short press; HOLD/LONGHOLD are emitted while pressed.
                            if (held < TAP_THRESHOLD || held < HOLD_THRESHOLD) {
                                snprintf(out, sizeof(out), "button %d TAP\nbutton %d RELEASED\n", idx+1, idx+1);
                            } else {
                                snprintf(out, sizeof(out), "button %d RELEASED\n", idx+1);
                            }
                        }
                        if (write(rb_fd,out,strlen(out))<0) { close(rb_fd); rb_fd=-1; }
                        down_time[idx]=0; hold_emitted[idx]=0; longhold_emitted[idx]=0; tap_pending[idx]=0;
                    }
                } else {
                    // Unknown command, ignore but continue loop
                }
            } else if (r == 0) {
                // emit HOLD if still pressed with no release
                struct timespec ts_now;
                clock_gettime(CLOCK_MONOTONIC, &ts_now);
                double now = (double)ts_now.tv_sec + (double)ts_now.tv_nsec / 1e9;
                for (int i = 0; i < 14; i++) {
                    if (down_time[i] > 0 && tap_pending[i]) {
                        double held = now - down_time[i];
                        if (!hold_emitted[i] && held >= HOLD_THRESHOLD) {
                            char out[128];
                            snprintf(out, sizeof(out), "button %d HOLD (%.2fs)\n", i + 1, held);
                            if (write(rb_fd, out, strlen(out)) < 0) { close(rb_fd); rb_fd = -1; break; }
                            hold_emitted[i] = 1;
                            if (debug) fprintf(stderr, "[dbg] idle HOLD idx=%d held=%.3f\n", i+1, held);
                        } else if (hold_emitted[i] && !longhold_emitted[i] && held >= LONGHOLD_THRESHOLD) {
                            char out[128];
                            snprintf(out, sizeof(out), "button %d LONGHOLD (%.2fs)\n", i + 1, held);
                            if (write(rb_fd, out, strlen(out)) < 0) { close(rb_fd); rb_fd = -1; break; }
                            longhold_emitted[i] = 1;
                            if (debug) fprintf(stderr, "[dbg] idle LONGHOLD idx=%d held=%.3f\n", i+1, held);
                        }
                    }
                }
            } else {
                const wchar_t *err = hid_error(dev);
                if (debug) {
                    fprintf(stderr, "[debug] hid_read_timeout failed: %d (%ls)\n", r, err ? err : L"?");
                } else {
                    fprintf(stderr, "[ulanzi] device disconnected (hid_read_timeout=%d)\n", r);
                }
                notify_rb_event(&rb_fd, "evt disconnected\n");
                hid_close(dev);
                dev = NULL;
                next_reconnect = 0.0;
                // Keep rb_fd open so subscribers (paging_daemon) stay connected and recover after reconnect.
                memset(down_time, 0, sizeof(down_time));
                memset(hold_emitted, 0, sizeof(hold_emitted));
                memset(longhold_emitted, 0, sizeof(longhold_emitted));
                memset(tap_pending, 0, sizeof(tap_pending));
            }
        }

        // auto keep-alive
        time_t now_keep = time(NULL);
        if (now_keep - last_keepalive >= KEEPALIVE_INTERVAL) {
            char buf_time[16];
            struct tm *tm = localtime(&now_keep);
            strftime(buf_time,sizeof(buf_time),"%H:%M:%S",tm);
            char payload[64];
            snprintf(payload,sizeof(payload),"%d|%d|%d|%s|%d",sw_mode,sw_cpu,sw_mem,buf_time,sw_gpu);
            if (dev) {
                if (send_command(dev,0x0006,(uint8_t*)payload,strlen(payload)) < 0) {
                    notify_rb_event(&rb_fd, "evt disconnected\n");
                    hid_close(dev);
                    dev = NULL;
                    next_reconnect = 0.0;
                }
            }
            last_keepalive = now_keep;
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 }; // 5ms
        nanosleep(&ts, NULL);
    }

    if (rb_fd>=0) close(rb_fd);
    close(listen_fd);
    if (dev) hid_close(dev);
    hid_exit();
    unlink(SOCK_PATH);
    return 0;
}

// Convertir les données RGBA en PNG en mémoire et créer le ZIP
static FD_UNUSED int build_zip_from_rgba_icons(const IconItem *items, size_t count, uint8_t **out_buf, size_t *out_len, int *pad_used, size_t *patched_count) {
    *pad_used = 0;
    *patched_count = 0;
    if (count == 0) return -1;
    
    // Créer des IconItem temporaires avec des données PNG
    IconItem *png_items = malloc(count * sizeof(IconItem));
    if (!png_items) return -1;
    memset(png_items, 0, count * sizeof(IconItem));
    
    int success = 1;
    for (size_t i = 0; i < count && success; i++) {
        // Copier les métadonnées
        png_items[i].btn_index = items[i].btn_index;
        png_items[i].name = strdup(items[i].name);
        png_items[i].label = strdup(items[i].label ? items[i].label : "");
        
        // Déterminer la taille de l'image
        int w = (i == 13) ? 392 : 196;  // Bouton 14 plus large
        int h = 196;
        
        // Convertir RGBA en PNG en mémoire
        if (rgba_to_png_memory(items[i].data, w, h, &png_items[i].data, &png_items[i].data_len) != 0) {
            success = 0;
        }
    }
    
    int result = -1;
    if (success) {
        // Utiliser la fonction existante pour créer le ZIP
        result = build_zip_from_icons(png_items, count, out_buf, out_len, pad_used, patched_count);
    }
    
    // Nettoyer
    for (size_t i = 0; i < count; i++) {
        free(png_items[i].name);
        free(png_items[i].label);
        if (png_items[i].data) free(png_items[i].data);
    }
    free(png_items);
    
    return result;
}

// Structure pour gérer l'écriture PNG en mémoire
typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} png_memory_buffer;

// Fonctions callback pour l'écriture en mémoire
static void png_write_data_to_memory(png_structp png_ptr, png_bytep data, png_size_t length) {
    png_memory_buffer *buffer = (png_memory_buffer*)png_get_io_ptr(png_ptr);
    if (buffer->length + length > buffer->capacity) {
        buffer->capacity = buffer->length + length + 4096;  // +4KB de marge
        buffer->data = realloc(buffer->data, buffer->capacity);
    }
    if (buffer->data) {
        memcpy(buffer->data + buffer->length, data, length);
        buffer->length += length;
    }
}

static void png_flush_data_to_memory(png_structp png_ptr) {
    (void)png_ptr;
    // Rien à faire pour l'écriture en mémoire
}

// Convertir les données RGBA en PNG en mémoire
static int rgba_to_png_memory(const uint8_t *rgba, int w, int h, uint8_t **png_data, size_t *png_len) {
    png_memory_buffer buffer = {0};
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return -1;
    
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); return -1; }
    
    if (setjmp(png_jmpbuf(png))) { 
        png_destroy_write_struct(&png, &info); 
        if (buffer.data) free(buffer.data);
        return -1; 
    }
    
    png_set_write_fn(png, &buffer, png_write_data_to_memory, png_flush_data_to_memory);
    
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, 
                PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_set_compression_level(png, Z_BEST_SPEED);
    png_set_filter(png, 0, PNG_ALL_FILTERS);
    
    png_bytep *rows = malloc(sizeof(png_bytep) * h);
    if (!rows) { 
        png_destroy_write_struct(&png, &info); 
        if (buffer.data) free(buffer.data);
        return -1; 
    }
    
    for (int y = 0; y < h; y++) {
        rows[y] = (png_bytep)(rgba + y * w * 4);
    }
    
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    
    free(rows);
    png_destroy_write_struct(&png, &info);
    
    *png_data = buffer.data;
    *png_len = buffer.length;
    return 0;
}
