// Single paging daemon for GoofyDeck (no Python).
//
// Responsibilities:
// - Connect to ulanzi_d200_demon unix socket (/tmp/ulanzi_device.sock)
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
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>
#include <png.h>

typedef struct {
    char *name;
    char *icon;
    char *preset;
    char *tap_action;
    char *tap_data;
} Item;

typedef struct {
    char *name;
    char *icon_background_color; // "RRGGBB" or "transparent"
    int icon_border_radius;      // percent (0..50)
    int icon_size;               // px (1..196)
    char *icon_color;            // "RRGGBB" or "transparent"
    char *text_color;            // "RRGGBB" or "transparent"
    int text_size;               // px
} Preset;

typedef struct {
    char *name;
    Item *items;
    size_t count;
    size_t cap;
} Page;

typedef struct {
    int pos_back;
    int pos_prev;
    int pos_next;
    Preset *presets;
    size_t preset_count;
    size_t preset_cap;
    Page *pages;
    size_t page_count;
    size_t page_cap;
} Config;

typedef struct {
    char *config_path;
    char *ulanzi_sock;
    char *cache_root;
    char *error_icon;
    char *sys_pregen_dir;
    char *root_dir;
} Options;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void die_errno(const char *msg) {
    fprintf(stderr, "[pagging] ERROR: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[pagging] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
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

static int indent_of(const char *s) {
    int i = 0;
    while (s[i] == ' ') i++;
    return i;
}

static void strip_inline_comment(char *s) {
    // Simple heuristic: cut at " #".
    for (size_t i = 1; s[i]; i++) {
        if (s[i] == '#' && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
            s[i - 1] = 0;
            break;
        }
    }
    rtrim_only(s); // keep indentation for YAML parsing
}

static char *strip_quotes_dup(const char *s) {
    if (!s) return xstrdup("");
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        char *out = malloc(n - 1);
        if (!out) die_errno("malloc");
        memcpy(out, s + 1, n - 2);
        out[n - 2] = 0;
        return out;
    }
    char *out = malloc(n + 1);
    if (!out) die_errno("malloc");
    memcpy(out, s, n);
    out[n] = 0;
    return out;
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
        if (errno == ENOENT) {
            fprintf(stderr, "[pagging] ERROR: ulanzi socket not found: %s (is ulanzi_d200_demon running?)\n", sock_path);
        }
        close(fd);
        return -1;
    }
    return fd;
}

static int send_line_and_read_reply(const char *sock_path, const char *line, char *reply, size_t reply_cap) {
    int fd = unix_connect(sock_path);
    if (fd < 0) return -1;
    size_t n = strlen(line);
    if (write(fd, line, n) != (ssize_t)n) {
        close(fd);
        return -1;
    }
    if (n == 0 || line[n - 1] != '\n') (void)write(fd, "\n", 1);
    ssize_t r = read(fd, reply, reply_cap - 1);
    if (r < 0) r = 0;
    reply[r] = 0;
    trim(reply);
    close(fd);
    return 0;
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

static int run_exec(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128;
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
}

static void preset_init_defaults(Preset *p, const char *name) {
    memset(p, 0, sizeof(*p));
    p->name = xstrdup(name ? name : "default");
    p->icon_background_color = xstrdup("241f31");
    p->icon_border_radius = 12;
    p->icon_size = 128;
    p->icon_color = xstrdup("FFFFFF");
    p->text_color = xstrdup("FFFFFF");
    p->text_size = 16;
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
        free(p->icon_background_color);
        free(p->icon_color);
        free(p->text_color);
    }
    free(cfg->presets);

    for (size_t i = 0; i < cfg->page_count; i++) {
        Page *p = &cfg->pages[i];
        for (size_t j = 0; j < p->count; j++) {
            free(p->items[j].name);
            free(p->items[j].icon);
            free(p->items[j].preset);
            free(p->items[j].tap_action);
            free(p->items[j].tap_data);
        }
        free(p->items);
        free(p->name);
    }
    free(cfg->pages);
    memset(cfg, 0, sizeof(*cfg));
}

static int load_config(const char *path, Config *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    Config cfg;
    config_init_defaults(&cfg);
    if (!config_get_preset_mut(&cfg, "default")) (void)config_add_preset(&cfg, "default");

    char *line = NULL;
    size_t cap = 0;
    char section[32] = {0};
    char cur_page[256] = {0};
    char cur_preset[128] = {0};
    bool in_buttons = false;
    Item cur_item = {0};
    bool have_item = false;
    bool in_tap_action = false;
    int tap_action_indent = 0;

    while (getline(&line, &cap, f) != -1) {
        strip_inline_comment(line);
        if (line[0] == 0) continue;
        int ind = indent_of(line);
        char *t = line;
        trim(t);
        if (t[0] == 0) continue;

        if (strcmp(t, "system_buttons:") == 0) {
            snprintf(section, sizeof(section), "system_buttons");
            continue;
        }
        if (strcmp(t, "presets:") == 0) {
            snprintf(section, sizeof(section), "presets");
            continue;
        }
        if (strcmp(t, "pages:") == 0) {
            snprintf(section, sizeof(section), "pages");
            continue;
        }

        if (strcmp(section, "system_buttons") == 0) {
            //   $page.back:
            //     position: 11
            if (ind == 2 && t[strlen(t) - 1] == ':') {
                // store current key in cur_page buffer temporarily
                snprintf(cur_page, sizeof(cur_page), "%.*s", (int)strlen(t) - 1, t);
                continue;
            }
            if (ind >= 4 && strncmp(t, "position:", 9) == 0) {
                int pos = atoi(t + 9);
                if (strcmp(cur_page, "$page.back") == 0) cfg.pos_back = pos;
                if (strcmp(cur_page, "$page.previous") == 0) cfg.pos_prev = pos;
                if (strcmp(cur_page, "$page.next") == 0) cfg.pos_next = pos;
                continue;
            }
        }

        if (strcmp(section, "pages") == 0) {
            if (ind == 2 && t[strlen(t) - 1] == ':') {
                snprintf(cur_page, sizeof(cur_page), "%.*s", (int)strlen(t) - 1, t);
                if (!config_get_page(&cfg, cur_page)) config_add_page(&cfg, cur_page);
                in_buttons = false;
                in_tap_action = false;
                tap_action_indent = 0;
                continue;
            }
            if (ind == 4 && strcmp(t, "buttons:") == 0) {
                in_buttons = true;
                in_tap_action = false;
                tap_action_indent = 0;
                continue;
            }
            if (!in_buttons || cur_page[0] == 0) continue;

            // Start of item: "- name: ..."
            if (ind == 6 && t[0] == '-' && (t[1] == ' ' || t[1] == 0)) {
                if (have_item) {
                    Page *p = config_get_page(&cfg, cur_page);
                    if (p) page_add_item(p, cur_item);
                    memset(&cur_item, 0, sizeof(cur_item));
                    have_item = false;
                }
                have_item = true;
                in_tap_action = false;
                tap_action_indent = 0;

                // inline field after "- "
                const char *rest = t + 1;
                while (*rest == ' ') rest++;
                if (strncmp(rest, "name:", 5) == 0) {
                    cur_item.name = strip_quotes_dup(rest + 5);
                } else if (strncmp(rest, "icon:", 5) == 0) {
                    cur_item.icon = strip_quotes_dup(rest + 5);
                }
                continue;
            }

            if (!have_item) continue;

            // Tap action block
            if (ind == 8 && strcmp(t, "tap_action:") == 0) {
                in_tap_action = true;
                tap_action_indent = ind;
                continue;
            }
            if (in_tap_action && ind <= tap_action_indent) {
                in_tap_action = false;
            }

            if (ind == 8 && strncmp(t, "name:", 5) == 0) { free(cur_item.name); cur_item.name = strip_quotes_dup(t + 5); continue; }
            if (ind == 8 && strncmp(t, "icon:", 5) == 0) { free(cur_item.icon); cur_item.icon = strip_quotes_dup(t + 5); continue; }
            if (ind == 8 && strncmp(t, "presets:", 8) == 0) {
                // presets: [default, ...]  -> keep first preset name
                const char *s = t + 8;
                while (*s == ' ' || *s == '\t') s++;
                while (*s == '[' || *s == ' ' || *s == '\t') s++;
                char first[128] = {0};
                size_t k = 0;
                while (*s && *s != ']' && *s != ',' && k + 1 < sizeof(first)) {
                    if (*s != ' ' && *s != '\t') first[k++] = *s;
                    s++;
                }
                first[k] = 0;
                if (first[0]) { free(cur_item.preset); cur_item.preset = xstrdup(first); }
                continue;
            }

            if (in_tap_action) {
                if (ind == 10 && strncmp(t, "action:", 7) == 0) { free(cur_item.tap_action); cur_item.tap_action = strip_quotes_dup(t + 7); continue; }
                if (ind == 10 && strncmp(t, "data:", 5) == 0) { free(cur_item.tap_data); cur_item.tap_data = strip_quotes_dup(t + 5); continue; }
                if (ind == 12 && strncmp(t, "data:", 5) == 0) { free(cur_item.tap_data); cur_item.tap_data = strip_quotes_dup(t + 5); continue; }
            }
        }

        if (strcmp(section, "presets") == 0) {
            //   default:
            //     icon_background_color: "241f31"
            //     icon_border_radius: 12
            if (ind == 2 && t[strlen(t) - 1] == ':') {
                snprintf(cur_preset, sizeof(cur_preset), "%.*s", (int)strlen(t) - 1, t);
                if (!config_get_preset_mut(&cfg, cur_preset)) config_add_preset(&cfg, cur_preset);
                continue;
            }
            if (cur_preset[0] == 0) continue;
            Preset *p = config_get_preset_mut(&cfg, cur_preset);
            if (!p) continue;
            if (ind >= 4 && strncmp(t, "icon_background_color:", 22) == 0) {
                free(p->icon_background_color);
                p->icon_background_color = strip_quotes_dup(t + 22);
                continue;
            }
            if (ind >= 4 && strncmp(t, "icon_border_radius:", 19) == 0) {
                p->icon_border_radius = atoi(t + 19);
                continue;
            }
            if (ind >= 4 && strncmp(t, "icon_size:", 10) == 0) {
                p->icon_size = atoi(t + 10);
                continue;
            }
            if (ind >= 4 && strncmp(t, "icon_color:", 11) == 0) {
                free(p->icon_color);
                p->icon_color = strip_quotes_dup(t + 11);
                continue;
            }
            if (ind >= 4 && strncmp(t, "text_color:", 11) == 0) {
                free(p->text_color);
                p->text_color = strip_quotes_dup(t + 11);
                continue;
            }
            if (ind >= 4 && strncmp(t, "text_size:", 10) == 0) {
                p->text_size = atoi(t + 10);
                continue;
            }
        }
    }

    if (have_item && cur_page[0]) {
        Page *p = config_get_page(&cfg, cur_page);
        if (p) page_add_item(p, cur_item);
        else {
            free(cur_item.name);
            free(cur_item.icon);
            free(cur_item.preset);
            free(cur_item.tap_action);
            free(cur_item.tap_data);
        }
    }

    free(line);
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

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int generate_icon_with_preset(const Options *opt, const Preset *preset,
                                     const char *out_png, const char *name, const char *icon_spec) {
    // Base: blank 196x196 (overwrite)
    ensure_dir_parent(out_png);
    if (write_blank_png(out_png, 196, 196) != 0) return -1;

    // Background (rounded square) if configured
    const char *bg = (preset && preset->icon_background_color) ? preset->icon_background_color : NULL;
    if (bg && bg[0] && strcasecmp(bg, "transparent") != 0) {
        int rad = preset ? clamp_int(preset->icon_border_radius, 0, 50) : 0;
        char draw_border_bin[PATH_MAX];
        snprintf(draw_border_bin, sizeof(draw_border_bin), "%s/icons/draw_border", opt->root_dir);
        char size_arg[64];
        char rad_arg[64];
        snprintf(size_arg, sizeof(size_arg), "--size=196");
        snprintf(rad_arg, sizeof(rad_arg), "--radius=%d", rad);
        if (access(draw_border_bin, X_OK) != 0) return -1;
        char *argv[] = { draw_border_bin, (char *)bg, size_arg, rad_arg, (char *)out_png, NULL };
        int rc = run_exec(argv);
        if (rc != 0) return -1;
    }

    if (icon_spec && strncmp(icon_spec, "mdi:", 4) == 0) {
        if (ensure_mdi_svg(opt, icon_spec) != 0) return -1;
        char draw_mdi_bin[PATH_MAX];
        snprintf(draw_mdi_bin, sizeof(draw_mdi_bin), "%s/icons/draw_mdi", opt->root_dir);

        const char *ic_color = (preset && preset->icon_color && preset->icon_color[0]) ? preset->icon_color : "FFFFFF";
        int ic_size = preset ? clamp_int(preset->icon_size, 1, 196) : 128;
        char size_arg[64];
        snprintf(size_arg, sizeof(size_arg), "--size=%d", ic_size);

        if (access(draw_mdi_bin, X_OK) != 0) return -1;
        char *argv[] = { draw_mdi_bin, (char *)icon_spec, (char *)ic_color, size_arg, (char *)out_png, NULL };
        int rc = run_exec(argv);
        if (rc != 0) return -1;
    }

    if (name && name[0]) {
        char draw_text_bin[PATH_MAX];
        snprintf(draw_text_bin, sizeof(draw_text_bin), "%s/icons/draw_text", opt->root_dir);
        char text_arg[512];
        snprintf(text_arg, sizeof(text_arg), "--text=%s", name);
        const char *tc = (preset && preset->text_color && preset->text_color[0]) ? preset->text_color : "FFFFFF";
        int ts = preset ? clamp_int(preset->text_size, 1, 64) : 16;
        char tc_arg[64];
        char ts_arg[64];
        snprintf(tc_arg, sizeof(tc_arg), "--text_color=%s", tc);
        snprintf(ts_arg, sizeof(ts_arg), "--text_size=%d", ts);
        if (access(draw_text_bin, X_OK) != 0) return -1;
        char *argv[] = { draw_text_bin, text_arg, tc_arg, (char *)"--text_align=bottom", ts_arg, (char *)out_png, NULL };
        int rc = run_exec(argv);
        if (rc != 0) return -1;
    }

    return 0;
}

static int read_hex_u32_file(const char *path, uint32_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = 0;
    trim(buf);
    uint32_t v = 0;
    if (sscanf(buf, "%x", &v) != 1) return -1;
    *out = v;
    return 0;
}

static void write_hex_u32_file(const char *path, uint32_t v) {
    ensure_dir_parent(path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%08x\n", v);
    fclose(f);
}

static bool cached_or_generated_into(const Options *opt, const Config *cfg, const char *page, size_t item_index, const Item *it,
                                     char *out_path, size_t out_cap) {
    if (!it) return false;
    const char *nm = it->name ? it->name : "";
    const char *ic = it->icon ? it->icon : "";
    if (nm[0] == 0 && ic[0] == 0) return false; // empty => no cache

    const char *pr = it->preset ? it->preset : "";
    char key[1024];
    snprintf(key, sizeof(key), "%s\n%zu\n%s\n%s\n%s\n%s\n%s\n", page, item_index, nm, ic, pr,
             it->tap_action ? it->tap_action : "", it->tap_data ? it->tap_data : "");
    uint32_t h = fnv1a32(key, strlen(key));

    if (page && strcmp(page, "_sys") == 0) {
        const char *sys_name = "sys";
        if (item_index == 1000) sys_name = "page_back";
        else if (item_index == 1001) sys_name = "page_prev";
        else if (item_index == 1002) sys_name = "page_next";
        snprintf(out_path, out_cap, "%s/%s.png", opt->sys_pregen_dir, sys_name);
        char meta[PATH_MAX];
        snprintf(meta, sizeof(meta), "%s/%s.meta", opt->sys_pregen_dir, sys_name);
        uint32_t prev = 0;
        if (file_exists(out_path) && read_hex_u32_file(meta, &prev) == 0 && prev == h) return true;
        const Preset *preset = config_get_preset(cfg, pr);
        if (!preset) preset = config_get_preset(cfg, "default");
        if (generate_icon_with_preset(opt, preset, out_path, nm, ic) != 0) {
            (void)copy_file(opt->error_icon, out_path);
        }
        write_hex_u32_file(meta, h);
        return true;
    } else {
        snprintf(out_path, out_cap, "%s/%s/item%zu_%08x.png", opt->cache_root, page, item_index + 1, h);
    }

    if (file_exists(out_path)) return true;

    const Preset *preset = config_get_preset(cfg, pr);
    if (!preset) preset = config_get_preset(cfg, "default");
    if (generate_icon_with_preset(opt, preset, out_path, nm, ic) != 0) {
        (void)copy_file(opt->error_icon, out_path);
    }
    return true;
}

static const Item *page_item_at(const Page *p, size_t idx) {
    if (!p || idx >= p->count) return NULL;
    return &p->items[idx];
}

static bool is_action_goto(const char *a) {
    if (!a) return false;
    return strcmp(a, "$page.go_to") == 0 || strcmp(a, "$page.goto") == 0 || strcmp(a, "$page_goto") == 0;
}

static void render_and_send(const Options *opt, const Config *cfg, const char *page_name, size_t offset,
                            char *blank_png, char *last_sig, size_t last_sig_cap) {
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
    size_t item_slots = base_item_slots - (need_pagination ? 2u : 0u);
    if (item_slots == 0) item_slots = 1;
    size_t max_offset = 0;
    if (p->count > 0) {
        max_offset = ((p->count - 1) / item_slots) * item_slots;
    }
    if (offset > max_offset) offset = max_offset;

    bool show_prev = need_pagination && offset > 0;
    bool show_next = need_pagination && offset < max_offset;

    char sig[256];
    snprintf(sig, sizeof(sig), "%s|%zu|%zu|%d|%d|%d|%d|%d", page_name, offset, item_slots,
             show_back ? 1 : 0, need_pagination ? 1 : 0, show_prev ? 1 : 0, show_next ? 1 : 0, (int)p->count);
    if (strncmp(sig, last_sig, last_sig_cap) == 0) {
        return;
    }
    snprintf(last_sig, last_sig_cap, "%s", sig);

    log_msg("render page='%s' offset=%zu slots=%zu items=%zu", page_name, offset, item_slots, p->count);

    char btn_path[14][PATH_MAX];
    bool btn_set[14] = {0};
    for (int i = 1; i <= 13; i++) {
        snprintf(btn_path[i], sizeof(btn_path[i]), "%s", blank_png);
        btn_set[i] = true;
    }

    // Reserve back/prev/next
    bool reserved[14] = {0};
    if (show_back && back_pos >= 1 && back_pos <= 13) reserved[back_pos] = true;
    if (need_pagination && prev_pos >= 1 && prev_pos <= 13) reserved[prev_pos] = true;
    if (need_pagination && next_pos >= 1 && next_pos <= 13) reserved[next_pos] = true;

    // Fill items
    size_t item_i = offset;
    for (int pos = 1; pos <= 13 && item_i < p->count; pos++) {
        if (reserved[pos]) continue;
        const Item *it = page_item_at(p, item_i);
        char tmp[PATH_MAX];
        if (cached_or_generated_into(opt, cfg, page_name, item_i, it, tmp, sizeof(tmp))) {
            snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", tmp);
            btn_set[pos] = true;
        } else {
            // empty => keep blank, no cache
            snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", blank_png);
            btn_set[pos] = true;
        }
        item_i++;
    }

    // System icons (only if visible)
    if (show_back && back_pos >= 1 && back_pos <= 13) {
        Item it = {.name = NULL, .icon = "mdi:arrow-left", .preset = "$nav", .tap_action = "$page_back", .tap_data = NULL};
        char tmp[PATH_MAX];
        if (cached_or_generated_into(opt, cfg, "_sys", 1000, &it, tmp, sizeof(tmp))) {
            snprintf(btn_path[back_pos], sizeof(btn_path[back_pos]), "%s", tmp);
            btn_set[back_pos] = true;
        }
    }
    if (need_pagination && prev_pos >= 1 && prev_pos <= 13) {
        if (show_prev) {
            Item it = {.name = NULL, .icon = "mdi:chevron-left", .preset = "$nav", .tap_action = "$page_prev", .tap_data = NULL};
            char tmp[PATH_MAX];
            if (cached_or_generated_into(opt, cfg, "_sys", 1001, &it, tmp, sizeof(tmp))) {
                snprintf(btn_path[prev_pos], sizeof(btn_path[prev_pos]), "%s", tmp);
                btn_set[prev_pos] = true;
            }
        }
    }
    if (need_pagination && next_pos >= 1 && next_pos <= 13) {
        if (show_next) {
            Item it = {.name = NULL, .icon = "mdi:chevron-right", .preset = "$nav", .tap_action = "$page_next", .tap_data = NULL};
            char tmp[PATH_MAX];
            if (cached_or_generated_into(opt, cfg, "_sys", 1002, &it, tmp, sizeof(tmp))) {
                snprintf(btn_path[next_pos], sizeof(btn_path[next_pos]), "%s", tmp);
                btn_set[next_pos] = true;
            }
        }
    }

    // Build command
    char cmd[8192];
    size_t w = 0;
    w += (size_t)snprintf(cmd + w, sizeof(cmd) - w, "set-buttons-explicit");
    for (int pos = 1; pos <= 13; pos++) {
        if (!btn_set[pos]) snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", blank_png);
        w += (size_t)snprintf(cmd + w, sizeof(cmd) - w, " --button-%d=%s", pos, btn_path[pos]);
        if (w >= sizeof(cmd)) break;
    }
    cmd[sizeof(cmd) - 1] = 0;

    char reply[64] = {0};
    if (send_line_and_read_reply(opt->ulanzi_sock, cmd, reply, sizeof(reply)) != 0) {
        log_msg("send failed: connect/write");
        return;
    }
    log_msg("send resp='%s'", reply[0] ? reply : "<empty>");
}

int main(int argc, char **argv) {
    Options opt;
    memset(&opt, 0, sizeof(opt));
    opt.config_path = xstrdup("config/configuration.yml");
    opt.ulanzi_sock = xstrdup("/tmp/ulanzi_device.sock");
    opt.cache_root = xstrdup(".cache");
    opt.error_icon = xstrdup("assets/pregen/error.png");
    opt.sys_pregen_dir = xstrdup("assets/pregen");

    // root_dir: cwd at startup
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) die_errno("getcwd");
    opt.root_dir = xstrdup(cwd);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            free(opt.config_path);
            opt.config_path = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--ulanzi-sock") == 0 && i + 1 < argc) {
            free(opt.ulanzi_sock);
            opt.ulanzi_sock = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
            free(opt.cache_root);
            opt.cache_root = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--error-icon") == 0 && i + 1 < argc) {
            free(opt.error_icon);
            opt.error_icon = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--sys-pregen-dir") == 0 && i + 1 < argc) {
            free(opt.sys_pregen_dir);
            opt.sys_pregen_dir = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--config path] [--ulanzi-sock path] [--cache dir]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    // Resolve relative paths against root_dir (cwd at start). Keep absolute paths as-is.
    char *cfg_abs = resolve_path(opt.root_dir, opt.config_path);
    char *cache_abs = resolve_path(opt.root_dir, opt.cache_root);
    char *err_abs = resolve_path(opt.root_dir, opt.error_icon);
    char *sys_abs = resolve_path(opt.root_dir, opt.sys_pregen_dir);
    free(opt.config_path); opt.config_path = cfg_abs;
    free(opt.cache_root); opt.cache_root = cache_abs;
    free(opt.error_icon); opt.error_icon = err_abs;
    free(opt.sys_pregen_dir); opt.sys_pregen_dir = sys_abs;

    ensure_dir(opt.cache_root);
    ensure_dir_parent(opt.error_icon);
    ensure_dir(opt.sys_pregen_dir);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Config cfg;
    config_init_defaults(&cfg);
    if (load_config(opt.config_path, &cfg) != 0) die_errno("load_config");
    if (!config_get_page(&cfg, "$root")) {
        log_msg("config missing $root page");
        return 1;
    }

    // Blank image in /dev/shm (not cached).
    char blank_png[PATH_MAX];
    snprintf(blank_png, sizeof(blank_png), "/dev/shm/pagging_blank_%ld.png", (long)getpid());
    if (write_blank_png(blank_png, 196, 196) != 0) {
        // fallback to error icon if blank cannot be created
        snprintf(blank_png, sizeof(blank_png), "%s", opt.error_icon);
    }

    // Subscribe to button events.
    int rb_fd = unix_connect(opt.ulanzi_sock);
    if (rb_fd < 0) die_errno("connect ulanzi socket");
    (void)write(rb_fd, "read-buttons\n", 13);

    FILE *rb = fdopen(rb_fd, "r");
    if (!rb) die_errno("fdopen");

    char cur_page[256] = "$root";
    size_t offset = 0;
    char last_sig[256] = {0};

    // Initial render once.
    render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));

    char evline[256];
    while (g_running && fgets(evline, sizeof(evline), rb)) {
        // Log exactly what we receive (minus trailing newline)
        rtrim_only(evline);
        log_msg("rx ulanzi: %s", evline);
        trim(evline);
        if (evline[0] == 0) continue;
        if (strcmp(evline, "ok") == 0) continue;

        int btn = 0;
        char evt[32] = {0};
        if (sscanf(evline, "button %d %31s", &btn, evt) != 2) continue;
        if (strcmp(evt, "RELEASED") == 0) snprintf(evt, sizeof(evt), "RELEASE");
        if (strcmp(evt, "TAP") != 0) continue;

        const Page *p = config_get_page(&cfg, cur_page);
        if (!p) { snprintf(cur_page, sizeof(cur_page), "$root"); offset = 0; p = config_get_page(&cfg, cur_page); }
        bool show_back = strcmp(cur_page, "$root") != 0;
        size_t base_item_slots = 13 - (show_back ? 1u : 0u);
        bool need_pagination = p->count > base_item_slots;
        size_t item_slots = base_item_slots - (need_pagination ? 2u : 0u);
        if (item_slots == 0) item_slots = 1;

        bool reserved_back = show_back;
        bool reserved_pn = need_pagination;
        int back_pos = cfg.pos_back;
        int prev_pos = cfg.pos_prev;
        int next_pos = cfg.pos_next;

        // System button presses
        if (reserved_back && btn == back_pos) {
            const char *par = parent_page(cur_page);
            if (strcmp(par, cur_page) != 0) {
                snprintf(cur_page, sizeof(cur_page), "%s", par);
                offset = 0;
                render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
            }
            continue;
        }
        if (reserved_pn && btn == prev_pos) {
            if (offset >= item_slots) {
                offset -= item_slots;
                render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
            }
            continue;
        }
        if (reserved_pn && btn == next_pos) {
            size_t max_offset = (p->count > 0) ? ((p->count - 1) / item_slots) * item_slots : 0;
            if (offset + item_slots <= max_offset) {
                offset += item_slots;
                render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
            }
            continue;
        }

        // Content button mapping: positions excluding reserved.
        size_t item_i = offset;
        size_t pressed_item = (size_t)-1;
        for (int pos = 1; pos <= 13; pos++) {
            bool reserved = false;
            if (reserved_back && pos == back_pos) reserved = true;
            if (reserved_pn && (pos == prev_pos || pos == next_pos)) reserved = true;
            if (reserved) continue;
            if (item_i >= p->count) break;
            if (pos == btn) { pressed_item = item_i; break; }
            item_i++;
        }

        if (pressed_item != (size_t)-1) {
            const Item *it = page_item_at(p, pressed_item);
            if (it && is_action_goto(it->tap_action) && it->tap_data && it->tap_data[0]) {
                snprintf(cur_page, sizeof(cur_page), "%s", it->tap_data);
                offset = 0;
                render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
            }
        }
    }

    fclose(rb);
    unlink(blank_png);
    config_free(&cfg);
    free(opt.config_path);
    free(opt.ulanzi_sock);
    free(opt.cache_root);
    free(opt.error_icon);
    free(opt.sys_pregen_dir);
    free(opt.root_dir);
    return 0;
}
