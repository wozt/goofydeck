// Single paging daemon for GoofyDeck (no Python).
//
//
// Responsibilities:
// - Connect to ulanzi_d200_daemon unix socket (/tmp/ulanzi_device.sock)
// - Subscribe to button events (read-buttons)
// - Load config/configuration.yml (minimal YAML subset)
// - Render and send pages only when needed (initial + navigation triggers)
// - Cache generated icons in .cache/<page>/ using short hash
//
// Notes:
// - Icon generation shells out to existing local tools (icons/draw_mdi, icons/draw_border, icons/draw_text).
// - Empty/undefined buttons send a transparent PNG (not cached).

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>

#include <zlib.h>
#include <png.h>
#include <yaml.h>
#include <execinfo.h>

#include "../third_party/jsmn.h"

typedef struct {
    char *key;
    char *name;
    char *icon;
    char *preset;
    char *text;
} StateOverride;

typedef struct {
    bool trim;
    int max_len;
} CmdTextOpts;

typedef struct {
    char *action;
    char *data;
    CmdTextOpts cmd_text;
} ActionStep;

typedef struct {
    ActionStep *steps;
    size_t len;
    size_t cap;
} ActionSeq;

typedef struct {
    char *name;
    char *icon;
    char *preset;
    char *text;
    
    char *tap_action;
    char *tap_data;
    CmdTextOpts tap_cmd_text;
    ActionSeq tap_seq;
    
    char *hold_action;
    char *hold_data;
    CmdTextOpts hold_cmd_text;
    ActionSeq hold_seq;
    
    char *longhold_action;
    char *longhold_data;
    CmdTextOpts longhold_cmd_text;
    ActionSeq longhold_seq;
    
    char *released_action;
    char *released_data;
    CmdTextOpts released_cmd_text;
    ActionSeq released_seq;
    
    char *entity_id;
    
    int poll_every_ms;
    char *poll_action;
    char *poll_cmd;
    CmdTextOpts poll_cmd_text;
    
    int state_every_ms;
    char *state_cmd;
    StateOverride *states;
    size_t state_count;
    size_t state_cap;
} Item;

typedef struct {
    char *name;
    char *icon;
    char *text;
    
    char *icon_background_color;
    
    int icon_border_radius;
    int icon_border_size;
    int icon_border_width;
    char *icon_border_color;
    
    int icon_size;
    int icon_padding;
    int icon_offset_x;
    int icon_offset_y;
    int icon_brightness;
    
    char *icon_color;
    
    char *text_color;
    char *text_align;
    char *text_font;
    int text_size;
    int text_offset_x;
    int text_offset_y;
} Preset;

typedef struct {
    char *name;
    Item *items;
    size_t count;
    size_t cap;
    
    char *wallpaper_path;
    int wallpaper_quality;
    int wallpaper_magnify;
    bool wallpaper_dithering;
    bool wallpaper_set;
} Page;

typedef struct {
    char *path;
    int quality;
    int magnify;
    bool dithering;
    bool set;
} WallpaperCfg;

typedef struct {
    int pos_back;
    int pos_prev;
    int pos_next;
    
    int base_brightness;
    int sleep_dim_brightness;
    int sleep_dim_timeout_sec;
    int sleep_timeout_sec;
    
    int cmd_timeout_ms;
    
    Preset *presets;
    size_t preset_count;
    size_t preset_cap;
    
    Page *pages;
    size_t page_count;
    size_t page_cap;
    
    WallpaperCfg wallpaper;
} Config;

typedef struct {
    char *config_path;
    char *ulanzi_sock;
    char *control_sock;
    char *ha_sock;
    char *cache_root;
    char *error_icon;
    char *sys_pregen_dir;
    char *root_dir;
} Options;

typedef struct CmdEngine CmdEngine;

static volatile sig_atomic_t g_running = 1;
static CmdEngine *g_cmd_engine = NULL;

static int g_ulanzi_send_debounce_ms = 300;
static int64_t g_ulanzi_last_send_end_ns = 0;
static int64_t g_last_action_ns = 0;
static bool g_ulanzi_device_ready = true;

static int g_cmd_logs = 0;
static int g_cmd_logs_verbose = 0;

// 0 = normal (only button press status line), 1 = debug (verbose console logs)
static int g_paging_debug = 0;

// When enabled, command loop updates (poll/state) trigger a full page resend
// (set-buttons-explicit or set-buttons-explicit-14 with wallpaper) instead of partial updates.
static int g_cmd_loop_full_page_refresh = 1;

typedef enum { 
    BR_NORMAL = 0,
    BR_DIM = 1,
    BR_SLEEP = 2
} BrightnessState;

typedef enum {
    BTN_EVT_UNKNOWN = 0,
    BTN_EVT_TAP,
    BTN_EVT_HOLD,
    BTN_EVT_LONGHOLD,
    BTN_EVT_RELEASED,
} ButtonEvent;

static const char *button_event_name(ButtonEvent e) {
    switch (e) {
        case BTN_EVT_TAP: return "TAP";
        case BTN_EVT_HOLD: return "HOLD";
        case BTN_EVT_LONGHOLD: return "LONGHOLD";
        case BTN_EVT_RELEASED: return "RELEASED";
        default: return "UNKNOWN";
    }
}

static ButtonEvent parse_button_event_word(const char *s) {
    if (!s || !s[0]) return BTN_EVT_UNKNOWN;
    
    switch (s[0]) {
        case 'T': // TAP event - Expected: "TAP"
            if (s[1] == 'A' && s[2] == 'P' && s[3] == 0) return BTN_EVT_TAP;
            return BTN_EVT_UNKNOWN;
            
        case 'H': // HOLD event - Expected: "HOLD"
            if (s[1] == 'O' && s[2] == 'L' && s[3] == 'D' && s[4] == 0) return BTN_EVT_HOLD;
            return BTN_EVT_UNKNOWN;
            
        case 'L': // LONGHOLD event - Expected: "LONGHOLD"
            if (s[1] == 'O' && s[2] == 'N' && s[3] == 'G' && s[4] == 'H' && s[5] == 'O' && s[6] == 'L' &&
                s[7] == 'D' && s[8] == 0)
                return BTN_EVT_LONGHOLD;
            return BTN_EVT_UNKNOWN;
            
        case 'R': // RELEASED event - Expected: "RELEASED"
            if (s[1] == 'E' && s[2] == 'L' && s[3] == 'E' && s[4] == 'A' && s[5] == 'S' && s[6] == 'E' &&
                s[7] == 'D' && s[8] == 0) {
                return BTN_EVT_RELEASED;
            }
            return BTN_EVT_UNKNOWN;
            
        default:
            return BTN_EVT_UNKNOWN;
    }
}

static int g_post_page_change_ignore_ms = 300;
static int64_t g_ignore_taps_until_ns = 0;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int64_t now_ns_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void drain_fd_nonblocking(int fd) {
    if (fd < 0) return;
    
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    char buf[4096];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r > 0) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        break;
    }
    
    (void)fcntl(fd, F_SETFL, flags);
}

static void flush_pending_button_events(int rb_fd, size_t *inlen, size_t *parse_start) {
    if (inlen) *inlen = 0;
    if (parse_start) *parse_start = 0;
    
    drain_fd_nonblocking(rb_fd);
    
    if (g_post_page_change_ignore_ms > 0) {
        g_ignore_taps_until_ns = now_ns_monotonic() + (int64_t)g_post_page_change_ignore_ms * 1000000LL;
    } else {
        g_ignore_taps_until_ns = 0;
    }
}

static void die_errno(const char *msg) {
    fprintf(stderr, "[pg] ERROR: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static int g_log_is_tty = -1;
static bool g_log_status_active = false;
static int g_paging_verbose_render_logs = 0;
static int g_paging_verbose_tool_logs = 0;
static int g_paging_refresh_logs = 1;
static char g_last_action_line[256] = {0};

static void paging_apply_log_mode(void) {
    if (g_paging_debug) {
        // Debug: log everything to the console (no refresh UI).
        g_cmd_logs = 1;
        g_cmd_logs_verbose = 1;
        g_paging_verbose_render_logs = 1;
        g_paging_verbose_tool_logs = 1;
        g_paging_refresh_logs = 0;
    } else {
        // Normal: only the button press status line (TTY refresh) + errors.
        g_cmd_logs = 0;
        g_cmd_logs_verbose = 0;
        g_paging_verbose_render_logs = 0;
        g_paging_verbose_tool_logs = 0;
        g_paging_refresh_logs = 1;
    }
}

static void log_clear_status_line(void) {
    if (g_log_is_tty <= 0) return;
    if (!g_log_status_active) return;
    
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
    g_log_status_active = false;
}

static void log_msg(const char *fmt, ...) {
    if (!g_paging_debug) return;
    log_clear_status_line();
    
    va_list ap;
    va_start(ap, fmt);
    
    fprintf(stderr, "[pg] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    
    va_end(ap);
}

static void log_render(const char *fmt, ...) {
    if (!g_paging_verbose_render_logs) return;
    
    log_clear_status_line();
    
    va_list ap;
    va_start(ap, fmt);
    
    fprintf(stderr, "[pg] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    
    va_end(ap);
}

static void cmd_log(const char *fmt, ...) {
    if (!g_cmd_logs) return;
    log_clear_status_line();
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[pg] cmd ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static bool handle_cmd_action(const Options *opt, Config *cfg, const char *cur_page, size_t offset, size_t pressed_item,
                              int btn, ButtonEvent evt, const Item *it, const char *action, const char *data,
                              const char *blank_png);

static void log_status(const char *fmt, ...) {
    if (!g_paging_refresh_logs || g_log_is_tty <= 0) {
        va_list ap;
        va_start(ap, fmt);
        
        fprintf(stderr, "[pg] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        
        va_end(ap);
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    
    fprintf(stderr, "\r[pg] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\033[K");
    
    va_end(ap);
    fflush(stderr);
    g_log_status_active = true;
}

static void log_action(const char *fmt, ...) {
    if (!g_paging_debug) return;
    if (!g_paging_refresh_logs || g_log_is_tty <= 0) {
        va_list ap;
        va_start(ap, fmt);
        
        fprintf(stderr, "[pg] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        
        va_end(ap);
        return;
    }

    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    
    if (strcmp(line, g_last_action_line) == 0) return;
    snprintf(g_last_action_line, sizeof(g_last_action_line), "%s", line);

    if (!g_log_status_active) {
        fprintf(stderr, "[pg] %s\n", line);
        fflush(stderr);
        return;
    }

    // \033[s = save cursor, \033[1A = move up one line, \r = start of line, \033[K = clear line, \033[u = restore cursor
    fprintf(stderr, "\033[s\033[1A\r[pg] %s\033[K\033[u", line);
    fflush(stderr);
}

static int file_exists(const char *path);
static int send_line_and_read_reply(const char *sock_path, const char *line, char *reply, size_t reply_cap);

static int ulanzi_apply_default_label_style(const Options *opt) {
    if (!opt) return -1;
    
    char style_json[PATH_MAX];
    snprintf(style_json, sizeof(style_json), "%s/assets/json/default.json", opt->root_dir);
    
    if (!file_exists(style_json)) {
        log_msg("WARN: missing label style JSON: %s", style_json);
        return -1;
    }
    
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "set-label-style %s", style_json);
    
    char reply[64] = {0};
    int rc = send_line_and_read_reply(opt->ulanzi_sock, cmd, reply, sizeof(reply));
    
    if (rc != 0) {
        log_msg("WARN: set-label-style failed (rc=%d, resp='%s')", rc, reply[0] ? reply : "<empty>");
        return -1;
    }
    
    log_msg("set-label-style resp='%s'", reply[0] ? reply : "<empty>");
    return 0;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    
    if (!p) die_errno("malloc");
    
    memcpy(p, s, n + 1);
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    
    if (!r) die_errno("realloc");
    
    return r;
}

static void trim(char *s) {
    if (!s) return;
    
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void rtrim_only(char *s) {
    if (!s) return;
    
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static uint32_t fnv1a32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void ensure_dir(const char *path) {
    struct stat st;
    
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return;
        errno = ENOTDIR;
        die_errno("ensure_dir");
    }
    
    if (mkdir(path, 0775) != 0 && errno != EEXIST) die_errno("mkdir");
}

static int try_ensure_dir(const char *path) {
    struct stat st;
    
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    
    if (mkdir(path, 0775) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int try_ensure_dir_parent(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    
    for (size_t i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            
            if (try_ensure_dir(tmp) != 0) return -1;
            
            tmp[i] = '/';
        }
    }
    return 0;
}

static void ensure_dir_parent(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    
    for (size_t i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            
            ensure_dir(tmp);
            
            tmp[i] = '/';
        }
    }
}

static int is_abs_path(const char *p) {
    return p && p[0] == '/';
}

static char *resolve_path(const char *root_dir, const char *p) {
    if (!p) return xstrdup("");
    
    if (is_abs_path(p)) return xstrdup(p);
    
    char out[PATH_MAX];
    snprintf(out, sizeof(out), "%s/%s", root_dir ? root_dir : ".", p);
    
    return xstrdup(out);
}

static int file_exists(const char *path) {
    struct stat st;
    
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int unix_connect(const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

static int make_unix_listen_socket(const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    
    unlink(sock_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    return fd;
}

static int send_line_and_read_reply(const char *sock_path, const char *line, char *reply, size_t reply_cap) {
    int ms = g_ulanzi_send_debounce_ms;
    if (ms > 0 && g_ulanzi_last_send_end_ns > 0) {
        int64_t now = now_ns_monotonic();
        int64_t min_gap = (int64_t)ms * 1000000LL;
        int64_t elapsed = now - g_ulanzi_last_send_end_ns;
        
        if (elapsed < min_gap) {
            int64_t rem = min_gap - elapsed;
            struct timespec ts;
            ts.tv_sec = (time_t)(rem / 1000000000LL);
            ts.tv_nsec = (long)(rem % 1000000000LL);
            (void)nanosleep(&ts, NULL);
        }
    }

    int fd = unix_connect(sock_path);
    if (fd < 0) {
        g_ulanzi_device_ready = false;
        return -1;
    }
    
    size_t n = strlen(line);
    if (write(fd, line, n) != (ssize_t)n) {
        close(fd);
        g_ulanzi_device_ready = false;
        return -1;
    }
    
    if (n == 0 || line[n - 1] != '\n') (void)write(fd, "\n", 1);
    
    ssize_t r = read(fd, reply, reply_cap - 1);
    if (r <= 0) {
        close(fd);
        g_ulanzi_device_ready = false;
        return -1;
    }
    
    reply[(size_t)r] = 0;
    trim(reply);
    close(fd);
    
    g_ulanzi_last_send_end_ns = now_ns_monotonic();
    
    if (reply[0] == 0) return -1;
    
    if (strncmp(reply, "ok", 2) == 0) {
        g_ulanzi_device_ready = true;
        return 0;
    }
    
    if (strcmp(reply, "err no_device") == 0) {
        g_ulanzi_device_ready = false;
        return -2;
    }
    
    return -1;
}

static int write_blank_png(const char *path, int w, int h) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }
    
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return -1;
    }
    
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }
    
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_set_compression_level(png, Z_BEST_SPEED);
    png_set_filter(png, 0, PNG_FILTER_NONE);

    png_bytep *rows = malloc(sizeof(png_bytep) * (size_t)h);
    if (!rows) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }
    
    size_t rowbytes = (size_t)w * 4;
    uint8_t *buf = calloc((size_t)h, rowbytes);
    if (!buf) {
        free(rows);
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }
    
    for (int y = 0; y < h; y++) rows[y] = (png_bytep)(buf + (size_t)y * rowbytes);
    
    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    
    free(buf);
    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    
    return 0;
}

static int png_read_wh(const char *path, int *out_w, int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    
    if (!path || !out_w || !out_h) return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    uint8_t sig[8];
    if (fread(sig, 1, 8, fp) != 8) {
        fclose(fp);
        return -1;
    }
    if (png_sig_cmp(sig, 0, 8) != 0) {
        fclose(fp);
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }
    
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return -1;
    }
    
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);
    
    png_uint_32 w = 0, h = 0;
    int bit_depth = 0, color_type = 0, interlace = 0, compression = 0, filter = 0;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, &interlace, &compression, &filter);
    
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *out_w = (int)w;
    *out_h = (int)h;
    
    return (w > 0 && h > 0) ? 0 : -1;
}

static int run_exec(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    
    if (pid == 0) {
        if (!g_paging_verbose_tool_logs) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                (void)dup2(dn, STDOUT_FILENO);
                (void)dup2(dn, STDERR_FILENO);
                if (dn > STDERR_FILENO) close(dn);
            }
        }
        
        execvp(argv[0], argv);
        _exit(127);
    }
    
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    
    return 128;
}

static void str_trim_inplace(char *s) {
    if (!s) return;
    trim(s);
}

static int run_shell_capture_text(const char *cmd, int timeout_ms, char *out, size_t out_cap,
                                  const CmdTextOpts *opts, bool is_state_cmd) {
    if (!out || out_cap == 0) return -1;
    out[0] = 0;
    
    if (!cmd || !cmd[0]) return -1;
    
    if (timeout_ms <= 0) timeout_ms = 3000;

    int outp[2] = {-1, -1};
    int errp[2] = {-1, -1};
    
    if (pipe(outp) != 0) return -1;
    if (pipe(errp) != 0) {
        close(outp[0]);
        close(outp[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        return -1;
    }
    
    if (pid == 0) {
        (void)dup2(outp[1], STDOUT_FILENO);
        (void)dup2(errp[1], STDERR_FILENO);
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        
        execl("/bin/sh", "sh", "-lc", cmd, (char *)NULL);
        _exit(127);
    }

    close(outp[1]);
    close(errp[1]);
    
    (void)fcntl(outp[0], F_SETFL, fcntl(outp[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(errp[0], F_SETFL, fcntl(errp[0], F_GETFL, 0) | O_NONBLOCK);

    char obuf[4096] = {0};
    char ebuf[4096] = {0};
    size_t olen = 0;
    size_t elen = 0;

    int64_t start_ns = now_ns_monotonic();
    bool out_open = true;
    bool err_open = true;
    bool timed_out = false;

    while (out_open || err_open) {
        int64_t now_ns = now_ns_monotonic();
        int64_t elapsed_ms = (now_ns - start_ns) / 1000000LL;
        
        if (elapsed_ms >= timeout_ms) {
            timed_out = true;
            break;
        }
        
        int wait_ms = timeout_ms - (int)elapsed_ms;
        if (wait_ms > 100) wait_ms = 100;
        if (wait_ms < 1) wait_ms = 1;

        struct pollfd fds[2];
        nfds_t nfds = 0;
        
        if (out_open) {
            fds[nfds].fd = outp[0];
            fds[nfds].events = POLLIN | POLLHUP | POLLERR;
            nfds++;
        }
        
        if (err_open) {
            fds[nfds].fd = errp[0];
            fds[nfds].events = POLLIN | POLLHUP | POLLERR;
            nfds++;
        }
        
        int pr = poll(fds, nfds, wait_ms);
        
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        if (pr == 0) continue;

        for (nfds_t i = 0; i < nfds; i++) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            
            int fd = fds[i].fd;
            bool saw_hup = (fds[i].revents & (POLLHUP | POLLERR)) != 0;
            
            for (;;) {
                char tmp[512];
                ssize_t n = read(fd, tmp, sizeof(tmp));
                
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    n = 0;
                }
                
                if (n == 0) {
                    if (fd == outp[0]) out_open = false;
                    if (fd == errp[0]) err_open = false;
                    break;
                }
                
                if (fd == outp[0]) {
                    size_t copy = (size_t)n;
                    
                    if (olen + copy >= sizeof(obuf)) copy = (sizeof(obuf) - 1) - olen;
                    
                    if (copy > 0) {
                        memcpy(obuf + olen, tmp, copy);
                        olen += copy;
                        obuf[olen] = 0;
                    }
                } else {
                    size_t copy = (size_t)n;
                    
                    if (elen + copy >= sizeof(ebuf)) copy = (sizeof(ebuf) - 1) - elen;
                    
                    if (copy > 0) {
                        memcpy(ebuf + elen, tmp, copy);
                        elen += copy;
                        ebuf[elen] = 0;
                    }
                }
                
                if ((fd == outp[0] && olen >= sizeof(obuf) - 1) || (fd == errp[0] && elen >= sizeof(ebuf) - 1)) {
                    break;
                }
            }

            // Some commands may exit quickly without producing further POLLIN events; POLLHUP indicates EOF.
            // If we saw HUP/ERR but didn't observe EOF in the read loop (e.g. because there was no POLLIN),
            // mark the stream closed to avoid timing out.
            if (saw_hup) {
                if (fd == outp[0]) out_open = false;
                if (fd == errp[0]) err_open = false;
            }
        }
    }

    int st = 0;
    
    if (timed_out) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &st, 0);
    } else {
        (void)waitpid(pid, &st, 0);
    }

    close(outp[0]);
    close(errp[0]);

    bool ok = !timed_out && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    
    if (!ok) {
        if (is_state_cmd) {
            snprintf(out, out_cap, "%s", "err");
        } else {
            snprintf(out, out_cap, "%s", "ERR");
        }
        if (timed_out) return -2;
        if (WIFEXITED(st)) return WEXITSTATUS(st); // non-zero
        if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
        return -4;
    }

    const char *picked = (obuf[0] != 0) ? obuf : ebuf;
    
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", picked ? picked : "");
    
    if (opts && opts->trim) str_trim_inplace(tmp);
    
    if (opts && opts->max_len > 0) {
        int ml = opts->max_len;
        if (ml < 1) ml = 1;
        int hi = (int)sizeof(tmp) - 1;
        if (ml > hi) ml = hi;
        tmp[ml] = 0;
    }
    
    snprintf(out, out_cap, "%s", tmp);
    return 0;
}

static int run_shell_nocapture(const char *cmd, int timeout_ms) {
    if (!cmd || !cmd[0]) return -1;
    
    if (timeout_ms <= 0) timeout_ms = 3000;
    
    pid_t pid = fork();
    if (pid < 0) return -1;
    
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            (void)dup2(dn, STDOUT_FILENO);
            (void)dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO) close(dn);
        }
        
        execl("/bin/sh", "sh", "-lc", cmd, (char *)NULL);
        _exit(127);
    }
    
    int st = 0;
    int64_t start_ns = now_ns_monotonic();
    
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        
        if (r == pid) break;
        
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        int64_t elapsed_ms = (now_ns_monotonic() - start_ns) / 1000000LL;
        
        if (elapsed_ms >= timeout_ms) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &st, 0);
            return -1;
        }
        
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000000L};
        nanosleep(&ts, NULL);
    }
    
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    
    return 128;
}

static yaml_node_t *yaml_mapping_get(yaml_document_t *doc, yaml_node_t *map, const char *key) {
    if (!doc || !map || map->type != YAML_MAPPING_NODE || !key) return NULL;
    
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start; p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = yaml_document_get_node(doc, p->key);
        yaml_node_t *v = yaml_document_get_node(doc, p->value);
        
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        
        const char *ks = (const char *)k->data.scalar.value;
        
        if (ks && strcmp(ks, key) == 0) return v;
    }
    
    return NULL;
}

static const char *yaml_scalar_cstr(yaml_node_t *n) {
    if (!n || n->type != YAML_SCALAR_NODE) return NULL;
    
    return (const char *)n->data.scalar.value;
}

// Forward declarations for action parsing helpers (defined later).
static CmdTextOpts cmd_text_opts_defaults(void);
static void parse_cmd_text_data_node(yaml_document_t *doc, yaml_node_t *node, char **out_cmd, CmdTextOpts *opts);
static void action_seq_push(ActionSeq *s, ActionStep st);

static void parse_action_mapping_node(yaml_document_t *doc, yaml_node_t *map, ActionSeq *out) {
    if (!doc || !map || map->type != YAML_MAPPING_NODE || !out) return;
    yaml_node_t *a = yaml_mapping_get(doc, map, "action");
    yaml_node_t *d = yaml_mapping_get(doc, map, "data");
    const char *as = yaml_scalar_cstr(a);
    if (!as || !as[0]) return;

    ActionStep st;
    memset(&st, 0, sizeof(st));
    st.cmd_text = cmd_text_opts_defaults();
    st.action = xstrdup(as);

    if (strncmp(as, "$cmd.", 5) == 0) {
        char *cmd = NULL;
        CmdTextOpts o;
        parse_cmd_text_data_node(doc, d, &cmd, &o);
        if (cmd) st.data = cmd;
        st.cmd_text = o;
    } else {
        if (yaml_scalar_cstr(d)) st.data = xstrdup(yaml_scalar_cstr(d));
    }

    action_seq_push(out, st);
}

static void parse_action_node(yaml_document_t *doc, yaml_node_t *node, ActionSeq *out) {
    if (!doc || !node || !out) return;
    if (node->type != YAML_MAPPING_NODE) return;

    yaml_node_t *actions = yaml_mapping_get(doc, node, "actions");
    if (actions && actions->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *it = actions->data.sequence.items.start; it < actions->data.sequence.items.top; it++) {
            yaml_node_t *step = yaml_document_get_node(doc, *it);
            if (!step || step->type != YAML_MAPPING_NODE) continue;
            parse_action_mapping_node(doc, step, out);
        }
        return;
    }

    parse_action_mapping_node(doc, node, out);
}

static int clamp_int(int v, int lo, int hi);

static int parse_int_scalar(const char *s, int *out) {
    if (!s || !out) return -1;
    
    char *end = NULL;
    long v = strtol(s, &end, 10);
    
    if (end == s) return -1;
    
    *out = (int)v;
    return 0;
}

static int parse_offset_scalar(const char *s, int *x, int *y) {
    if (!s || !x || !y) return -1;
    
    int a = 0, b = 0;
    
    if (sscanf(s, "%d,%d", &a, &b) == 2) {
        *x = a;
        *y = b;
        return 0;
    }
    
    return -1;
}

static int parse_bool_scalar(const char *s, bool *out) {
    if (!s || !out) return -1;
    
    if (strcasecmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || 
        strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
        *out = true;
        return 0;
    }
    
    if (strcasecmp(s, "0") == 0 || strcasecmp(s, "false") == 0 || 
        strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
        *out = false;
        return 0;
    }
    
    return -1;
}

static CmdTextOpts cmd_text_opts_defaults(void) {
    CmdTextOpts o;
    o.trim = true;
    o.max_len = 32;
    return o;
}

static void action_seq_free(ActionSeq *s) {
    if (!s) return;
    for (size_t i = 0; i < s->len; i++) {
        free(s->steps[i].action);
        free(s->steps[i].data);
    }
    free(s->steps);
    s->steps = NULL;
    s->len = 0;
    s->cap = 0;
}

static void action_seq_push(ActionSeq *s, ActionStep st) {
    if (!s) return;
    if (s->len + 1 > s->cap) {
        size_t nc = s->cap ? (s->cap * 2) : 4;
        s->steps = xrealloc(s->steps, nc * sizeof(s->steps[0]));
        s->cap = nc;
    }
    s->steps[s->len++] = st;
}

static void parse_cmd_text_data_node(yaml_document_t *doc, yaml_node_t *node, char **out_cmd, CmdTextOpts *opts) {
    (void)doc;
    
    if (out_cmd) *out_cmd = NULL;
    if (opts) *opts = cmd_text_opts_defaults();
    
    if (!node) return;

    const char *s = yaml_scalar_cstr(node);
    
    if (s) {
        if (out_cmd && s[0]) *out_cmd = xstrdup(s);
        return;
    }

    if (node->type != YAML_MAPPING_NODE) return;

    yaml_node_t *cn = yaml_mapping_get(doc, node, "cmd");
    if (out_cmd && yaml_scalar_cstr(cn) && yaml_scalar_cstr(cn)[0]) {
        *out_cmd = xstrdup(yaml_scalar_cstr(cn));
    }

    if (!opts) return;
    
    yaml_node_t *tn = yaml_mapping_get(doc, node, "trim");
    if (yaml_scalar_cstr(tn)) {
        bool bv = true;
        if (parse_bool_scalar(yaml_scalar_cstr(tn), &bv) == 0) opts->trim = bv;
    }
    
    yaml_node_t *mn = yaml_mapping_get(doc, node, "max_len");
    if (yaml_scalar_cstr(mn)) {
        int v = 0;
        if (parse_int_scalar(yaml_scalar_cstr(mn), &v) == 0) {
            opts->max_len = clamp_int(v, 1, 256);
        }
    }
}

static void wallpaper_apply_defaults(WallpaperCfg *w) {
    if (!w) return;
    w->quality = 30;
    w->magnify = 100;
    w->dithering = true;
}

static void page_wallpaper_apply_defaults(Page *p) {
    if (!p) return;
    p->wallpaper_quality = 30;
    p->wallpaper_magnify = 100;
    p->wallpaper_dithering = true;
}

static void parse_wallpaper_node(yaml_document_t *doc, yaml_node_t *node, WallpaperCfg *out) {
    if (!doc || !out || !node) return;
    wallpaper_apply_defaults(out);

    if (node->type == YAML_SCALAR_NODE) {
        const char *s = yaml_scalar_cstr(node);
        if (s && s[0]) {
            free(out->path);
            out->path = xstrdup(s);
            out->set = true;
        }
        return;
    }

    if (node->type != YAML_MAPPING_NODE) return;
    yaml_node_t *n = yaml_mapping_get(doc, node, "path");
    if (yaml_scalar_cstr(n) && yaml_scalar_cstr(n)[0]) {
        free(out->path);
        out->path = xstrdup(yaml_scalar_cstr(n));
        out->set = true;
    }
    n = yaml_mapping_get(doc, node, "quality");
    if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) out->quality = iv; }
    n = yaml_mapping_get(doc, node, "magnify");
    if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) out->magnify = iv; }
    n = yaml_mapping_get(doc, node, "dithering");
    if (yaml_scalar_cstr(n)) { bool bv = true; if (parse_bool_scalar(yaml_scalar_cstr(n), &bv) == 0) out->dithering = bv; }
}

static void parse_page_wallpaper_node(yaml_document_t *doc, yaml_node_t *node, Page *p) {
    if (!doc || !node || !p) return;
    page_wallpaper_apply_defaults(p);
    p->wallpaper_set = true;

    if (node->type == YAML_SCALAR_NODE) {
        const char *s = yaml_scalar_cstr(node);
        if (s && s[0]) {
            free(p->wallpaper_path);
            p->wallpaper_path = xstrdup(s);
        }
        return;
    }
    if (node->type != YAML_MAPPING_NODE) return;

    yaml_node_t *n = yaml_mapping_get(doc, node, "path");
    if (yaml_scalar_cstr(n) && yaml_scalar_cstr(n)[0]) {
        free(p->wallpaper_path);
        p->wallpaper_path = xstrdup(yaml_scalar_cstr(n));
    }
    n = yaml_mapping_get(doc, node, "quality");
    if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) p->wallpaper_quality = iv; }
    n = yaml_mapping_get(doc, node, "magnify");
    if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) p->wallpaper_magnify = iv; }
    n = yaml_mapping_get(doc, node, "dithering");
    if (yaml_scalar_cstr(n)) { bool bv = true; if (parse_bool_scalar(yaml_scalar_cstr(n), &bv) == 0) p->wallpaper_dithering = bv; }
}

static Page *config_get_page(Config *cfg, const char *name) {
    for (size_t i = 0; i < cfg->page_count; i++) {
        if (strcmp(cfg->pages[i].name, name) == 0) return &cfg->pages[i];
    }
    return NULL;
}

static Page *config_add_page(Config *cfg, const char *name) {
    if (cfg->page_count >= cfg->page_cap) {
        cfg->page_cap = cfg->page_cap ? cfg->page_cap * 2 : 8;
        cfg->pages = xrealloc(cfg->pages, cfg->page_cap * sizeof(Page));
    }
    Page *p = &cfg->pages[cfg->page_count++];
    memset(p, 0, sizeof(*p));
    p->name = xstrdup(name);
    p->wallpaper_path = NULL;
    p->wallpaper_quality = 30;
    p->wallpaper_magnify = 100;
    p->wallpaper_dithering = true;
    p->wallpaper_set = false;
    return p;
}

static void page_add_item(Page *p, Item it) {
    if (p->count >= p->cap) {
        p->cap = p->cap ? p->cap * 2 : 16;
        p->items = xrealloc(p->items, p->cap * sizeof(Item));
    }
    p->items[p->count++] = it;
}

static void config_init_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->pos_back = 11;
    cfg->pos_prev = 12;
    cfg->pos_next = 13;
    cfg->base_brightness = 90;
    cfg->sleep_dim_brightness = 20;
    cfg->sleep_dim_timeout_sec = 0;
    cfg->sleep_timeout_sec = 0;
    cfg->cmd_timeout_ms = 3000;
    cfg->wallpaper.path = NULL;
    cfg->wallpaper.quality = 30;
    cfg->wallpaper.magnify = 100;
    cfg->wallpaper.dithering = true;
    cfg->wallpaper.set = false;
}

static void preset_init_defaults(Preset *p, const char *name) {
    memset(p, 0, sizeof(*p));
    p->name = xstrdup(name ? name : "default");
    p->icon = NULL;
    p->text = NULL;
    p->icon_background_color = xstrdup("241f31");
    p->icon_border_radius = 12;
    p->icon_border_size = 196;
    p->icon_border_width = 0;
    p->icon_border_color = xstrdup("FFFFFF");
    p->icon_size = 128;
    p->icon_padding = 0;
    p->icon_offset_x = 0;
    p->icon_offset_y = 0;
    p->icon_brightness = 100;
    p->icon_color = xstrdup("FFFFFF");
    p->text_color = xstrdup("FFFFFF");
    p->text_align = xstrdup("center");
    p->text_font = xstrdup("");
    p->text_size = 40;
    p->text_offset_x = 0;
    p->text_offset_y = 0;
}

static Preset *config_get_preset_mut(Config *cfg, const char *name) {
    if (!cfg || !name) return NULL;
    for (size_t i = 0; i < cfg->preset_count; i++) {
        if (cfg->presets[i].name && strcmp(cfg->presets[i].name, name) == 0) return &cfg->presets[i];
    }
    return NULL;
}

static const Preset *config_get_preset(const Config *cfg, const char *name) {
    if (!cfg || !name) return NULL;
    for (size_t i = 0; i < cfg->preset_count; i++) {
        if (cfg->presets[i].name && strcmp(cfg->presets[i].name, name) == 0) return &cfg->presets[i];
    }
    return NULL;
}

static Preset *config_add_preset(Config *cfg, const char *name) {
    if (cfg->preset_count >= cfg->preset_cap) {
        cfg->preset_cap = cfg->preset_cap ? cfg->preset_cap * 2 : 8;
        cfg->presets = xrealloc(cfg->presets, cfg->preset_cap * sizeof(Preset));
    }
    Preset *p = &cfg->presets[cfg->preset_count++];
    preset_init_defaults(p, name);
    return p;
}

static void config_free(Config *cfg) {
    for (size_t i = 0; i < cfg->preset_count; i++) {
        Preset *p = &cfg->presets[i];
        free(p->name);
        free(p->icon);
        free(p->text);
        free(p->icon_background_color);
        free(p->icon_border_color);
        free(p->icon_color);
        free(p->text_color);
        free(p->text_align);
        free(p->text_font);
    }
    free(cfg->presets);

    for (size_t i = 0; i < cfg->page_count; i++) {
        Page *p = &cfg->pages[i];
        free(p->wallpaper_path);
        for (size_t j = 0; j < p->count; j++) {
            free(p->items[j].name);
            free(p->items[j].icon);
            free(p->items[j].preset);
            free(p->items[j].text);
            free(p->items[j].tap_action);
            free(p->items[j].tap_data);
            free(p->items[j].hold_action);
            free(p->items[j].hold_data);
            free(p->items[j].longhold_action);
            free(p->items[j].longhold_data);
            free(p->items[j].released_action);
            free(p->items[j].released_data);
            action_seq_free(&p->items[j].tap_seq);
            action_seq_free(&p->items[j].hold_seq);
            action_seq_free(&p->items[j].longhold_seq);
            action_seq_free(&p->items[j].released_seq);
            free(p->items[j].entity_id);
            free(p->items[j].poll_action);
            free(p->items[j].poll_cmd);
            free(p->items[j].state_cmd);
            for (size_t k = 0; k < p->items[j].state_count; k++) {
                free(p->items[j].states[k].key);
                free(p->items[j].states[k].name);
                free(p->items[j].states[k].icon);
                free(p->items[j].states[k].preset);
                free(p->items[j].states[k].text);
            }
            free(p->items[j].states);
        }
        free(p->items);
        free(p->name);
    }
    free(cfg->pages);
    free(cfg->wallpaper.path);
    memset(cfg, 0, sizeof(*cfg));
}

static int load_config(const char *path, Config *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    yaml_parser_t parser;
    yaml_document_t doc;
    memset(&parser, 0, sizeof(parser));
    memset(&doc, 0, sizeof(doc));

    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        return -1;
    }
    yaml_parser_set_input_file(&parser, f);
    if (!yaml_parser_load(&parser, &doc)) {
        fprintf(stderr, "[paging] ERROR: YAML parse failed: %s\n", parser.problem ? parser.problem : "unknown");
        yaml_parser_delete(&parser);
        fclose(f);
        return -1;
    }

    Config cfg;
    config_init_defaults(&cfg);
    if (!config_get_preset_mut(&cfg, "default")) (void)config_add_preset(&cfg, "default");

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root || root->type != YAML_MAPPING_NODE) {
        fprintf(stderr, "[paging] ERROR: YAML root is not a mapping\n");
        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
        fclose(f);
        return -1;
    }

    // brightness (root scalar)
    {
        yaml_node_t *bn = yaml_mapping_get(&doc, root, "brightness");
        const char *bs = yaml_scalar_cstr(bn);
        int b = 0;
        if (bs && parse_int_scalar(bs, &b) == 0) cfg.base_brightness = clamp_int(b, 0, 100);
    }

    // sleep: { dim_brightness, dim_timeout, sleep_timeout }
    {
        yaml_node_t *sn = yaml_mapping_get(&doc, root, "sleep");
        if (sn && sn->type == YAML_MAPPING_NODE) {
            yaml_node_t *n;
            int v = 0;
            n = yaml_mapping_get(&doc, sn, "dim_brightness");
            if (yaml_scalar_cstr(n) && parse_int_scalar(yaml_scalar_cstr(n), &v) == 0) cfg.sleep_dim_brightness = clamp_int(v, 0, 100);
            n = yaml_mapping_get(&doc, sn, "dim_timeout");
            if (yaml_scalar_cstr(n) && parse_int_scalar(yaml_scalar_cstr(n), &v) == 0) cfg.sleep_dim_timeout_sec = (v < 0) ? 0 : v;
            n = yaml_mapping_get(&doc, sn, "sleep_timeout");
            if (yaml_scalar_cstr(n) && parse_int_scalar(yaml_scalar_cstr(n), &v) == 0) cfg.sleep_timeout_sec = (v < 0) ? 0 : v;
        }
    }

    // cmd_timeout_ms (root scalar)
    {
        yaml_node_t *cn = yaml_mapping_get(&doc, root, "cmd_timeout_ms");
        const char *cs = yaml_scalar_cstr(cn);
        int v = 0;
        if (cs && parse_int_scalar(cs, &v) == 0) cfg.cmd_timeout_ms = (v < 0) ? 0 : v;
    }

    // wallpaper (global): string path or mapping { path, quality, magnify, dithering }
    {
        yaml_node_t *wn = yaml_mapping_get(&doc, root, "wallpaper");
        if (wn) parse_wallpaper_node(&doc, wn, &cfg.wallpaper);
        cfg.wallpaper.quality = clamp_int(cfg.wallpaper.quality, 10, 100);
        cfg.wallpaper.magnify = clamp_int(cfg.wallpaper.magnify, 50, 300);
    }

    // system_buttons
    yaml_node_t *sys = yaml_mapping_get(&doc, root, "system_buttons");
    if (sys && sys->type == YAML_MAPPING_NODE) {
        for (yaml_node_pair_t *p = sys->data.mapping.pairs.start; p < sys->data.mapping.pairs.top; p++) {
            yaml_node_t *k = yaml_document_get_node(&doc, p->key);
            yaml_node_t *v = yaml_document_get_node(&doc, p->value);
            const char *key = yaml_scalar_cstr(k);
            if (!key || !v || v->type != YAML_MAPPING_NODE) continue;
            yaml_node_t *posn = yaml_mapping_get(&doc, v, "position");
            const char *pos_s = yaml_scalar_cstr(posn);
            int pos = 0;
            if (parse_int_scalar(pos_s, &pos) != 0) continue;
            if (strcmp(key, "$page.back") == 0) cfg.pos_back = pos;
            else if (strcmp(key, "$page.previous") == 0) cfg.pos_prev = pos;
            else if (strcmp(key, "$page.next") == 0) cfg.pos_next = pos;
        }
    }

    // presets
    yaml_node_t *presets = yaml_mapping_get(&doc, root, "presets");
    if (presets && presets->type == YAML_MAPPING_NODE) {
        for (yaml_node_pair_t *p = presets->data.mapping.pairs.start; p < presets->data.mapping.pairs.top; p++) {
            yaml_node_t *k = yaml_document_get_node(&doc, p->key);
            yaml_node_t *v = yaml_document_get_node(&doc, p->value);
            const char *preset_name = yaml_scalar_cstr(k);
            if (!preset_name || !v || v->type != YAML_MAPPING_NODE) continue;

            if (!config_get_preset_mut(&cfg, preset_name)) config_add_preset(&cfg, preset_name);
            Preset *pr = config_get_preset_mut(&cfg, preset_name);
            if (!pr) continue;

            yaml_node_t *n;

            n = yaml_mapping_get(&doc, v, "icon");
            if (yaml_scalar_cstr(n)) { free(pr->icon); pr->icon = xstrdup(yaml_scalar_cstr(n)); }
            n = yaml_mapping_get(&doc, v, "text");
            if (yaml_scalar_cstr(n)) { free(pr->text); pr->text = xstrdup(yaml_scalar_cstr(n)); }

            n = yaml_mapping_get(&doc, v, "icon_background_color");
            if (yaml_scalar_cstr(n)) { free(pr->icon_background_color); pr->icon_background_color = xstrdup(yaml_scalar_cstr(n)); }
            n = yaml_mapping_get(&doc, v, "icon_border_radius");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->icon_border_radius = iv; }
            n = yaml_mapping_get(&doc, v, "icon_border_size");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->icon_border_size = iv; }
            n = yaml_mapping_get(&doc, v, "icon_border_width");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->icon_border_width = iv; }
            n = yaml_mapping_get(&doc, v, "icon_border_color");
            if (yaml_scalar_cstr(n)) { free(pr->icon_border_color); pr->icon_border_color = xstrdup(yaml_scalar_cstr(n)); }
            n = yaml_mapping_get(&doc, v, "icon_size");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->icon_size = iv; }
            n = yaml_mapping_get(&doc, v, "icon_padding");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->icon_padding = iv; }
            n = yaml_mapping_get(&doc, v, "icon_offset");
            if (yaml_scalar_cstr(n)) { (void)parse_offset_scalar(yaml_scalar_cstr(n), &pr->icon_offset_x, &pr->icon_offset_y); }
            n = yaml_mapping_get(&doc, v, "icon_brightness");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->icon_brightness = iv; }
            n = yaml_mapping_get(&doc, v, "icon_color");
            if (yaml_scalar_cstr(n)) { free(pr->icon_color); pr->icon_color = xstrdup(yaml_scalar_cstr(n)); }

            n = yaml_mapping_get(&doc, v, "text_color");
            if (yaml_scalar_cstr(n)) { free(pr->text_color); pr->text_color = xstrdup(yaml_scalar_cstr(n)); }
            n = yaml_mapping_get(&doc, v, "text_align");
            if (yaml_scalar_cstr(n)) { free(pr->text_align); pr->text_align = xstrdup(yaml_scalar_cstr(n)); }
            n = yaml_mapping_get(&doc, v, "text_font");
            if (yaml_scalar_cstr(n)) { free(pr->text_font); pr->text_font = xstrdup(yaml_scalar_cstr(n)); }
            n = yaml_mapping_get(&doc, v, "text_size");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->text_size = iv; }
            n = yaml_mapping_get(&doc, v, "text_offset");
            if (yaml_scalar_cstr(n)) { (void)parse_offset_scalar(yaml_scalar_cstr(n), &pr->text_offset_x, &pr->text_offset_y); }
        }
    }

    // pages
    yaml_node_t *pages = yaml_mapping_get(&doc, root, "pages");
    if (pages && pages->type == YAML_MAPPING_NODE) {
        for (yaml_node_pair_t *pp = pages->data.mapping.pairs.start; pp < pages->data.mapping.pairs.top; pp++) {
            yaml_node_t *k = yaml_document_get_node(&doc, pp->key);
            yaml_node_t *v = yaml_document_get_node(&doc, pp->value);
            const char *page_name = yaml_scalar_cstr(k);
            if (!page_name || !v || v->type != YAML_MAPPING_NODE) continue;

            if (!config_get_page(&cfg, page_name)) config_add_page(&cfg, page_name);
            Page *page = config_get_page(&cfg, page_name);
            if (!page) continue;

            // Optional wallpaper override per page.
            {
                yaml_node_t *wn = yaml_mapping_get(&doc, v, "wallpaper");
                if (wn) {
                    parse_page_wallpaper_node(&doc, wn, page);
                    page->wallpaper_quality = clamp_int(page->wallpaper_quality, 10, 100);
                    page->wallpaper_magnify = clamp_int(page->wallpaper_magnify, 50, 300);
                }
            }

            yaml_node_t *buttons = yaml_mapping_get(&doc, v, "buttons");
            if (!buttons || buttons->type != YAML_SEQUENCE_NODE) continue;
            for (yaml_node_item_t *it = buttons->data.sequence.items.start; it < buttons->data.sequence.items.top; it++) {
                yaml_node_t *item = yaml_document_get_node(&doc, *it);
                if (!item || item->type != YAML_MAPPING_NODE) continue;

                Item out_item;
                memset(&out_item, 0, sizeof(out_item));
                out_item.tap_cmd_text = cmd_text_opts_defaults();
                out_item.hold_cmd_text = cmd_text_opts_defaults();
                out_item.longhold_cmd_text = cmd_text_opts_defaults();
                out_item.released_cmd_text = cmd_text_opts_defaults();
                out_item.poll_cmd_text = cmd_text_opts_defaults();

                yaml_node_t *n;
                n = yaml_mapping_get(&doc, item, "name");
                if (yaml_scalar_cstr(n)) out_item.name = xstrdup(yaml_scalar_cstr(n));
                n = yaml_mapping_get(&doc, item, "icon");
                if (yaml_scalar_cstr(n)) out_item.icon = xstrdup(yaml_scalar_cstr(n));
                n = yaml_mapping_get(&doc, item, "text");
                if (yaml_scalar_cstr(n)) out_item.text = xstrdup(yaml_scalar_cstr(n));
                n = yaml_mapping_get(&doc, item, "entity_id");
                if (yaml_scalar_cstr(n)) out_item.entity_id = xstrdup(yaml_scalar_cstr(n));

                // presets: [p01, ...] => keep first
                n = yaml_mapping_get(&doc, item, "presets");
                if (n) {
                    if (n->type == YAML_SEQUENCE_NODE && n->data.sequence.items.start < n->data.sequence.items.top) {
                        yaml_node_t *first = yaml_document_get_node(&doc, *n->data.sequence.items.start);
                        const char *s = yaml_scalar_cstr(first);
                        if (s && s[0]) out_item.preset = xstrdup(s);
                    } else if (n->type == YAML_SCALAR_NODE) {
                        const char *s = yaml_scalar_cstr(n);
                        if (s && s[0]) out_item.preset = xstrdup(s);
                    }
                }

                // tap_action: { action: "$page.go_to", data: "scripts" }
                n = yaml_mapping_get(&doc, item, "tap_action");
                if (n && n->type == YAML_MAPPING_NODE) {
                    parse_action_node(&doc, n, &out_item.tap_seq);
                    if (out_item.tap_seq.len > 0) {
                        out_item.tap_action = xstrdup(out_item.tap_seq.steps[0].action ? out_item.tap_seq.steps[0].action : "");
                        if (out_item.tap_seq.steps[0].data) out_item.tap_data = xstrdup(out_item.tap_seq.steps[0].data);
                        out_item.tap_cmd_text = out_item.tap_seq.steps[0].cmd_text;
                    }
                }

                // hold_action: { action: "...", data: "..." }
                n = yaml_mapping_get(&doc, item, "hold_action");
                if (n && n->type == YAML_MAPPING_NODE) {
                    parse_action_node(&doc, n, &out_item.hold_seq);
                    if (out_item.hold_seq.len > 0) {
                        out_item.hold_action = xstrdup(out_item.hold_seq.steps[0].action ? out_item.hold_seq.steps[0].action : "");
                        if (out_item.hold_seq.steps[0].data) out_item.hold_data = xstrdup(out_item.hold_seq.steps[0].data);
                        out_item.hold_cmd_text = out_item.hold_seq.steps[0].cmd_text;
                    }
                }

                // longhold_action: { action: "...", data: "..." }
                n = yaml_mapping_get(&doc, item, "longhold_action");
                if (n && n->type == YAML_MAPPING_NODE) {
                    parse_action_node(&doc, n, &out_item.longhold_seq);
                    if (out_item.longhold_seq.len > 0) {
                        out_item.longhold_action = xstrdup(out_item.longhold_seq.steps[0].action ? out_item.longhold_seq.steps[0].action : "");
                        if (out_item.longhold_seq.steps[0].data) out_item.longhold_data = xstrdup(out_item.longhold_seq.steps[0].data);
                        out_item.longhold_cmd_text = out_item.longhold_seq.steps[0].cmd_text;
                    }
                }

                // released_action: { action: "...", data: "..." } (triggered on Ulanzi RELEASED)
                n = yaml_mapping_get(&doc, item, "released_action");
                if (n && n->type == YAML_MAPPING_NODE) {
                    parse_action_node(&doc, n, &out_item.released_seq);
                    if (out_item.released_seq.len > 0) {
                        out_item.released_action = xstrdup(out_item.released_seq.steps[0].action ? out_item.released_seq.steps[0].action : "");
                        if (out_item.released_seq.steps[0].data) out_item.released_data = xstrdup(out_item.released_seq.steps[0].data);
                        out_item.released_cmd_text = out_item.released_seq.steps[0].cmd_text;
                    }
                }

                // poll: { every_ms, action: { action, data: { cmd, trim, max_len } } }
                n = yaml_mapping_get(&doc, item, "poll");
                if (n && n->type == YAML_MAPPING_NODE) {
                    yaml_node_t *en = yaml_mapping_get(&doc, n, "every_ms");
                    int v = 0;
                    if (yaml_scalar_cstr(en) && parse_int_scalar(yaml_scalar_cstr(en), &v) == 0) out_item.poll_every_ms = (v < 0) ? 0 : v;

                    yaml_node_t *an = yaml_mapping_get(&doc, n, "action");
                    if (an && an->type == YAML_MAPPING_NODE) {
                        yaml_node_t *a = yaml_mapping_get(&doc, an, "action");
                        yaml_node_t *d = yaml_mapping_get(&doc, an, "data");
                        const char *as = yaml_scalar_cstr(a);
                        if (as && as[0]) out_item.poll_action = xstrdup(as);
                        if (as && strncmp(as, "$cmd.", 5) == 0) {
                            char *cmd = NULL;
                            CmdTextOpts o;
                            parse_cmd_text_data_node(&doc, d, &cmd, &o);
                            if (cmd) out_item.poll_cmd = cmd;
                            out_item.poll_cmd_text = o;
                        } else if (yaml_scalar_cstr(d)) {
                            // allow scalar (cmd string) for convenience
                            out_item.poll_cmd = xstrdup(yaml_scalar_cstr(d));
                        }
                    }
                }

                // state_cmd: { cmd, every_ms }
                n = yaml_mapping_get(&doc, item, "state_cmd");
                if (n && n->type == YAML_MAPPING_NODE) {
                    yaml_node_t *cn = yaml_mapping_get(&doc, n, "cmd");
                    if (yaml_scalar_cstr(cn) && yaml_scalar_cstr(cn)[0]) out_item.state_cmd = xstrdup(yaml_scalar_cstr(cn));
                    yaml_node_t *en = yaml_mapping_get(&doc, n, "every_ms");
                    int v = 0;
                    if (yaml_scalar_cstr(en) && parse_int_scalar(yaml_scalar_cstr(en), &v) == 0) out_item.state_every_ms = (v < 0) ? 0 : v;
                }

                // states: { "on": { name, presets, icon, text }, ... }
                n = yaml_mapping_get(&doc, item, "states");
                if (n && n->type == YAML_MAPPING_NODE) {
                    for (yaml_node_pair_t *sp = n->data.mapping.pairs.start; sp < n->data.mapping.pairs.top; sp++) {
                        yaml_node_t *sk = yaml_document_get_node(&doc, sp->key);
                        yaml_node_t *sv = yaml_document_get_node(&doc, sp->value);
                        const char *state_key = yaml_scalar_cstr(sk);
                        if (!state_key || !sv || sv->type != YAML_MAPPING_NODE) continue;

                        StateOverride ov;
                        memset(&ov, 0, sizeof(ov));
                        ov.key = xstrdup(state_key);

                        yaml_node_t *sn;
                        sn = yaml_mapping_get(&doc, sv, "name");
                        if (yaml_scalar_cstr(sn)) ov.name = xstrdup(yaml_scalar_cstr(sn));
                        sn = yaml_mapping_get(&doc, sv, "icon");
                        if (yaml_scalar_cstr(sn)) ov.icon = xstrdup(yaml_scalar_cstr(sn));
                        sn = yaml_mapping_get(&doc, sv, "text");
                        if (yaml_scalar_cstr(sn)) ov.text = xstrdup(yaml_scalar_cstr(sn));

                        sn = yaml_mapping_get(&doc, sv, "presets");
                        if (sn) {
                            if (sn->type == YAML_SEQUENCE_NODE && sn->data.sequence.items.start < sn->data.sequence.items.top) {
                                yaml_node_t *first = yaml_document_get_node(&doc, *sn->data.sequence.items.start);
                                const char *s = yaml_scalar_cstr(first);
                                if (s && s[0]) ov.preset = xstrdup(s);
                            } else if (sn->type == YAML_SCALAR_NODE) {
                                const char *s = yaml_scalar_cstr(sn);
                                if (s && s[0]) ov.preset = xstrdup(s);
                            }
                        }

                        if (out_item.state_count >= out_item.state_cap) {
                            out_item.state_cap = out_item.state_cap ? out_item.state_cap * 2 : 4;
                            out_item.states = xrealloc(out_item.states, out_item.state_cap * sizeof(StateOverride));
                        }
                        out_item.states[out_item.state_count++] = ov;
                    }
                }

                page_add_item(page, out_item);
            }
        }
    }

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(f);

    config_free(out);
    *out = cfg;
    return 0;
}

static const char *parent_page(const char *page) {
    if (!page || strcmp(page, "$root") == 0) return "$root";
    const char *slash = strrchr(page, '/');
    if (!slash) return "$root";
    static char buf[256];
    size_t n = (size_t)(slash - page);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, page, n);
    buf[n] = 0;
    if (buf[0] == 0) return "$root";
    return buf;
}

static int ensure_mdi_svg(const Options *opt, const char *icon_spec) {
    if (!icon_spec || strncmp(icon_spec, "mdi:", 4) != 0) return 0;
    const char *name = icon_spec + 4;
    char svg[PATH_MAX];
    snprintf(svg, sizeof(svg), "%s/assets/mdi/%s.svg", opt->root_dir, name);
    if (file_exists(svg)) return 0;
    // Best-effort download (may fail if no network); only once per missing icon to avoid loops.
    char marker[PATH_MAX];
    uint32_t h = fnv1a32(name, strlen(name));
    snprintf(marker, sizeof(marker), "%s/.cache/mdi_dl_%08x.once", opt->root_dir, h);
    if (file_exists(marker)) return -1;
    ensure_dir_parent(marker);
    FILE *m = fopen(marker, "wb");
    if (m) fclose(m);
    char script[PATH_MAX];
    snprintf(script, sizeof(script), "%s/icons/download_mdi.sh", opt->root_dir);
    char *argv[] = { script, NULL };
    (void)run_exec(argv);
    return file_exists(svg) ? 0 : -1;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    ensure_dir_parent(dst);
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static void state_dir(const Options *opt, char *out, size_t cap);
static void sanitize_suffix(const char *in, char *out, size_t cap);

typedef struct {
    const char *path;
    int quality;
    int magnify;
    bool dithering;
    bool enabled;
} WallpaperEff;

static bool path_basename(const char *path, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!path || !path[0]) return false;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (!b[0]) return false;
    snprintf(out, cap, "%s", b);
    return out[0] != 0;
}

static WallpaperEff effective_wallpaper(const Config *cfg, const Page *page) {
    WallpaperEff out;
    memset(&out, 0, sizeof(out));
    out.quality = 30;
    out.magnify = 100;
    out.dithering = true;
    out.enabled = false;

    if (page && page->wallpaper_set) {
        if (page->wallpaper_path && page->wallpaper_path[0]) {
            out.path = page->wallpaper_path;
            out.quality = page->wallpaper_quality;
            out.magnify = page->wallpaper_magnify;
            out.dithering = page->wallpaper_dithering;
            out.enabled = true;
            return out;
        }
        // Explicitly set but empty => disable.
        return out;
    }

    if (cfg && cfg->wallpaper.set && cfg->wallpaper.path && cfg->wallpaper.path[0]) {
        out.path = cfg->wallpaper.path;
        out.quality = cfg->wallpaper.quality;
        out.magnify = cfg->wallpaper.magnify;
        out.dithering = cfg->wallpaper.dithering;
        out.enabled = true;
    }
    return out;
}

static int resolve_path_root(const Options *opt, const char *in, char *out, size_t cap) {
    if (!opt || !in || !out || cap == 0) return -1;
    out[0] = 0;
    if (in[0] == '/') {
        snprintf(out, cap, "%s", in);
        return 0;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/%s", opt->root_dir, in);
    snprintf(out, cap, "%s", tmp);
    return 0;
}

static bool is_under_prefix(const char *path, const char *prefix) {
    if (!path || !prefix) return false;
    size_t n = strlen(prefix);
    if (n == 0) return false;
    return strncmp(path, prefix, n) == 0 && (path[n] == 0 || path[n] == '/');
}

static int session_cache_icon(const Options *opt, const char *src_png, char *out_png, size_t out_cap) {
    if (!opt || !src_png || !src_png[0] || !out_png || out_cap == 0) return -1;
    out_png[0] = 0;
    if (!file_exists(src_png)) return -1;

    // Skip temp files under the paging state dir (/dev/shm) - they are already RAM-backed.
    char sdir[PATH_MAX];
    state_dir(opt, sdir, sizeof(sdir));
    if (is_under_prefix(src_png, sdir)) return -1;

    struct stat st;
    if (stat(src_png, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) return -1;

    char base[PATH_MAX];
    if (!path_basename(src_png, base, sizeof(base))) return -1;

    char dir[PATH_MAX];
    state_dir(opt, dir, sizeof(dir));
    char cdir[PATH_MAX];
    snprintf(cdir, sizeof(cdir), "%s/icon_cache", dir);
    ensure_dir(cdir);

    char key[PATH_MAX + 64];
    snprintf(key, sizeof(key), "%s|%lld|%lld", src_png, (long long)st.st_mtime, (long long)st.st_size);
    uint32_t h = fnv1a32(key, strlen(key));

    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/%08x_%s", cdir, (unsigned)h, base);
    if (!file_exists(dst)) {
        if (copy_file(src_png, dst) != 0) return -1;
    }
    snprintf(out_png, out_cap, "%s", dst);
    return 0;
}

static int wallpaper_render_dir_and_prefix(const char *wallpaper_abs_png,
                                           char *out_dir, size_t dir_cap,
                                           char *out_prefix, size_t prefix_cap) {
    if (!wallpaper_abs_png || !out_dir || !out_prefix || dir_cap == 0 || prefix_cap == 0) return -1;
    out_dir[0] = 0;
    out_prefix[0] = 0;

    const char *base = strrchr(wallpaper_abs_png, '/');
    base = base ? base + 1 : wallpaper_abs_png;
    size_t bl = strlen(base);
    if (bl < 4 || strcmp(base + bl - 4, ".png") != 0) return -1;
    size_t name_len = bl - 4;
    if (name_len + 1 > prefix_cap) return -1;
    memcpy(out_prefix, base, name_len);
    out_prefix[name_len] = 0;

    char dirbuf[PATH_MAX];
    snprintf(dirbuf, sizeof(dirbuf), "%s", wallpaper_abs_png);
    char *slash = strrchr(dirbuf, '/');
    if (slash) *slash = 0;
    else snprintf(dirbuf, sizeof(dirbuf), ".");

    snprintf(out_dir, dir_cap, "%s/%s", dirbuf, out_prefix);
    return 0;
}

static bool wallpaper_tiles_exist(const char *dir, const char *prefix) {
    if (!dir || !prefix) return false;
    char path[PATH_MAX];
    for (int i = 1; i <= 14; i++) {
        snprintf(path, sizeof(path), "%s/%s-%d.png", dir, prefix, i);
        if (!file_exists(path)) return false;
    }
    return true;
}

static int ensure_wallpaper_rendered(const Options *opt, const WallpaperEff *wp, char *out_dir, size_t dir_cap,
                                     char *out_prefix, size_t prefix_cap) {
    if (!opt || !wp || !wp->enabled || !wp->path || !out_dir || !out_prefix) return -1;

    char abs_png[PATH_MAX];
    if (resolve_path_root(opt, wp->path, abs_png, sizeof(abs_png)) != 0) return -1;
    if (!file_exists(abs_png)) return -1;

    if (wallpaper_render_dir_and_prefix(abs_png, out_dir, dir_cap, out_prefix, prefix_cap) != 0) return -1;
    if (wallpaper_tiles_exist(out_dir, out_prefix)) return 0;

    char script[PATH_MAX];
    snprintf(script, sizeof(script), "%s/bin/render_image_page_wrapper.sh", opt->root_dir);
    if (access(script, X_OK) != 0) return -1;

    int q = clamp_int(wp->quality, 10, 100);
    // magnify is a percentage (10..100)
    int m = clamp_int(wp->magnify, 10, 100);
    char qarg[32];
    char marg[32];
    snprintf(qarg, sizeof(qarg), "-q=%d", q);
    snprintf(marg, sizeof(marg), "-m=%d", m);

    if (wp->dithering) {
        char *argv[] = { script, qarg, marg, (char *)"-d", abs_png, NULL };
        if (run_exec(argv) != 0) return -1;
    } else {
        char *argv[] = { script, qarg, marg, abs_png, NULL };
        if (run_exec(argv) != 0) return -1;
    }

    return wallpaper_tiles_exist(out_dir, out_prefix) ? 0 : -1;
}

static int wallpaper_session_tile(const Options *opt, const char *render_dir, const char *prefix,
                                  const WallpaperEff *wp, int tile_num, char *out_path, size_t out_cap);

static int wp_compose_cached(const Options *opt, uint32_t wp_sig, const char *render_dir, const char *prefix,
                             const WallpaperEff *wp, int pos, const char *icon_path,
                             char *out_png, size_t out_cap, bool *out_is_tmp) {
    if (out_is_tmp) *out_is_tmp = false;
    if (!opt || !wp || !wp->enabled || !render_dir || !prefix || !icon_path || !out_png || out_cap == 0) return -1;
    out_png[0] = 0;
    if (pos < 1 || pos > 13) return -1;

    char tile[PATH_MAX];
    if (wallpaper_session_tile(opt, render_dir, prefix, wp, pos, tile, sizeof(tile)) != 0 || tile[0] == 0) return -1;

    // Only cache for non-temp, stable icons (already in cache/pregen).
    bool can_cache = true;
    char dir[PATH_MAX];
    state_dir(opt, dir, sizeof(dir));
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", dir);
    if (strncmp(icon_path, tmpdir, strlen(tmpdir)) == 0) can_cache = false;

    char base[PATH_MAX];
    if (!path_basename(icon_path, base, sizeof(base))) return -1;

    char cache_dir[PATH_MAX];
    snprintf(cache_dir, sizeof(cache_dir), "%s/wp_comp/%08x/%02d", dir, (unsigned)wp_sig, pos);
    if (can_cache) {
        // This is a nested path; create parents best-effort. If it fails, fall back to tmp (no cache).
        if (try_ensure_dir_parent(cache_dir) != 0 || try_ensure_dir(cache_dir) != 0) {
            can_cache = false;
        }
    }
    char cached[PATH_MAX];
    // IMPORTANT: include the position in the filename (not just the directory). The Ulanzi daemon
    // may use basenames when preparing zips/patches, and without this we can end up with multiple
    // different images collapsing into the same entry if the same icon is reused across positions.
    snprintf(cached, sizeof(cached), "%s/%02d_%s", cache_dir, pos, base);
    if (can_cache && file_exists(cached)) {
        snprintf(out_png, out_cap, "%s", cached);
        return 0;
    }

    char draw_over_bin[PATH_MAX];
    snprintf(draw_over_bin, sizeof(draw_over_bin), "%s/icons/draw_over", opt->root_dir);
    if (access(draw_over_bin, X_OK) != 0) return -1;

    ensure_dir(tmpdir);
    pid_t pid = getpid();
    long t = (long)time(NULL);
    char tmp_out[PATH_MAX];
    snprintf(tmp_out, sizeof(tmp_out), "%s/wp_comp_tmp_%d_%ld_%02d.png", tmpdir, (int)pid, t, pos);
    if (copy_file(tile, tmp_out) != 0) return -1;

    char *argv_over[] = { draw_over_bin, (char *)icon_path, tmp_out, NULL };
    if (run_exec(argv_over) != 0) {
        unlink(tmp_out);
        return -1;
    }

    if (can_cache) {
        // Best-effort atomic move into cache.
        if (rename(tmp_out, cached) == 0) {
            snprintf(out_png, out_cap, "%s", cached);
            return 0;
        }
        // Cross-filesystem fallback.
        if (copy_file(tmp_out, cached) == 0) {
            unlink(tmp_out);
            snprintf(out_png, out_cap, "%s", cached);
            return 0;
        }
    }

    // Fallback: use tmp directly (caller must unlink).
    if (out_is_tmp) *out_is_tmp = true;
    snprintf(out_png, out_cap, "%s", tmp_out);
    return 0;
}

static int wallpaper_session_tile(const Options *opt, const char *render_dir, const char *prefix,
                                  const WallpaperEff *wp, int tile_num, char *out_path, size_t out_cap) {
    if (!opt || !render_dir || !prefix || !wp || !out_path || out_cap == 0) return -1;
    out_path[0] = 0;
    if (tile_num < 1 || tile_num > 14) return -1;

    char src[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s-%d.png", render_dir, prefix, tile_num);
    if (!file_exists(src)) return -1;

    // Session copy dir under state_dir (prefers /dev/shm via state_dir()).
    char dir[PATH_MAX];
    state_dir(opt, dir, sizeof(dir));
    char wdir[PATH_MAX];
    snprintf(wdir, sizeof(wdir), "%s/wallpaper", dir);
    ensure_dir(wdir);

    char key[PATH_MAX + 128];
    snprintf(key, sizeof(key), "dir:%s\nprefix:%s\nq:%d\nm:%d\nd:%d\n",
             render_dir, prefix, wp->quality, wp->magnify, wp->dithering ? 1 : 0);
    uint32_t h = fnv1a32(key, strlen(key));
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/%08x", wdir, h);
    ensure_dir(sub);

    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/%s-%d.png", sub, prefix, tile_num);
    if (!file_exists(dst)) {
        (void)copy_file(src, dst);
    }
    if (file_exists(dst)) {
        snprintf(out_path, out_cap, "%s", dst);
        return 0;
    }
    // Fallback to on-disk render tile
    snprintf(out_path, out_cap, "%s", src);
    return 0;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double now_sec_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    char *entity_id;
    char *state; // e.g. "on", "off", "23.4"
    char *unit;  // optional unit_of_measurement
} HaEntityState;

typedef struct {
    HaEntityState *items;
    size_t len;
    size_t cap;
} HaStateMap;

typedef struct {
    char page[256];
    size_t item_index;

    // Configured behavior (from YAML). These do NOT auto-start: they must be enabled via $cmd.poll_start.
    int cfg_poll_every_ms;
    const char *cfg_poll_cmd;
    bool cfg_poll_is_text;
    CmdTextOpts cfg_poll_opts;

    int cfg_state_every_ms;
    const char *cfg_state_cmd;

    // Active behavior (runtime). Enabled/disabled via actions.
    int poll_every_ms;
    const char *poll_cmd;
    bool poll_is_text;
    CmdTextOpts poll_opts;

    int state_every_ms;
    const char *state_cmd;

    pthread_mutex_t mu;
    bool poll_running;
    bool state_running;
    int64_t next_poll_ns;
    int64_t next_state_ns;

    uint32_t poll_gen;
    uint32_t state_gen;

    char last_text[256];
    char last_state[64];

    // What we've already pushed to the device (current session).
    char last_sent_text[256];
    char last_sent_state[64];
} CmdEntry;

struct CmdEngine {
    // IMPORTANT: CmdEntry contains a pthread_mutex_t, which must not be moved in memory.
    // So we store pointers and allocate each entry independently.
    CmdEntry **items;
    size_t len;
    size_t cap;
    int timeout_ms;
    int notify_r;
    int notify_w;
    pthread_t th;
    bool th_started;
    pthread_mutex_t mu;
    bool stop;
};

static void cmd_engine_notify(const CmdEngine *e);

static void cmd_state_on_enter_page(CmdEngine *e, const char *page) {
    if (!e || !page || !page[0]) return;
    pthread_mutex_lock(&e->mu);
    for (size_t i = 0; i < e->len; i++) {
        CmdEntry *ce = e->items ? e->items[i] : NULL;
        if (!ce) continue;
        if (strcmp(ce->page, page) != 0) continue;
        if (ce->cfg_state_every_ms <= 0 || !ce->cfg_state_cmd || !ce->cfg_state_cmd[0]) continue;
        pthread_mutex_lock(&ce->mu);
        ce->state_gen++;
        ce->state_every_ms = ce->cfg_state_every_ms;
        ce->state_cmd = ce->cfg_state_cmd;
        ce->next_state_ns = 0;
        pthread_mutex_unlock(&ce->mu);
    }
    pthread_mutex_unlock(&e->mu);
    cmd_engine_notify(e);
}

static void cmd_state_on_leave_page(CmdEngine *e, const char *page) {
    if (!e || !page || !page[0]) return;
    pthread_mutex_lock(&e->mu);
    for (size_t i = 0; i < e->len; i++) {
        CmdEntry *ce = e->items ? e->items[i] : NULL;
        if (!ce) continue;
        if (strcmp(ce->page, page) != 0) continue;
        pthread_mutex_lock(&ce->mu);
        ce->state_gen++;
        ce->state_every_ms = 0;
        ce->state_cmd = NULL;
        ce->state_running = false;
        ce->next_state_ns = 0;
        pthread_mutex_unlock(&ce->mu);
    }
    pthread_mutex_unlock(&e->mu);
    cmd_engine_notify(e);
}

static void ha_state_map_free(HaStateMap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->len; i++) {
        free(m->items[i].entity_id);
        free(m->items[i].state);
        free(m->items[i].unit);
    }
    free(m->items);
    memset(m, 0, sizeof(*m));
}

static HaEntityState *ha_state_get_mut(HaStateMap *m, const char *entity_id) {
    if (!m || !entity_id || !entity_id[0]) return NULL;
    for (size_t i = 0; i < m->len; i++) {
        if (m->items[i].entity_id && strcmp(m->items[i].entity_id, entity_id) == 0) return &m->items[i];
    }
    if (m->len >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->items = xrealloc(m->items, m->cap * sizeof(HaEntityState));
    }
    HaEntityState *e = &m->items[m->len++];
    memset(e, 0, sizeof(*e));
    e->entity_id = xstrdup(entity_id);
    e->state = NULL;
    e->unit = NULL;
    return e;
}

static void cmd_engine_notify(const CmdEngine *e) {
    if (!e || e->notify_w < 0) return;
    (void)write(e->notify_w, "u", 1);
}

static CmdEntry *cmd_engine_find(CmdEngine *e, const char *page, size_t item_index) {
    if (!e || !page || !page[0]) return NULL;
    CmdEntry *found = NULL;
    pthread_mutex_lock(&e->mu);
    for (size_t i = 0; i < e->len; i++) {
        CmdEntry *ce = e->items ? e->items[i] : NULL;
        if (!ce) continue;
        if (ce->item_index == item_index && strcmp(ce->page, page) == 0) {
            found = ce;
            break;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return found;
}

static CmdEntry *cmd_engine_get_or_add(CmdEngine *e, const char *page, size_t item_index) {
    if (!e || !page || !page[0]) return NULL;
    pthread_mutex_lock(&e->mu);
    for (size_t i = 0; i < e->len; i++) {
        CmdEntry *ce = e->items ? e->items[i] : NULL;
        if (!ce) continue;
        if (ce->item_index == item_index && strcmp(ce->page, page) == 0) {
            pthread_mutex_unlock(&e->mu);
            return ce;
        }
    }
    if (e->len >= e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->items = xrealloc(e->items, e->cap * sizeof(CmdEntry *));
    }
    CmdEntry *ce = calloc(1, sizeof(*ce));
    if (!ce) die_errno("calloc");
    snprintf(ce->page, sizeof(ce->page), "%s", page);
    ce->item_index = item_index;
    ce->poll_opts = cmd_text_opts_defaults();
    ce->cfg_poll_opts = cmd_text_opts_defaults();
    ce->poll_gen = 1;
    ce->state_gen = 1;
    (void)pthread_mutex_init(&ce->mu, NULL);
    e->items[e->len++] = ce;
    pthread_mutex_unlock(&e->mu);
    return ce;
}

typedef struct {
    CmdEngine *engine;
    CmdEntry *entry;
    bool is_poll;
    bool is_state;
    char *cmd;
    CmdTextOpts opts;
    uint32_t gen;
} CmdRunArgs;

static void *cmd_run_thread(void *arg) {
    CmdRunArgs *a = (CmdRunArgs *)arg;
    if (!a || !a->engine || !a->entry || !a->cmd) {
        free(a ? a->cmd : NULL);
        free(a);
        return NULL;
    }

    char out[256] = {0};
    int rc = run_shell_capture_text(a->cmd, a->engine->timeout_ms, out, sizeof(out), &a->opts, a->is_state);
    // rc==0 => ok; rc==-2 => timeout; rc>0 => command exit status; rc<0 => internal/other failure.

    pthread_mutex_lock(&a->entry->mu);
    if (a->is_state) {
        bool accept = (a->gen == a->entry->state_gen) && (a->entry->state_every_ms > 0);
        if (accept) {
            snprintf(a->entry->last_state, sizeof(a->entry->last_state), "%s", out);
        }
        a->entry->state_running = false;
        pthread_mutex_unlock(&a->entry->mu);
        if (g_cmd_logs && accept) {
            bool is_err = (strncmp(out, "ERR", 3) == 0) || (strncmp(out, "err", 3) == 0);
            if (is_err) {
                if (g_cmd_logs_verbose) {
                    if (rc == -2) cmd_log("state err page=%s btn=%zu rc=timeout", a->entry->page, a->entry->item_index + 1);
                    else cmd_log("state err page=%s btn=%zu rc=%d", a->entry->page, a->entry->item_index + 1, rc);
                } else {
                    cmd_log("state err page=%s btn=%zu", a->entry->page, a->entry->item_index + 1);
                }
            } else if (g_cmd_logs_verbose) {
                cmd_log("state ok page=%s btn=%zu state='%s'", a->entry->page, a->entry->item_index + 1, out);
            }
        }
        if (accept) cmd_engine_notify(a->engine);
        free(a->cmd);
        free(a);
        return NULL;
    }
    bool accept = (a->gen == a->entry->poll_gen) && (a->entry->poll_every_ms > 0);
    if (accept) {
        snprintf(a->entry->last_text, sizeof(a->entry->last_text), "%s", out);
    }
    a->entry->poll_running = false;
    pthread_mutex_unlock(&a->entry->mu);

    if (g_cmd_logs && accept) {
        bool is_err = (strncmp(out, "ERR", 3) == 0) || (strncmp(out, "err", 3) == 0);
        if (is_err) {
            if (g_cmd_logs_verbose) {
                if (rc == -2) cmd_log("poll err page=%s btn=%zu rc=timeout", a->entry->page, a->entry->item_index + 1);
                else cmd_log("poll err page=%s btn=%zu rc=%d", a->entry->page, a->entry->item_index + 1, rc);
            } else {
                cmd_log("poll err page=%s btn=%zu", a->entry->page, a->entry->item_index + 1);
            }
        } else if (g_cmd_logs_verbose) {
            cmd_log("poll ok page=%s btn=%zu text='%s'", a->entry->page, a->entry->item_index + 1, out);
        }
    }
    if (accept) cmd_engine_notify(a->engine);
    free(a->cmd);
    free(a);
    return NULL;
}

typedef struct {
    CmdEngine *engine;
    CmdEntry *entry;
    char *cmd;
    CmdTextOpts opts;
} CmdOneshotTextArgs;

static void *cmd_oneshot_text_thread(void *arg) {
    CmdOneshotTextArgs *a = (CmdOneshotTextArgs *)arg;
    if (!a || !a->engine || !a->entry || !a->cmd) {
        free(a ? a->cmd : NULL);
        free(a);
        return NULL;
    }
    char out[256] = {0};
    int rc = run_shell_capture_text(a->cmd, a->engine->timeout_ms, out, sizeof(out), &a->opts, false);
    pthread_mutex_lock(&a->entry->mu);
    snprintf(a->entry->last_text, sizeof(a->entry->last_text), "%s", out);
    pthread_mutex_unlock(&a->entry->mu);
    if (g_cmd_logs) {
        bool is_err = (strncmp(out, "ERR", 3) == 0) || (strncmp(out, "err", 3) == 0);
        if (is_err) {
            if (g_cmd_logs_verbose) {
                if (rc == -2) cmd_log("exec_text err page=%s btn=%zu rc=timeout", a->entry->page, a->entry->item_index + 1);
                else cmd_log("exec_text err page=%s btn=%zu rc=%d", a->entry->page, a->entry->item_index + 1, rc);
            } else {
                cmd_log("exec_text err page=%s btn=%zu", a->entry->page, a->entry->item_index + 1);
            }
        } else if (g_cmd_logs_verbose) {
            cmd_log("exec_text ok page=%s btn=%zu text='%s'", a->entry->page, a->entry->item_index + 1, out);
        } else {
            cmd_log("exec_text ok page=%s btn=%zu", a->entry->page, a->entry->item_index + 1);
        }
    }
    cmd_engine_notify(a->engine);
    free(a->cmd);
    free(a);
    return NULL;
}

typedef struct {
    CmdEngine *engine;
    char *cmd;
} CmdOneshotExecArgs;

static void *cmd_oneshot_exec_thread(void *arg) {
    CmdOneshotExecArgs *a = (CmdOneshotExecArgs *)arg;
    if (!a || !a->engine || !a->cmd) {
        free(a ? a->cmd : NULL);
        free(a);
        return NULL;
    }
    int rc = run_shell_nocapture(a->cmd, a->engine->timeout_ms);
    if (g_cmd_logs && rc != 0) {
        cmd_log("exec err rc=%d", rc);
    }
    free(a->cmd);
    free(a);
    return NULL;
}

static void *cmd_engine_thread(void *arg) {
    CmdEngine *e = (CmdEngine *)arg;
    if (!e) return NULL;

    CmdEntry **snap = NULL;
    size_t snap_cap = 0;

    while (!e->stop) {
        int64_t now = now_ns_monotonic();
        int64_t next_wake_ns = now + 200 * 1000000LL; // 200ms default

        pthread_mutex_lock(&e->mu);
        size_t n = e->len;
        if (n > snap_cap) {
            snap_cap = n;
            snap = xrealloc(snap, snap_cap * sizeof(*snap));
        }
        if (n > 0 && e->items) memcpy(snap, e->items, n * sizeof(*snap));
        pthread_mutex_unlock(&e->mu);

        for (size_t i = 0; i < n; i++) {
            CmdEntry *ce = snap[i];
            if (!ce) continue;

            // Poll (text or exec) - currently only exec_text supported for rendering.
            if (ce->poll_every_ms > 0 && ce->poll_cmd && ce->poll_cmd[0]) {
                pthread_mutex_lock(&ce->mu);
                if (ce->next_poll_ns == 0) ce->next_poll_ns = now; // run quickly after start
                int64_t due = ce->next_poll_ns;
                bool can_run = (!ce->poll_running && now >= due);
                uint32_t gen = ce->poll_gen;
                if (can_run) {
                    ce->poll_running = true;
                    ce->next_poll_ns = now + (int64_t)ce->poll_every_ms * 1000000LL;
                }
                if (ce->next_poll_ns > 0 && ce->next_poll_ns < next_wake_ns) next_wake_ns = ce->next_poll_ns;
                pthread_mutex_unlock(&ce->mu);

                if (can_run) {
                    if (!ce->poll_is_text) {
                        // No feedback; fire and forget with timeout.
                        (void)run_shell_nocapture(ce->poll_cmd, e->timeout_ms);
                        pthread_mutex_lock(&ce->mu);
                        ce->poll_running = false;
                        pthread_mutex_unlock(&ce->mu);
                    } else {
                        CmdRunArgs *a = calloc(1, sizeof(*a));
                        if (!a) die_errno("calloc");
                        a->engine = e;
                        a->entry = ce;
                        a->is_poll = true;
                        a->is_state = false;
                        a->cmd = xstrdup(ce->poll_cmd);
                        a->opts = ce->poll_opts;
                        a->gen = gen;
                        pthread_t th;
                        if (pthread_create(&th, NULL, cmd_run_thread, a) == 0) {
                            pthread_detach(th);
                        } else {
                            pthread_mutex_lock(&ce->mu);
                            ce->poll_running = false;
                            pthread_mutex_unlock(&ce->mu);
                            free(a->cmd);
                            free(a);
                        }
                    }
                }
            }

            // State polling
            if (ce->state_every_ms > 0 && ce->state_cmd && ce->state_cmd[0]) {
                pthread_mutex_lock(&ce->mu);
                if (ce->next_state_ns == 0) ce->next_state_ns = now;
                int64_t due = ce->next_state_ns;
                bool can_run = (!ce->state_running && now >= due);
                uint32_t gen = ce->state_gen;
                if (can_run) {
                    ce->state_running = true;
                    ce->next_state_ns = now + (int64_t)ce->state_every_ms * 1000000LL;
                }
                if (ce->next_state_ns > 0 && ce->next_state_ns < next_wake_ns) next_wake_ns = ce->next_state_ns;
                pthread_mutex_unlock(&ce->mu);

                if (can_run) {
                    CmdRunArgs *a = calloc(1, sizeof(*a));
                    if (!a) die_errno("calloc");
                    a->engine = e;
                    a->entry = ce;
                    a->is_poll = false;
                    a->is_state = true;
                    a->cmd = xstrdup(ce->state_cmd);
                    a->opts = cmd_text_opts_defaults();
                    a->opts.trim = true;
                    a->opts.max_len = 32;
                    a->gen = gen;
                    pthread_t th;
                    if (pthread_create(&th, NULL, cmd_run_thread, a) == 0) {
                        pthread_detach(th);
                    } else {
                        pthread_mutex_lock(&ce->mu);
                        ce->state_running = false;
                        pthread_mutex_unlock(&ce->mu);
                        free(a->cmd);
                        free(a);
                    }
                }
            }
        }

        now = now_ns_monotonic();
        int64_t sleep_ns = next_wake_ns - now;
        if (sleep_ns < 5 * 1000000LL) sleep_ns = 5 * 1000000LL;
        if (sleep_ns > 500 * 1000000LL) sleep_ns = 500 * 1000000LL;
        struct timespec ts;
        ts.tv_sec = (time_t)(sleep_ns / 1000000000LL);
        ts.tv_nsec = (long)(sleep_ns % 1000000000LL);
        nanosleep(&ts, NULL);
    }
    free(snap);
    return NULL;
}

static void cmd_engine_free(CmdEngine *e) {
    if (!e) return;
    e->stop = true;
    if (e->th_started) (void)pthread_join(e->th, NULL);
    if (e->notify_r >= 0) close(e->notify_r);
    if (e->notify_w >= 0) close(e->notify_w);
    for (size_t i = 0; i < e->len; i++) {
        CmdEntry *ce = e->items[i];
        if (!ce) continue;
        (void)pthread_mutex_destroy(&ce->mu);
        free(ce);
    }
    free(e->items);
    (void)pthread_mutex_destroy(&e->mu);
    memset(e, 0, sizeof(*e));
}

static int cmd_engine_init(CmdEngine *e, const Config *cfg) {
    if (!e || !cfg) return -1;
    memset(e, 0, sizeof(*e));
    e->timeout_ms = cfg->cmd_timeout_ms > 0 ? cfg->cmd_timeout_ms : 3000;
    e->notify_r = -1;
    e->notify_w = -1;
    e->th_started = false;
    (void)pthread_mutex_init(&e->mu, NULL);
    int p[2];
    if (pipe(p) != 0) return -1;
    e->notify_r = p[0];
    e->notify_w = p[1];
    int flags = fcntl(e->notify_r, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(e->notify_r, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(e->notify_w, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(e->notify_w, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

static int cmd_engine_start(CmdEngine *e) {
    if (!e) return -1;
    if (e->th_started) return 0;
    if (pthread_create(&e->th, NULL, cmd_engine_thread, e) != 0) return -1;
    e->th_started = true;
    return 0;
}

static void crash_handler(int sig) {
    const char *hdr = "\n[pg] FATAL: paging_daemon crashed\n";
    (void)write(STDERR_FILENO, hdr, strlen(hdr));

    void *bt[64];
    int n = backtrace(bt, (int)(sizeof(bt) / sizeof(bt[0])));
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(128 + sig);
}

static void cmd_engine_build_from_config(CmdEngine *e, const Config *cfg) {
    if (!e || !cfg) return;
    for (size_t pi = 0; pi < cfg->page_count; pi++) {
        const Page *p = &cfg->pages[pi];
        if (!p->name) continue;
        for (size_t ii = 0; ii < p->count; ii++) {
            const Item *it = &p->items[ii];
            if (!it) continue;

            bool need = false;
            if (it->poll_every_ms > 0 && it->poll_action && it->poll_action[0] && it->poll_cmd && it->poll_cmd[0]) need = true;
            if (it->state_every_ms > 0 && it->state_cmd && it->state_cmd[0]) need = true;
            // also create entries for one-shot exec_text actions so we can store results,
            // and for poll control actions (so runtime doesn't need to allocate entries).
            const ActionSeq *seqs[] = { &it->tap_seq, &it->hold_seq, &it->longhold_seq, &it->released_seq };
            for (size_t si = 0; si < sizeof(seqs) / sizeof(seqs[0]); si++) {
                const ActionSeq *s = seqs[si];
                if (!s || s->len == 0) continue;
                for (size_t ai = 0; ai < s->len; ai++) {
                    const char *a = s->steps[ai].action;
                    if (!a || !a[0]) continue;
                    if (strncmp(a, "$cmd.", 5) == 0) need = true;
                    if (strcmp(a, "$cmd.exec_text") == 0) need = true;
                }
            }
            // Backward compatibility: if a config didn't provide action sequences (old configs), fall back
            // to the legacy single action fields.
            if (!need) {
                if (it->tap_action && strncmp(it->tap_action, "$cmd.", 5) == 0) need = true;
                if (it->hold_action && strncmp(it->hold_action, "$cmd.", 5) == 0) need = true;
                if (it->longhold_action && strncmp(it->longhold_action, "$cmd.", 5) == 0) need = true;
                if (it->released_action && strncmp(it->released_action, "$cmd.", 5) == 0) need = true;
            }
            if (!need) continue;

            CmdEntry *ce = cmd_engine_get_or_add(e, p->name, ii);
            if (!ce) continue;

            if (it->poll_every_ms > 0 && it->poll_cmd && it->poll_cmd[0] && it->poll_action && it->poll_action[0]) {
                ce->cfg_poll_every_ms = it->poll_every_ms;
                ce->cfg_poll_cmd = it->poll_cmd;
                ce->cfg_poll_is_text = (strcmp(it->poll_action, "$cmd.exec_text") == 0);
                ce->cfg_poll_opts = it->poll_cmd_text;
            }
            if (it->state_every_ms > 0 && it->state_cmd && it->state_cmd[0]) {
                ce->cfg_state_every_ms = it->state_every_ms;
                ce->cfg_state_cmd = it->state_cmd;
            }

            // Never auto-start polls at daemon boot. They must be started via $cmd.poll_start.
            ce->poll_every_ms = 0;
            ce->poll_cmd = NULL;
            ce->poll_is_text = false;
            ce->poll_opts = cmd_text_opts_defaults();
            ce->state_every_ms = 0;
            ce->state_cmd = NULL;
        }
    }
}

static int json_tok_eq(const char *json, const jsmntok_t *t, const char *s) {
    if (!json || !t || !s) return 0;
    size_t sl = strlen(s);
    size_t tl = (size_t)(t->end - t->start);
    if (sl != tl) return 0;
    return strncmp(json + t->start, s, sl) == 0;
}

static char *json_tok_strdup(const char *json, const jsmntok_t *t) {
    if (!json || !t) return NULL;
    int n = t->end - t->start;
    if (n <= 0) return xstrdup("");
    char *out = malloc((size_t)n + 1);
    if (!out) die_errno("malloc");
    memcpy(out, json + t->start, (size_t)n);
    out[n] = 0;
    return out;
}

static int jsmn_skip_subtree(const jsmntok_t *toks, int tok_count, int idx) {
    int i = idx;
    int remaining = 1;
    while (remaining > 0 && i < tok_count) {
        const jsmntok_t *t = &toks[i];
        remaining--;
        if (t->type == JSMN_OBJECT) remaining += t->size * 2;
        else if (t->type == JSMN_ARRAY) remaining += t->size;
        i++;
    }
    return i;
}

static int json_find_object_value(const char *json, const jsmntok_t *toks, int tok_count, int obj_idx, const char *key) {
    if (!json || !toks || tok_count <= 0 || !key) return -1;
    if (obj_idx < 0 || obj_idx >= tok_count) return -1;
    const jsmntok_t *obj = &toks[obj_idx];
    if (obj->type != JSMN_OBJECT) return -1;

    int i = obj_idx + 1;
    for (int pair = 0; pair < obj->size; pair++) {
        if (i + 1 >= tok_count) return -1;
        const jsmntok_t *k = &toks[i];
        if (k->type == JSMN_STRING && json_tok_eq(json, k, key)) return i + 1;
        // Move to next pair: skip key + skip value subtree.
        i = jsmn_skip_subtree(toks, tok_count, i + 1);
    }
    return -1;
}

static int parse_ha_state_json(const char *json, char **out_state, char **out_unit) {
    if (!json || !out_state || !out_unit) return -1;
    *out_state = NULL;
    *out_unit = NULL;

    jsmn_parser p;
    jsmn_init(&p);
    jsmntok_t toks[512];
    int rc = jsmn_parse(&p, json, strlen(json), toks, (int)(sizeof(toks) / sizeof(toks[0])));
    if (rc < 0) return -1;
    int tok_count = rc;
    if (tok_count <= 0 || toks[0].type != JSMN_OBJECT) return -1;

    int state_idx = json_find_object_value(json, toks, tok_count, 0, "state");
    if (state_idx >= 0 && state_idx < tok_count && toks[state_idx].type == JSMN_STRING) {
        *out_state = json_tok_strdup(json, &toks[state_idx]);
    }

    int attrs_idx = json_find_object_value(json, toks, tok_count, 0, "attributes");
    if (attrs_idx >= 0 && attrs_idx < tok_count && toks[attrs_idx].type == JSMN_OBJECT) {
        int unit_idx = json_find_object_value(json, toks, tok_count, attrs_idx, "unit_of_measurement");
        if (unit_idx >= 0 && unit_idx < tok_count && toks[unit_idx].type == JSMN_STRING) {
            *out_unit = json_tok_strdup(json, &toks[unit_idx]);
        }
    }

    if (!*out_state) *out_state = xstrdup("");
    return 0;
}

static void ha_state_update_from_json(HaStateMap *m, const char *entity_id, const char *json_state) {
    if (!m || !entity_id || !entity_id[0] || !json_state) return;
    char *st = NULL;
    char *unit = NULL;
    if (parse_ha_state_json(json_state, &st, &unit) != 0) {
        free(st);
        free(unit);
        return;
    }
    HaEntityState *e = ha_state_get_mut(m, entity_id);
    if (!e) { free(st); free(unit); return; }
    free(e->state);
    free(e->unit);
    e->state = st;
    e->unit = unit;
}

static void ha_state_update_from_get_reply(HaStateMap *m, const char *entity_id, const char *reply_line) {
    // reply: "ok {json_state}" or "err ..."
    if (!m || !entity_id || !reply_line) return;
    if (strncmp(reply_line, "ok", 2) != 0) return;
    const char *p = reply_line + 2;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '{') return;
    ha_state_update_from_json(m, entity_id, p);
}

static void ha_format_value_text(const HaStateMap *m, const char *entity_id, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!m || !entity_id) return;
    for (size_t i = 0; i < m->len; i++) {
        const HaEntityState *e = &m->items[i];
        if (!e->entity_id || strcmp(e->entity_id, entity_id) != 0) continue;
        const char *s = e->state ? e->state : "";
        const char *u = e->unit ? e->unit : "";
        if (s[0] == 0) { snprintf(out, cap, "..."); return; }
        if (u[0]) snprintf(out, cap, "%s %s", s, u);
        else snprintf(out, cap, "%s", s);
        return;
    }
}

static bool ha_entity_is_value_display(const char *entity_id) {
    if (!entity_id || !entity_id[0]) return false;
    // Only show raw HA state as text for value-like domains (sensor readings, numbers, etc).
    // For toggle-like domains (script/light/switch/...), users should define `states:` overrides.
    return (strncmp(entity_id, "sensor.", 7) == 0) ||
           (strncmp(entity_id, "number.", 7) == 0) ||
           (strncmp(entity_id, "input_number.", 13) == 0);
}

typedef struct {
    char *entity_id;
    int sub_id;
} HaSub;

typedef struct {
    HaSub *items;
    size_t len;
    size_t cap;
} HaSubs;

static void ha_subs_free(HaSubs *s) {
    if (!s) return;
    for (size_t i = 0; i < s->len; i++) free(s->items[i].entity_id);
    free(s->items);
    memset(s, 0, sizeof(*s));
}

static void ha_subs_clear_no_unsub(HaSubs *s) {
    if (!s) return;
    for (size_t i = 0; i < s->len; i++) free(s->items[i].entity_id);
    s->len = 0;
}

static int write_all_fd(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int ha_send_line_fd(int fd, const char *line) {
    if (fd < 0 || !line) return -1;
    size_t n = strlen(line);
    if (write_all_fd(fd, line, n) != 0) return -1;
    if (n == 0 || line[n - 1] != '\n') {
        if (write_all_fd(fd, "\n", 1) != 0) return -1;
    }
    return 0;
}

static int read_line_from_fd(int fd, char *out, size_t out_cap, char *buf, size_t *inlen) {
    if (!out || out_cap == 0 || !buf || !inlen) return -1;
    out[0] = 0;

    for (;;) {
        // Check for a full line.
        for (size_t i = 0; i < *inlen; i++) {
            if (buf[i] == '\n') {
                size_t n = i + 1;
                if (n >= out_cap) n = out_cap - 1;
                memcpy(out, buf, n);
                out[n] = 0;
                trim(out);
                // Consume from buffer.
                memmove(buf, buf + i + 1, *inlen - (i + 1));
                *inlen -= (i + 1);
                return 1;
            }
        }

        // Need more data.
        ssize_t r = read(fd, buf + *inlen, 8192 - *inlen);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (r == 0) return -1;
        *inlen += (size_t)r;
        if (*inlen >= 8192) {
            // Too long line; drop buffer.
            *inlen = 0;
            return -1;
        }
    }
}

static int generate_icon_pipeline(const Options *opt, const Preset *preset, const Item *it, const char *out_png) {
    // Base: draw_square, optional borders, optional mdi, optimize, optional text, optimize
    if (!it) return -1;
    ensure_dir_parent(out_png);
    char draw_square_bin[PATH_MAX];
    char draw_border_bin[PATH_MAX];
    char draw_mdi_bin[PATH_MAX];
    char draw_text_bin[PATH_MAX];
    char draw_opt_bin[PATH_MAX];
    snprintf(draw_square_bin, sizeof(draw_square_bin), "%s/icons/draw_square", opt->root_dir);
    snprintf(draw_border_bin, sizeof(draw_border_bin), "%s/icons/draw_border", opt->root_dir);
    snprintf(draw_mdi_bin, sizeof(draw_mdi_bin), "%s/icons/draw_mdi", opt->root_dir);
    snprintf(draw_text_bin, sizeof(draw_text_bin), "%s/icons/draw_text", opt->root_dir);
    snprintf(draw_opt_bin, sizeof(draw_opt_bin), "%s/icons/draw_optimize", opt->root_dir);
    if (access(draw_square_bin, X_OK) != 0) return -1;
    if (access(draw_text_bin, X_OK) != 0) return -1;
    if (access(draw_opt_bin, X_OK) != 0) return -1;

    const char *bg = (preset && preset->icon_background_color && preset->icon_background_color[0]) ? preset->icon_background_color : "transparent";
    const char *border_c = (preset && preset->icon_border_color && preset->icon_border_color[0]) ? preset->icon_border_color : "FFFFFF";
    const char *ic_color = (preset && preset->icon_color && preset->icon_color[0]) ? preset->icon_color : "FFFFFF";
    bool icon_color_transparent = (ic_color && strcasecmp(ic_color, "transparent") == 0);
    int rad = preset ? clamp_int(preset->icon_border_radius, 0, 50) : 0;
    int border_size = preset ? clamp_int(preset->icon_border_size, 98, 196) : 196;
    int bw = preset ? clamp_int(preset->icon_border_width, 0, 98) : 0;
    int pad = preset ? clamp_int(preset->icon_padding, 0, 98) : 0;
    int off_x = preset ? preset->icon_offset_x : 0;
    int off_y = preset ? preset->icon_offset_y : 0;
    int bright = preset ? clamp_int(preset->icon_brightness, 1, 99) : 99;

    // Pipeline:
    //   draw_square
    //   draw_border (outer + inner) if border_width > 0
    //   draw_mdi (optional)
    //   draw_optimize (mandatory)
    //   draw_text (optional)
    //   draw_optimize (optional)

    char sq_size[32];
    snprintf(sq_size, sizeof(sq_size), "--size=196");

    // If border is enabled, start from transparent square; borders will define outer + inner fill.
    const char *sq_color = (bw > 0) ? "transparent" : bg;
    {
        char *argv[] = { draw_square_bin, (char *)sq_color, sq_size, (char *)out_png, NULL };
        if (run_exec(argv) != 0) return -1;
    }

    if (bw > 0) {
        if (access(draw_border_bin, X_OK) != 0) return -1;
        char size_outer[32];
        char rad_arg[32];
        snprintf(size_outer, sizeof(size_outer), "--size=%d", border_size);
        snprintf(rad_arg, sizeof(rad_arg), "--radius=%d", rad);
        char *argv_outer[] = { draw_border_bin, (char *)border_c, size_outer, rad_arg, (char *)out_png, NULL };
        if (run_exec(argv_outer) != 0) return -1;

        int inner = border_size - 2 * bw;
        inner = clamp_int(inner, 1, 196);
        char size_inner[32];
        snprintf(size_inner, sizeof(size_inner), "--size=%d", inner);
        char *argv_inner[] = { draw_border_bin, (char *)bg, size_inner, rad_arg, (char *)out_png, NULL };
        if (run_exec(argv_inner) != 0) return -1;
    }

    // draw_mdi (optional)
    bool mdi_transparent = false;
    if (it->icon && strncmp(it->icon, "mdi:", 4) == 0) {
        if (access(draw_mdi_bin, X_OK) != 0) return -1;
        if (ensure_mdi_svg(opt, it->icon) != 0) return -1;
        mdi_transparent = icon_color_transparent;
        int max_allowed = 196 - 2 * (bw + pad);
        max_allowed = clamp_int(max_allowed, 1, 196);
        int icon_size = preset ? preset->icon_size : 128;
        if (icon_size <= 0) icon_size = max_allowed;
        icon_size = clamp_int(icon_size, 1, 196);
        if (icon_size > max_allowed) icon_size = max_allowed;
        char size_arg[32];
        snprintf(size_arg, sizeof(size_arg), "--size=%d", icon_size);
        char off_arg[64];
        snprintf(off_arg, sizeof(off_arg), "--offset=%d,%d", off_x, off_y);
        char bri_arg[32];
        snprintf(bri_arg, sizeof(bri_arg), "--brightness=%d", bright);
        char *argv[] = { draw_mdi_bin, (char *)it->icon, (char *)ic_color, size_arg, off_arg, bri_arg, (char *)out_png, NULL };
        if (run_exec(argv) != 0) return -1;
    }

    // draw_optimize (mandatory)
    // For transparent MDI mode, skip this first optimize pass for now.
    // (We still optimize after draw_text if text is present.)
    if (!mdi_transparent) {
        char *argv[] = { draw_opt_bin, (char *)"-c", (char *)"4", (char *)out_png, NULL };
        if (run_exec(argv) != 0) return -1;
    }

    // draw_text (optional)
    if (it->text && it->text[0]) {
        const char *tc = (preset && preset->text_color && preset->text_color[0]) ? preset->text_color : "FFFFFF";
        const char *ta = (preset && preset->text_align && preset->text_align[0]) ? preset->text_align : "center";
        const char *tf = (preset && preset->text_font && preset->text_font[0]) ? preset->text_font : "Roboto";
        bool used_default_font = !(preset && preset->text_font && preset->text_font[0]);
        int ts = preset ? clamp_int(preset->text_size, 1, 64) : 40;
        int tox = preset ? preset->text_offset_x : 0;
        int toy = preset ? preset->text_offset_y : 0;

        char text_arg[768];
        snprintf(text_arg, sizeof(text_arg), "--text=%s", it->text);
        char tc_arg[64];
        snprintf(tc_arg, sizeof(tc_arg), "--text_color=%s", tc);
        char ta_arg[64];
        snprintf(ta_arg, sizeof(ta_arg), "--text_align=%s", ta);
        char ts_arg[64];
        snprintf(ts_arg, sizeof(ts_arg), "--text_size=%d", ts);
        char to_arg[64];
        snprintf(to_arg, sizeof(to_arg), "--text_offset=%d,%d", tox, toy);

        int rc = 0;
        if (tf && tf[0]) {
            char tf_arg[PATH_MAX];
            snprintf(tf_arg, sizeof(tf_arg), "--text_font=%s", tf);
            char *argv[] = { draw_text_bin, text_arg, tc_arg, ta_arg, tf_arg, ts_arg, to_arg, (char *)out_png, NULL };
            rc = run_exec(argv);
            if (rc != 0 && used_default_font) {
                // If "Roboto" isn't available, fall back to the draw_text default font behavior.
                char *argv2[] = { draw_text_bin, text_arg, tc_arg, ta_arg, ts_arg, to_arg, (char *)out_png, NULL };
                rc = run_exec(argv2);
            }
        } else {
            char *argv[] = { draw_text_bin, text_arg, tc_arg, ta_arg, ts_arg, to_arg, (char *)out_png, NULL };
            rc = run_exec(argv);
        }
        if (rc != 0) return -1;

        // Second optimize pass (after draw_text).
        char *argv2[] = { draw_opt_bin, (char *)"-c", (char *)"4", (char *)out_png, NULL };
        if (run_exec(argv2) != 0) return -1;
    }

    return 0;
}

static int render_value_text_on_base_tmp(const Options *opt, const Preset *preset, const char *page_name, int pos,
                                      const char *base_png, const char *text, char *out_tmp_png, size_t out_cap) {
    if (!opt || !page_name || !base_png || !text || !out_tmp_png || out_cap == 0) return -1;
    out_tmp_png[0] = 0;
    if (!file_exists(base_png)) return -1;

    char dir[PATH_MAX];
    state_dir(opt, dir, sizeof(dir));
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", dir);
    ensure_dir(tmpdir);

    char page_tag[96];
    sanitize_suffix(page_name, page_tag, sizeof(page_tag));
    if (page_tag[0] == 0) snprintf(page_tag, sizeof(page_tag), "page");

    long t = (long)time(NULL);
    pid_t pid = getpid();
    char outpng[PATH_MAX];
    snprintf(outpng, sizeof(outpng), "%s/value_%s_%d_%ld_%d.png", tmpdir, page_tag, (int)pid, t, pos);

    // If base_png is the minimal 1x1 empty.png (used to keep zips small), drawing text on it produces a
    // single pixel that the device scales up. In that case, create a proper 196x196 base first.
    int bw = 0, bh = 0;
    bool base_is_1x1 = (png_read_wh(base_png, &bw, &bh) == 0 && bw == 1 && bh == 1);
    if (!base_is_1x1) {
        if (copy_file(base_png, outpng) != 0) return -1;
    } else {
        char draw_square_bin[PATH_MAX];
        char draw_border_bin[PATH_MAX];
        snprintf(draw_square_bin, sizeof(draw_square_bin), "%s/icons/draw_square", opt->root_dir);
        snprintf(draw_border_bin, sizeof(draw_border_bin), "%s/icons/draw_border", opt->root_dir);
        if (access(draw_square_bin, X_OK) != 0) return -1;

        const char *bg = (preset && preset->icon_background_color && preset->icon_background_color[0]) ? preset->icon_background_color : "transparent";
        int bwid = preset ? clamp_int(preset->icon_border_width, 0, 98) : 0;
        int rad = preset ? clamp_int(preset->icon_border_radius, 0, 50) : 0;
        int border_size = preset ? clamp_int(preset->icon_border_size, 98, 196) : 196;
        const char *border_c = (preset && preset->icon_border_color && preset->icon_border_color[0]) ? preset->icon_border_color : "FFFFFF";

        // Same rule as icon pipeline: if border is enabled, start from transparent square; borders define the fill.
        const char *sq_color = (bwid > 0) ? "transparent" : bg;
        char size_arg[32];
        snprintf(size_arg, sizeof(size_arg), "--size=196");
        char *argv_sq[] = { draw_square_bin, (char *)sq_color, size_arg, outpng, NULL };
        if (run_exec(argv_sq) != 0) { unlink(outpng); return -1; }

        if (bwid > 0) {
            if (access(draw_border_bin, X_OK) != 0) { unlink(outpng); return -1; }
            char size_outer[32];
            char rad_arg[32];
            snprintf(size_outer, sizeof(size_outer), "--size=%d", border_size);
            snprintf(rad_arg, sizeof(rad_arg), "--radius=%d", rad);
            char *argv_outer[] = { draw_border_bin, (char *)border_c, size_outer, rad_arg, outpng, NULL };
            if (run_exec(argv_outer) != 0) { unlink(outpng); return -1; }

            int inner = border_size - 2 * bwid;
            inner = clamp_int(inner, 1, 196);
            char size_inner[32];
            snprintf(size_inner, sizeof(size_inner), "--size=%d", inner);
            char *argv_inner[] = { draw_border_bin, (char *)bg, size_inner, rad_arg, outpng, NULL };
            if (run_exec(argv_inner) != 0) { unlink(outpng); return -1; }
        }
    }

    char draw_text_bin[PATH_MAX];
    char draw_opt_bin[PATH_MAX];
    snprintf(draw_text_bin, sizeof(draw_text_bin), "%s/icons/draw_text", opt->root_dir);
    snprintf(draw_opt_bin, sizeof(draw_opt_bin), "%s/icons/draw_optimize", opt->root_dir);
    if (access(draw_text_bin, X_OK) != 0) { unlink(outpng); return -1; }
    if (access(draw_opt_bin, X_OK) != 0) { unlink(outpng); return -1; }

    // If the target image isn't 196x196 (e.g. wallpaper tiles / external icons), scale text params so a config
    // written for 196px keeps similar proportions.
    int img_w = 0, img_h = 0;
    bool have_wh = (png_read_wh(outpng, &img_w, &img_h) == 0);
    int ref = 196;
    int min_wh = (have_wh ? (img_w < img_h ? img_w : img_h) : ref);
    double ratio = (have_wh && min_wh > 0) ? ((double)min_wh / (double)ref) : 1.0;
    if (ratio <= 0.0) ratio = 1.0;
    bool is_ref_size = (!have_wh) || (img_w == ref && img_h == ref);

    const char *tc = (preset && preset->text_color && preset->text_color[0]) ? preset->text_color : "FFFFFF";
    const char *ta = (preset && preset->text_align && preset->text_align[0]) ? preset->text_align : "center";
    const char *tf = (preset && preset->text_font && preset->text_font[0]) ? preset->text_font : "Roboto";
    bool used_default_font = !(preset && preset->text_font && preset->text_font[0]);
    int ts = preset ? clamp_int(preset->text_size, 1, 64) : 40;
    int tox = preset ? preset->text_offset_x : 0;
    int toy = preset ? preset->text_offset_y : 0;

    int ts_eff = ts;
    int tox_eff = tox;
    int toy_eff = toy;
    if (!is_ref_size) {
        ts_eff = (int)(ts * ratio + 0.5);
        if (ts_eff < 6) ts_eff = 6;
        if (ts_eff > 196) ts_eff = 196;
        tox_eff = (int)(tox * ratio + (tox >= 0 ? 0.5 : -0.5));
        toy_eff = (int)(toy * ratio + (toy >= 0 ? 0.5 : -0.5));
    }

    char text_arg[768];
    snprintf(text_arg, sizeof(text_arg), "--text=%s", text);
    char tc_arg[64];
    snprintf(tc_arg, sizeof(tc_arg), "--text_color=%s", tc);
    char ta_arg[64];
    snprintf(ta_arg, sizeof(ta_arg), "--text_align=%s", ta);
    char tf_arg[PATH_MAX];
    snprintf(tf_arg, sizeof(tf_arg), "--text_font=%s", tf);
    char ts_arg[64];
    snprintf(ts_arg, sizeof(ts_arg), "--text_size=%d", ts_eff);
    char to_arg[64];
    snprintf(to_arg, sizeof(to_arg), "--text_offset=%d,%d", tox_eff, toy_eff);

    char *argv_text[] = { draw_text_bin, text_arg, tc_arg, ta_arg, tf_arg, ts_arg, to_arg, outpng, NULL };
    int rc = run_exec(argv_text);
    if (rc != 0 && used_default_font) {
        // If "Roboto" isn't available, fall back to the draw_text default font behavior.
        char *argv_text2[] = { draw_text_bin, text_arg, tc_arg, ta_arg, ts_arg, to_arg, outpng, NULL };
        rc = run_exec(argv_text2);
    }
    if (rc != 0) { unlink(outpng); return -1; }

    // Post-text optimize:
    // - Classic 196x196 icons: keep the existing 4-color behavior (fast + small ZIPs).
    // - Other sizes (wallpaper tiles, external icons): never quantize to 4 colors; only optimize if needed for the
    //   device icon size constraint (<= 6KB), and then use 128 colors.
    if (is_ref_size) {
        char *argv_opt[] = { draw_opt_bin, (char *)"-d", (char *)"-c=4", outpng, NULL };
        if (run_exec(argv_opt) != 0) { unlink(outpng); return -1; }
    } else {
        struct stat st;
        if (stat(outpng, &st) == 0 && st.st_size > 6 * 1024) {
            char *argv_opt[] = { draw_opt_bin, (char *)"-d", (char *)"-c=128", outpng, NULL };
            if (run_exec(argv_opt) != 0) { unlink(outpng); return -1; }
        }
    }

    snprintf(out_tmp_png, out_cap, "%s", outpng);
    return 0;
}

static void sanitize_suffix(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in) return;
    size_t w = 0;
    for (size_t i = 0; in[i] && w + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[w++] = (char)c;
        } else {
            out[w++] = '_';
        }
    }
    out[w] = 0;
}

static const char *file_too_big_png(const Options *opt) {
    static const char *fallback = "assets/pregen/filetobig.png";
    if (!opt) return fallback;
    static char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s/filetobig.png", opt->sys_pregen_dir ? opt->sys_pregen_dir : "assets/pregen");
    if (file_exists(buf)) return buf;
    if (opt->error_icon && file_exists(opt->error_icon)) return opt->error_icon;
    return fallback;
}

static bool icon_is_prefixed(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static int validate_external_png_final(const char *path) {
    if (!path || !path[0]) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (st.st_size <= 0) return -1;
    if (st.st_size > 6 * 1024) return -2;
    int w = 0, h = 0;
    if (png_read_wh(path, &w, &h) != 0) return -3;
    if (w != h) return -4;
    if (w > 196 || h > 196) return -5;
    return 0;
}

typedef enum {
    EXT_FILE_UNKNOWN = 0,
    EXT_FILE_PNG = 1,
    EXT_FILE_SVG = 2,
} ExtFileType;

static bool str_endswith_ci(const char *s, const char *suf) {
    if (!s || !suf) return false;
    size_t sl = strlen(s);
    size_t su = strlen(suf);
    if (su > sl) return false;
    return strcasecmp(s + (sl - su), suf) == 0;
}

static ExtFileType sniff_external_file_type(const char *path) {
    if (!path || !path[0]) return EXT_FILE_UNKNOWN;
    if (str_endswith_ci(path, ".svg")) return EXT_FILE_SVG;
    if (str_endswith_ci(path, ".png")) return EXT_FILE_PNG;

    FILE *fp = fopen(path, "rb");
    if (!fp) return EXT_FILE_UNKNOWN;
    unsigned char hdr[256];
    size_t n = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    if (n >= 8 && memcmp(hdr, "\x89PNG\r\n\x1a\n", 8) == 0) return EXT_FILE_PNG;

    // Skip leading whitespace.
    size_t i = 0;
    while (i < n && (hdr[i] == ' ' || hdr[i] == '\t' || hdr[i] == '\n' || hdr[i] == '\r')) i++;
    if (i < n && hdr[i] == '<') {
        // Very cheap check for svg/XML.
        for (size_t j = i; j + 3 < n; j++) {
            if ((hdr[j] == '<' || hdr[j] == ' ') &&
                (hdr[j + 1] == 's' || hdr[j + 1] == 'S') &&
                (hdr[j + 2] == 'v' || hdr[j + 2] == 'V') &&
                (hdr[j + 3] == 'g' || hdr[j + 3] == 'G'))
                return EXT_FILE_SVG;
        }
        // Could still be svg even if not matched; leave unknown.
    }
    return EXT_FILE_UNKNOWN;
}

static int download_url_to_file(const char *url, const char *out_path) {
    if (!url || !url[0] || !out_path || !out_path[0]) return -1;

    // Try curl first, then wget.
    {
        char *argv[] = {
            (char *)"curl",
            (char *)"-fsSL",
            (char *)"--max-time",
            (char *)"5",
            (char *)"-o",
            (char *)out_path,
            (char *)url,
            NULL,
        };
        int rc = run_exec(argv);
        if (rc == 0) return 0;
        // 127 is a good signal that the executable isn't present.
        if (rc != 127) return -1;
    }
    {
        char *argv[] = {
            (char *)"wget",
            (char *)"-q",
            (char *)"-O",
            (char *)out_path,
            (char *)url,
            NULL,
        };
        int rc = run_exec(argv);
        return (rc == 0) ? 0 : -1;
    }
}

static bool resolve_external_icon_session(const Options *opt, const char *spec, char *out_path, size_t out_cap) {
    if (!opt || !spec || !spec[0] || !out_path || out_cap == 0) return false;
    out_path[0] = 0;

    const char *kind = NULL;
    const char *val = NULL;
    if (icon_is_prefixed(spec, "local:")) {
        kind = "local";
        val = spec + 6;
    } else if (icon_is_prefixed(spec, "url:")) {
        kind = "url";
        val = spec + 4;
    } else {
        return false;
    }
    if (!val || !val[0]) return false;

    uint32_t h = fnv1a32(spec, strlen(spec));

    // Disk cache (normalized) under .cache
    char cache_dir[PATH_MAX];
    snprintf(cache_dir, sizeof(cache_dir), "%s/external_icons", opt->cache_root);
    ensure_dir(cache_dir);

    char disk[PATH_MAX];
    snprintf(disk, sizeof(disk), "%s/%08x.png", cache_dir, (unsigned)h);

    // Session cache under /dev/shm (copy of disk cache)
    char sdir[PATH_MAX];
    state_dir(opt, sdir, sizeof(sdir));
    char sess_dir[PATH_MAX];
    snprintf(sess_dir, sizeof(sess_dir), "%s/external_icons_session", sdir);
    ensure_dir(sess_dir);
    char sess[PATH_MAX];
    snprintf(sess, sizeof(sess), "%s/%08x.png", sess_dir, (unsigned)h);

    // If session copy exists and is valid, use it.
    if (file_exists(sess) && validate_external_png_final(sess) == 0) {
        snprintf(out_path, out_cap, "%s", sess);
        return true;
    }

    // If disk cache exists and is valid, copy into session and use it.
    if (file_exists(disk) && validate_external_png_final(disk) == 0) {
        (void)copy_file(disk, sess);
        if (file_exists(sess) && validate_external_png_final(sess) == 0) {
            snprintf(out_path, out_cap, "%s", sess);
            return true;
        }
        (void)unlink(sess);
        return false;
    }

    // Build cache.
    char tmp_out[PATH_MAX];
    snprintf(tmp_out, sizeof(tmp_out), "%s/%08x.tmp.%d.png", cache_dir, (unsigned)h, (int)getpid());
    (void)unlink(tmp_out);

    char draw_norm_bin[PATH_MAX];
    snprintf(draw_norm_bin, sizeof(draw_norm_bin), "%s/icons/draw_normalize", opt->root_dir);
    char draw_svg_bin[PATH_MAX];
    snprintf(draw_svg_bin, sizeof(draw_svg_bin), "%s/icons/draw_svg", opt->root_dir);

    // Prepare an input file path (downloaded to /dev/shm/tmp for url:).
    char input_path[PATH_MAX] = {0};
    char dl_tmp[PATH_MAX] = {0};

    if (strcmp(kind, "local") == 0) {
        if (val[0] == '/') snprintf(input_path, sizeof(input_path), "%s", val);
        else snprintf(input_path, sizeof(input_path), "%s/%s", opt->root_dir, val);
        if (!file_exists(input_path)) return false;
    } else {
        char tmpdir[PATH_MAX];
        snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", sdir);
        ensure_dir(tmpdir);
        snprintf(dl_tmp, sizeof(dl_tmp), "%s/url_%08x.bin", tmpdir, (unsigned)h);
        (void)unlink(dl_tmp);
        if (download_url_to_file(val, dl_tmp) != 0) {
            (void)unlink(dl_tmp);
            return false;
        }
        snprintf(input_path, sizeof(input_path), "%s", dl_tmp);
    }

    ExtFileType ft = sniff_external_file_type(input_path);
    int gen_ok = -1;
    if (ft == EXT_FILE_SVG) {
        if (access(draw_svg_bin, X_OK) != 0) {
            if (dl_tmp[0]) (void)unlink(dl_tmp);
            return false;
        }
        if (access(draw_norm_bin, X_OK) != 0) {
            if (dl_tmp[0]) (void)unlink(dl_tmp);
            return false;
        }
        // Render SVG to a temporary PNG (196x196), then normalize to target size (default 128x128).
        char tmp_svg[PATH_MAX];
        snprintf(tmp_svg, sizeof(tmp_svg), "%s/%08x.svg.%d.png", cache_dir, (unsigned)h, (int)getpid());
        (void)unlink(tmp_svg);
        char *argv_svg[] = { draw_svg_bin, input_path, (char *)"keep", tmp_svg, NULL };
        if (run_exec(argv_svg) == 0 && file_exists(tmp_svg)) {
            char *argv_norm[] = { draw_norm_bin, tmp_svg, tmp_out, NULL };
            gen_ok = run_exec(argv_norm);
        } else {
            gen_ok = -1;
        }
        (void)unlink(tmp_svg);
    } else if (ft == EXT_FILE_PNG) {
        if (access(draw_norm_bin, X_OK) != 0) {
            if (dl_tmp[0]) (void)unlink(dl_tmp);
            return false;
        }
        char *argv[] = { draw_norm_bin, input_path, tmp_out, NULL };
        gen_ok = run_exec(argv);
    } else {
        if (dl_tmp[0]) (void)unlink(dl_tmp);
        return false;
    }

    if (dl_tmp[0]) (void)unlink(dl_tmp);

    if (gen_ok != 0 || !file_exists(tmp_out)) {
        (void)unlink(tmp_out);
        return false;
    }

    // If too large for the device ZIP, first try a lossless recompress pass (keep colors),
    // then (if still too big) fall back to a gentler quantization (128 colors, not 4).
    struct stat st;
    if (stat(tmp_out, &st) == 0 && st.st_size > 6 * 1024) {
        if (access(draw_norm_bin, X_OK) == 0) {
            char tmp2[PATH_MAX];
            snprintf(tmp2, sizeof(tmp2), "%s/%08x.repack.%d.png", cache_dir, (unsigned)h, (int)getpid());
            (void)unlink(tmp2);
            char *argv_repack[] = { draw_norm_bin, tmp_out, tmp2, NULL };
            if (run_exec(argv_repack) == 0 && file_exists(tmp2)) {
                (void)unlink(tmp_out);
                (void)rename(tmp2, tmp_out);
            } else {
                (void)unlink(tmp2);
            }
        }
    }

    if (stat(tmp_out, &st) == 0 && st.st_size > 6 * 1024) {
        // Quantize to 128 colors (still preserves gradients better than the 4-color pipeline).
        char draw_opt_bin[PATH_MAX];
        snprintf(draw_opt_bin, sizeof(draw_opt_bin), "%s/icons/draw_optimize", opt->root_dir);
        if (access(draw_opt_bin, X_OK) == 0) {
            char *argv_opt[] = { draw_opt_bin, (char *)"-d", (char *)"-c=128", tmp_out, NULL };
            (void)run_exec(argv_opt);
        }
    }

    // Validate final normalized file.
    if (validate_external_png_final(tmp_out) != 0) {
        (void)unlink(tmp_out);
        return false;
    }

    // Move into disk cache.
    (void)unlink(disk);
    if (rename(tmp_out, disk) != 0) {
        // Fallback copy if rename fails (cross-device), but it shouldn't since same dir.
        (void)copy_file(tmp_out, disk);
        (void)unlink(tmp_out);
    }

    if (file_exists(disk) && validate_external_png_final(disk) == 0) {
        (void)copy_file(disk, sess);
        if (file_exists(sess) && validate_external_png_final(sess) == 0) {
            snprintf(out_path, out_cap, "%s", sess);
            return true;
        }
    }
    (void)unlink(sess);
    return false;
}

static uint32_t item_file_hash(const char *page, size_t item_index, const Item *it) {
    (void)it;
    // Short hash based only on <page name> + <button number> (stable, no meta).
    const char *pg = page ? page : "";
    int btn = (int)item_index + 1;
    char key[512];
    snprintf(key, sizeof(key), "page:%s\nbutton:%d\n", pg, btn);
    return fnv1a32(key, strlen(key));
}

static bool item_has_cmd_features(const Item *it);

static bool cached_or_generated_into_state(const Options *opt, const Config *cfg, const char *page, size_t item_index, const Item *it,
                                           const char *icon_override, const char *text_override, const char *preset_override,
                                           const char *variant, char *out_path, size_t out_cap) {
    if (!it) return false;
    const char *pr_name = (preset_override && preset_override[0]) ? preset_override : (it->preset ? it->preset : "default");
    const Preset *preset = config_get_preset(cfg, pr_name);
    if (!preset) preset = config_get_preset(cfg, "default");

    const char *ic = (icon_override != NULL) ? icon_override :
                     (it->icon && it->icon[0]) ? it->icon :
                     (preset && preset->icon) ? preset->icon : "";
		const char *tx = (text_override != NULL) ? text_override :
			                     (it->text && it->text[0]) ? it->text :
			                     (preset && preset->text) ? preset->text : "";

    // For $cmd buttons, icon text is dynamic: do not bake it into cached icons.
    if (text_override == NULL && item_has_cmd_features(it)) tx = "";

	bool is_defined = (it->icon && it->icon[0]) || (it->text && it->text[0]) || (it->preset && it->preset[0]) ||
		                      (it->entity_id && it->entity_id[0]) || (it->tap_action && it->tap_action[0]) || (it->state_count > 0);
		if (!is_defined) return false; // empty/unconfigured => no cache
    if (ic[0] == 0 && tx[0] == 0 && it->state_count == 0 && !(it->entity_id && it->entity_id[0])) {
        // return false; // plain empty
        // Allow "base-only" icons (background/border) when preset styling is visible.
        bool has_bg = (preset && preset->icon_background_color && preset->icon_background_color[0] &&
                       strcasecmp(preset->icon_background_color, "transparent") != 0);
        bool has_border = (preset && preset->icon_border_width > 0);
        if (!has_bg && !has_border) return false;
    }

    // Special-case: external icons (local:/url:) are used as-is (no pipeline composition) and cached in RAM per session.
    if (ic && (icon_is_prefixed(ic, "local:") || icon_is_prefixed(ic, "url:"))) {
        char ext[PATH_MAX];
        if (resolve_external_icon_session(opt, ic, ext, sizeof(ext))) {
            snprintf(out_path, out_cap, "%s", ext);
            return true;
        }
        snprintf(out_path, out_cap, "%s", file_too_big_png(opt));
        return true;
    }

	    uint32_t file_h = item_file_hash(page, item_index, it);
    char suf[128] = {0};
    if (variant && variant[0]) sanitize_suffix(variant, suf, sizeof(suf));
    int btn = (int)item_index + 1;

    if (suf[0]) snprintf(out_path, out_cap, "%s/%s/%d-%08x-%s.png", opt->cache_root, page, btn, file_h, suf);
    else snprintf(out_path, out_cap, "%s/%s/%d-%08x.png", opt->cache_root, page, btn, file_h);

    if (file_exists(out_path)) return true;

    ensure_dir_parent(out_path);
    Item tmp = *it;
    tmp.icon = (char *)ic;
    tmp.text = (char *)tx;
    tmp.preset = (char *)pr_name;
    if (generate_icon_pipeline(opt, preset, &tmp, out_path) != 0) {
        (void)copy_file(opt->error_icon, out_path);
    }
    return true;
}

static bool item_has_cmd_features(const Item *it) {
    if (!it) return false;
    const char *as[] = { it->tap_action, it->hold_action, it->longhold_action, it->released_action };
    for (size_t i = 0; i < sizeof(as) / sizeof(as[0]); i++) {
        if (as[i] && strncmp(as[i], "$cmd.", 5) == 0) return true;
    }
    if (it->poll_every_ms > 0 && it->poll_action && strncmp(it->poll_action, "$cmd.", 5) == 0) return true;
    if (it->state_every_ms > 0 && it->state_cmd && it->state_cmd[0]) return true;
    return false;
}

static bool item_has_static_text_variant(const Item *it, const Preset *preset, const char **out_eff_text) {
    if (out_eff_text) *out_eff_text = NULL;
    if (!it) return false;
    if (item_has_cmd_features(it)) return false;
    // If bound to a Home Assistant entity without explicit states, text is typically dynamic (sensor value).
    // Keep these as runtime overlays (/dev/shm), not cached variants.
    if (it->entity_id && it->entity_id[0] && it->state_count == 0) return false;
    if (it->state_count > 0) return false;

    const char *tx = (it->text && it->text[0]) ? it->text : (preset && preset->text) ? preset->text : "";
    if (!tx || tx[0] == 0) return false;
    if (out_eff_text) *out_eff_text = tx;
    return true;
}



static bool cached_or_generated_static_text_into(const Options *opt, const Config *cfg, const char *page, size_t item_index, const Item *it,
                                                 char *out_path, size_t out_cap) {
    if (!opt || !cfg || !page || !it || !out_path || out_cap == 0) return false;

    const char *pr_name = (it->preset && it->preset[0]) ? it->preset : "default";
    const Preset *preset = config_get_preset(cfg, pr_name);
    if (!preset) preset = config_get_preset(cfg, "default");

    const char *eff_text = NULL;
    if (!item_has_static_text_variant(it, preset, &eff_text)) return false;

    // Ensure base icon exists (no text).
    char base_png[PATH_MAX];
    if (!cached_or_generated_into_state(opt, cfg, page, item_index, it, NULL, (const char *)"", pr_name, NULL, base_png, sizeof(base_png))) {
        return false;
    }

    uint32_t file_h = item_file_hash(page, item_index, it);
    int btn = (int)item_index + 1;
    snprintf(out_path, out_cap, "%s/%s/%d-%08x-text.png", opt->cache_root, page, btn, file_h);
    if (file_exists(out_path)) return true;

    ensure_dir_parent(out_path);
    if (copy_file(base_png, out_path) != 0) return false;

    // Apply draw_text (static) onto the cached base icon.
    char draw_text_bin[PATH_MAX];
    char draw_opt_bin[PATH_MAX];
    snprintf(draw_text_bin, sizeof(draw_text_bin), "%s/icons/draw_text", opt->root_dir);
    snprintf(draw_opt_bin, sizeof(draw_opt_bin), "%s/icons/draw_optimize", opt->root_dir);
    if (access(draw_text_bin, X_OK) != 0) { unlink(out_path); return false; }
    if (access(draw_opt_bin, X_OK) != 0) { unlink(out_path); return false; }

    const char *tc = (preset && preset->text_color && preset->text_color[0]) ? preset->text_color : "FFFFFF";
    const char *ta = (preset && preset->text_align && preset->text_align[0]) ? preset->text_align : "center";
    const char *tf = (preset && preset->text_font && preset->text_font[0]) ? preset->text_font : "Roboto";
    bool used_default_font = !(preset && preset->text_font && preset->text_font[0]);
    int ts = preset ? clamp_int(preset->text_size, 1, 64) : 40;
    int tox = preset ? preset->text_offset_x : 0;
    int toy = preset ? preset->text_offset_y : 0;

    char text_arg[768];
    snprintf(text_arg, sizeof(text_arg), "--text=%s", eff_text);
    char tc_arg[64];
    snprintf(tc_arg, sizeof(tc_arg), "--text_color=%s", tc);
    char ta_arg[64];
    snprintf(ta_arg, sizeof(ta_arg), "--text_align=%s", ta);
    char tf_arg[PATH_MAX];
    snprintf(tf_arg, sizeof(tf_arg), "--text_font=%s", tf);
    char ts_arg[64];
    snprintf(ts_arg, sizeof(ts_arg), "--text_size=%d", ts);
    char to_arg[64];
    snprintf(to_arg, sizeof(to_arg), "--text_offset=%d,%d", tox, toy);

    int rc = 0;
    if (tf && tf[0]) {
        char *argv[] = { draw_text_bin, text_arg, tc_arg, ta_arg, tf_arg, ts_arg, to_arg, out_path, NULL };
        rc = run_exec(argv);
        if (rc != 0 && used_default_font) {
            char *argv2[] = { draw_text_bin, text_arg, tc_arg, ta_arg, ts_arg, to_arg, out_path, NULL };
            rc = run_exec(argv2);
        }
    } else {
        char *argv[] = { draw_text_bin, text_arg, tc_arg, ta_arg, ts_arg, to_arg, out_path, NULL };
        rc = run_exec(argv);
    }
    if (rc != 0) { unlink(out_path); return false; }

    bool is_external = (it->icon && (icon_is_prefixed(it->icon, "local:") || icon_is_prefixed(it->icon, "url:")));
    if (!is_external) {
        // Built icons: keep the existing 4-color behavior.
        char *argv_opt[] = { draw_opt_bin, (char *)"-d", (char *)"-c=4", out_path, NULL };
        if (run_exec(argv_opt) != 0) { unlink(out_path); return false; }
    } else {
        // External icons: preserve colors; only quantize if needed for the device icon size constraint (<= 6KB).
        struct stat st;
        if (stat(out_path, &st) == 0 && st.st_size > 6 * 1024) {
            char *argv_opt[] = { draw_opt_bin, (char *)"-d", (char *)"-c=128", out_path, NULL };
            if (run_exec(argv_opt) != 0) { unlink(out_path); return false; }
        }
    }

    return true;
}

static bool cached_or_generated_into(const Options *opt, const Config *cfg, const char *page, size_t item_index, const Item *it,
                                     char *out_path, size_t out_cap) {
    return cached_or_generated_into_state(opt, cfg, page, item_index, it, NULL, NULL, NULL, NULL, out_path, out_cap);
}

static const Item *page_item_at(const Page *p, size_t idx) {
    if (!p || idx >= p->count) return NULL;
    return &p->items[idx];
}

static const StateOverride *item_find_state_override(const Item *it, const char *state) {
    if (!it || !state || !state[0] || it->state_count == 0) return NULL;
    for (size_t i = 0; i < it->state_count; i++) {
        if (it->states[i].key && strcmp(it->states[i].key, state) == 0) return &it->states[i];
    }
    return NULL;
}

typedef struct {
    size_t start;      // item index
    size_t cap;        // number of content slots for this sheet
    bool show_prev;    // prev system button visible on this sheet
    bool show_next;    // next system button visible on this sheet
    size_t prev_start; // start of previous sheet (if show_prev)
    size_t next_start; // start of next sheet (if show_next)
} SheetLayout;

static SheetLayout compute_sheet_layout(size_t total_items, bool show_back, size_t desired_offset) {
    SheetLayout out;
    memset(&out, 0, sizeof(out));

    size_t base_slots = 13 - (show_back ? 1u : 0u);
    if (base_slots == 0) base_slots = 1;

    // No pagination: all content slots available.
    if (total_items <= base_slots) {
        out.start = 0;
        out.cap = base_slots;
        out.show_prev = false;
        out.show_next = false;
        out.prev_start = 0;
        out.next_start = 0;
        return out;
    }

    // Build variable-capacity sheets:
    // - First sheet: no prev; next shown => reserve 1 slot for next => cap = base_slots - 1
    // - Middle sheets: prev+next shown => reserve 2 slots => cap = base_slots - 2
    // - Last sheet: prev shown, next hidden => reserve 1 slot => cap = base_slots - 1
    size_t starts[256];
    size_t caps[256];
    bool prevs[256];
    bool nexts[256];
    size_t count = 0;

    size_t start = 0;
    size_t idx = 0;
    while (start < total_items && count < (sizeof(starts) / sizeof(starts[0]))) {
        bool prev = (idx > 0);
        size_t cap_last = base_slots - (prev ? 1u : 0u);
        if (cap_last == 0) cap_last = 1;
        size_t cap_next = base_slots - (prev ? 1u : 0u) - 1u;
        if (cap_next == 0) cap_next = 1;

        bool next = (start + cap_last < total_items);
        size_t cap = next ? cap_next : cap_last;

        starts[count] = start;
        caps[count] = cap;
        prevs[count] = prev;
        nexts[count] = next;
        count++;

        start += cap;
        idx++;
    }

    // Find sheet by matching start or by containment.
    size_t sel = 0;
    for (size_t i = 0; i < count; i++) {
        if (starts[i] == desired_offset) { sel = i; break; }
        if (desired_offset >= starts[i] && (i + 1 == count || desired_offset < starts[i + 1])) {
            sel = i;
        }
    }

    out.start = starts[sel];
    out.cap = caps[sel];
    out.show_prev = prevs[sel];
    out.show_next = nexts[sel];
    out.prev_start = (sel > 0) ? starts[sel - 1] : starts[sel];
    out.next_start = (sel + 1 < count) ? starts[sel + 1] : starts[sel];
    return out;
}

static bool is_action_goto(const char *a) {
    if (!a) return false;
    return strcmp(a, "$page.go_to") == 0;
}

static const ActionSeq *item_action_seq_for_event(const Item *it, ButtonEvent evt) {
    if (!it) return NULL;
    switch (evt) {
        case BTN_EVT_TAP: return &it->tap_seq;
        case BTN_EVT_HOLD: return &it->hold_seq;
        case BTN_EVT_LONGHOLD: return &it->longhold_seq;
        case BTN_EVT_RELEASED: return &it->released_seq;
        default: return NULL;
    }
}

static void item_action_seq_ensure_legacy_single(const Item *it, ButtonEvent evt, ActionSeq *tmp) {
    if (!it || !tmp) return;
    memset(tmp, 0, sizeof(*tmp));

    const char *a = NULL;
    const char *d = NULL;
    CmdTextOpts o = cmd_text_opts_defaults();
    if (evt == BTN_EVT_TAP) { a = it->tap_action; d = it->tap_data; o = it->tap_cmd_text; }
    else if (evt == BTN_EVT_HOLD) { a = it->hold_action; d = it->hold_data; o = it->hold_cmd_text; }
    else if (evt == BTN_EVT_LONGHOLD) { a = it->longhold_action; d = it->longhold_data; o = it->longhold_cmd_text; }
    else if (evt == BTN_EVT_RELEASED) { a = it->released_action; d = it->released_data; o = it->released_cmd_text; }
    if (!a || !a[0]) return;

    ActionStep st;
    memset(&st, 0, sizeof(st));
    st.action = xstrdup(a);
    if (d && d[0]) st.data = xstrdup(d);
    st.cmd_text = o;
    action_seq_push(tmp, st);
}

static int ensure_sys_icon(const Options *opt, const Config *cfg, const char *name, const char *mdi_icon, char *out, size_t cap) {
    if (!opt || !cfg || !name || !mdi_icon || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/%s.png", opt->sys_pregen_dir, name);
    if (file_exists(out)) return 0;

    const Preset *preset = config_get_preset(cfg, "$nav");
    if (!preset) preset = config_get_preset(cfg, "default");
    Item it;
    memset(&it, 0, sizeof(it));
    it.icon = (char *)mdi_icon;
    it.preset = (char *)"$nav";
    it.text = NULL;
    it.tap_action = NULL;
    it.tap_data = NULL;
    it.entity_id = NULL;
    it.states = NULL;
    it.state_count = 0;
    it.state_cap = 0;

    if (generate_icon_pipeline(opt, preset, &it, out) != 0) {
        (void)copy_file(opt->error_icon, out);
    }
    return file_exists(out) ? 0 : -1;
}

static void page_tag_for_cache_dir(const char *page_name, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!page_name || !page_name[0]) {
        snprintf(out, cap, "page");
        return;
    }
    if (strcmp(page_name, "$root") == 0) {
        snprintf(out, cap, "root");
        return;
    }
    sanitize_suffix(page_name, out, cap);
    if (out[0] == 0) snprintf(out, cap, "page");
}

static bool nav_wallpaper_composed_cached(const Options *opt, const Config *cfg, const char *page_name,
                                         const char *nav_name, const char *mdi_icon, int pos,
                                         uint32_t wp_sig, const WallpaperEff *wp,
                                         const char *wp_render_dir, const char *wp_prefix,
                                         char *out_png, size_t out_cap, bool *out_is_tmp) {
    if (out_is_tmp) *out_is_tmp = false;
    if (!opt || !cfg || !page_name || !nav_name || !mdi_icon || !out_png || out_cap == 0) return false;
    out_png[0] = 0;
    if (pos < 1 || pos > 13) return false;
    if (!wp || !wp->enabled) return false;
    if (!wp_render_dir || !wp_prefix || !wp_render_dir[0] || !wp_prefix[0]) return false;

    // Disk cache (persistent): .cache/nav/<page>/<wp_sig>/<nav>_<pos>.png
    char page_tag[96];
    page_tag_for_cache_dir(page_name, page_tag, sizeof(page_tag));

    char disk_dir[PATH_MAX];
    snprintf(disk_dir, sizeof(disk_dir), "%s/nav/%s", opt->cache_root, page_tag);
    bool disk_ok = (try_ensure_dir_parent(disk_dir) == 0 && try_ensure_dir(disk_dir) == 0 && access(disk_dir, W_OK | X_OK) == 0);

    char disk_png[PATH_MAX];
    // IMPORTANT: include wp_sig in the filename (not just the directory) so the Ulanzi daemon,
    // which may use basenames when creating zip entries, cannot accidentally treat two different
    // wallpapers as the "same" nav file.
    snprintf(disk_png, sizeof(disk_png), "%s/%s_%08x_%02d.png", disk_dir, nav_name, (unsigned)wp_sig, pos);

    // Session RAM cache: /dev/shm/.../nav/<page>/<wp_sig>/<nav>_<pos>.png
    char sdir[PATH_MAX];
    state_dir(opt, sdir, sizeof(sdir));
    char shm_dir[PATH_MAX];
    snprintf(shm_dir, sizeof(shm_dir), "%s/nav/%s", sdir, page_tag);
    bool shm_ok = (try_ensure_dir_parent(shm_dir) == 0 && try_ensure_dir(shm_dir) == 0 && access(shm_dir, W_OK | X_OK) == 0);

    char shm_png[PATH_MAX];
    snprintf(shm_png, sizeof(shm_png), "%s/%s_%08x_%02d.png", shm_dir, nav_name, (unsigned)wp_sig, pos);

    // Prefer RAM copy.
    if (shm_ok && file_exists(shm_png)) {
        snprintf(out_png, out_cap, "%s", shm_png);
        return true;
    }

    // If disk cache exists, copy to RAM and use.
    if (disk_ok && file_exists(disk_png)) {
        if (shm_ok) {
            (void)copy_file(disk_png, shm_png);
            if (file_exists(shm_png)) {
                snprintf(out_png, out_cap, "%s", shm_png);
                return true;
            }
        }
        snprintf(out_png, out_cap, "%s", disk_png);
        return true;
    }

    // Ensure base sys icon exists.
    char base[PATH_MAX];
    bool have_base = (ensure_sys_icon(opt, cfg, nav_name, mdi_icon, base, sizeof(base)) == 0 && file_exists(base));

    // Compose tile(+icon) into a tmp RAM file, then persist both disk+RAM.
    char tile[PATH_MAX];
    if (wallpaper_session_tile(opt, wp_render_dir, wp_prefix, wp, pos, tile, sizeof(tile)) != 0 || !tile[0]) return false;

    char draw_over_bin[PATH_MAX];
    snprintf(draw_over_bin, sizeof(draw_over_bin), "%s/icons/draw_over", opt->root_dir);
    bool have_draw_over = (access(draw_over_bin, X_OK) == 0);

    // If we can't overlay (missing nav icon or draw_over), still ensure the nav background is correct by sending the tile.
    // This avoids "stale wallpaper" artifacts from a previous page on the device.
    if (!have_draw_over || !have_base) {
        snprintf(out_png, out_cap, "%s", tile);
        return true;
    }

    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", sdir);
    if (try_ensure_dir(tmpdir) != 0) return false;
    char tmp_out[PATH_MAX];
    snprintf(tmp_out, sizeof(tmp_out), "%s/nav_%s_%02d_%d.png", tmpdir, nav_name, pos, (int)getpid());
    (void)unlink(tmp_out);

    if (copy_file(tile, tmp_out) != 0) return false;
    char *argv_over[] = { draw_over_bin, (char *)base, tmp_out, NULL };
    if (run_exec(argv_over) != 0) {
        unlink(tmp_out);
        // Fallback to tile only if overlay fails.
        snprintf(out_png, out_cap, "%s", tile);
        return true;
    }

    // Persist to disk and RAM. Best-effort: if disk write fails, keep RAM.
    if (disk_ok) (void)copy_file(tmp_out, disk_png);
    if (shm_ok) (void)copy_file(tmp_out, shm_png);

    if (shm_ok && file_exists(shm_png)) {
        unlink(tmp_out);
        snprintf(out_png, out_cap, "%s", shm_png);
        return true;
    }
    if (disk_ok && file_exists(disk_png)) {
        unlink(tmp_out);
        snprintf(out_png, out_cap, "%s", disk_png);
        return true;
    }

    // Fallback: use tmp directly for this render; caller must clean it.
    if (out_is_tmp) *out_is_tmp = true;
    snprintf(out_png, out_cap, "%s", tmp_out);

    return true;
}

static void make_device_label(const char *src, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    out[0] = 0;
    if (!src || !src[0]) return;
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < out_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        // Keep UTF-8 bytes as-is (Ulanzi socket protocol is text), but avoid whitespace/control chars
        // because the device daemon splits args on spaces/tabs/newlines.
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            out[w++] = '_';
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            out[w++] = '_';
            continue;
        }
        out[w++] = (char)c;
    }
    out[w] = 0;
}

static void appendf_dyn(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    if (!buf || !len || !cap || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) {
        va_end(ap);
        die_errno("vsnprintf");
    }
    size_t required = *len + (size_t)need + 1;
    if (required > *cap) {
        size_t new_cap = (*cap == 0) ? 1024u : *cap;
        while (new_cap < required) new_cap *= 2u;
        *buf = xrealloc(*buf, new_cap);
        *cap = new_cap;
    }
    vsnprintf(*buf + *len, *cap - *len, fmt, ap);
    va_end(ap);
    *len += (size_t)need;
}

static void render_and_send(const Options *opt, const Config *cfg, const char *page_name, size_t offset,
                            const HaStateMap *ha_map, char *blank_png, char *last_sig, size_t last_sig_cap) {
    const Page *p = config_get_page((Config *)cfg, page_name);
    if (!p) {
        log_msg("unknown page '%s' (render skipped)", page_name);
        return;
    }

    bool show_back = strcmp(page_name, "$root") != 0;
    int back_pos = cfg->pos_back;
    int prev_pos = cfg->pos_prev;
    int next_pos = cfg->pos_next;

    size_t base_item_slots = 13 - (show_back ? 1u : 0u);
    bool need_pagination = p->count > base_item_slots;
    SheetLayout sheet = compute_sheet_layout(p->count, show_back, offset);
    offset = sheet.start;
    size_t item_slots = sheet.cap;
    bool show_prev = sheet.show_prev;
    bool show_next = sheet.show_next;

    WallpaperEff sig_wp = effective_wallpaper(cfg, p);
    uint32_t wp_sig = 0;
    if (sig_wp.enabled && sig_wp.path && sig_wp.path[0]) {
        char wkey[1024];
        snprintf(wkey, sizeof(wkey), "path:%s\nq:%d\nm:%d\nd:%d\n", sig_wp.path, sig_wp.quality, sig_wp.magnify, sig_wp.dithering ? 1 : 0);
        wp_sig = fnv1a32(wkey, strlen(wkey));
    }

    char sig[256];
    snprintf(sig, sizeof(sig), "%s|%zu|%zu|%d|%d|%d|%d|%d|%08x", page_name, offset, item_slots,
             show_back ? 1 : 0, need_pagination ? 1 : 0, show_prev ? 1 : 0, show_next ? 1 : 0, (int)p->count, wp_sig);
    if (strncmp(sig, last_sig, last_sig_cap) == 0) {
        return;
    }

    log_render("render page='%s' offset=%zu slots=%zu items=%zu", page_name, offset, item_slots, p->count);

		char btn_path[14][PATH_MAX];
		bool btn_set[14] = {0};
		char btn_label[14][64];
		bool label_set[14] = {0};
		bool cleanup_tmp[14] = {0};
	    bool wp_already_composed[14] = {0};
        CmdEntry *cmd_entry_for_pos[14] = {0};
        bool cmd_text_set[14] = {0};
        char cmd_text_for_pos[14][256];
        bool cmd_state_set[14] = {0};
        char cmd_state_for_pos[14][64];
		for (int i = 1; i <= 13; i++) {
		    snprintf(btn_path[i], sizeof(btn_path[i]), "%s", blank_png);
		    btn_set[i] = true;
		    btn_label[i][0] = 0;
		    label_set[i] = false;
            cmd_text_for_pos[i][0] = 0;
            cmd_state_for_pos[i][0] = 0;
		}

    // Reserve back/prev/next
    bool reserved[14] = {0};
    if (show_back && back_pos >= 1 && back_pos <= 13) reserved[back_pos] = true;
    if (show_prev && prev_pos >= 1 && prev_pos <= 13) reserved[prev_pos] = true;
    if (show_next && next_pos >= 1 && next_pos <= 13) reserved[next_pos] = true;

    // Optional wallpaper context for this page. We use it to cache composed tile+icon in /dev/shm, and
    // to allow dynamic text updates (HA value) without re-running draw_over every time.
    WallpaperEff wp = effective_wallpaper(cfg, p);
    bool wp_active = false;
    char wp_render_dir[PATH_MAX] = {0};
    char wp_prefix[PATH_MAX] = {0};
    char wp_tile14[PATH_MAX] = {0};
    bool have_draw_over = false;
    if (wp.enabled && wp.path && wp.path[0]) {
        if (ensure_wallpaper_rendered(opt, &wp, wp_render_dir, sizeof(wp_render_dir), wp_prefix, sizeof(wp_prefix)) == 0) {
            wp_active = true;
            (void)wallpaper_session_tile(opt, wp_render_dir, wp_prefix, &wp, 14, wp_tile14, sizeof(wp_tile14));
            char draw_over_bin[PATH_MAX];
            snprintf(draw_over_bin, sizeof(draw_over_bin), "%s/icons/draw_over", opt->root_dir);
            have_draw_over = (access(draw_over_bin, X_OK) == 0);
        }
    }

	    // Fill items
	    size_t item_i = offset;
		for (int pos = 1; pos <= 13 && item_i < p->count; pos++) {
		    if (reserved[pos]) continue;
		    const Item *it = page_item_at(p, item_i);
		    const char *label_src = (it && it->name && it->name[0]) ? it->name : NULL;
            CmdEntry *cmd_ce = NULL;
            char cmd_text[256] = {0};
            char cmd_state[64] = {0};
            if (g_cmd_engine && it) {
                cmd_ce = cmd_engine_find(g_cmd_engine, page_name, item_i);
                if (cmd_ce) {
                    pthread_mutex_lock(&cmd_ce->mu);
                    snprintf(cmd_text, sizeof(cmd_text), "%s", cmd_ce->last_text);
                    snprintf(cmd_state, sizeof(cmd_state), "%s", cmd_ce->last_state);
                    pthread_mutex_unlock(&cmd_ce->mu);
                }
            }
            if (cmd_ce) {
                cmd_entry_for_pos[pos] = cmd_ce;
                if (it && it->state_count > 0) {
                    cmd_state_set[pos] = true;
                    snprintf(cmd_state_for_pos[pos], sizeof(cmd_state_for_pos[pos]), "%s", cmd_state);
                }
            }
		    char tmp[PATH_MAX];
		    bool have_icon = false;

		    // HA-driven states: pick state variant if known.
		    if (it && it->entity_id && it->entity_id[0] && it->state_count > 0 && ha_map) {
            const char *cur_state = NULL;
            for (size_t si = 0; si < ha_map->len; si++) {
                if (ha_map->items[si].entity_id && strcmp(ha_map->items[si].entity_id, it->entity_id) == 0) {
                    cur_state = ha_map->items[si].state;
                    break;
                }
            }
            if (cur_state && cur_state[0]) {
                const StateOverride *ov = item_find_state_override(it, cur_state);
                if (ov) {
                    const char *preset_ov = (ov->preset && ov->preset[0]) ? ov->preset : NULL;
                    const char *icon_ov = (ov->icon && ov->icon[0]) ? ov->icon : NULL;
                    const char *text_ov = (ov->text && ov->text[0]) ? ov->text : NULL;
                    if (cached_or_generated_into_state(opt, cfg, page_name, item_i, it, icon_ov, text_ov, preset_ov, cur_state, tmp, sizeof(tmp))) {
                        snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
                        btn_set[pos] = true;
                        have_icon = true;
                    }
                    if (ov->name && ov->name[0]) label_src = ov->name;
                }
                // Unknown/missing override => fallback to base.
                if (!have_icon) {
                    if (cached_or_generated_into_state(opt, cfg, page_name, item_i, it, NULL, NULL, NULL, "base", tmp, sizeof(tmp))) {
                        snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
                        btn_set[pos] = true;
                        have_icon = true;
                    }
                }
            } else {
                // No known state yet => show base.
                if (cached_or_generated_into_state(opt, cfg, page_name, item_i, it, NULL, NULL, NULL, "base", tmp, sizeof(tmp))) {
                    snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
                    btn_set[pos] = true;
                    have_icon = true;
                }
            }
	        }

            // Command-driven states (stdout is the state key): pick state variant if known.
            if (!have_icon && it && (!it->entity_id || !it->entity_id[0]) && it->state_count > 0 && cmd_ce) {
                const char *cur_state = (cmd_state[0]) ? cmd_state : NULL;
                if (cur_state && cur_state[0]) {
                    const StateOverride *ov = item_find_state_override(it, cur_state);
                    if (ov) {
                        const char *preset_ov = (ov->preset && ov->preset[0]) ? ov->preset : NULL;
                        const char *icon_ov = (ov->icon && ov->icon[0]) ? ov->icon : NULL;
                        const char *text_ov = (ov->text && ov->text[0]) ? ov->text : NULL;
                        if (cached_or_generated_into_state(opt, cfg, page_name, item_i, it, icon_ov, text_ov, preset_ov, cur_state, tmp,
                                                           sizeof(tmp))) {
                            snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
                            btn_set[pos] = true;
                            have_icon = true;
                        }
                        if (ov->name && ov->name[0]) label_src = ov->name;
                    }
                }
                if (!have_icon) {
                    if (cached_or_generated_into_state(opt, cfg, page_name, item_i, it, NULL, NULL, NULL, "base", tmp, sizeof(tmp))) {
                        snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
                        btn_set[pos] = true;
                        have_icon = true;
                    }
                }
            }

	        // HA entity value display (sensor, etc): if no states are defined, show HA state as text
            // only for value-like domains (otherwise we'd show raw "off" for scripts, etc).
	        if (!have_icon && it && it->entity_id && it->entity_id[0] && it->state_count == 0 && ha_map &&
                ha_entity_is_value_display(it->entity_id)) {
	            char value_text[128] = {0};
	            ha_format_value_text(ha_map, it->entity_id, value_text, sizeof(value_text));
            const char *pr_name = (it->preset && it->preset[0]) ? it->preset : "default";
            const Preset *pr = config_get_preset(cfg, pr_name);
            if (!pr) pr = config_get_preset(cfg, "default");
            const char *eff_icon = (it->icon && it->icon[0]) ? it->icon : (pr && pr->icon) ? pr->icon : NULL;
            const char *eff_text = (it->text && it->text[0]) ? it->text : value_text;

            if ((eff_icon && eff_icon[0]) || (eff_text && eff_text[0])) {
                // Generate a stable base icon in cache, then overlay the current value into a /dev/shm temp and send it.
                Item tmp_it = *it;
                tmp_it.icon = (char *)(eff_icon ? eff_icon : "");
                tmp_it.text = (char *)""; // base icon has no dynamic value text

                char base_png[PATH_MAX];
                if (cached_or_generated_into_state(opt, cfg, page_name, item_i, &tmp_it, NULL, (const char *)"", pr_name, NULL, base_png, sizeof(base_png))) {
                    const char *text_base = base_png;
                    char composed_base[PATH_MAX] = {0};
                    bool composed_is_tmp = false;
                    bool cleanup_text_base = false;
                    if (wp_active && have_draw_over) {
                        // Compose tile+base icon once (cached), then draw_text on top for dynamic updates.
                        if (wp_compose_cached(opt, wp_sig, wp_render_dir, wp_prefix, &wp, pos, base_png,
                                              composed_base, sizeof(composed_base), &composed_is_tmp) == 0 &&
                            composed_base[0]) {
                            text_base = composed_base;
                            cleanup_text_base = composed_is_tmp;
                        }
                    }

                    char tmp_out[PATH_MAX];
                    if (render_value_text_on_base_tmp(opt, pr, page_name, pos, text_base, eff_text, tmp_out, sizeof(tmp_out)) == 0) {
                        snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp_out);
                        btn_set[pos] = true;
                        cleanup_tmp[pos] = true;
                        have_icon = true;
                        if (text_base != base_png) {
                            // Already includes wallpaper tile.
                            wp_already_composed[pos] = true;
                        }
                        if (cleanup_text_base) unlink(text_base);
                    } else {
                        if (cleanup_text_base) unlink(text_base);
                    }
                }
            }
        }

	        if (!have_icon) {
	            if (cached_or_generated_static_text_into(opt, cfg, page_name, item_i, it, tmp, sizeof(tmp))) {
	                snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
	                btn_set[pos] = true;
	            } else if (cached_or_generated_into(opt, cfg, page_name, item_i, it, tmp, sizeof(tmp))) {
	                snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
	                btn_set[pos] = true;
	            } else {
	                // empty => keep blank, no cache
	                snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", blank_png);
	                btn_set[pos] = true;
	            }
	        }

            // Command-driven text overlay: draw stdout on top of the current base icon (and wallpaper tile if enabled).
            if (cmd_ce && cmd_text[0]) {
                const Preset *pr = NULL;
                const char *pr_name = (it && it->preset && it->preset[0]) ? it->preset : "default";
                pr = config_get_preset(cfg, pr_name);
                if (!pr) pr = config_get_preset(cfg, "default");
                if (pr) {
                    const char *text_base = btn_path[pos];
                    char composed_base[PATH_MAX] = {0};
                    bool composed_is_tmp = false;
                    bool cleanup_text_base = false;
                    if (wp_active && have_draw_over) {
                        if (wp_compose_cached(opt, wp_sig, wp_render_dir, wp_prefix, &wp, pos, text_base,
                                              composed_base, sizeof(composed_base), &composed_is_tmp) == 0 &&
                            composed_base[0]) {
                            text_base = composed_base;
                            cleanup_text_base = composed_is_tmp;
                        }
                    }
                    char tmp_out[PATH_MAX];
                    if (render_value_text_on_base_tmp(opt, pr, page_name, pos, text_base, cmd_text, tmp_out, sizeof(tmp_out)) == 0) {
                        if (cleanup_tmp[pos]) unlink(btn_path[pos]);
                        snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp_out);
                        cleanup_tmp[pos] = true;
                        btn_set[pos] = true;
                        have_icon = true;
                        cmd_text_set[pos] = true;
                        snprintf(cmd_text_for_pos[pos], sizeof(cmd_text_for_pos[pos]), "%s", cmd_text);
                        if (composed_base[0]) wp_already_composed[pos] = true;
                    }
                    if (cleanup_text_base) unlink(text_base);
                }
            } else if (cmd_ce && cmd_entry_for_pos[pos]) {
                // If we have a cmd entry but no current text, ensure we clear any previous sent text state after a full render.
                cmd_text_set[pos] = true;
                cmd_text_for_pos[pos][0] = 0;
            }

	        // name is the device label (no spaces supported by daemon's argv parser)
	        if (label_src && label_src[0]) {
	            make_device_label(label_src, btn_label[pos], sizeof(btn_label[pos]));
	            if (btn_label[pos][0]) label_set[pos] = true;
	        }
	        item_i++;
	    }

    // System icons (only if visible)
    if (show_back && back_pos >= 1 && back_pos <= 13) {
        char tmp[PATH_MAX];
        if (wp_active && wp_sig != 0 &&
            nav_wallpaper_composed_cached(opt, cfg, page_name, "page_back", "mdi:arrow-left", back_pos,
                                          wp_sig, &wp, wp_render_dir, wp_prefix,
                                          tmp, sizeof(tmp), &cleanup_tmp[back_pos])) {
            snprintf(btn_path[back_pos], sizeof(btn_path[back_pos]), "%s", tmp);
            btn_set[back_pos] = true;
            wp_already_composed[back_pos] = true;
        } else if (ensure_sys_icon(opt, cfg, "page_back", "mdi:arrow-left", tmp, sizeof(tmp)) == 0) {
            snprintf(btn_path[back_pos], sizeof(btn_path[back_pos]), "%s", tmp);
            btn_set[back_pos] = true;
        }
    }
    if (show_prev && prev_pos >= 1 && prev_pos <= 13) {
        char tmp[PATH_MAX];
        if (wp_active && wp_sig != 0 &&
            nav_wallpaper_composed_cached(opt, cfg, page_name, "page_prev", "mdi:chevron-left", prev_pos,
                                          wp_sig, &wp, wp_render_dir, wp_prefix,
                                          tmp, sizeof(tmp), &cleanup_tmp[prev_pos])) {
            snprintf(btn_path[prev_pos], sizeof(btn_path[prev_pos]), "%s", tmp);
            btn_set[prev_pos] = true;
            wp_already_composed[prev_pos] = true;
        } else if (ensure_sys_icon(opt, cfg, "page_prev", "mdi:chevron-left", tmp, sizeof(tmp)) == 0) {
            snprintf(btn_path[prev_pos], sizeof(btn_path[prev_pos]), "%s", tmp);
            btn_set[prev_pos] = true;
        }
    }
    if (show_next && next_pos >= 1 && next_pos <= 13) {
        char tmp[PATH_MAX];
        if (wp_active && wp_sig != 0 &&
            nav_wallpaper_composed_cached(opt, cfg, page_name, "page_next", "mdi:chevron-right", next_pos,
                                          wp_sig, &wp, wp_render_dir, wp_prefix,
                                          tmp, sizeof(tmp), &cleanup_tmp[next_pos])) {
            snprintf(btn_path[next_pos], sizeof(btn_path[next_pos]), "%s", tmp);
            btn_set[next_pos] = true;
            wp_already_composed[next_pos] = true;
        } else if (ensure_sys_icon(opt, cfg, "page_next", "mdi:chevron-right", tmp, sizeof(tmp)) == 0) {
            snprintf(btn_path[next_pos], sizeof(btn_path[next_pos]), "%s", tmp);
            btn_set[next_pos] = true;
        }
    }

    // Optional wallpaper: reuse cached composition tile+icon in /dev/shm.
    // For dynamic value overlays we already produced a composed image (tile+base+text), so we skip those.
	    if (wp_active) {
	        for (int pos = 1; pos <= 13; pos++) {
	            // Never skip wallpaper composition for navigation/system buttons. Those must always be refreshed
	            // when page context changes (back/prev/next visibility), otherwise the device may keep stale
	            // composed nav buttons from a previous sheet/page.
	            bool is_nav_pos = false;
	            if (show_back && pos == back_pos) is_nav_pos = true;
	            if (show_prev && pos == prev_pos) is_nav_pos = true;
	            if (show_next && pos == next_pos) is_nav_pos = true;

	            if (wp_already_composed[pos] && !is_nav_pos) continue;

            // Blank => wallpaper tile only.
            if (strcmp(btn_path[pos], blank_png) == 0) {
                char tile[PATH_MAX];
                if (wallpaper_session_tile(opt, wp_render_dir, wp_prefix, &wp, pos, tile, sizeof(tile)) == 0 && tile[0]) {
                    snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tile);
                    btn_set[pos] = true;
                }
                continue;
            }

            if (!have_draw_over) continue;

            char icon_top[PATH_MAX];
            snprintf(icon_top, sizeof(icon_top), "%s", btn_path[pos]);
            bool icon_top_is_tmp = cleanup_tmp[pos];

            char composed[PATH_MAX];
            bool composed_is_tmp = false;
            if (wp_compose_cached(opt, wp_sig, wp_render_dir, wp_prefix, &wp, pos, icon_top,
                                  composed, sizeof(composed), &composed_is_tmp) == 0 &&
                composed[0]) {
                if (icon_top_is_tmp) unlink(icon_top);
                cleanup_tmp[pos] = composed_is_tmp;
                snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", composed);
                btn_set[pos] = true;
            }
	        }
	    }

	    // Without wallpaper, keep current stable icons in a session RAM cache to reduce disk reads.
	    if (!wp_active) {
	        for (int pos = 1; pos <= 13; pos++) {
	            if (!btn_set[pos]) continue;
	            if (cleanup_tmp[pos]) continue; // temp overlays are already in /dev/shm and will be deleted
	            if (btn_path[pos][0] == 0) continue;
	            if (blank_png && strcmp(btn_path[pos], blank_png) == 0) continue;

	            char cached[PATH_MAX];
	            if (session_cache_icon(opt, btn_path[pos], cached, sizeof(cached)) == 0 && cached[0]) {
	                snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", cached);
	                btn_set[pos] = true;
	            }
	        }
	    }

	    // Build command
	    char *cmd = NULL;
	    size_t w = 0;
	    size_t cap = 0;
    appendf_dyn(&cmd, &w, &cap, "%s", wp_active ? "set-buttons-explicit-14" : "set-buttons-explicit");
    for (int pos = 1; pos <= 13; pos++) {
        if (!btn_set[pos]) snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", blank_png);
        appendf_dyn(&cmd, &w, &cap, " --button-%d=%s", pos, btn_path[pos]);
        if (label_set[pos]) {
            appendf_dyn(&cmd, &w, &cap, " --label-%d=%s", pos, btn_label[pos]);
        }
    }
    if (wp_active && wp_tile14[0]) {
        appendf_dyn(&cmd, &w, &cap, " --button-14=%s", wp_tile14);
    }
    if (!cmd) return;
    if (w > 8000) log_msg("send cmd_len=%zu (was previously truncated at 8192)", w);

    char reply[64] = {0};
    int sr = send_line_and_read_reply(opt->ulanzi_sock, cmd, reply, sizeof(reply));
    if (sr != 0) {
        free(cmd);
        for (int pos = 1; pos <= 13; pos++) {
            if (cleanup_tmp[pos]) unlink(btn_path[pos]);
        }
        log_msg("send failed (rc=%d, resp='%s')", sr, reply[0] ? reply : "<empty>");
        return;
    }
    free(cmd);
    snprintf(last_sig, last_sig_cap, "%s", sig);
    log_render("send resp='%s'", reply[0] ? reply : "<empty>");

    // Mark cmd-driven values as pushed (so we don't spam partial updates right after a full render).
    if (g_cmd_engine) {
        for (int pos = 1; pos <= 13; pos++) {
            CmdEntry *ce = cmd_entry_for_pos[pos];
            if (!ce) continue;
            pthread_mutex_lock(&ce->mu);
            if (cmd_text_set[pos]) {
                snprintf(ce->last_sent_text, sizeof(ce->last_sent_text), "%s", cmd_text_for_pos[pos]);
            }
            if (cmd_state_set[pos]) {
                snprintf(ce->last_sent_state, sizeof(ce->last_sent_state), "%s", cmd_state_for_pos[pos]);
            }
            pthread_mutex_unlock(&ce->mu);
        }
    }

    // Cleanup any temporary per-render images created in /dev/shm.
    for (int pos = 1; pos <= 13; pos++) {
        if (cleanup_tmp[pos]) {
            unlink(btn_path[pos]);
        }
    }
}

static void state_dir(const Options *opt, char *out, size_t cap) {
    // Prefer RAM-backed /dev/shm for ALL runtime state (tmp overlays, wallpaper session tiles,
    // composed caches, etc). This directory is wiped at paging_daemon startup.
    //
    // We prefer a single shared folder (`/dev/shm/goofydeck/paging`) to make cleanup simple and
    // avoid "root-only" per-user dirs created by past sudo runs. If it's not writable (e.g. root-owned),
    // fall back to a per-uid folder under /dev/shm.
    if (!out || cap == 0) return;
    const char *shared = "/dev/shm/goofydeck/paging";
    if (try_ensure_dir_parent(shared) == 0) {
        // Create best-effort with permissive mode (umask may still reduce it).
        (void)mkdir("/dev/shm/goofydeck", 0777);
        (void)mkdir(shared, 0777);
        if (access(shared, W_OK | X_OK) == 0) {
            snprintf(out, cap, "%s", shared);
            return;
        }
    }

    char per_uid[PATH_MAX];
    snprintf(per_uid, sizeof(per_uid), "/dev/shm/goofydeck_%u/paging", (unsigned)getuid());
    if (try_ensure_dir_parent(per_uid) == 0 && try_ensure_dir(per_uid) == 0 && access(per_uid, W_OK | X_OK) == 0) {
        snprintf(out, cap, "%s", per_uid);
        return;
    }

    // Fallback: /tmp is also typically tmpfs and avoids repo permission issues.
    snprintf(out, cap, "/tmp/goofydeck_paging_%u", (unsigned)getuid());
    if (try_ensure_dir(out) == 0 && access(out, W_OK | X_OK) == 0) return;

    // Last resort: inside repo cache
    snprintf(out, cap, "%s/paging", (opt && opt->cache_root) ? opt->cache_root : ".cache");
    ensure_dir(out);
}

static int rm_tree_contents(const char *dir_path) {
    if (!dir_path || !dir_path[0]) return -1;

    DIR *d = opendir(dir_path);
    if (!d) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (lstat(p, &st) != 0) {
            rc = -1;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (rm_tree_contents(p) != 0) rc = -1;
            if (rmdir(p) != 0) rc = -1;
        } else {
            if (unlink(p) != 0) rc = -1;
        }
    }
    closedir(d);
    return rc;
}

static void wipe_paging_state_dir_at_startup(const Options *opt) {
    char dir[PATH_MAX];
    state_dir(opt, dir, sizeof(dir));

    // Safety: only wipe RAM-backed state dirs.
    if (strncmp(dir, "/dev/shm/", 9) != 0) return;

    (void)rm_tree_contents(dir);
}

static int join_path(char *out, size_t cap, const char *a, const char *b) {
    if (!out || cap == 0 || !a || !b) return -1;
    size_t al = strlen(a);
    size_t bl = strlen(b);
    // +1 for '/', +1 for '\0'
    if (al + 1 + bl + 1 > cap) return -1;
    memcpy(out, a, al);
    out[al] = '/';
    memcpy(out + al + 1, b, bl);
    out[al + 1 + bl] = 0;
    return 0;
}

static void persist_last_page(const Options *opt, const char *page_name, size_t offset) {
    if (!page_name) return;
    char dir[PATH_MAX];
    state_dir(opt, dir, sizeof(dir));

    char ppath[PATH_MAX];
    if (join_path(ppath, sizeof(ppath), dir, "last_page") != 0) return;
    FILE *pf = fopen(ppath, "wb");
    if (pf) { fprintf(pf, "%s\n", page_name); fclose(pf); }

    char opath[PATH_MAX];
    if (join_path(opath, sizeof(opath), dir, "last_offset") != 0) return;
    FILE *of = fopen(opath, "wb");
    if (of) { fprintf(of, "%zu\n", offset); fclose(of); }
}

static int load_last_page(const Options *opt, char *out_page, size_t out_page_cap, size_t *out_offset) {
    if (!out_page || out_page_cap == 0 || !out_offset) return -1;
    char dir[PATH_MAX];
    state_dir(opt, dir, sizeof(dir));

    char ppath[PATH_MAX];
    if (join_path(ppath, sizeof(ppath), dir, "last_page") != 0) return -1;
    FILE *pf = fopen(ppath, "rb");
    if (!pf) return -1;
    if (!fgets(out_page, (int)out_page_cap, pf)) { fclose(pf); return -1; }
    fclose(pf);
    trim(out_page);
    if (out_page[0] == 0) return -1;

    char opath[PATH_MAX];
    if (join_path(opath, sizeof(opath), dir, "last_offset") != 0) { *out_offset = 0; return 0; }
    FILE *of = fopen(opath, "rb");
    if (!of) { *out_offset = 0; return 0; }
    char obuf[64] = {0};
    if (!fgets(obuf, sizeof(obuf), of)) { fclose(of); *out_offset = 0; return 0; }
    fclose(of);
    trim(obuf);
    unsigned long long v = 0;
    if (sscanf(obuf, "%llu", &v) != 1) v = 0;
    *out_offset = (size_t)v;
    return 0;
}

static int set_nonblocking_fd(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int ha_connect_events(const Options *opt) {
    if (!opt || !opt->ha_sock) return -1;
    int fd = unix_connect(opt->ha_sock);
    if (fd < 0) return -1;
    (void)set_nonblocking_fd(fd);
    return fd;
}

static bool g_ha_connected_logged = false;

static void ha_handle_line(HaStateMap *ha_map, const char *line) {
    if (!line) return;
    if (strncmp(line, "evt connected", 13) == 0) {
        if (!g_ha_connected_logged) log_msg("ha: connected");
        g_ha_connected_logged = true;
        return;
    }
    if (strncmp(line, "evt disconnected", 16) == 0) {
        if (g_ha_connected_logged) log_msg("ha: disconnected");
        g_ha_connected_logged = false;
        return;
    }
    if (strncmp(line, "err ", 4) == 0) {
        log_msg("ha: %s", line);
        return;
    }
    if (strncmp(line, "evt state ", 10) == 0) {
        const char *p = line + 10;
        while (*p == ' ' || *p == '\t') p++;
        char entity[256] = {0};
        size_t w = 0;
        while (*p && *p != ' ' && *p != '\t' && w + 1 < sizeof(entity)) entity[w++] = *p++;
        entity[w] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (entity[0] && *p == '{') {
            ha_state_update_from_json(ha_map, entity, p);
        }
        return;
    }
}

static int ha_send_and_wait_reply(int ha_fd, char *ha_buf, size_t *ha_len, HaStateMap *ha_map, const char *cmd,
                                  char *out_line, size_t out_cap, int timeout_ms) {
    if (!out_line || out_cap == 0) return -1;
    out_line[0] = 0;
    if (ha_fd < 0 || !cmd) return -1;
    if (ha_send_line_fd(ha_fd, cmd) != 0) return -1;

    double start = now_sec_monotonic();
    for (;;) {
        // Try to extract already buffered lines first.
        char line[8192];
        int lr = read_line_from_fd(ha_fd, line, sizeof(line), ha_buf, ha_len);
        if (lr == 1) {
            if (strncmp(line, "ok", 2) == 0 || strncmp(line, "err", 3) == 0) {
                snprintf(out_line, out_cap, "%.*s", (int)(out_cap - 1), line);
                return 0;
            }
            ha_handle_line(ha_map, line);
            continue;
        }
        if (lr < 0) return -1;

        // Wait for more data.
        int elapsed_ms = (int)((now_sec_monotonic() - start) * 1000.0);
        if (timeout_ms >= 0 && elapsed_ms >= timeout_ms) return -1;
        int remain = timeout_ms < 0 ? 250 : (timeout_ms - elapsed_ms);
        if (remain > 250) remain = 250;
        if (remain < 0) remain = 0;
        struct pollfd pfd[1] = {{.fd = ha_fd, .events = POLLIN}};
        (void)poll(pfd, 1, remain);
    }
}

static void ha_unsubscribe_all(int *ha_fd, char *ha_buf, size_t *ha_len, HaStateMap *ha_map, HaSubs *subs) {
    if (!subs) return;
    if (!ha_fd || *ha_fd < 0) {
        ha_subs_clear_no_unsub(subs);
        return;
    }
    for (size_t i = 0; i < subs->len; i++) {
        if (subs->items[i].sub_id <= 0) continue;
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "unsub %d", subs->items[i].sub_id);
        char reply[256];
        (void)ha_send_and_wait_reply(*ha_fd, ha_buf, ha_len, ha_map, cmd, reply, sizeof(reply), 1000);
    }
    ha_subs_clear_no_unsub(subs);
}

static int ha_subscribe_entity(int *ha_fd, char *ha_buf, size_t *ha_len, HaStateMap *ha_map, HaSubs *subs, const char *entity_id) {
    if (!ha_fd || !ha_buf || !ha_len || !ha_map || !subs || !entity_id || !entity_id[0]) return -1;
    for (size_t i = 0; i < subs->len; i++) {
        if (subs->items[i].entity_id && strcmp(subs->items[i].entity_id, entity_id) == 0) return 0;
    }
    if (*ha_fd < 0) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sub-state %s", entity_id);
    char reply[256];
    if (ha_send_and_wait_reply(*ha_fd, ha_buf, ha_len, ha_map, cmd, reply, sizeof(reply), 1500) != 0) return -1;
    if (strncmp(reply, "ok", 2) != 0) return -1;

    int sub_id = 0;
    const char *p = strstr(reply, "sub_id=");
    if (p) sub_id = atoi(p + 7);
    if (sub_id <= 0) return -1;

    if (subs->len >= subs->cap) {
        subs->cap = subs->cap ? subs->cap * 2 : 32;
        subs->items = xrealloc(subs->items, subs->cap * sizeof(HaSub));
    }
    subs->items[subs->len].entity_id = xstrdup(entity_id);
    subs->items[subs->len].sub_id = sub_id;
    subs->len++;
    return 0;
}

static int ha_get_entity_fd(int *ha_fd, char *ha_buf, size_t *ha_len, HaStateMap *ha_map, const char *entity_id) {
    if (!ha_fd || *ha_fd < 0 || !ha_buf || !ha_len || !ha_map || !entity_id || !entity_id[0]) return -1;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "get %s", entity_id);
    char reply[2048];
    if (ha_send_and_wait_reply(*ha_fd, ha_buf, ha_len, ha_map, cmd, reply, sizeof(reply), 1500) != 0) return -1;
    ha_state_update_from_get_reply(ha_map, entity_id, reply);
    return 0;
}

static void precache_state_icons(const Options *opt, const Config *cfg) {
    if (!opt || !cfg) return;
    for (size_t pi = 0; pi < cfg->page_count; pi++) {
        const Page *p = &cfg->pages[pi];
        if (!p->name || strcmp(p->name, "_sys") == 0) continue;
        for (size_t ii = 0; ii < p->count; ii++) {
            const Item *it = &p->items[ii];
            if (!it) continue;

            // Precache:
            // - HA states: base + variants
            // - HA value-only: base without dynamic text
            // - Static text-only: base + "-text" cached variant
            // - Others: normal cached icon
            if (it->state_count > 0) {
                char tmp[PATH_MAX];
                (void)cached_or_generated_into_state(opt, cfg, p->name, ii, it, NULL, NULL, NULL, "base", tmp, sizeof(tmp));
            } else if (it->entity_id && it->entity_id[0]) {
                char tmp[PATH_MAX];
                (void)cached_or_generated_into_state(opt, cfg, p->name, ii, it, NULL, (const char *)"", NULL, NULL, tmp, sizeof(tmp));
            } else {
                char tmp[PATH_MAX];
                if (!cached_or_generated_static_text_into(opt, cfg, p->name, ii, it, tmp, sizeof(tmp))) {
                    (void)cached_or_generated_into(opt, cfg, p->name, ii, it, tmp, sizeof(tmp));
                }
            }

            // State variants
            if (it->state_count > 0) {
                for (size_t si = 0; si < it->state_count; si++) {
                    const StateOverride *ov = &it->states[si];
                    char tmp[PATH_MAX];
                    (void)cached_or_generated_into_state(opt, cfg, p->name, ii, it,
                                                         (ov->icon && ov->icon[0]) ? ov->icon : NULL,
                                                         (ov->text && ov->text[0]) ? ov->text : NULL,
                                                         (ov->preset && ov->preset[0]) ? ov->preset : NULL,
                                                         (ov->key && ov->key[0]) ? ov->key : NULL,
                                                         tmp, sizeof(tmp));
                }
            }
        }
    }
}

static void ha_enter_page(const Options *opt, const Config *cfg, const char *page_name,
                          int *ha_fd, char *ha_buf, size_t *ha_len, HaStateMap *ha_map, HaSubs *subs) {
    if (!opt || !cfg || !page_name || !ha_fd || !ha_buf || !ha_len || !ha_map || !subs) return;

    ha_unsubscribe_all(ha_fd, ha_buf, ha_len, ha_map, subs);

    const Page *p = config_get_page((Config *)cfg, page_name);
    if (!p) return;

    // Only connect to ha_daemon if this page needs it.
    bool needs_ha = false;
    for (size_t i = 0; i < p->count; i++) {
        if (p->items[i].entity_id && p->items[i].entity_id[0]) { needs_ha = true; break; }
    }
    if (!needs_ha) return;

    if (*ha_fd < 0) {
        *ha_fd = ha_connect_events(opt);
        if (*ha_fd >= 0) log_msg("ha socket: %s", opt->ha_sock);
        else log_msg("ha socket not available: %s (ha integration disabled)", opt->ha_sock);
    }
    if (*ha_fd < 0) return;

    for (size_t i = 0; i < p->count; i++) {
        const Item *it = &p->items[i];
        if (!it->entity_id || !it->entity_id[0]) continue;
        (void)ha_subscribe_entity(ha_fd, ha_buf, ha_len, ha_map, subs, it->entity_id);
        (void)ha_get_entity_fd(ha_fd, ha_buf, ha_len, ha_map, it->entity_id);
    }
}

static void ulanzi_send_partial(const Options *opt, int pos, const char *png_path, const char *label_src) {
    if (!opt || !opt->ulanzi_sock || pos < 1 || pos > 13 || !png_path || !png_path[0]) return;
    char label[64] = {0};
    if (label_src && label_src[0]) make_device_label(label_src, label, sizeof(label));

    // Session cache: even without wallpaper, keep stable icons in RAM to avoid disk reads.
    const char *send_png = png_path;
    char cached_png[PATH_MAX] = {0};
    if (session_cache_icon(opt, png_path, cached_png, sizeof(cached_png)) == 0 && cached_png[0]) {
        send_png = cached_png;
    }

    char cmd[PATH_MAX + 256];
    if (label[0]) snprintf(cmd, sizeof(cmd), "set-partial-explicit --button-%d=%s --label-%d=%s", pos, send_png, pos, label);
    else snprintf(cmd, sizeof(cmd), "set-partial-explicit --button-%d=%s", pos, send_png);

    char reply[128] = {0};
    if (send_line_and_read_reply(opt->ulanzi_sock, cmd, reply, sizeof(reply)) != 0) {
        log_msg("partial send failed (pos=%d)", pos);
        return;
    }
    (void)reply;
}

static void ulanzi_send_partial_wallpaper(const Options *opt, const Config *cfg, const char *page_name, int pos,
                                          const char *png_path, const char *label_src, const char *blank_png) {
    if (!opt || !cfg || !page_name || pos < 1 || pos > 13 || !png_path || !png_path[0]) return;
    const Page *page = config_get_page((Config *)cfg, page_name);
    WallpaperEff wp = effective_wallpaper(cfg, page);
    if (!wp.enabled) {
        ulanzi_send_partial(opt, pos, png_path, label_src);
        return;
    }

    char render_dir[PATH_MAX];
    char prefix[PATH_MAX];
    if (ensure_wallpaper_rendered(opt, &wp, render_dir, sizeof(render_dir), prefix, sizeof(prefix)) != 0) {
        ulanzi_send_partial(opt, pos, png_path, label_src);
        return;
    }

    // Blank => wallpaper tile only.
    if (blank_png && strcmp(png_path, blank_png) == 0) {
        char tile[PATH_MAX];
        if (wallpaper_session_tile(opt, render_dir, prefix, &wp, pos, tile, sizeof(tile)) == 0 && tile[0]) {
            ulanzi_send_partial(opt, pos, tile, label_src);
            return;
        }
        ulanzi_send_partial(opt, pos, png_path, label_src);
        return;
    }

    // Signature for caching composed images.
    uint32_t wp_sig = 0;
    if (wp.path && wp.path[0]) {
        char wkey[1024];
        snprintf(wkey, sizeof(wkey), "path:%s\nq:%d\nm:%d\nd:%d\n", wp.path, wp.quality, wp.magnify, wp.dithering ? 1 : 0);
        wp_sig = fnv1a32(wkey, strlen(wkey));
    }

    char composed[PATH_MAX];
    bool composed_is_tmp = false;
    if (wp_compose_cached(opt, wp_sig, render_dir, prefix, &wp, pos, png_path, composed, sizeof(composed), &composed_is_tmp) == 0 &&
        composed[0]) {
        ulanzi_send_partial(opt, pos, composed, label_src);
        if (composed_is_tmp) unlink(composed);
        return;
    }

    // Fallback: send without wallpaper if composition fails.
    ulanzi_send_partial(opt, pos, png_path, label_src);
}

static void ha_partial_update_visible(const Options *opt, const Config *cfg, const char *page_name, size_t offset,
                                      const HaStateMap *ha_map, const char *blank_png, const char *changed_entity_id) {
    if (!opt || !cfg || !page_name || !ha_map || !blank_png || !changed_entity_id) return;
    const Page *p = config_get_page((Config *)cfg, page_name);
    if (!p) return;

    bool show_back = strcmp(page_name, "$root") != 0;
    SheetLayout sheet = compute_sheet_layout(p->count, show_back, offset);
    offset = sheet.start;

    bool reserved_back = show_back;
    bool reserved_prev = sheet.show_prev;
    bool reserved_next = sheet.show_next;
    int back_pos = cfg->pos_back;
    int prev_pos = cfg->pos_prev;
    int next_pos = cfg->pos_next;

    size_t item_i = offset;
    for (int pos = 1; pos <= 13; pos++) {
        bool reserved = false;
        if (reserved_back && pos == back_pos) reserved = true;
        if (reserved_prev && pos == prev_pos) reserved = true;
        if (reserved_next && pos == next_pos) reserved = true;
        if (reserved) continue;
        if (item_i >= p->count) break;

        const Item *it = page_item_at(p, item_i);
        if (it && it->entity_id && strcmp(it->entity_id, changed_entity_id) == 0) {
            const char *label_src = (it->name && it->name[0]) ? it->name : NULL;
            char icon_path[PATH_MAX];
            bool have_icon = false;
            bool sent = false;

            // State variants (only if the state exists in config states mapping).
            if (it->state_count > 0) {
                const char *cur_state = NULL;
                for (size_t si = 0; si < ha_map->len; si++) {
                    if (ha_map->items[si].entity_id && strcmp(ha_map->items[si].entity_id, it->entity_id) == 0) {
                        cur_state = ha_map->items[si].state;
                        break;
                    }
                }
                if (cur_state && cur_state[0]) {
                    const StateOverride *ov = item_find_state_override(it, cur_state);
                    if (ov) {
                        if (ov->name && ov->name[0]) label_src = ov->name;
                        (void)cached_or_generated_into_state(opt, cfg, page_name, item_i, it,
                                                             (ov->icon && ov->icon[0]) ? ov->icon : NULL,
                                                             (ov->text && ov->text[0]) ? ov->text : NULL,
                                                             (ov->preset && ov->preset[0]) ? ov->preset : NULL,
                                                             cur_state, icon_path, sizeof(icon_path));
                        if (file_exists(icon_path)) have_icon = true;
                    }
                    if (!have_icon) {
                        (void)cached_or_generated_into_state(opt, cfg, page_name, item_i, it, NULL, NULL, NULL, "base", icon_path, sizeof(icon_path));
                        if (file_exists(icon_path)) have_icon = true;
                    }
                } else {
                    (void)cached_or_generated_into_state(opt, cfg, page_name, item_i, it, NULL, NULL, NULL, "base", icon_path, sizeof(icon_path));
                    if (file_exists(icon_path)) have_icon = true;
                }
            }

            // Value display (only for value-like domains; otherwise we'd show raw "off" for scripts, etc).
            if (!have_icon && it->state_count == 0 && ha_entity_is_value_display(it->entity_id)) {
                char value_text[128] = {0};
                ha_format_value_text(ha_map, it->entity_id, value_text, sizeof(value_text));
                const char *pr_name = (it->preset && it->preset[0]) ? it->preset : "default";
                const Preset *pr = config_get_preset(cfg, pr_name);
                if (!pr) pr = config_get_preset(cfg, "default");
                const char *eff_icon = (it->icon && it->icon[0]) ? it->icon : (pr && pr->icon) ? pr->icon : NULL;
                const char *eff_text = (it->text && it->text[0]) ? it->text : value_text;
                if ((eff_icon && eff_icon[0]) || (eff_text && eff_text[0])) {
                    Item tmp_it = *it;
                    tmp_it.icon = (char *)(eff_icon ? eff_icon : "");
                    tmp_it.text = (char *)"";
                    char base_png[PATH_MAX];
                    if (cached_or_generated_into_state(opt, cfg, page_name, item_i, &tmp_it, NULL, (const char *)"", pr_name, NULL, base_png, sizeof(base_png))) {
                        // If wallpaper is active, compose tile+base once (cached) and draw the value text on top,
                        // so updates don't need draw_over every time.
                        const Page *page = config_get_page((Config *)cfg, page_name);
                        WallpaperEff wp = effective_wallpaper(cfg, page);
                        if (wp.enabled && wp.path && wp.path[0]) {
                            char render_dir[PATH_MAX];
                            char prefix[PATH_MAX];
                            char draw_over_bin[PATH_MAX];
                            snprintf(draw_over_bin, sizeof(draw_over_bin), "%s/icons/draw_over", opt->root_dir);
                            if (access(draw_over_bin, X_OK) == 0 &&
                                ensure_wallpaper_rendered(opt, &wp, render_dir, sizeof(render_dir), prefix, sizeof(prefix)) == 0) {
                                uint32_t wp_sig = 0;
                                char wkey[1024];
                                snprintf(wkey, sizeof(wkey), "path:%s\nq:%d\nm:%d\nd:%d\n", wp.path, wp.quality, wp.magnify, wp.dithering ? 1 : 0);
                                wp_sig = fnv1a32(wkey, strlen(wkey));

                                char composed_base[PATH_MAX] = {0};
                                bool composed_tmp = false;
                                if (wp_compose_cached(opt, wp_sig, render_dir, prefix, &wp, pos, base_png,
                                                      composed_base, sizeof(composed_base), &composed_tmp) == 0 &&
                                    composed_base[0]) {
                                    char tmp_out[PATH_MAX];
                                    if (render_value_text_on_base_tmp(opt, pr, page_name, pos, composed_base, eff_text, tmp_out, sizeof(tmp_out)) == 0) {
                                        ulanzi_send_partial(opt, pos, tmp_out, label_src);
                                        unlink(tmp_out);
                                        sent = true;
                                    }
                                    if (composed_tmp) unlink(composed_base);
                                }
                            }
                        }
                        if (!sent) {
                            char tmp_out[PATH_MAX];
                            if (render_value_text_on_base_tmp(opt, pr, page_name, pos, base_png, eff_text, tmp_out, sizeof(tmp_out)) == 0) {
                                snprintf(icon_path, sizeof(icon_path), "%s", tmp_out);
                                ulanzi_send_partial_wallpaper(opt, cfg, page_name, pos, icon_path, label_src, blank_png);
                                unlink(icon_path);
                                sent = true;
                            }
                        }
                    }
                }
            }

            if (sent) {
                item_i++;
                continue;
            }

            // Fallback to existing rendering/cached icon.
            if (!have_icon) {
                if (cached_or_generated_into(opt, cfg, page_name, item_i, it, icon_path, sizeof(icon_path))) {
                    have_icon = true;
                } else {
                    snprintf(icon_path, sizeof(icon_path), "%s", blank_png);
                    have_icon = true;
                }
            }

            if (have_icon) ulanzi_send_partial_wallpaper(opt, cfg, page_name, pos, icon_path, label_src, blank_png);
        }
        item_i++;
    }
}

static void cmd_apply_updates_current_page(const Options *opt, const Config *cfg, const char *page_name, size_t offset,
                                          const char *blank_png) {
    if (!opt || !cfg || !page_name || !page_name[0] || !blank_png) return;
    if (!g_cmd_engine) return;
    if (!g_ulanzi_device_ready) return;

    const Page *p = config_get_page((Config *)cfg, page_name);
    if (!p) return;
    bool show_back = strcmp(page_name, "$root") != 0;
    SheetLayout sheet = compute_sheet_layout(p->count, show_back, offset);
    offset = sheet.start;

    // Wallpaper context (optional)
    WallpaperEff wp = effective_wallpaper(cfg, p);
    bool wp_active = false;
    char wp_render_dir[PATH_MAX] = {0};
    char wp_prefix[PATH_MAX] = {0};
    bool have_draw_over = false;
    uint32_t wp_sig = 0;
    if (wp.enabled && wp.path && wp.path[0]) {
        if (ensure_wallpaper_rendered(opt, &wp, wp_render_dir, sizeof(wp_render_dir), wp_prefix, sizeof(wp_prefix)) == 0) {
            wp_active = true;
            char wkey[1024];
            snprintf(wkey, sizeof(wkey), "path:%s\nq:%d\nm:%d\nd:%d\n", wp.path, wp.quality, wp.magnify, wp.dithering ? 1 : 0);
            wp_sig = fnv1a32(wkey, strlen(wkey));
            char draw_over_bin[PATH_MAX];
            snprintf(draw_over_bin, sizeof(draw_over_bin), "%s/icons/draw_over", opt->root_dir);
            have_draw_over = (access(draw_over_bin, X_OK) == 0);
        }
    }

    // Walk currently visible items and push partial updates if their cmd-driven state/text changed.
    size_t item_i = offset;
    for (int pos = 1; pos <= 13 && item_i < p->count; pos++) {
        bool reserved = false;
        if (show_back && pos == cfg->pos_back) reserved = true;
        if (sheet.show_prev && pos == cfg->pos_prev) reserved = true;
        if (sheet.show_next && pos == cfg->pos_next) reserved = true;
        if (reserved) continue;

        const Item *it = page_item_at(p, item_i);
        if (!it) { item_i++; continue; }

        CmdEntry *ce = cmd_engine_find(g_cmd_engine, page_name, item_i);
        if (!ce) { item_i++; continue; }

        char cur_text[256] = {0};
        char cur_state[64] = {0};
        char sent_text[256] = {0};
        char sent_state[64] = {0};
        pthread_mutex_lock(&ce->mu);
        snprintf(cur_text, sizeof(cur_text), "%s", ce->last_text);
        snprintf(cur_state, sizeof(cur_state), "%s", ce->last_state);
        snprintf(sent_text, sizeof(sent_text), "%s", ce->last_sent_text);
        snprintf(sent_state, sizeof(sent_state), "%s", ce->last_sent_state);
        pthread_mutex_unlock(&ce->mu);

        // Determine label (state override may change it).
        const char *label_src = (it->name && it->name[0]) ? it->name : NULL;
        const StateOverride *state_ov = NULL;
        if (it->state_count > 0 && cur_state[0]) {
            state_ov = item_find_state_override(it, cur_state);
            if (state_ov && state_ov->name && state_ov->name[0]) label_src = state_ov->name;
        }

        // 1) State-driven icon update
        bool state_changed = false;
        if (it->state_count > 0) {
            if (strcmp(cur_state, sent_state) != 0) state_changed = true;
        }
        if (state_changed) {
            char icon_path[PATH_MAX] = {0};
            bool have_icon = false;
            if (cur_state[0] && state_ov) {
                if (cached_or_generated_into_state(opt, cfg, page_name, item_i, it,
                                                   (state_ov->icon && state_ov->icon[0]) ? state_ov->icon : NULL,
                                                   (state_ov->text && state_ov->text[0]) ? state_ov->text : NULL,
                                                   (state_ov->preset && state_ov->preset[0]) ? state_ov->preset : NULL,
                                                   cur_state, icon_path, sizeof(icon_path))) {
                    have_icon = true;
                }
            }
            if (!have_icon) {
                if (cached_or_generated_into_state(opt, cfg, page_name, item_i, it, NULL, NULL, NULL, "base", icon_path,
                                                   sizeof(icon_path))) {
                    have_icon = true;
                }
            }
            if (!have_icon) snprintf(icon_path, sizeof(icon_path), "%s", blank_png);

            ulanzi_send_partial_wallpaper(opt, cfg, page_name, pos, icon_path, label_src, blank_png);
            pthread_mutex_lock(&ce->mu);
            snprintf(ce->last_sent_state, sizeof(ce->last_sent_state), "%s", cur_state);
            pthread_mutex_unlock(&ce->mu);
        }

        // 2) Text update (exec_text / poll text)
        if (strcmp(cur_text, sent_text) != 0) {
            const Preset *pr = NULL;
            const char *pr_name = (it->preset && it->preset[0]) ? it->preset : "default";
            pr = config_get_preset(cfg, pr_name);
            if (!pr) pr = config_get_preset(cfg, "default");

            // Determine base icon for this slot (respect current state if any).
            char base_png[PATH_MAX] = {0};
            bool have_base = false;
            if (it->state_count > 0 && cur_state[0]) {
                const StateOverride *ov = state_ov;
                if (ov) {
                    have_base = cached_or_generated_into_state(opt, cfg, page_name, item_i, it,
                                                               (ov->icon && ov->icon[0]) ? ov->icon : NULL,
                                                               (ov->text && ov->text[0]) ? ov->text : NULL,
                                                               (ov->preset && ov->preset[0]) ? ov->preset : NULL,
                                                               cur_state, base_png, sizeof(base_png));
                }
                if (!have_base) {
                    have_base = cached_or_generated_into_state(opt, cfg, page_name, item_i, it, NULL, NULL, NULL, "base", base_png,
                                                               sizeof(base_png));
                }
            } else {
                have_base = cached_or_generated_into(opt, cfg, page_name, item_i, it, base_png, sizeof(base_png));
                if (!have_base) {
                    // allow text-only on an empty base
                    snprintf(base_png, sizeof(base_png), "%s", blank_png);
                    have_base = true;
                }
            }

            if (have_base && pr) {
                // Empty output means "clear the overlay": just send the base icon again.
                if (!cur_text[0]) {
                    ulanzi_send_partial_wallpaper(opt, cfg, page_name, pos, base_png, label_src, blank_png);
                    pthread_mutex_lock(&ce->mu);
                    ce->last_sent_text[0] = 0;
                    pthread_mutex_unlock(&ce->mu);
                    item_i++;
                    continue;
                }

                const char *eff_text = cur_text;

                if (wp_active && have_draw_over) {
                    char composed_base[PATH_MAX] = {0};
                    bool composed_tmp = false;
                    if (wp_compose_cached(opt, wp_sig, wp_render_dir, wp_prefix, &wp, pos, base_png,
                                          composed_base, sizeof(composed_base), &composed_tmp) == 0 &&
                        composed_base[0]) {
                        char tmp_out[PATH_MAX];
                        if (render_value_text_on_base_tmp(opt, pr, page_name, pos, composed_base, eff_text, tmp_out,
                                                          sizeof(tmp_out)) == 0) {
                            ulanzi_send_partial(opt, pos, tmp_out, label_src);
                            unlink(tmp_out);
                            pthread_mutex_lock(&ce->mu);
                            snprintf(ce->last_sent_text, sizeof(ce->last_sent_text), "%s", cur_text);
                            pthread_mutex_unlock(&ce->mu);
                        }
                        if (composed_tmp) unlink(composed_base);
                    }
                } else {
                    char tmp_out[PATH_MAX];
                    if (render_value_text_on_base_tmp(opt, pr, page_name, pos, base_png, eff_text, tmp_out, sizeof(tmp_out)) == 0) {
                        ulanzi_send_partial_wallpaper(opt, cfg, page_name, pos, tmp_out, label_src, blank_png);
                        unlink(tmp_out);
                        pthread_mutex_lock(&ce->mu);
                        snprintf(ce->last_sent_text, sizeof(ce->last_sent_text), "%s", cur_text);
                        pthread_mutex_unlock(&ce->mu);
                    }
                }
            }
        }

        item_i++;
    }
}

static bool handle_cmd_action(const Options *opt, Config *cfg, const char *cur_page, size_t offset, size_t pressed_item,
                              int btn, ButtonEvent evt, const Item *it, const char *action, const char *data,
                              const char *blank_png) {
    if (!opt || !cfg || !cur_page || !it || !action || !blank_png) return false;
    if (strncmp(action, "$cmd.", 5) != 0) return false;
    if (!g_cmd_engine) return true;

    const bool is_tap = (evt == BTN_EVT_TAP);
    const bool is_hold = (evt == BTN_EVT_HOLD);
    const bool is_longhold = (evt == BTN_EVT_LONGHOLD);
    const bool is_release = (evt == BTN_EVT_RELEASED);

    const char *cmd = (data && data[0]) ? data : NULL;

    if (strcmp(action, "$cmd.exec") == 0 || strcmp(action, "$cmd.execute") == 0) {
        if (!cmd) return true;
        if (g_cmd_logs) cmd_log("exec btn=%d", btn);
        CmdOneshotExecArgs *a = calloc(1, sizeof(*a));
        if (!a) die_errno("calloc");
        a->engine = g_cmd_engine;
        a->cmd = xstrdup(cmd);
        pthread_t th;
        if (pthread_create(&th, NULL, cmd_oneshot_exec_thread, a) == 0) {
            pthread_detach(th);
        } else {
            free(a->cmd);
            free(a);
        }
        return true;
    }

    if (strcmp(action, "$cmd.exec_text") == 0) {
        if (!cmd) return true;
        CmdEntry *ce = cmd_engine_get_or_add(g_cmd_engine, cur_page, pressed_item);
        if (!ce) return true;
        if (g_cmd_logs) cmd_log("exec_text btn=%d", btn);
        CmdTextOpts o = cmd_text_opts_defaults();
        if (is_tap) o = it->tap_cmd_text;
        else if (is_hold) o = it->hold_cmd_text;
        else if (is_longhold) o = it->longhold_cmd_text;
        else if (is_release) o = it->released_cmd_text;

        CmdOneshotTextArgs *a = calloc(1, sizeof(*a));
        if (!a) die_errno("calloc");
        a->engine = g_cmd_engine;
        a->entry = ce;
        a->cmd = xstrdup(cmd);
        a->opts = o;
        pthread_t th;
        if (pthread_create(&th, NULL, cmd_oneshot_text_thread, a) == 0) {
            pthread_detach(th);
        } else {
            free(a->cmd);
            free(a);
        }
        return true;
    }

    if (strcmp(action, "$cmd.poll_start") == 0) {
        CmdEntry *ce = cmd_engine_get_or_add(g_cmd_engine, cur_page, pressed_item);
        if (!ce) return true;
        pthread_mutex_lock(&ce->mu);
        ce->poll_gen++;
        ce->state_gen++;
        if (ce->cfg_poll_every_ms > 0 && ce->cfg_poll_cmd && ce->cfg_poll_cmd[0]) {
            ce->poll_every_ms = ce->cfg_poll_every_ms;
            ce->poll_cmd = ce->cfg_poll_cmd;
            ce->poll_is_text = ce->cfg_poll_is_text;
            ce->poll_opts = ce->cfg_poll_opts;
            ce->next_poll_ns = 0;
        }
        if (ce->cfg_state_every_ms > 0 && ce->cfg_state_cmd && ce->cfg_state_cmd[0]) {
            ce->state_every_ms = ce->cfg_state_every_ms;
            ce->state_cmd = ce->cfg_state_cmd;
            ce->next_state_ns = 0;
        }
        pthread_mutex_unlock(&ce->mu);
        if (g_cmd_logs) cmd_log("poll_start btn=%d", btn);
        return true;
    }

    if (strcmp(action, "$cmd.poll_stop") == 0) {
        CmdEntry *ce = cmd_engine_find(g_cmd_engine, cur_page, pressed_item);
        if (!ce) return true;
        pthread_mutex_lock(&ce->mu);
        ce->poll_gen++;
        ce->state_gen++;
        ce->poll_every_ms = 0;
        ce->state_every_ms = 0;
        ce->next_poll_ns = 0;
        ce->next_state_ns = 0;
        // Also clear the displayed text so the base icon is resent (like $cmd.text_clear).
        // Keep last_sent_text intact so cmd_apply_updates_current_page() detects a change.
        ce->last_text[0] = 0;
        pthread_mutex_unlock(&ce->mu);
        if (g_cmd_logs) cmd_log("poll_stop btn=%d", btn);
        cmd_engine_notify(g_cmd_engine);
        cmd_apply_updates_current_page(opt, cfg, cur_page, offset, blank_png);
        return true;
    }

    if (strcmp(action, "$cmd.text_clear") == 0) {
        CmdEntry *ce = cmd_engine_find(g_cmd_engine, cur_page, pressed_item);
        if (!ce) return true;
        pthread_mutex_lock(&ce->mu);
        ce->last_text[0] = 0;
        pthread_mutex_unlock(&ce->mu);
        if (g_cmd_logs) cmd_log("text_clear btn=%d", btn);
        cmd_engine_notify(g_cmd_engine);
        cmd_apply_updates_current_page(opt, cfg, cur_page, offset, blank_png);
        return true;
    }

    if (strcmp(action, "$cmd.exec_stop") == 0) {
        CmdEntry *ce = cmd_engine_find(g_cmd_engine, cur_page, pressed_item);
        if (!ce) return true;
        pthread_mutex_lock(&ce->mu);
        ce->poll_gen++;
        ce->state_gen++;
        ce->poll_every_ms = 0;
        ce->state_every_ms = 0;
        ce->poll_running = false;
        ce->state_running = false;
        ce->next_poll_ns = 0;
        ce->next_state_ns = 0;
        ce->last_text[0] = 0;
        ce->last_state[0] = 0;
        pthread_mutex_unlock(&ce->mu);
        if (g_cmd_logs) cmd_log("exec_stop btn=%d", btn);
        cmd_engine_notify(g_cmd_engine);
        cmd_apply_updates_current_page(opt, cfg, cur_page, offset, blank_png);
        return true;
    }

    return true;
}

static int ha_call_from_item(const Options *opt, int ha_fd, const Item *it) {
    if (!opt || !it || !it->tap_action || !it->tap_action[0]) return -1;
    if (it->tap_action[0] == '$') return -1; // paging/internal
    if (!opt->ha_sock || !opt->ha_sock[0]) return -1;

    const char *action = it->tap_action;
    const char *dot = strchr(action, '.');
    if (!dot || dot == action || !dot[1]) return -1;

    char domain[64] = {0};
    char service[64] = {0};
    char json_buf[4096] = {0};
    const char *json = NULL;

    // Special case: "script.<entity>" means "call script turn_on {entity_id: script.<entity>}"
    // unless the suffix is a known service.
    if (strncmp(action, "script.", 7) == 0) {
        const char *suffix = dot + 1;
        if (strcmp(suffix, "turn_on") == 0 || strcmp(suffix, "turn_off") == 0 || strcmp(suffix, "toggle") == 0) {
            snprintf(domain, sizeof(domain), "script");
            snprintf(service, sizeof(service), "%s", suffix);
        } else {
            snprintf(domain, sizeof(domain), "script");
            snprintf(service, sizeof(service), "turn_on");
            snprintf(json_buf, sizeof(json_buf), "{\"entity_id\":\"%s\"}", action);
            json = json_buf;
        }
    }

    if (domain[0] == 0) {
        snprintf(domain, sizeof(domain), "%.*s", (int)(dot - action), action);
        snprintf(service, sizeof(service), "%s", dot + 1);
    }

    if (!json) {
        const char *data = (it->tap_data && it->tap_data[0]) ? it->tap_data : NULL;
        if (data && (data[0] == '{' || data[0] == '[')) {
            if (it->entity_id && it->entity_id[0] && data[0] == '{' && strstr(data, "\"entity_id\"") == NULL) {
                // Inject entity_id into an object.
                const char *inner = data + 1;
                while (*inner == ' ' || *inner == '\t' || *inner == '\n' || *inner == '\r') inner++;
                if (*inner == '}') {
                    snprintf(json_buf, sizeof(json_buf), "{\"entity_id\":\"%s\"}", it->entity_id);
                } else {
                    snprintf(json_buf, sizeof(json_buf), "{\"entity_id\":\"%s\",%s", it->entity_id, inner);
                }
                json = json_buf;
            } else {
                json = data;
            }
        } else if (it->entity_id && it->entity_id[0]) {
            snprintf(json_buf, sizeof(json_buf), "{\"entity_id\":\"%s\"}", it->entity_id);
            json = json_buf;
        } else {
            json = "{}";
        }
    }

    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "call %s %s %s", domain, service, json);
    // Keep paging logs short: only print the JSON payload for HA calls, as an "action" line above the button status.
    log_action("%s", json);

    // Prefer the persistent HA session socket if available.
    int fd = ha_fd;
    bool close_fd = false;
    if (fd < 0) {
        fd = unix_connect(opt->ha_sock);
        if (fd < 0) return -1;
        close_fd = true;
    }
    // Fire-and-forget: do not wait for ok/err to keep UI responsive. HA state changes will drive UI.
    int rc = ha_send_line_fd(fd, cmd);
    if (close_fd) close(fd);
    return (rc == 0) ? 0 : -1;
}

static int parse_sim_button_arg(const char *arg, ButtonEvent *evt_out, int *btn_out) {
    if (!arg || !evt_out || !btn_out) return -1;

    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == 0) return -1;

    char buf[64];
    snprintf(buf, sizeof(buf), "%s", arg);
    trim(buf);

    // Allow quotes: "TAP1" or 'TAP1'
    size_t n = strlen(buf);
    if (n >= 2 && ((buf[0] == '"' && buf[n - 1] == '"') || (buf[0] == '\'' && buf[n - 1] == '\''))) {
        buf[n - 1] = 0;
        memmove(buf, buf + 1, n - 1);
        trim(buf);
    }

    // Uppercase for comparison (ASCII).
    for (size_t i = 0; buf[i]; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 'a' + 'A');
    }

    const char *p = buf;
    ButtonEvent evt = BTN_EVT_UNKNOWN;
    size_t evt_len = 0;

    if (strncmp(p, "LONGHOLD", 8) == 0) {
        evt = BTN_EVT_LONGHOLD;
        evt_len = 8;
    } else if (strncmp(p, "RELEASED", 8) == 0) {
        evt = BTN_EVT_RELEASED;
        evt_len = 8;
    } else if (strncmp(p, "HOLD", 4) == 0) {
        evt = BTN_EVT_HOLD;
        evt_len = 4;
    } else if (strncmp(p, "TAP", 3) == 0) {
        evt = BTN_EVT_TAP;
        evt_len = 3;
    } else {
        return -1;
    }

    const char *num = p + evt_len;
    if (*num == 0) return -1;
    char *end = NULL;
    long v = strtol(num, &end, 10);
    if (!end || *end != 0) return -1;
    if (v < 1 || v > 14) return -1;

    *evt_out = evt;
    *btn_out = (int)v;
    return 0;
}

static void handle_button_event(const Options *opt,
                                Config *cfg,
                                char *blank_png,
                                int rb_fd,
                                size_t *inlen,
                                int btn,
                                ButtonEvent evt,
                                BrightnessState *br_state,
                                int *last_sent_brightness,
                                double *next_brightness_retry,
                                double *last_activity,
                                bool *control_enabled,
                                char *cur_page,
                                size_t cur_page_cap,
                                size_t *offset,
                                char page_stack[][256],
                                int page_stack_cap,
                                int *page_stack_len,
                                int *ha_fd,
                                char *ha_buf,
                                size_t *ha_len,
                                HaStateMap *ha_map,
                                HaSubs *ha_subs,
                                char *last_sig,
                                size_t last_sig_cap) {
    if (!opt || !cfg || !blank_png || !br_state || !last_sent_brightness || !next_brightness_retry || !last_activity || !control_enabled ||
        !cur_page || cur_page_cap == 0 || !offset || !page_stack_len || !ha_fd || !ha_buf || !ha_len || !ha_map || !ha_subs ||
        !last_sig || last_sig_cap == 0)
        return;
    if (btn < 1 || btn > 14) return;

    // Any button event counts as activity (even when stop-control).
    *last_activity = now_sec_monotonic();

    // Wake behavior: if screen is in sleep (brightness 0), any button wakes WITHOUT triggering actions.
    if (*br_state == BR_SLEEP) {
        int b = clamp_int(cfg->base_brightness, 0, 100);
        if (b != *last_sent_brightness) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "set-brightness %d", b);
            char reply[64] = {0};
            if (send_line_and_read_reply(opt->ulanzi_sock, cmd, reply, sizeof(reply)) == 0) {
                *last_sent_brightness = b;
            } else {
                *next_brightness_retry = now_sec_monotonic() + 1.0;
            }
        }
        *br_state = BR_NORMAL;
        return;
    }

    // If dimmed, restore base brightness but keep normal button handling.
    if (*br_state == BR_DIM) {
        int b = clamp_int(cfg->base_brightness, 0, 100);
        if (b != *last_sent_brightness) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "set-brightness %d", b);
            char reply[64] = {0};
            if (send_line_and_read_reply(opt->ulanzi_sock, cmd, reply, sizeof(reply)) == 0) {
                *last_sent_brightness = b;
            } else {
                *next_brightness_retry = now_sec_monotonic() + 1.0;
            }
        }
        *br_state = BR_NORMAL;
    }

    // Emergency resume: LONGHOLD 5s on button 14 forces start-control.
    if (btn == 14 && evt == BTN_EVT_LONGHOLD) {
        if (!*control_enabled) {
            log_msg("start-control (forced by button 14 LONGHOLD)");
            *control_enabled = true;
            last_sig[0] = 0; // force refresh
            render_and_send(opt, cfg, cur_page, *offset, ha_map, blank_png, last_sig, last_sig_cap);
            persist_last_page(opt, cur_page, *offset);
        }
        return;
    }

    if (!*control_enabled) return;

    const bool is_tap = (evt == BTN_EVT_TAP);
    const bool is_hold = (evt == BTN_EVT_HOLD);
    const bool is_longhold = (evt == BTN_EVT_LONGHOLD);
    const bool is_release = (evt == BTN_EVT_RELEASED);
    if (!(is_tap || is_hold || is_longhold || is_release)) return;

    // After a page transition, ignore any immediate follow-up events (including RELEASED)
    // to avoid triggering actions on the newly-entered page.
    {
        int64_t now = now_ns_monotonic();
        if (g_ignore_taps_until_ns > 0 && now < g_ignore_taps_until_ns) {
            return;
        }
    }

    // Action debounce: ignore rapid successive TAPs (avoid queuing renders).
    if (is_tap) {
        int ms = g_ulanzi_send_debounce_ms;
        if (ms > 0) {
            int64_t now = now_ns_monotonic();
            int64_t min_gap = (int64_t)ms * 1000000LL;
            if (g_last_action_ns > 0 && (now - g_last_action_ns) < min_gap) {
                return;
            }
            g_last_action_ns = now;
        }
    }

    const Page *p = config_get_page(cfg, cur_page);
    if (!p) {
        snprintf(cur_page, cur_page_cap, "%s", "$root");
        *offset = 0;
        p = config_get_page(cfg, cur_page);
    }
    if (!p) return;

    bool show_back = strcmp(cur_page, "$root") != 0;
    SheetLayout sheet = compute_sheet_layout(p->count, show_back, *offset);
    *offset = sheet.start;

    bool reserved_back = show_back;
    bool reserved_prev = sheet.show_prev;
    bool reserved_next = sheet.show_next;
    int back_pos = cfg->pos_back;
    int prev_pos = cfg->pos_prev;
    int next_pos = cfg->pos_next;

    // System button presses (TAP only)
    if (!is_tap) {
        // non-tap events never trigger nav system buttons
        goto content_buttons;
    }
    if (reserved_back && btn == back_pos) {
        char old_page[256];
        snprintf(old_page, sizeof(old_page), "%s", cur_page);
        bool changed = false;
        if (*page_stack_len > 0) {
            (*page_stack_len)--;
            snprintf(cur_page, cur_page_cap, "%s", page_stack[*page_stack_len]);
            changed = true;
        } else {
            const char *par = parent_page(cur_page);
            if (strcmp(par, cur_page) != 0) {
                snprintf(cur_page, cur_page_cap, "%s", par);
                changed = true;
            }
        }
        if (changed) {
            *offset = 0;
            if (g_cmd_engine) cmd_state_on_leave_page(g_cmd_engine, old_page);
            ha_enter_page(opt, cfg, cur_page, ha_fd, ha_buf, ha_len, ha_map, ha_subs);
            if (g_cmd_engine) cmd_state_on_enter_page(g_cmd_engine, cur_page);
            render_and_send(opt, cfg, cur_page, *offset, ha_map, blank_png, last_sig, last_sig_cap);
            persist_last_page(opt, cur_page, *offset);
            flush_pending_button_events(rb_fd, inlen, NULL);
        }
        return;
    }
    if (reserved_prev && btn == prev_pos) {
        *offset = sheet.prev_start;
        render_and_send(opt, cfg, cur_page, *offset, ha_map, blank_png, last_sig, last_sig_cap);
        persist_last_page(opt, cur_page, *offset);
        return;
    }
    if (reserved_next && btn == next_pos) {
        *offset = sheet.next_start;
        render_and_send(opt, cfg, cur_page, *offset, ha_map, blank_png, last_sig, last_sig_cap);
        persist_last_page(opt, cur_page, *offset);
        return;
    }

content_buttons:
    // Content button mapping: positions excluding reserved.
    size_t item_i = *offset;
    size_t pressed_item = (size_t)-1;
    for (int pos = 1; pos <= 13; pos++) {
        bool reserved = false;
        if (reserved_back && pos == back_pos) reserved = true;
        if (reserved_prev && pos == prev_pos) reserved = true;
        if (reserved_next && pos == next_pos) reserved = true;
        if (reserved) continue;
        if (item_i >= p->count) break;
        if (pos == btn) {
            pressed_item = item_i;
            break;
        }
        item_i++;
    }

    if (pressed_item == (size_t)-1) return;
    const Item *it = page_item_at(p, pressed_item);
    if (!it) return;

    ActionSeq tmp_seq;
    const ActionSeq *seq = item_action_seq_for_event(it, evt);
    if (!seq || seq->len == 0) {
        item_action_seq_ensure_legacy_single(it, evt, &tmp_seq);
        seq = &tmp_seq;
    } else {
        memset(&tmp_seq, 0, sizeof(tmp_seq));
    }

    for (size_t si = 0; seq && si < seq->len; si++) {
        const char *action = seq->steps[si].action;
        const char *data = seq->steps[si].data;
        if (!action || !action[0]) continue;

        if (is_action_goto(action) && data && data[0]) {
            if (g_cmd_engine) cmd_state_on_leave_page(g_cmd_engine, cur_page);
            if (*page_stack_len < page_stack_cap) {
                snprintf(page_stack[*page_stack_len], sizeof(page_stack[*page_stack_len]), "%s", cur_page);
                (*page_stack_len)++;
            }
            snprintf(cur_page, cur_page_cap, "%s", data);
            *offset = 0;
            ha_enter_page(opt, cfg, cur_page, ha_fd, ha_buf, ha_len, ha_map, ha_subs);
            if (g_cmd_engine) cmd_state_on_enter_page(g_cmd_engine, cur_page);
            render_and_send(opt, cfg, cur_page, *offset, ha_map, blank_png, last_sig, last_sig_cap);
            persist_last_page(opt, cur_page, *offset);
            flush_pending_button_events(rb_fd, inlen, NULL);
            action_seq_free(&tmp_seq);
            return;
        }

        if (strncmp(action, "$cmd.", 5) == 0) {
            Item tmp = *it;
            if (evt == BTN_EVT_TAP) tmp.tap_cmd_text = seq->steps[si].cmd_text;
            else if (evt == BTN_EVT_HOLD) tmp.hold_cmd_text = seq->steps[si].cmd_text;
            else if (evt == BTN_EVT_LONGHOLD) tmp.longhold_cmd_text = seq->steps[si].cmd_text;
            else if (evt == BTN_EVT_RELEASED) tmp.released_cmd_text = seq->steps[si].cmd_text;
            (void)handle_cmd_action(opt, cfg, cur_page, *offset, pressed_item, btn, evt, &tmp, action, data, blank_png);
            continue;
        }

        if (action[0] != '$') {
            Item tmp = *it;
            tmp.tap_action = (char *)action;
            tmp.tap_data = (char *)(data ? data : "");
            if (ha_call_from_item(opt, *ha_fd, &tmp) != 0) {
                log_msg("ha call failed (action='%s')", action ? action : "");
            }
            continue;
        }
    }
    action_seq_free(&tmp_seq);
	}

int main(int argc, char **argv) {
    Options opt;
    memset(&opt, 0, sizeof(opt));

    // Ensure we don't lose logs if the process crashes quickly.
    setvbuf(stderr, NULL, _IONBF, 0);

    // Broken pipe on socket write must not kill the daemon (device disconnects are expected).
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    (void)sigaction(SIGSEGV, &sa, NULL);
    (void)sigaction(SIGABRT, &sa, NULL);
    (void)sigaction(SIGBUS, &sa, NULL);
    (void)sigaction(SIGILL, &sa, NULL);
    (void)sigaction(SIGFPE, &sa, NULL);
    
    opt.config_path = xstrdup("config/configuration.yml");
    opt.ulanzi_sock = xstrdup("/tmp/ulanzi_device.sock");
    opt.control_sock = xstrdup("/tmp/goofydeck_paging_control.sock");
    opt.ha_sock = xstrdup("/tmp/goofydeck_ha.sock");
    opt.cache_root = xstrdup(".cache");
    opt.error_icon = xstrdup("assets/pregen/error.png");
    opt.sys_pregen_dir = xstrdup("assets/pregen");
    
    bool dump_config = false;

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) die_errno("getcwd");
    opt.root_dir = xstrdup(cwd);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            free(opt.config_path);
            opt.config_path = xstrdup(argv[++i]);
        } 
        else if (strcmp(argv[i], "--ulanzi-sock") == 0 && i + 1 < argc) {
            free(opt.ulanzi_sock);
            opt.ulanzi_sock = xstrdup(argv[++i]);
        } 
        else if (strcmp(argv[i], "--control-sock") == 0 && i + 1 < argc) {
            free(opt.control_sock);
            opt.control_sock = xstrdup(argv[++i]);
        } 
        else if (strcmp(argv[i], "--ha-sock") == 0 && i + 1 < argc) {
            free(opt.ha_sock);
            opt.ha_sock = xstrdup(argv[++i]);
        } 
        else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
            free(opt.cache_root);
            opt.cache_root = xstrdup(argv[++i]);
        } 
        else if (strcmp(argv[i], "--error-icon") == 0 && i + 1 < argc) {
            free(opt.error_icon);
            opt.error_icon = xstrdup(argv[++i]);
        } 
        else if (strcmp(argv[i], "--sys-pregen-dir") == 0 && i + 1 < argc) {
            free(opt.sys_pregen_dir);
            opt.sys_pregen_dir = xstrdup(argv[++i]);
        } 
        else if (strcmp(argv[i], "--dump-config") == 0) {
            dump_config = true;
        } 
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--config path] [--ulanzi-sock path] [--control-sock path] [--ha-sock path] [--cache dir]\n", argv[0]);
            return 0;
        } 
        else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    char *cfg_abs = resolve_path(opt.root_dir, opt.config_path);
    char *cache_abs = resolve_path(opt.root_dir, opt.cache_root);
    char *err_abs = resolve_path(opt.root_dir, opt.error_icon);
    char *sys_abs = resolve_path(opt.root_dir, opt.sys_pregen_dir);
    char *ctl_abs = resolve_path(opt.root_dir, opt.control_sock);
    char *ha_abs = resolve_path(opt.root_dir, opt.ha_sock);
    
    free(opt.config_path); opt.config_path = cfg_abs;
    free(opt.cache_root); opt.cache_root = cache_abs;
    free(opt.error_icon); opt.error_icon = err_abs;
    free(opt.sys_pregen_dir); opt.sys_pregen_dir = sys_abs;
    free(opt.control_sock); opt.control_sock = ctl_abs;
    free(opt.ha_sock); opt.ha_sock = ha_abs;

    ensure_dir(opt.cache_root);
    ensure_dir_parent(opt.error_icon);
    ensure_dir(opt.sys_pregen_dir);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    g_log_is_tty = isatty(STDERR_FILENO) ? 1 : 0;
    paging_apply_log_mode();

    wipe_paging_state_dir_at_startup(&opt);

    Config cfg;
    config_init_defaults(&cfg);
    
    if (load_config(opt.config_path, &cfg) != 0) die_errno("load_config");
    
    if (!config_get_page(&cfg, "$root")) {
        fprintf(stderr, "[pg] ERROR: config missing $root page\n");
        return 1;
    }

    (void)ulanzi_apply_default_label_style(&opt);
    
    if (dump_config) {
        fprintf(stderr, "[paging] dump-config: pages=%zu presets=%zu\n", cfg.page_count, cfg.preset_count);
        
        for (size_t i = 0; i < cfg.page_count; i++) {
            const Page *p = &cfg.pages[i];
            fprintf(stderr, "[paging] page '%s' items=%zu\n", p->name ? p->name : "<null>", p->count);
            
            for (size_t j = 0; j < p->count && j < 20; j++) {
                const Item *it = &p->items[j];
                fprintf(stderr, "  - name='%s' preset='%s' icon='%s' text='%s' action='%s' data='%s'\n",
                        it->name ? it->name : "",
                        it->preset ? it->preset : "",
                        it->icon ? it->icon : "",
                        it->text ? it->text : "",
                        it->tap_action ? it->tap_action : "",
                        it->tap_data ? it->tap_data : "");
            }
        }
        
        config_free(&cfg);
        free(opt.config_path);
        free(opt.ulanzi_sock);
        free(opt.cache_root);
        free(opt.error_icon);
        free(opt.sys_pregen_dir);
        free(opt.root_dir);
        return 0;
    }

    // Use a stable pre-generated empty icon when a button is undefined/empty.
    // If it's missing, create it once.
    char blank_png[PATH_MAX];
    snprintf(blank_png, sizeof(blank_png), "%s/assets/pregen/empty.png", opt.root_dir);
    if (!file_exists(blank_png)) {
        ensure_dir_parent(blank_png);
        // Prefer the C drawing binary (no Python, no shell scripts).
        char draw_square_bin[PATH_MAX];
        snprintf(draw_square_bin, sizeof(draw_square_bin), "%s/icons/draw_square", opt.root_dir);
        if (access(draw_square_bin, X_OK) == 0) {
            char *argv[] = { draw_square_bin, (char *)"transparent", (char *)"--size=196", blank_png, NULL };
            if (run_exec(argv) != 0) {
                (void)write_blank_png(blank_png, 196, 196);
            }
        } else {
            (void)write_blank_png(blank_png, 196, 196);
        }
    }
    if (!file_exists(blank_png)) {
        // fallback to error icon if empty cannot be created
        snprintf(blank_png, sizeof(blank_png), "%s", opt.error_icon);
    }

    // Best-effort pre-generation of all declared state icons at daemon start.
    precache_state_icons(&opt, &cfg);

    // Background command engine (polling + exec_text). Commands run even when their page isn't visible,
    // but we only render/send updates for the current page.
    CmdEngine cmd_engine;
    memset(&cmd_engine, 0, sizeof(cmd_engine));
    if (cmd_engine_init(&cmd_engine, &cfg) == 0) {
        cmd_engine_build_from_config(&cmd_engine, &cfg);
        if (cmd_engine_start(&cmd_engine) == 0) {
            g_cmd_engine = &cmd_engine;
        } else {
            cmd_engine_free(&cmd_engine);
        }
    }

    // Subscribe to button events.
    int rb_fd = unix_connect(opt.ulanzi_sock);
    if (rb_fd < 0) die_errno("connect ulanzi socket");
    (void)write(rb_fd, "read-buttons\n", 13);

    int rb_flags = fcntl(rb_fd, F_GETFL, 0);
    if (rb_flags >= 0) (void)fcntl(rb_fd, F_SETFL, rb_flags | O_NONBLOCK);

    int ctl_fd = make_unix_listen_socket(opt.control_sock);
    if (ctl_fd < 0) die_errno("control listen socket");
    log_msg("control socket: %s", opt.control_sock);

    // Home Assistant integration (optional; only used on pages with entity_id).
    int ha_fd = -1;
    char ha_buf[8192];
    size_t ha_len = 0;
    HaStateMap ha_map;
    memset(&ha_map, 0, sizeof(ha_map));
    HaSubs ha_subs;
    memset(&ha_subs, 0, sizeof(ha_subs));

    char cur_page[256] = "$root";
    size_t offset = 0;
    char last_sig[256] = {0};
    char page_stack[64][256];
    int page_stack_len = 0;
    bool control_enabled = true;

    // Brightness/sleep state machine (driven by config).
    BrightnessState br_state = BR_NORMAL;
    int last_sent_brightness = -1;
    double last_activity = now_sec_monotonic();
    double next_brightness_retry = 0.0;

    // Apply base brightness at start (best-effort).
    {
        char cmd[64];
        int b = clamp_int(cfg.base_brightness, 0, 100);
        snprintf(cmd, sizeof(cmd), "set-brightness %d", b);
        char reply[64] = {0};
        if (send_line_and_read_reply(opt.ulanzi_sock, cmd, reply, sizeof(reply)) == 0) {
            last_sent_brightness = b;
        } else {
            next_brightness_retry = now_sec_monotonic() + 1.0;
        }
    }

    // Initial HA subscriptions for $root (usually none).
    ha_enter_page(&opt, &cfg, cur_page, &ha_fd, ha_buf, &ha_len, &ha_map, &ha_subs);
    if (g_cmd_engine) cmd_state_on_enter_page(g_cmd_engine, cur_page);

    // Initial render once.
    render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
    persist_last_page(&opt, cur_page, offset);

    char inbuf[4096];
    size_t inlen = 0;

    bool prev_device_ready = g_ulanzi_device_ready;
    bool need_resync_on_reconnect = !g_ulanzi_device_ready;
    double next_device_probe = 0.0;

	    while (g_running) {
	        struct pollfd fds[4];
	        memset(fds, 0, sizeof(fds));
	        fds[0].fd = rb_fd;
	        fds[0].events = POLLIN;
	        fds[1].fd = ctl_fd;
	        fds[1].events = POLLIN;
	        fds[2].fd = ha_fd;
	        fds[2].events = (ha_fd >= 0) ? POLLIN : 0;
	        fds[3].fd = (g_cmd_engine && g_cmd_engine->notify_r >= 0) ? g_cmd_engine->notify_r : -1;
	        fds[3].events = (fds[3].fd >= 0) ? POLLIN : 0;

	        int pr = poll(fds, 4, 100);
	        if (pr < 0) {
	            if (errno == EINTR) continue;
	            die_errno("poll");
	        }

        // If the USB device disappears and comes back (USB reset), re-apply label style + current page.
        {
            double now = now_sec_monotonic();

            if (prev_device_ready && !g_ulanzi_device_ready) {
                log_msg("ulanzi device disconnected");
                need_resync_on_reconnect = true;
                last_sig[0] = 0; // force full resend on reconnect
            }

            if (!g_ulanzi_device_ready) {
                if (now >= next_device_probe) {
                    char reply[64] = {0};
                    int rc = send_line_and_read_reply(opt.ulanzi_sock, "ping", reply, sizeof(reply));
                    if (rc == 0) {
                        log_msg("ulanzi device reconnected");
                        if (need_resync_on_reconnect) {
                            (void)ulanzi_apply_default_label_style(&opt);
                            // Force brightness/page refresh to restore on-screen state.
                            last_sent_brightness = -1;
                            render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                            persist_last_page(&opt, cur_page, offset);
                            need_resync_on_reconnect = false;
                        }
                    }
                    next_device_probe = now + 0.5;
                }
            }

            prev_device_ready = g_ulanzi_device_ready;
        }

        // Idle brightness management (does not depend on control_enabled).
        {
            double now = now_sec_monotonic();
            double idle = now - last_activity;

            int desired_state = BR_NORMAL;
            int desired_brightness = clamp_int(cfg.base_brightness, 0, 100);
            if (cfg.sleep_timeout_sec > 0 && idle >= (double)cfg.sleep_timeout_sec) {
                desired_state = BR_SLEEP;
                desired_brightness = 0;
            } else if (cfg.sleep_dim_timeout_sec > 0 && idle >= (double)cfg.sleep_dim_timeout_sec) {
                desired_state = BR_DIM;
                desired_brightness = clamp_int(cfg.sleep_dim_brightness, 0, 100);
            }

            if (desired_brightness == last_sent_brightness) {
                br_state = desired_state;
            } else if (now >= next_brightness_retry) {
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "set-brightness %d", desired_brightness);
                char reply[64] = {0};
                if (send_line_and_read_reply(opt.ulanzi_sock, cmd, reply, sizeof(reply)) == 0) {
                    last_sent_brightness = desired_brightness;
                    br_state = desired_state;
                } else {
                    next_brightness_retry = now + 1.0;
                }
            }
        }

        // Control commands
        if (fds[1].revents & POLLIN) {
            for (;;) {
                int cfd = accept(ctl_fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                char cmdline[256] = {0};
                ssize_t n = read(cfd, cmdline, sizeof(cmdline) - 1);
                if (n < 0) n = 0;
                cmdline[n] = 0;
                trim(cmdline);
                if (cmdline[0]) log_msg("rx control: %s", cmdline);

                const char *resp = "ok\n";
                if (strcmp(cmdline, "stop-control") == 0) {
                    control_enabled = false;
                } else if (strcmp(cmdline, "start-control") == 0) {
                    control_enabled = true;
                } else if (strncmp(cmdline, "simule-button", 12) == 0 || strncmp(cmdline, "simulate-button", 15) == 0) {
                    const char *p = strchr(cmdline, ' ');
                    if (!p) {
                        resp = "err bad_args\n";
                    } else {
                        while (*p == ' ') p++;
                        ButtonEvent evt = BTN_EVT_UNKNOWN;
                        int btn = 0;
                        if (parse_sim_button_arg(p, &evt, &btn) != 0 || evt == BTN_EVT_UNKNOWN) {
                            resp = "err bad_args\n";
                        } else {
                            log_msg("simulate button %d %s", btn, button_event_name(evt));
                            handle_button_event(&opt, &cfg, blank_png, rb_fd, &inlen, btn, evt,
                                                &br_state, &last_sent_brightness, &next_brightness_retry, &last_activity,
                                                &control_enabled,
                                                cur_page, sizeof(cur_page), &offset,
                                                page_stack, (int)(sizeof(page_stack) / sizeof(page_stack[0])), &page_stack_len,
                                                &ha_fd, ha_buf, &ha_len, &ha_map, &ha_subs,
                                                last_sig, sizeof(last_sig));
                            resp = "ok\n";
                        }
                    }
                } else if (strcmp(cmdline, "load-last-page") == 0) {
                    char lp[256] = {0};
                    size_t lo = 0;
                    if (load_last_page(&opt, lp, sizeof(lp), &lo) == 0) {
                        if (config_get_page(&cfg, lp)) {
                            char old_page[256];
                            snprintf(old_page, sizeof(old_page), "%s", cur_page);
                            snprintf(cur_page, sizeof(cur_page), "%s", lp);
                            offset = lo;
                            last_sig[0] = 0; // force render
                            if (g_cmd_engine) cmd_state_on_leave_page(g_cmd_engine, old_page);
                            ha_enter_page(&opt, &cfg, cur_page, &ha_fd, ha_buf, &ha_len, &ha_map, &ha_subs);
                            if (g_cmd_engine) cmd_state_on_enter_page(g_cmd_engine, cur_page);
                            render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                            persist_last_page(&opt, cur_page, offset);
                        } else {
                            resp = "err\n";
                        }
                    } else {
                        resp = "err\n";
                    }
                } else if (cmdline[0] == 0) {
                    // ignore empty
                } else {
                    resp = "unknown\n";
                }
                (void)write(cfd, resp, strlen(resp));
                close(cfd);
            }
        }

        // HA events (push)
        if (ha_fd >= 0 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                char line[8192];
                int lr = read_line_from_fd(ha_fd, line, sizeof(line), ha_buf, &ha_len);
                if (lr == 1) {
                    if (strncmp(line, "evt state ", 10) == 0) {
                        const char *p = line + 10;
                        while (*p == ' ' || *p == '\t') p++;
                        char entity[256] = {0};
                        size_t w = 0;
                        while (*p && *p != ' ' && *p != '\t' && w + 1 < sizeof(entity)) entity[w++] = *p++;
                        entity[w] = 0;
                        ha_handle_line(&ha_map, line);
                        if (entity[0]) {
                            ha_partial_update_visible(&opt, &cfg, cur_page, offset, &ha_map, blank_png, entity);
                        }
                    } else {
                        ha_handle_line(&ha_map, line);
                    }
                    continue;
                }
                if (lr == 0) break;
                // disconnected
                close(ha_fd);
                ha_fd = -1;
                ha_len = 0;
                ha_subs_clear_no_unsub(&ha_subs);
                break;
            }
        }

        // Command updates (poll/exec_text): render/send only for current page.
        if (fds[3].revents & POLLIN) {
            char buf[256];
            for (;;) {
                ssize_t n = read(fds[3].fd, buf, sizeof(buf));
                if (n > 0) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                break;
            }
            if (g_cmd_loop_full_page_refresh) {
                // Force a full resend so command-driven text/state changes are reflected without partial updates.
                last_sig[0] = 0;
                render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
            } else {
                cmd_apply_updates_current_page(&opt, &cfg, cur_page, offset, blank_png);
            }
        }

        // Ulanzi events
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char tmp[512];
            ssize_t n = read(rb_fd, tmp, sizeof(tmp));
            if (n == 0) break;
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
                n = 0;
            }
            if (n > 0) {
                if (inlen + (size_t)n > sizeof(inbuf)) {
                    // drop buffer on overflow (shouldn't happen)
                    inlen = 0;
                }
                memcpy(inbuf + inlen, tmp, (size_t)n);
                inlen += (size_t)n;
            }

            size_t start = 0;
            for (;;) {
                void *nlp = memchr(inbuf + start, '\n', inlen - start);
                if (!nlp) break;
                size_t nl = (size_t)((char *)nlp - (inbuf + start));
                char evline[256];
                size_t cpy = nl;
                if (cpy >= sizeof(evline)) cpy = sizeof(evline) - 1;
                memcpy(evline, inbuf + start, cpy);
                evline[cpy] = 0;
                start += nl + 1;

                rtrim_only(evline);
                if (strncmp(evline, "button ", 7) == 0) {
                    log_status("rx ulanzi: %s", evline);
                } else {
                    log_msg("rx ulanzi: %s", evline);
                }
                trim(evline);
                if (evline[0] == 0) continue;
                if (strcmp(evline, "ok") == 0) continue;

                if (strncmp(evline, "evt ", 4) == 0) {
                    if (strcmp(evline, "evt disconnected") == 0) {
                        if (g_ulanzi_device_ready) {
                            log_msg("ulanzi device disconnected");
                        }
                        g_ulanzi_device_ready = false;
                        need_resync_on_reconnect = true;
                        last_sig[0] = 0; // force full resend on reconnect
                    } else if (strcmp(evline, "evt connected") == 0) {
                        log_msg("ulanzi device reconnected");
                        g_ulanzi_device_ready = true;
                        if (need_resync_on_reconnect) {
                            (void)ulanzi_apply_default_label_style(&opt);
                            last_sent_brightness = -1;
                            render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                            persist_last_page(&opt, cur_page, offset);
                            need_resync_on_reconnect = !g_ulanzi_device_ready;
                        }
                    }
                    continue;
                }

                int btn = 0;
                char evt_word[32] = {0};
                if (sscanf(evline, "button %d %31s", &btn, evt_word) != 2) continue;
                ButtonEvent evt = parse_button_event_word(evt_word);
                if (evt == BTN_EVT_UNKNOWN) continue;

                // Any button event counts as activity (even when stop-control).
                last_activity = now_sec_monotonic();

                // Wake behavior: if screen is in sleep (brightness 0), any button wakes WITHOUT triggering actions.
                if (br_state == BR_SLEEP) {
                    int b = clamp_int(cfg.base_brightness, 0, 100);
                    if (b != last_sent_brightness) {
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "set-brightness %d", b);
                        char reply[64] = {0};
                        if (send_line_and_read_reply(opt.ulanzi_sock, cmd, reply, sizeof(reply)) == 0) {
                            last_sent_brightness = b;
                        } else {
                            next_brightness_retry = now_sec_monotonic() + 1.0;
                        }
                    }
                    br_state = BR_NORMAL;
                    continue;
                }

                // If dimmed, restore base brightness but keep normal button handling.
                if (br_state == BR_DIM) {
                    int b = clamp_int(cfg.base_brightness, 0, 100);
                    if (b != last_sent_brightness) {
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "set-brightness %d", b);
                        char reply[64] = {0};
                        if (send_line_and_read_reply(opt.ulanzi_sock, cmd, reply, sizeof(reply)) == 0) {
                            last_sent_brightness = b;
                        } else {
                            next_brightness_retry = now_sec_monotonic() + 1.0;
                        }
                    }
                    br_state = BR_NORMAL;
                }

                // Emergency resume: LONGHOLD 5s on button 14 forces start-control.
                if (btn == 14 && evt == BTN_EVT_LONGHOLD) {
                    if (!control_enabled) {
                        log_msg("start-control (forced by button 14 LONGHOLD)");
                        control_enabled = true;
                        last_sig[0] = 0; // force refresh
                        render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                        persist_last_page(&opt, cur_page, offset);
                    }
                    continue;
                }

                if (!control_enabled) continue;

                const bool is_tap = (evt == BTN_EVT_TAP);
                const bool is_hold = (evt == BTN_EVT_HOLD);
                const bool is_longhold = (evt == BTN_EVT_LONGHOLD);
                const bool is_release = (evt == BTN_EVT_RELEASED);
                if (!(is_tap || is_hold || is_longhold || is_release)) continue;

                // After a page transition, ignore any immediate follow-up events (including RELEASED)
                // to avoid triggering actions on the newly-entered page.
                {
                    int64_t now = now_ns_monotonic();
                    if (g_ignore_taps_until_ns > 0 && now < g_ignore_taps_until_ns) {
                        continue;
                    }
                }

                // Action debounce: ignore rapid successive TAPs (avoid queuing renders).
                if (is_tap) {
                    int ms = g_ulanzi_send_debounce_ms;
                    if (ms > 0) {
                        int64_t now = now_ns_monotonic();
                        int64_t min_gap = (int64_t)ms * 1000000LL;
                        if (g_last_action_ns > 0 && (now - g_last_action_ns) < min_gap) {
                            continue;
                        }
                        g_last_action_ns = now;
                    }
                }

                const Page *p = config_get_page(&cfg, cur_page);
                if (!p) { snprintf(cur_page, sizeof(cur_page), "$root"); offset = 0; p = config_get_page(&cfg, cur_page); }
                bool show_back = strcmp(cur_page, "$root") != 0;
                SheetLayout sheet = compute_sheet_layout(p->count, show_back, offset);
                offset = sheet.start;

                bool reserved_back = show_back;
                bool reserved_prev = sheet.show_prev;
                bool reserved_next = sheet.show_next;
                int back_pos = cfg.pos_back;
                int prev_pos = cfg.pos_prev;
                int next_pos = cfg.pos_next;

                // System button presses (TAP only)
                if (!is_tap) goto content_buttons;
                if (reserved_back && btn == back_pos) {
                    char old_page[256];
                    snprintf(old_page, sizeof(old_page), "%s", cur_page);
                    bool changed = false;
                    if (page_stack_len > 0) {
                        page_stack_len--;
                        snprintf(cur_page, sizeof(cur_page), "%s", page_stack[page_stack_len]);
                        changed = true;
                    } else {
                        // Legacy fallback: parent by path segment.
                        const char *par = parent_page(cur_page);
                        if (strcmp(par, cur_page) != 0) {
                            snprintf(cur_page, sizeof(cur_page), "%s", par);
                            changed = true;
                        }
                    }
                    if (changed) {
                        offset = 0;
                        if (g_cmd_engine) cmd_state_on_leave_page(g_cmd_engine, old_page);
                        ha_enter_page(&opt, &cfg, cur_page, &ha_fd, ha_buf, &ha_len, &ha_map, &ha_subs);
                        if (g_cmd_engine) cmd_state_on_enter_page(g_cmd_engine, cur_page);
                        render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                        persist_last_page(&opt, cur_page, offset);
                        flush_pending_button_events(rb_fd, &inlen, &start);
                        goto parse_done;
                    }
                    continue;
                }
                if (reserved_prev && btn == prev_pos) {
                    offset = sheet.prev_start;
                    render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                    persist_last_page(&opt, cur_page, offset);
                    continue;
                }
                if (reserved_next && btn == next_pos) {
                    offset = sheet.next_start;
                    render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                    persist_last_page(&opt, cur_page, offset);
                    continue;
                }

content_buttons:
                // Content button mapping: positions excluding reserved.
                size_t item_i = offset;
                size_t pressed_item = (size_t)-1;
                for (int pos = 1; pos <= 13; pos++) {
                    bool reserved = false;
                    if (reserved_back && pos == back_pos) reserved = true;
                    if (reserved_prev && pos == prev_pos) reserved = true;
                    if (reserved_next && pos == next_pos) reserved = true;
                    if (reserved) continue;
                    if (item_i >= p->count) break;
                    if (pos == btn) { pressed_item = item_i; break; }
                    item_i++;
                }

                if (pressed_item != (size_t)-1) {
                    const Item *it = page_item_at(p, pressed_item);
                    if (!it) continue;

                    ActionSeq tmp_seq;
                    const ActionSeq *seq = item_action_seq_for_event(it, evt);
                    if (!seq || seq->len == 0) {
                        item_action_seq_ensure_legacy_single(it, evt, &tmp_seq);
                        seq = &tmp_seq;
                    } else {
                        memset(&tmp_seq, 0, sizeof(tmp_seq));
                    }

                    for (size_t si = 0; seq && si < seq->len; si++) {
                        const char *action = seq->steps[si].action;
                        const char *data = seq->steps[si].data;
                        if (!action || !action[0]) continue;

                        if (is_action_goto(action) && data && data[0]) {
                            if (page_stack_len < (int)(sizeof(page_stack) / sizeof(page_stack[0]))) {
                                snprintf(page_stack[page_stack_len], sizeof(page_stack[page_stack_len]), "%s", cur_page);
                                page_stack_len++;
                            }
                            char old_page[256];
                            snprintf(old_page, sizeof(old_page), "%s", cur_page);
                            snprintf(cur_page, sizeof(cur_page), "%s", data);
                            offset = 0;
                            if (g_cmd_engine) cmd_state_on_leave_page(g_cmd_engine, old_page);
                            ha_enter_page(&opt, &cfg, cur_page, &ha_fd, ha_buf, &ha_len, &ha_map, &ha_subs);
                            if (g_cmd_engine) cmd_state_on_enter_page(g_cmd_engine, cur_page);
                            render_and_send(&opt, &cfg, cur_page, offset, &ha_map, blank_png, last_sig, sizeof(last_sig));
                            persist_last_page(&opt, cur_page, offset);
                            flush_pending_button_events(rb_fd, &inlen, &start);
                            action_seq_free(&tmp_seq);
                            goto parse_done;
                        }

                        if (strncmp(action, "$cmd.", 5) == 0) {
                            Item tmp = *it;
                            if (evt == BTN_EVT_TAP) tmp.tap_cmd_text = seq->steps[si].cmd_text;
                            else if (evt == BTN_EVT_HOLD) tmp.hold_cmd_text = seq->steps[si].cmd_text;
                            else if (evt == BTN_EVT_LONGHOLD) tmp.longhold_cmd_text = seq->steps[si].cmd_text;
                            else if (evt == BTN_EVT_RELEASED) tmp.released_cmd_text = seq->steps[si].cmd_text;
                            (void)handle_cmd_action(&opt, &cfg, cur_page, offset, pressed_item, btn, evt, &tmp, action, data, blank_png);
                            continue;
                        }

                        if (action[0] != '$') {
                            // Home Assistant call (domain.service or script.<entity> shortcut).
                            Item tmp = *it;
                            tmp.tap_action = (char *)action;
                            tmp.tap_data = (char *)(data ? data : "");
                            if (ha_call_from_item(&opt, ha_fd, &tmp) != 0) {
                                log_msg("ha call failed (action='%s')", action ? action : "");
                            }
                            continue;
                        }
                    }
                    action_seq_free(&tmp_seq);
                }
            }

parse_done:
            if (start > 0 && start < inlen) {
                memmove(inbuf, inbuf + start, inlen - start);
                inlen -= start;
            } else if (start >= inlen) {
                inlen = 0;
            }
        }
    }

    close(rb_fd);
    close(ctl_fd);
    unlink(opt.control_sock);
    // blank_png points to a shared, persistent asset (assets/pregen/empty.png or error.png fallback).
    // Do not unlink it.
    ha_unsubscribe_all(&ha_fd, ha_buf, &ha_len, &ha_map, &ha_subs);
    if (ha_fd >= 0) close(ha_fd);
    ha_state_map_free(&ha_map);
    ha_subs_free(&ha_subs);
    if (g_cmd_engine) {
        cmd_engine_free(g_cmd_engine);
        g_cmd_engine = NULL;
    }
    config_free(&cfg);
    free(opt.config_path);
    free(opt.ulanzi_sock);
    free(opt.control_sock);
    free(opt.ha_sock);
    free(opt.cache_root);
    free(opt.error_icon);
    free(opt.sys_pregen_dir);
    free(opt.root_dir);
    return 0;
}
