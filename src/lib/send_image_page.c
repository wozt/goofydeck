#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <png.h>
#include <zlib.h>
#include <pthread.h>
#define SOCK_PATH "/tmp/ulanzi_device.sock"

// Silence intentional unused warnings for static helpers kept for future refactors.
#if defined(__GNUC__) || defined(__clang__)
#define FD_UNUSED __attribute__((unused))
#else
#define FD_UNUSED
#endif

// Structure for parallel PNG conversion tasks
typedef struct {
    const uint8_t *rgba_data;
    int w, h;
    uint8_t *png_data;
    size_t png_len;
    int tile_id;
    int status; // 0 = pending, 1 = completed, -1 = error
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} tile_task_t;

// Structure for thread pool
typedef struct {
    tile_task_t *tasks;
    int task_count;
    int next_task;
    int completed_tasks;
    int total_tasks;
    pthread_t *threads;
    int thread_count;
    pthread_mutex_t work_mutex;
    pthread_cond_t work_cond;
    pthread_cond_t done_cond;
    int shutdown;
} thread_pool_t;

// --- PNG helpers ---
static int read_png_rgba(const char *path, uint8_t **out, int *w, int *h) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return -1; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return -1; }
    png_init_io(png, fp);
    png_read_info(png, info);
    int width = png_get_image_width(png, info);
    int height = png_get_image_height(png, info);
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_gray_to_rgb(png);
    png_read_update_info(png, info);
    size_t rowbytes = png_get_rowbytes(png, info);
    uint8_t *data = malloc(rowbytes * height);
    if (!data) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return -1; }
    png_bytep *rows = malloc(sizeof(png_bytep) * height);
    if (!rows) { free(data); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return -1; }
    for (int y=0;y<height;y++) rows[y]=data + y*rowbytes;
    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    *out = data; *w = width; *h = height;
    return 0;
}

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

static uint8_t *crop_rgba(const uint8_t *src, int sw, int sh, int x0, int y0, int cw, int ch) {
    (void)sh;
    uint8_t *dst = malloc((size_t)cw*ch*4);
    if (!dst) return NULL;
    for (int y=0;y<ch;y++) {
        const uint8_t *p = src + ((size_t)(y0+y)*sw + x0)*4;
        memcpy(dst + (size_t)y*cw*4, p, (size_t)cw*4);
    }
    return dst;
}

static uint8_t *resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh) {
    uint8_t *dst = malloc((size_t)dw*dh*4);
    if (!dst) return NULL;
    
    double sx = (double)sw / dw;
    double sy = (double)sh / dh;
    
    for (int y=0;y<dh;y++) {
        for (int x=0;x<dw;x++) {
            double src_x = x * sx;
            double src_y = y * sy;
            int x0 = (int)src_x;
            int y0 = (int)src_y;
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
            
            double fx = src_x - x0;
            double fy = src_y - y0;
            
            for (int c=0;c<4;c++) {
                uint8_t p00 = src[((size_t)y0*sw + x0)*4 + c];
                uint8_t p01 = src[((size_t)y0*sw + x1)*4 + c];
                uint8_t p10 = src[((size_t)y1*sw + x0)*4 + c];
                uint8_t p11 = src[((size_t)y1*sw + x1)*4 + c];
                
                double val = p00*(1-fx)*(1-fy) + p01*fx*(1-fy) + p10*(1-fx)*fy + p11*fx*fy;
                dst[((size_t)y*dw + x)*4 + c] = (uint8_t)(val + 0.5);
            }
        }
    }
    return dst;
}

static uint8_t *ensure_16_9_crop(const uint8_t *src, int sw, int sh, int *out_w, int *out_h) {
    double aspect = (double)sw / sh;
    double target_aspect = 16.0 / 9.0;
    
    uint8_t *cropped = NULL;
    int crop_w = sw, crop_h = sh;
    
    if (fabs(aspect - target_aspect) < 0.01) {
        // Already 16:9, just copy
        uint8_t *copy = malloc((size_t)sw*sh*4);
        if (copy) memcpy(copy, src, (size_t)sw*sh*4);
        cropped = copy;
        crop_w = sw;
        crop_h = sh;
    } else if (aspect > target_aspect) {
        // Too wide, crop sides
        crop_w = (int)(sh * target_aspect);
        int crop_x = (sw - crop_w) / 2;
        cropped = crop_rgba(src, sw, sh, crop_x, 0, crop_w, sh);
    } else {
        // Too tall, crop top/bottom
        crop_h = (int)(sw / target_aspect);
        int crop_y = (sh - crop_h) / 2;
        cropped = crop_rgba(src, sw, sh, 0, crop_y, sw, crop_h);
    }
    
    if (cropped) {
        *out_w = crop_w;
        *out_h = crop_h;
    }
    
    return cropped;
}

static FD_UNUSED uint8_t *ensure_16_9_then_resize(const uint8_t *src, int sw, int sh, int *out_w, int *out_h) {
    double aspect = (double)sw / sh;
    double target_aspect = 16.0 / 9.0;
    
    uint8_t *cropped = NULL;
    int crop_w = sw, crop_h = sh;
    
    if (fabs(aspect - target_aspect) < 0.01) {
        // Already 16:9, just copy
        uint8_t *copy = malloc((size_t)sw*sh*4);
        if (copy) memcpy(copy, src, (size_t)sw*sh*4);
        cropped = copy;
    } else if (aspect > target_aspect) {
        // Too wide, crop sides
        crop_w = (int)(sh * target_aspect);
        int crop_x = (sw - crop_w) / 2;
        cropped = crop_rgba(src, sw, sh, crop_x, 0, crop_w, sh);
    } else {
        // Too tall, crop top/bottom
        crop_h = (int)(sw / target_aspect);
        int crop_y = (sh - crop_h) / 2;
        cropped = crop_rgba(src, sw, sh, 0, crop_y, sw, crop_h);
    }
    
    if (!cropped) return NULL;
    
    uint8_t *resized = resize_rgba(cropped, crop_w, crop_h, 1280, 720);
    free(cropped);
    
    if (resized) {
        *out_w = 1280;
        *out_h = 720;
    }
    
    return resized;
}

// quantize to 8 colors using 5-bit buckets and top8 palette
static FD_UNUSED void quantize8(uint8_t *img, int w, int h) {
    const int buckets = 32768; // 5 bits per channel
    uint32_t *count = calloc(buckets, sizeof(uint32_t));
    uint64_t *sr = calloc(buckets, sizeof(uint64_t));
    uint64_t *sg = calloc(buckets, sizeof(uint64_t));
    uint64_t *sb = calloc(buckets, sizeof(uint64_t));
    uint32_t *count_copy = NULL;
    if (!count || !sr || !sg || !sb) { free(count); free(sr); free(sg); free(sb); return; }
    int pixels = w*h;
    for (int i=0;i<pixels;i++) {
        uint8_t *p = img + i*4;
        int idx = ((p[0]>>3)<<10) | ((p[1]>>3)<<5) | (p[2]>>3);
        count[idx]++; sr[idx]+=p[0]; sg[idx]+=p[1]; sb[idx]+=p[2];
    }
    count_copy = malloc(sizeof(uint32_t)*buckets);
    if (!count_copy) { free(count); free(sr); free(sg); free(sb); return; }
    memcpy(count_copy, count, sizeof(uint32_t)*buckets);

    int palette_idx[8]; memset(palette_idx, -1, sizeof(palette_idx));
    for (int k=0;k<8;k++) {
        uint32_t best=0; int best_i=-1;
        for (int i=0;i<buckets;i++) {
            if (count_copy[i]>best) { best=count_copy[i]; best_i=i; }
        }
        if (best_i<0 || best==0) { palette_idx[k]=0; continue; }
        palette_idx[k]=best_i; count_copy[best_i]=0;
    }
    uint8_t palette[8][3];
    for (int k=0;k<8;k++) {
        int idx = palette_idx[k];
        uint32_t cnt = count[idx];
        if (cnt==0) { palette[k][0]=palette[k][1]=palette[k][2]=0; }
        else {
            palette[k][0]=(uint8_t)(sr[idx]/cnt);
            palette[k][1]=(uint8_t)(sg[idx]/cnt);
            palette[k][2]=(uint8_t)(sb[idx]/cnt);
        }
    }
    for (int i=0;i<pixels;i++) {
        uint8_t *p = img + i*4;
        int best_k = 0;
        int best_dist = 2147483647;
        for (int k=0;k<8;k++) {
            int dr = (int)p[0] - (int)palette[k][0];
            int dg = (int)p[1] - (int)palette[k][1];
            int db = (int)p[2] - (int)palette[k][2];
            int dist = dr*dr + dg*dg + db*db;
            if (dist < best_dist) { best_dist = dist; best_k = k; }
        }
        p[0]=palette[best_k][0];
        p[1]=palette[best_k][1];
        p[2]=palette[best_k][2];
    }
    free(count); free(sr); free(sg); free(sb); free(count_copy);
}

static int send_cmd(const char *line) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    if (connect(fd,(struct sockaddr*)&addr,sizeof(addr))<0) { close(fd); return -1; }
    if (write(fd, line, strlen(line))<0) { close(fd); return -1; }
    if (write(fd, "\n", 1)<0) { close(fd); return -1; }
    char buf[64]; ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n > 0) { buf[n]='\0'; if (strncmp(buf,"ok",2)==0) return 0; }
    return -1;
}

static void unique_tag(char *out, size_t cap) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(out, cap, "%ld%06ld", (long)ts.tv_sec, ts.tv_nsec/1000);
}

// Structure for processing options
typedef struct {
    int optimize_input;  // Optimize input image (dithering before quantization)
    int dither;         // Enable Floyd-Steinberg dithering
    int compress;         // Enable PNG compression
    int colors;           // Number of colors for quantization (8 or 16)
    int tile_optimize;    // Tile optimization (default: true)
    int buffer_mode;      // Send data directly to daemon (no files)
    int icon_size;        // Reference icon size (calculated dynamically)
    int quality_percent;   // Quality percentage (100=original size, 50=half, default: 100)
    int magnify_percent;   // Magnification percentage (100=normal, 200=2x, default: 100)
    char *keep_folder;     // Folder to copy icons to (-k/--keep-icons, NULL = disabled)
    char *filename_prefix; // Prefix for filenames (NULL = "icon")
} process_options_t;

// Function declarations
static void quantize_to_256_colors(uint8_t *img, int w, int h);
static void optimize_input_image(uint8_t *img, int w, int h);
static void apply_dithering(uint8_t *img, int w, int h);
static int write_png_8bit(const char *path, const uint8_t *data, int w, int h);
static int send_rgba_data_direct(const uint8_t *tiles[14], const int tile_w[14], const int tile_h[14]);

// Function to copy generated icons to a folder
static void copy_icons_to_folder(const uint8_t *tiles[14], const int tile_w[14], const int tile_h[14], const char *folder, const char *filename_prefix) {
    if (!folder) return;
    
    // Créer le dossier s'il n'existe pas
    char mkdir_cmd[PATH_MAX];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", folder);
    system(mkdir_cmd);
    
    // Déterminer le préfixe de nom de fichier
    const char *prefix = filename_prefix ? filename_prefix : "icon";
    
    // Copy each icon
    for (int i = 0; i < 14; i++) {
        char filename[PATH_MAX];
        snprintf(filename, sizeof(filename), "%s/%s-%d.png", folder, prefix, i + 1);
        write_png_rgba(filename, tiles[i], tile_w[i], tile_h[i]);
    }
}

// Structure for parallel PNG writing tasks (file mode)
typedef struct {
    const uint8_t *rgba_data;
    int w, h;
    char filepath[PATH_MAX];
    int tile_id;
    int status; // 0 = pending, 1 = completed, -1 = error
} png_write_task_t;

// Structure for PNG writing thread pool
typedef struct {
    png_write_task_t *tasks;
    int task_count;
    int next_task;
    int completed_tasks;
    int total_tasks;
    pthread_t *threads;
    int thread_count;
    pthread_mutex_t work_mutex;
    pthread_cond_t work_cond;
    pthread_cond_t done_cond;
    int shutdown;
} png_write_pool_t;

// Function to resize an icon
static uint8_t* resize_icon(const uint8_t *src, int src_w, int src_h, int dst_w, int dst_h) {
    if (src_w == dst_w && src_h == dst_h) {
        uint8_t *copy = malloc((size_t)dst_w * dst_h * 4);
        if (copy) memcpy(copy, src, (size_t)dst_w * dst_h * 4);
        return copy;
    }
    
    uint8_t *dst = malloc((size_t)dst_w * dst_h * 4);
    if (!dst) return NULL;
    
    // Simple bilinear resizing
    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            float src_x = (float)x * src_w / dst_w;
            float src_y = (float)y * src_h / dst_h;
            
            int x0 = (int)src_x;
            int y0 = (int)src_y;
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            
            if (x1 >= src_w) x1 = src_w - 1;
            if (y1 >= src_h) y1 = src_h - 1;
            
            float fx = src_x - x0;
            float fy = src_y - y0;
            
            for (int c = 0; c < 4; c++) {
                uint8_t p00 = src[(y0 * src_w + x0) * 4 + c];
                uint8_t p01 = src[(y0 * src_w + x1) * 4 + c];
                uint8_t p10 = src[(y1 * src_w + x0) * 4 + c];
                uint8_t p11 = src[(y1 * src_w + x1) * 4 + c];
                
                uint8_t val = (uint8_t)(
                    p00 * (1 - fx) * (1 - fy) +
                    p01 * fx * (1 - fy) +
                    p10 * (1 - fx) * fy +
                    p11 * fx * fy
                );
                
                dst[(y * dst_w + x) * 4 + c] = val;
            }
        }
    }
    
    return dst;
}

// PNG writing in 8-bit format with 256 color palette
static FD_UNUSED int write_png_8bit(const char *path, const uint8_t *data, int w, int h) {
    FILE *fp = fopen(path, "wb"); 
    if (!fp) return -1;
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }
    
    if (setjmp(png_jmpbuf(png))) { 
        png_destroy_write_struct(&png, &info); 
        fclose(fp); 
        return -1; 
    }
    
    png_init_io(png, fp);
    png_set_compression_level(png, 9);  // Maximum compression
    
    // Create 256 color palette (6x6x6 + gray)
    png_color palette[256];
    int idx = 0;
    
    // Palette 6x6x6 = 216 couleurs
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                palette[idx].red = (r * 255) / 5;
                palette[idx].green = (g * 255) / 5;
                palette[idx].blue = (b * 255) / 5;
                idx++;
            }
        }
    }
    
    // Gray levels (last 40 entries)
    for (int i = 0; i < 40; i++) {
        int gray = (i * 255) / 39;
        palette[idx].red = palette[idx].green = palette[idx].blue = gray;
        idx++;
    }
    
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE, 
                PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_set_PLTE(png, info, palette, 256);
    
    png_bytep *rows = malloc(sizeof(png_bytep) * h); 
    if (!rows) { 
        png_destroy_write_struct(&png, &info); 
        fclose(fp); 
        return -1; 
    }
    
    // Convert RGBA to palette indices
    png_bytep indexed_data = malloc(w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = data + (y * w + x) * 4;
            uint8_t r = p[0], g = p[1], b = p[2];
            
            // Determine if it's a gray level
            int diff_rg = abs(r - g);
            int diff_rb = abs(r - b);
            int diff_gb = abs(g - b);
            
            if (diff_rg < 30 && diff_rb < 30 && diff_gb < 30) {
                // Gray index (216-255)
                int gray = (r + g + b) / 3;
                indexed_data[y * w + x] = 216 + (gray * 39 / 255);
            } else {
                // Index in 6x6x6 grid
                int ri = (r * 5) / 255;
                int gi = (g * 5) / 255;
                int bi = (b * 5) / 255;
                indexed_data[y * w + x] = ri * 36 + gi * 6 + bi;
            }
        }
        rows[y] = indexed_data + y * w;
    }
    
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    
    free(rows);
    free(indexed_data);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

// Structure pour gérer l'écriture PNG en mémoire (thread-safe)
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

// Conversion RGBA en PNG en mémoire optimisée pour les threads
static int rgba_to_png_memory_thread(const uint8_t *rgba, int w, int h, uint8_t **png_data, size_t *png_len) {
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
    png_set_compression_level(png, Z_BEST_SPEED);  // Rapide mais compressé
    png_set_filter(png, 0, PNG_FILTER_NONE);  // Pas de filtres coûteux
    
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

// Fonction worker pour les threads
static void* worker_thread(void *arg) {
    thread_pool_t *pool = (thread_pool_t*)arg;
    
    while (!pool->shutdown) {
        pthread_mutex_lock(&pool->work_mutex);
        
        // Attendre une tâche
        while (pool->next_task >= pool->total_tasks && !pool->shutdown) {
            pthread_cond_wait(&pool->work_cond, &pool->work_mutex);
        }
        
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->work_mutex);
            break;
        }
        
        // Prendre une tâche
        int task_id = pool->next_task++;
        pthread_mutex_unlock(&pool->work_mutex);
        
        // Exécuter la conversion PNG
        tile_task_t *task = &pool->tasks[task_id];
        task->status = rgba_to_png_memory_thread(task->rgba_data, task->w, task->h, 
                                                &task->png_data, &task->png_len);
        
        // Signaler la completion
        pthread_mutex_lock(&pool->work_mutex);
        pool->completed_tasks++;
        if (pool->completed_tasks == pool->total_tasks) {
            pthread_cond_signal(&pool->done_cond);
        }
        pthread_mutex_unlock(&pool->work_mutex);
    }
    
    return NULL;
}

// Initialiser le thread pool
static int thread_pool_init(thread_pool_t *pool, int thread_count) {
    pool->tasks = NULL;
    pool->task_count = 0;
    pool->next_task = 0;
    pool->completed_tasks = 0;
    pool->total_tasks = 0;
    pool->thread_count = thread_count;
    pool->shutdown = 0;
    
    pool->threads = malloc(sizeof(pthread_t) * thread_count);
    if (!pool->threads) return -1;
    
    if (pthread_mutex_init(&pool->work_mutex, NULL) != 0) {
        free(pool->threads);
        return -1;
    }
    
    if (pthread_cond_init(&pool->work_cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->work_mutex);
        free(pool->threads);
        return -1;
    }
    
    if (pthread_cond_init(&pool->done_cond, NULL) != 0) {
        pthread_cond_destroy(&pool->work_cond);
        pthread_mutex_destroy(&pool->work_mutex);
        free(pool->threads);
        return -1;
    }
    
    // Créer les threads
    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work_cond);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_cond_destroy(&pool->done_cond);
            pthread_cond_destroy(&pool->work_cond);
            pthread_mutex_destroy(&pool->work_mutex);
            free(pool->threads);
            return -1;
        }
    }
    
    return 0;
}

// Exécuter les tâches en parallèle
static int thread_pool_execute(thread_pool_t *pool, tile_task_t *tasks, int task_count) {
    pool->tasks = tasks;
    pool->task_count = task_count;
    pool->next_task = 0;
    pool->completed_tasks = 0;
    pool->total_tasks = task_count;
    
    // Réveiller les threads
    pthread_mutex_lock(&pool->work_mutex);
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);
    
    // Attendre la completion
    pthread_mutex_lock(&pool->work_mutex);
    while (pool->completed_tasks < task_count) {
        pthread_cond_wait(&pool->done_cond, &pool->work_mutex);
    }
    pthread_mutex_unlock(&pool->work_mutex);
    
    return 0;
}

// Fonction worker pour l'écriture PNG
static void* png_write_worker_thread(void *arg) {
    png_write_pool_t *pool = (png_write_pool_t*)arg;
    
    while (!pool->shutdown) {
        pthread_mutex_lock(&pool->work_mutex);
        
        // Attendre une tâche
        while (pool->next_task >= pool->total_tasks && !pool->shutdown) {
            pthread_cond_wait(&pool->work_cond, &pool->work_mutex);
        }
        
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->work_mutex);
            break;
        }
        
        // Prendre une tâche
        int task_id = pool->next_task++;
        pthread_mutex_unlock(&pool->work_mutex);
        
        // Exécuter l'écriture PNG (utiliser write_png_rgba comme le mode fichiers original)
        png_write_task_t *task = &pool->tasks[task_id];
        task->status = write_png_rgba(task->filepath, task->rgba_data, task->w, task->h);
        
        // Signaler la completion
        pthread_mutex_lock(&pool->work_mutex);
        pool->completed_tasks++;
        if (pool->completed_tasks == pool->total_tasks) {
            pthread_cond_signal(&pool->done_cond);
        }
        pthread_mutex_unlock(&pool->work_mutex);
    }
    
    return NULL;
}

// Initialiser le thread pool d'écriture PNG
static int png_write_pool_init(png_write_pool_t *pool, int thread_count) {
    pool->tasks = NULL;
    pool->task_count = 0;
    pool->next_task = 0;
    pool->completed_tasks = 0;
    pool->total_tasks = 0;
    pool->thread_count = thread_count;
    pool->shutdown = 0;
    
    pool->threads = malloc(sizeof(pthread_t) * thread_count);
    if (!pool->threads) return -1;
    
    if (pthread_mutex_init(&pool->work_mutex, NULL) != 0) {
        free(pool->threads);
        return -1;
    }
    
    if (pthread_cond_init(&pool->work_cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->work_mutex);
        free(pool->threads);
        return -1;
    }
    
    if (pthread_cond_init(&pool->done_cond, NULL) != 0) {
        pthread_cond_destroy(&pool->work_cond);
        pthread_mutex_destroy(&pool->work_mutex);
        free(pool->threads);
        return -1;
    }
    
    // Créer les threads
    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, png_write_worker_thread, pool) != 0) {
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work_cond);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_cond_destroy(&pool->done_cond);
            pthread_cond_destroy(&pool->work_cond);
            pthread_mutex_destroy(&pool->work_mutex);
            free(pool->threads);
            return -1;
        }
    }
    
    return 0;
}

// Exécuter les tâches d'écriture en parallèle
static int png_write_pool_execute(png_write_pool_t *pool, png_write_task_t *tasks, int task_count) {
    pool->tasks = tasks;
    pool->task_count = task_count;
    pool->next_task = 0;
    pool->completed_tasks = 0;
    pool->total_tasks = task_count;
    
    // Réveiller les threads
    pthread_mutex_lock(&pool->work_mutex);
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);
    
    // Attendre la completion
    pthread_mutex_lock(&pool->work_mutex);
    while (pool->completed_tasks < task_count) {
        pthread_cond_wait(&pool->done_cond, &pool->work_mutex);
    }
    pthread_mutex_unlock(&pool->work_mutex);
    
    return 0;
}

// Détruire le thread pool d'écriture PNG
static void png_write_pool_destroy(png_write_pool_t *pool) {
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_cond);
    
    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    pthread_cond_destroy(&pool->done_cond);
    pthread_cond_destroy(&pool->work_cond);
    pthread_mutex_destroy(&pool->work_mutex);
    free(pool->threads);
}

// Détruire le thread pool (conversion mémoire)
static void thread_pool_destroy(thread_pool_t *pool) {
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_cond);
    
    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    pthread_cond_destroy(&pool->done_cond);
    pthread_cond_destroy(&pool->work_cond);
    pthread_mutex_destroy(&pool->work_mutex);
    free(pool->threads);
}

// Envoyer les données RGBA directement au démon via socket (optimisé avec threads)
static int send_rgba_data_direct(const uint8_t *tiles[14], const int tile_w[14], const int tile_h[14]) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    // Envoyer la commande
    const char *cmd = "set-buttons-explicit-14-data\n";
    if (write(sock, cmd, strlen(cmd)) < 0) {
        perror("write cmd");
        close(sock);
        return -1;
    }
    
    // Flush pour s'assurer que la commande est envoyée avant les données
    fsync(sock);
    sleep(1); // 1 seconde pour laisser le démon traiter la commande
    
    // Initialiser le thread pool (utiliser 4 threads = nombre de coeurs CPU typique)
    thread_pool_t pool;
    if (thread_pool_init(&pool, 4) != 0) {
        close(sock);
        return -1;
    }
    
    // Préparer les tâches de conversion PNG
    tile_task_t tasks[14];
    for (int i = 0; i < 14; i++) {
        tasks[i].rgba_data = tiles[i];
        tasks[i].w = tile_w[i];
        tasks[i].h = tile_h[i];
        tasks[i].png_data = NULL;
        tasks[i].png_len = 0;
        tasks[i].tile_id = i;
        tasks[i].status = 0;
    }
    
    // Exécuter les conversions en parallèle
    // printf("Conversion PNG en parallèle avec %d threads...\n", pool.thread_count);
    if (thread_pool_execute(&pool, tasks, 14) != 0) {
        thread_pool_destroy(&pool);
        close(sock);
        return -1;
    }
    
    // Envoyer chaque tuile PNG avec sa taille
    for (int i = 0; i < 14; i++) {
        if (tasks[i].status != 0) {
            fprintf(stderr, "Erreur conversion tuile %d\n", i);
            continue;
        }
        
        uint32_t png_size = tasks[i].png_len;
        
        // Envoyer la taille (4 bytes, big-endian)
        uint8_t size_buf[4];
        size_buf[0] = (png_size >> 24) & 0xff;
        size_buf[1] = (png_size >> 16) & 0xff;
        size_buf[2] = (png_size >> 8) & 0xff;
        size_buf[3] = png_size & 0xff;
        
        if (write(sock, size_buf, 4) < 0) {
            perror("write size");
            close(sock);
            thread_pool_destroy(&pool);
            return -1;
        }
        
        // Envoyer les données PNG
        size_t sent = 0;
        while (sent < png_size) {
            ssize_t n = write(sock, tasks[i].png_data + sent, png_size - sent);
            if (n <= 0) {
                perror("write data");
                close(sock);
                thread_pool_destroy(&pool);
                return -1;
            }
            sent += n;
        }
    }
    
    // Nettoyer les ressources
    for (int i = 0; i < 14; i++) {
        if (tasks[i].png_data) {
            free(tasks[i].png_data);
        }
    }
    thread_pool_destroy(&pool);
    
    // Lire la réponse
    char response[8];
    ssize_t resp_len = read(sock, response, sizeof(response) - 1);
    if (resp_len > 0) {
        response[resp_len] = '\0';
        printf("Réponse du démon: %s\n", response);
    } else {
        perror("read response");
    }
    
    close(sock);
    return 0;
}

// Fonction de dithering Floyd-Steinberg pour réduire les bandes de couleurs
static void apply_dithering(uint8_t *img, int w, int h) {
    // Matrice de Floyd-Steinberg pour répartir l'erreur de quantification
    // Les coefficients sont: 7/16 droite, 3/16 bas-gauche, 5/16 bas, 1/16 bas-droite
    const int step = 255 / 5; // Pour quantification 6x6x6
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *pixel = img + (y * w + x) * 4;
            
            // Pour chaque canal RGB
            for (int c = 0; c < 3; c++) {
                int old_val = pixel[c];
                // Quantifier selon la grille 6x6x6 (pas noir/blanc !)
                int new_val = (old_val / step) * step;
                int error = old_val - new_val;
                pixel[c] = new_val;
                
                // Propager l'erreur aux pixels voisins
                if (x + 1 < w) {
                    uint8_t *right = img + (y * w + (x + 1)) * 4;
                    right[c] = (uint8_t)fmax(0, fmin(255, right[c] + error * 7 / 16));
                }
                if (y + 1 < h) {
                    if (x > 0) {
                        uint8_t *bottom_left = img + ((y + 1) * w + (x - 1)) * 4;
                        bottom_left[c] = (uint8_t)fmax(0, fmin(255, bottom_left[c] + error * 3 / 16));
                    }
                    uint8_t *bottom = img + ((y + 1) * w + x) * 4;
                    bottom[c] = (uint8_t)fmax(0, fmin(255, bottom[c] + error * 5 / 16));
                    if (x + 1 < w) {
                        uint8_t *bottom_right = img + ((y + 1) * w + (x + 1)) * 4;
                        bottom_right[c] = (uint8_t)fmax(0, fmin(255, bottom_right[c] + error / 16));
                    }
                }
            }
        }
    }
}

// Optimisation de l'image input: juste conversion en PNG-8
static void optimize_input_image(uint8_t *img, int w, int h) {
    // Pas d'optimisation de contraste - juste quantification en 256 couleurs
    // Quantifier l'image en 256 couleurs (palette 6x6x6)
    quantize_to_256_colors(img, w, h);
}

// Quantification à 256 couleurs (palette 6x6x6)
static void quantize_to_256_colors(uint8_t *img, int w, int h) {
    // Palette 6x6x6 = 216 couleurs (pas de détection de gris)
    const int levels = 6;
    const int step = 255 / (levels - 1);
    
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = img + i * 4;
        uint8_t r = p[0], g = p[1], b = p[2];
        
        // TOUTES les couleurs passent par la grille 6x6x6
        // Pas de détection de gris pour éviter les problèmes
        p[0] = (r / step) * step;  // Rouge : 0-5
        p[1] = (g / step) * step;  // Vert : 0-5  
        p[2] = (b / step) * step;  // Bleu : 0-5
        // Alpha reste inchangé
    }
}

// Version modifiée de quantize8 pour supporter différents nombres de couleurs
static void quantize_colors(uint8_t *img, int w, int h, int colors) {
    const int buckets = 32768; // 5 bits per channel
    uint32_t *count = calloc(buckets, sizeof(uint32_t));
    uint64_t *sr = calloc(buckets, sizeof(uint64_t));
    uint64_t *sg = calloc(buckets, sizeof(uint64_t));
    uint64_t *sb = calloc(buckets, sizeof(uint64_t));
    uint32_t *count_copy = NULL;
    
    if (!count || !sr || !sg || !sb) { 
        free(count); free(sr); free(sg); free(sb); 
        return; 
    }
    
    int pixels = w * h;
    
    // Compter les couleurs dans l'image
    for (int i = 0; i < pixels; i++) {
        uint8_t *p = img + i * 4;
        int idx = ((p[0]>>3)<<10) | ((p[1]>>3)<<5) | (p[2]>>3);
        count[idx]++; 
        sr[idx] += p[0]; 
        sg[idx] += p[1]; 
        sb[idx] += p[2];
    }
    
    count_copy = malloc(sizeof(uint32_t) * buckets);
    if (!count_copy) { 
        free(count); free(sr); free(sg); free(sb); 
        return; 
    }
    memcpy(count_copy, count, sizeof(uint32_t) * buckets);

    // Extraire les couleurs les plus fréquentes
    int palette_idx[64]; // Support jusqu'à 64 couleurs
    memset(palette_idx, -1, sizeof(palette_idx));
    
    for (int k = 0; k < colors; k++) {
        uint32_t best = 0; 
        int best_i = -1;
        for (int i = 0; i < buckets; i++) {
            if (count_copy[i] > best) { 
                best = count_copy[i]; 
                best_i = i; 
            }
        }
        if (best_i < 0 || best == 0) { 
            palette_idx[k] = 0; 
            continue; 
        }
        palette_idx[k] = best_i; 
        count_copy[best_i] = 0;
    }
    
    // Construire la palette
    uint8_t palette[64][3]; // Support jusqu'à 64 couleurs
    for (int k = 0; k < colors; k++) {
        int idx = palette_idx[k];
        uint32_t cnt = count[idx];
        if (cnt == 0) { 
            palette[k][0] = palette[k][1] = palette[k][2] = 0; 
        } else {
            palette[k][0] = (uint8_t)(sr[idx] / cnt);
            palette[k][1] = (uint8_t)(sg[idx] / cnt);
            palette[k][2] = (uint8_t)(sb[idx] / cnt);
        }
    }
    
    // Appliquer la palette à chaque pixel
    for (int i = 0; i < pixels; i++) {
        uint8_t *p = img + i * 4;
        int best_k = 0;
        int best_dist = 2147483647;
        
        // Trouver la couleur la plus proche dans la palette
        for (int k = 0; k < colors; k++) {
            int dr = (int)p[0] - (int)palette[k][0];
            int dg = (int)p[1] - (int)palette[k][1];
            int db = (int)p[2] - (int)palette[k][2];
            int dist = dr*dr + dg*dg + db*db;
            if (dist < best_dist) { 
                best_dist = dist; 
                best_k = k; 
            }
        }
        
        p[0] = palette[best_k][0];
        p[1] = palette[best_k][1];
        p[2] = palette[best_k][2];
    }
    
    free(count); free(sr); free(sg); free(sb); free(count_copy);
}

// Version modifiée de write_png_rgba avec support de compression
static FD_UNUSED int write_png_rgba_compressed(const char *path, const uint8_t *data, int w, int h, int compress_level) {
    FILE *fp = fopen(path, "wb"); 
    if (!fp) return -1;
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }
    
    if (setjmp(png_jmpbuf(png))) { 
        png_destroy_write_struct(&png, &info); 
        fclose(fp); 
        return -1; 
    }
    
    png_init_io(png, fp);
    
    // Niveau de compression: 0 (aucune) à 9 (maximale)
    png_set_compression_level(png, compress_level);
    
    // Désactiver les filtres pour une meilleure compression si demandé
    if (compress_level >= 6) {
        png_set_filter(png, 0, PNG_FILTER_NONE);
    } else {
        png_set_filter(png, 0, PNG_ALL_FILTERS);
    }
    
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, 
                PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    
    png_bytep *rows = malloc(sizeof(png_bytep) * h); 
    if (!rows) { 
        png_destroy_write_struct(&png, &info); 
        fclose(fp); 
        return -1; 
    }
    
    for (int y = 0; y < h; y++) {
        rows[y] = (png_bytep)(data + (size_t)y * w * 4);
    }
    
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    
    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

// Fonction pour afficher l'aide
static void show_help(const char *prog_name) {
    printf("Usage: %s [OPTIONS] <image.png>\n", prog_name);
    printf("\nOptions:\n");
    printf("  -o, --optimize-input    Optimiser l'image input (contraste, netteté)\n");
    printf("  -d, --dither          Appliquer dithering Floyd-Steinberg\n");
    printf("  -z, --compress        Activer compression PNG\n");
    printf("  -c, --colors=N        Nombre de couleurs (8, 16, 32 ou 64, défaut: 8)\n");
    printf("  -q, --quality=PCT    Qualité des icônes en pourcentage (10-100, défaut: 100)\n");
    printf("  -m, --magnify=PCT   Magnification des icônes en pourcentage (50-300, défaut: 100)\n");
    printf("  -k, --keep-icons=F[=P]   Copier les icônes générées dans le dossier F [avec préfixe P]\n");
    printf("  --no-tile-optimize    Désactiver optimisation des tuiles\n");
    printf("  -b, --buffer            Envoie les données directement au démon (plus rapide)\n");
    printf("  -h, --help            Afficher cette aide\n");
    printf("\nExemples:\n");
    printf("  %s image.png                           # Comportement par défaut\n", prog_name);
    printf("  %s -q=50 image.png                    # Icônes à 50%% de la taille\n", prog_name);
    printf("  %s -m=150 image.png                   # Icônes 1.5x plus grandes\n", prog_name);
    printf("  %s -z -c=16 image.png                  # Compression + 16 couleurs\n", prog_name);
    printf("  %s -o -z -c=32 image.png               # Optimisation input + compression + 32 couleurs\n", prog_name);
    printf("  %s -o -d -z -c=64 image.png            # Toutes les options + 64 couleurs\n", prog_name);
    printf("  %s -m=128 image.png                    # Magnifier les icônes en 128x128\n", prog_name);
    printf("  %s -k=icons image.png                  # Copier les icônes dans le dossier 'icons'\n", prog_name);
    printf("  %s -k=icons=mybutton image.png         # Copier avec préfixe 'mybutton'\n", prog_name);
    printf("  %s --optimize-input --dither --compress --colors=32 image.png\n", prog_name);
}

// Fonction pour copier les icônes depuis des fichiers temporaires déjà redimensionnés
static void copy_icons_from_files(const png_write_task_t *write_tasks, int count, const char *folder, const char *filename_prefix) {
    if (!folder) return;
    
    // Créer le dossier s'il n'existe pas
    char mkdir_cmd[PATH_MAX];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", folder);
    system(mkdir_cmd);
    
    // Déterminer le préfixe de nom de fichier
    const char *prefix = filename_prefix ? filename_prefix : "icon";
    
    // Copier chaque fichier temporaire vers le dossier de destination
    for (int i = 0; i < count; i++) {
        if (write_tasks[i].status != 0) continue;
        
        char dst_filename[PATH_MAX];
        snprintf(dst_filename, sizeof(dst_filename), "%s/%s-%d.png", folder, prefix, i + 1);
        
        // Copier le fichier temporaire vers la destination
        char cp_cmd[PATH_MAX * 2];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp \"%s\" \"%s\"", write_tasks[i].filepath, dst_filename);
        system(cp_cmd);
    }
}

int main(int argc, char **argv) {
    // Initialiser les options par défaut
    process_options_t opts = {
        .optimize_input = 0,
        .dither = 0,
        .compress = 0,
        .colors = 8,  // Défaut: 8 couleurs
        .tile_optimize = 1,  // Défaut: optimisation tuiles activée
        .buffer_mode = 0,  // Défaut: mode fichiers
        .icon_size = 0,  // Calculé dynamiquement
        .quality_percent = 100,  // Défaut: pas de redimensionnement
        .magnify_percent = 100,  // Défaut: pas de magnification
        .keep_folder = NULL,   // Défaut: pas de copie des icônes
        .filename_prefix = NULL  // Défaut: préfixe "icon"
    };
    
    const char *img_path = NULL;
    
    // Parser les arguments de la ligne de commande
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            show_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--optimize-input") == 0) {
            opts.optimize_input = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dither") == 0) {
            opts.dither = 1;
        } else if (strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--compress") == 0) {
            opts.compress = 1;
        } else if (strncmp(argv[i], "-c=", 3) == 0) {
            opts.colors = atoi(argv[i] + 3);
            if (opts.colors != 8 && opts.colors != 16 && opts.colors != 32 && opts.colors != 64) {
                fprintf(stderr, "Erreur: nombre de couleurs doit être 8, 16, 32 ou 64\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--colors=", 9) == 0) {
            opts.colors = atoi(argv[i] + 9);
            if (opts.colors != 8 && opts.colors != 16 && opts.colors != 32 && opts.colors != 64) {
                fprintf(stderr, "Erreur: nombre de couleurs doit être 8, 16, 32 ou 64\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--no-tile-optimize") == 0) {
            opts.tile_optimize = 0;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--buffer") == 0) {
            opts.buffer_mode = 1;
        } else if (strncmp(argv[i], "-q=", 3) == 0) {
            opts.quality_percent = atoi(argv[i] + 3);
            if (opts.quality_percent < 10 || opts.quality_percent > 100) {
                fprintf(stderr, "Erreur: pourcentage de qualité doit être entre 10 et 100\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--quality=", 10) == 0) {
            opts.quality_percent = atoi(argv[i] + 10);
            if (opts.quality_percent < 10 || opts.quality_percent > 100) {
                fprintf(stderr, "Erreur: pourcentage de qualité doit être entre 10 et 100\n");
                return 1;
            }
        } else if (strncmp(argv[i], "-m=", 3) == 0) {
            opts.magnify_percent = atoi(argv[i] + 3);
            if (opts.magnify_percent < 50 || opts.magnify_percent > 300) {
                fprintf(stderr, "Erreur: pourcentage de magnification doit être entre 50 et 300\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--magnify=", 10) == 0) {
            opts.magnify_percent = atoi(argv[i] + 10);
            if (opts.magnify_percent < 50 || opts.magnify_percent > 300) {
                fprintf(stderr, "Erreur: pourcentage de magnification doit être entre 50 et 300\n");
                return 1;
            }
        } else if (strncmp(argv[i], "-k=", 3) == 0) {
            char *arg = argv[i] + 3;
            char *equals = strchr(arg, '=');
            if (equals) {
                // Format: -k=folder=prefix
                *equals = '\0';  // Séparer folder et prefix
                opts.keep_folder = strdup(arg);
                opts.filename_prefix = strdup(equals + 1);
            } else {
                // Format: -k=folder
                opts.keep_folder = strdup(arg);
                opts.filename_prefix = NULL;  // Utilisera "icon" par défaut
            }
        } else if (strncmp(argv[i], "--keep-icons=", 13) == 0) {
            char *arg = argv[i] + 13;
            char *equals = strchr(arg, '=');
            if (equals) {
                // Format: --keep-icons=folder=prefix
                *equals = '\0';  // Séparer folder et prefix
                opts.keep_folder = strdup(arg);
                opts.filename_prefix = strdup(equals + 1);
            } else {
                // Format: --keep-icons=folder
                opts.keep_folder = strdup(arg);
                opts.filename_prefix = NULL;  // Utilisera "icon" par défaut
            }
        } else if (argv[i][0] != '-') {
            if (img_path == NULL) {
                img_path = argv[i];
            } else {
                fprintf(stderr, "Erreur: une seule image doit être spécifiée\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Erreur: option inconnue %s\n", argv[i]);
            return 1;
        }
    }
    
    // Vérifier qu'une image a été spécifiée
    if (img_path == NULL) {
        fprintf(stderr, "Erreur: aucune image spécifiée\n");
        show_help(argv[0]);
        return 1;
    }
    
    // Nettoyer les anciens fichiers temporaires au démarrage
    // printf("Nettoyage des anciens fichiers temporaires...\n");
    system("find /dev/shm -name 'd200_tiles*' -type d -exec rm -rf {} + 2>/dev/null || true");
    // Ne pas supprimer les frame_*.png car ils peuvent être utilisés par send_video_page_wrapper
    
    // printf("Traitement de: %s\n", img_path);
    // printf("Options: optimisation_input=%d, dither=%d, compress=%d, couleurs=%d, tile_optimize=%d, buffer_mode=%d, quality_percent=%d, magnify_percent=%d, keep_folder=%s\n",
    //     opts.optimize_input, opts.dither, opts.compress, opts.colors, opts.tile_optimize, opts.buffer_mode, opts.quality_percent, opts.magnify_percent, opts.keep_folder ? opts.keep_folder : "NULL");
    
    // Lire l'image source
    uint8_t *src = NULL;
    int sw = 0, sh = 0;
    if (read_png_rgba(img_path, &src, &sw, &sh) != 0) { 
        fprintf(stderr, "Erreur: impossible de lire %s\n", img_path); 
        return 1; 
    }
    
    // Étape 1: Crop intelligent pour ratio 16:9 (PAS de redimensionnement)
    uint8_t *processed_src = NULL;
    int crop_w, crop_h;
    
    // Vérifier si déjà 16:9
    double aspect = (double)sw / sh;
    double target_aspect = 16.0 / 9.0;
    
    if (fabs(aspect - target_aspect) < 0.01) {
        // Déjà 16:9, utiliser l'image telle quelle
        processed_src = src;
        crop_w = sw;
        crop_h = sh;
        // printf("Image déjà en 16:9: %dx%d\n", sw, sh);
    } else {
        // Crop intelligent vers 16:9
        processed_src = ensure_16_9_crop(src, sw, sh, &crop_w, &crop_h);
        free(src);
        if (!processed_src) {
            fprintf(stderr, "Erreur: impossible de croper l'image vers 16:9\n");
            return 1;
        }
        // printf("Image cropée vers 16:9: %dx%d\n", crop_w, crop_h);
    }
    
    // Mettre à jour les dimensions pour la suite
    sw = crop_w;
    sh = crop_h;
    // printf("Image traitée: %dx%d pixels\n", sw, sh);
    
    // Étape 2: Appliquer dithering si demandé (sur l'image originale)
    if (opts.dither) {
        // printf("Application du dithering Floyd-Steinberg sur image %dx%d...\n", sw, sh);
        apply_dithering(processed_src, sw, sh);
        // printf("Dithering appliqué.\n");
    }
    
    // Étape 3: Optimiser l'image input si demandé (conversion en 256 couleurs)
    if (opts.optimize_input) {
        // printf("Optimisation de l'image input (conversion 256 couleurs)...\n");
        optimize_input_image(processed_src, sw, sh);
    }
    
    // Configuration des tuiles (14 boutons: 5x3 + 1 bouton large en bas)
    // Calcul dynamique basé sur la résolution de l'image
    // Référence: 1280x720 → icones 196x196, gap 50
    double scale_factor = (double)sw / 1280.0;
    
    // Taille de base des icônes (calculée dynamiquement)
    int base_icon_size = (int)(196 * scale_factor);
    int base_gap = (int)(50 * scale_factor);
    
    // Appliquer le pourcentage de magnification
    int btn = (base_icon_size * opts.magnify_percent) / 100;
    int gap = (base_gap * opts.magnify_percent) / 100;
    
    // Éviter les valeurs trop petites
    if (btn < 8) btn = 8;
    if (gap < 1) gap = 1;
    
    // Calculer la taille finale avec qualité
    int final_btn = (btn * opts.quality_percent) / 100;
    if (final_btn < 4) final_btn = 4;  // Minimum 4x4 pixels
    
    // printf("Image: %dx%d, scale=%.2f, base_icon=%d, final_icon=%d, gap=%d\n", 
    //        sw, sh, scale_factor, base_icon_size, final_btn, gap);
    
    int margin_x = (sw - (btn * 5 + gap * 4)) / 2;
    int margin_y = (sh - (btn * 3 + gap * 2)) / 2;
    
    // printf("Génération des icônes: taille=%dx%d, gap=%d\n", btn, btn, gap);
    // if (opts.quality_percent < 100) {
    //     printf("Qualité: %d%% -> taille finale: %dx%d\n", opts.quality_percent, final_btn, final_btn);
    // }
    int x[5]; 
    for (int c = 0; c < 5; c++) x[c] = margin_x + c * (btn + gap);
    int y[3]; 
    for (int r = 0; r < 3; r++) y[r] = margin_y + r * (btn + gap);
    
    if (opts.buffer_mode) {
        // Mode buffer: créer les tuiles en mémoire et envoyer directement
        // printf("Mode buffer: création des tuiles en mémoire...\n");
        
        const uint8_t *tiles[14];
        int tile_w[14], tile_h[14];
        
        // Traiter les 13 premiers boutons (grille 5x3)
        for (int i = 0; i < 13; i++) {
            int r2 = (i < 10) ? i / 5 : 2;
            int c = (i < 10) ? i % 5 : (i - 10);
            
            // Extraire la tuile avec la taille de base
            uint8_t *tile = crop_rgba(processed_src, sw, sh, x[c], y[r2], btn, btn);
            
            // Optimiser la tuile si demandé
            if (opts.tile_optimize) {
                // printf("Optimisation tuile %d...\n", i + 1);
                quantize_colors(tile, btn, btn, opts.colors);
            }
            
            // Redimensionner avec qualité si nécessaire
            if (opts.quality_percent < 100) {
                uint8_t *resized = resize_icon(tile, btn, btn, final_btn, final_btn);
                free(tile);
                tile = resized;
                tile_w[i] = final_btn;
                tile_h[i] = final_btn;
            } else {
                tile_w[i] = btn;
                tile_h[i] = btn;
            }
            
            tiles[i] = tile;
        }
        
        // Traiter le 14ème bouton (bouton large en bas)
        int b14w = btn + gap + btn;  // Largeur: btn + gap + btn
        uint8_t *tile14 = crop_rgba(processed_src, sw, sh, x[3], y[2], b14w, btn);
        
        if (opts.tile_optimize) {
            // printf("Optimisation tuile 14...\n");
            quantize_colors(tile14, b14w, btn, opts.colors);
        }
        
        // Redimensionner avec qualité si nécessaire
        if (opts.quality_percent < 100) {
            int new_b14w = final_btn + gap + final_btn; // (final_btn + gap + final_btn)
            uint8_t *resized = resize_icon(tile14, b14w, btn, new_b14w, final_btn);
            free(tile14);
            tile14 = resized;
            tiles[13] = tile14;
            tile_w[13] = new_b14w;
            tile_h[13] = final_btn;
        } else {
            tiles[13] = tile14;
            tile_w[13] = b14w;
            tile_h[13] = btn;
        }
        
        // Copier les icônes dans le dossier spécifié si demandé
        copy_icons_to_folder(tiles, tile_w, tile_h, opts.keep_folder, opts.filename_prefix);
        
        // Envoyer directement au démon
        // printf("Envoi direct des données au démon...\n");
        if (send_rgba_data_direct(tiles, tile_w, tile_h) != 0) {
            fprintf(stderr, "Erreur: échec de l'envoi direct\n");
        } else {
            // printf("Données envoyées avec succès!\n");
        }
        
        // Nettoyer les tuiles
        for (int i = 0; i < 14; i++) {
            free((void*)tiles[i]);
        }
        
    } else {
        // File mode: create temporary directory and write files in parallel
        // printf("File mode: creating tiles on disk (threads enabled)...\n");
        
        char tag[32];
        unique_tag(tag, sizeof(tag));
        char tmpdir[] = "/dev/shm/d200_tilesXXXXXX";
        if (!mkdtemp(tmpdir)) {
            strcpy(tmpdir, "/dev/shm/d200_tilesXXXXXX");
            if (!mkdtemp(tmpdir)) { 
                fprintf(stderr, "Error: mkdtemp failed\n"); 
                free(processed_src); 
                return 1; 
            }
        }
        // printf("Temporary directory: %s\n", tmpdir);
        
        // Initialize thread pool for parallel PNG writing
        png_write_pool_t write_pool;
        if (png_write_pool_init(&write_pool, 4) != 0) {
            fprintf(stderr, "Error: failed to initialize thread pool\n");
            free(processed_src);
            return 1;
        }
        
        // Prepare PNG writing tasks for the 14 tiles
        png_write_task_t write_tasks[14];
        const uint8_t *tiles_data[14];
        int tiles_w[14], tiles_h[14];
        
        // Process first 13 buttons (5x3 grid)
        // printf("Preparing tiles for buttons 1-13...\n");
        for (int i = 0; i < 13; i++) {
            int r2 = (i < 10) ? i / 5 : 2;
            int c = (i < 10) ? i % 5 : (i - 10);
            
            // Extraire la tuile avec la taille de base
            uint8_t *tile = crop_rgba(processed_src, sw, sh, x[c], y[r2], btn, btn);
            
            // Optimiser la tuile si demandé
            if (opts.tile_optimize) {
                // printf("Optimisation tuile %d...\n", i + 1);
                quantize_colors(tile, btn, btn, opts.colors);
            }
            
            // Redimensionner avec qualité si nécessaire
            if (opts.quality_percent < 100) {
                uint8_t *resized = resize_icon(tile, btn, btn, final_btn, final_btn);
                free(tile);
                tile = resized;
                tiles_data[i] = tile;
                tiles_w[i] = final_btn;
                tiles_h[i] = final_btn;
                write_tasks[i].rgba_data = tile;
                write_tasks[i].w = final_btn;
                write_tasks[i].h = final_btn;
            } else {
                tiles_data[i] = tile;
                tiles_w[i] = btn;
                tiles_h[i] = btn;
                write_tasks[i].rgba_data = tile;
                write_tasks[i].w = btn;
                write_tasks[i].h = btn;
            }
            
            write_tasks[i].tile_id = i;
            write_tasks[i].status = 0;
            snprintf(write_tasks[i].filepath, sizeof(write_tasks[i].filepath), 
                    "%s/b%d_%s.png", tmpdir, i + 1, tag);
        }
        
        // Traiter le 14ème bouton (bouton large en bas)
        // printf("Préparation de la tuile pour le bouton 14...\n");
        int b14w = btn + gap + btn;  // Largeur: btn + gap + btn
        uint8_t *tile14 = crop_rgba(processed_src, sw, sh, x[3], y[2], b14w, btn);
        
        if (opts.tile_optimize) {
            // printf("Optimisation tuile 14...\n");
            quantize_colors(tile14, b14w, btn, opts.colors);
        }
        
        // Redimensionner avec qualité si nécessaire
        if (opts.quality_percent < 100) {
            int new_b14w = final_btn + gap + final_btn; // (final_btn + gap + final_btn)
            uint8_t *resized = resize_icon(tile14, b14w, btn, new_b14w, final_btn);
            free(tile14);
            tile14 = resized;
            tiles_data[13] = tile14;
            tiles_w[13] = new_b14w;
            tiles_h[13] = final_btn;
            write_tasks[13].rgba_data = tile14;
            write_tasks[13].w = new_b14w;
            write_tasks[13].h = final_btn;
        } else {
            tiles_data[13] = tile14;
            tiles_w[13] = b14w;
            tiles_h[13] = btn;
            write_tasks[13].rgba_data = tile14;
            write_tasks[13].w = b14w;
            write_tasks[13].h = btn;
        }
        
        write_tasks[13].tile_id = 13;
        write_tasks[13].status = 0;
        snprintf(write_tasks[13].filepath, sizeof(write_tasks[13].filepath), 
                "%s/b14_%s.png", tmpdir, tag);
        
        // Exécuter toutes les écritures PNG en parallèle
        // printf("Écriture PNG en parallèle avec %d threads...\n", write_pool.thread_count);
        if (png_write_pool_execute(&write_pool, write_tasks, 14) != 0) {
            fprintf(stderr, "Erreur: échec de l'écriture parallèle\n");
            png_write_pool_destroy(&write_pool);
            for (int i = 0; i < 14; i++) {
                free((void*)tiles_data[i]);
            }
            free(processed_src);
            return 1;
        }
        
        // Copier les icônes dans le dossier spécifié si demandé
        // Si quality est activé, copier les fichiers redimensionnés, sinon copier depuis tiles_data
        if (opts.quality_percent < 100) {
            // Mode qualité: copier les fichiers temporaires déjà redimensionnés
            copy_icons_from_files(write_tasks, 14, opts.keep_folder, opts.filename_prefix);
        } else {
            // Mode normal: copier depuis les données en mémoire
            copy_icons_to_folder(tiles_data, tiles_w, tiles_h, opts.keep_folder, opts.filename_prefix);
        }
        
        // Préparer la commande pour le daemon
        char sendline[8192];
        size_t len = 0;
        strcpy(sendline, "set-buttons-explicit-14");
        len = strlen(sendline);
        char pathbuf[PATH_MAX];
        
        // Ajouter tous les fichiers à la commande
        for (int i = 0; i < 14; i++) {
            if (write_tasks[i].status != 0) {
                fprintf(stderr, "Erreur: écriture tuile %d a échoué\n", i + 1);
                continue;
            }
            
            // Ajouter à la commande
            int m = snprintf(sendline + len, sizeof(sendline) - len, " --button-%d=%s", i + 1, write_tasks[i].filepath);
            if (m < 0 || (size_t)m >= sizeof(sendline) - len) { 
                fprintf(stderr, "Erreur: commande trop longue\n"); 
                break; 
            }
            len += (size_t)m;
        }
        
        // Envoyer la commande au daemon
        // printf("Envoi de la commande au daemon...\n");
        // printf("Commande: %s\n", sendline);
        if (send_cmd(sendline) != 0) {
            fprintf(stderr, "Erreur: échec de l'envoi de la commande\n");
        } else {
            // printf("Commande envoyée avec succès!\n");
        }
        
        // Nettoyer
        png_write_pool_destroy(&write_pool);
        for (int i = 0; i < 14; i++) {
            free((void*)tiles_data[i]);
        }
        
        // Nettoyer les anciens fichiers temporaires dans /dev/shm/
        // printf("Nettoyage des anciens fichiers temporaires...\n");
        system("find /dev/shm -name 'd200_tiles*' -type d -exec rm -rf {} + 2>/dev/null || true");
        // Ne pas supprimer les frame_*.png car ils peuvent être utilisés par send_video_page_wrapper
        
        // Nettoyer le répertoire temporaire
        snprintf(pathbuf, sizeof(pathbuf), "rm -rf \"%s\"", tmpdir);
        system(pathbuf);
        // printf("Nettoyage terminé.\n");
    }
    
    free(processed_src);
    if (opts.keep_folder) free(opts.keep_folder);
    if (opts.filename_prefix) free(opts.filename_prefix);
    return 0;
}
