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
#include <poll.h>

#include <zlib.h>
#include <png.h>
#include <yaml.h>

typedef struct {
    char *name;
    char *icon;
    char *preset;
    char *text;
    char *tap_action;
    char *tap_data;
} Item;

typedef struct {
    char *name;
    char *icon_background_color; // "RRGGBB" or "transparent"
    int icon_border_radius;      // percent (0..50)
    int icon_border_width;       // px (0..98)
    char *icon_border_color;     // "RRGGBB" or "transparent"
    int icon_size;               // px (0..196), 0=auto
    int icon_padding;            // px (>=0)
    int icon_offset_x;           // px
    int icon_offset_y;           // px
    int icon_brightness;         // percent (1..200)
    char *icon_color;            // "RRGGBB" or "transparent"
    char *text_color;            // "RRGGBB" or "transparent"
    char *text_align;            // top|center|bottom
    char *text_font;             // font filename or system font name
    int text_size;               // px
    int text_offset_x;           // px
    int text_offset_y;           // px
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
    int base_brightness;        // 0..100
    int sleep_dim_brightness;   // 0..100
    int sleep_dim_timeout_sec;  // seconds, 0=disabled
    int sleep_timeout_sec;      // seconds, 0=disabled
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
    char *control_sock;
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
        if (errno == ENOENT) {
            fprintf(stderr, "[pagging] ERROR: ulanzi socket not found: %s (is ulanzi_d200_demon running?)\n", sock_path);
        }
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
    cfg->base_brightness = 90;
    cfg->sleep_dim_brightness = 20;
    cfg->sleep_dim_timeout_sec = 0;
    cfg->sleep_timeout_sec = 0;
}

static void preset_init_defaults(Preset *p, const char *name) {
    memset(p, 0, sizeof(*p));
    p->name = xstrdup(name ? name : "default");
    p->icon_background_color = xstrdup("241f31");
    p->icon_border_radius = 12;
    p->icon_border_width = 0;
    p->icon_border_color = xstrdup("FFFFFF");
    p->icon_size = 128;
    p->icon_padding = 0;
    p->icon_offset_x = 0;
    p->icon_offset_y = 0;
    p->icon_brightness = 100;
    p->icon_color = xstrdup("FFFFFF");
    p->text_color = xstrdup("FFFFFF");
    p->text_align = xstrdup("bottom");
    p->text_font = xstrdup("");
    p->text_size = 16;
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
        for (size_t j = 0; j < p->count; j++) {
            free(p->items[j].name);
            free(p->items[j].icon);
            free(p->items[j].preset);
            free(p->items[j].text);
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
        fprintf(stderr, "[pagging] ERROR: YAML parse failed: %s\n", parser.problem ? parser.problem : "unknown");
        yaml_parser_delete(&parser);
        fclose(f);
        return -1;
    }

    Config cfg;
    config_init_defaults(&cfg);
    if (!config_get_preset_mut(&cfg, "default")) (void)config_add_preset(&cfg, "default");

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root || root->type != YAML_MAPPING_NODE) {
        fprintf(stderr, "[pagging] ERROR: YAML root is not a mapping\n");
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

            n = yaml_mapping_get(&doc, v, "icon_background_color");
            if (yaml_scalar_cstr(n)) { free(pr->icon_background_color); pr->icon_background_color = xstrdup(yaml_scalar_cstr(n)); }
            n = yaml_mapping_get(&doc, v, "icon_border_radius");
            if (yaml_scalar_cstr(n)) { int iv = 0; if (parse_int_scalar(yaml_scalar_cstr(n), &iv) == 0) pr->icon_border_radius = iv; }
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

            yaml_node_t *buttons = yaml_mapping_get(&doc, v, "buttons");
            if (!buttons || buttons->type != YAML_SEQUENCE_NODE) continue;
            for (yaml_node_item_t *it = buttons->data.sequence.items.start; it < buttons->data.sequence.items.top; it++) {
                yaml_node_t *item = yaml_document_get_node(&doc, *it);
                if (!item || item->type != YAML_MAPPING_NODE) continue;

                Item out_item;
                memset(&out_item, 0, sizeof(out_item));

                yaml_node_t *n;
                n = yaml_mapping_get(&doc, item, "name");
                if (yaml_scalar_cstr(n)) out_item.name = xstrdup(yaml_scalar_cstr(n));
                n = yaml_mapping_get(&doc, item, "icon");
                if (yaml_scalar_cstr(n)) out_item.icon = xstrdup(yaml_scalar_cstr(n));
                n = yaml_mapping_get(&doc, item, "text");
                if (yaml_scalar_cstr(n)) out_item.text = xstrdup(yaml_scalar_cstr(n));

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
                    yaml_node_t *a = yaml_mapping_get(&doc, n, "action");
                    yaml_node_t *d = yaml_mapping_get(&doc, n, "data");
                    if (yaml_scalar_cstr(a)) out_item.tap_action = xstrdup(yaml_scalar_cstr(a));
                    if (yaml_scalar_cstr(d)) out_item.tap_data = xstrdup(yaml_scalar_cstr(d));
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
    int rad = preset ? clamp_int(preset->icon_border_radius, 0, 50) : 0;
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
        snprintf(size_outer, sizeof(size_outer), "--size=196");
        snprintf(rad_arg, sizeof(rad_arg), "--radius=%d", rad);
        char *argv_outer[] = { draw_border_bin, (char *)border_c, size_outer, rad_arg, (char *)out_png, NULL };
        if (run_exec(argv_outer) != 0) return -1;

        int inner = 196 - 2 * bw;
        inner = clamp_int(inner, 1, 196);
        char size_inner[32];
        snprintf(size_inner, sizeof(size_inner), "--size=%d", inner);
        char *argv_inner[] = { draw_border_bin, (char *)bg, size_inner, rad_arg, (char *)out_png, NULL };
        if (run_exec(argv_inner) != 0) return -1;
    }

    // draw_mdi (optional)
    if (it->icon && strncmp(it->icon, "mdi:", 4) == 0) {
        if (access(draw_mdi_bin, X_OK) != 0) return -1;
        if (ensure_mdi_svg(opt, it->icon) != 0) return -1;
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
    {
        char *argv[] = { draw_opt_bin, (char *)"-c", (char *)"4", (char *)out_png, NULL };
        if (run_exec(argv) != 0) return -1;
    }

    // draw_text (optional)
    if (it->text && it->text[0]) {
        const char *tc = (preset && preset->text_color && preset->text_color[0]) ? preset->text_color : "FFFFFF";
        const char *ta = (preset && preset->text_align && preset->text_align[0]) ? preset->text_align : "bottom";
        const char *tf = (preset && preset->text_font) ? preset->text_font : "";
        int ts = preset ? clamp_int(preset->text_size, 1, 64) : 16;
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
        } else {
            char *argv[] = { draw_text_bin, text_arg, tc_arg, ta_arg, ts_arg, to_arg, (char *)out_png, NULL };
            rc = run_exec(argv);
        }
        if (rc != 0) return -1;

        // RE draw_optimize
        char *argv2[] = { draw_opt_bin, (char *)"-c", (char *)"4", (char *)out_png, NULL };
        if (run_exec(argv2) != 0) return -1;
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

static void append_preset_sig(char *dst, size_t cap, size_t *len, const Preset *p) {
    if (!dst || !cap || !len) return;
    if (!p) {
        int n = snprintf(dst + *len, cap - *len, "preset:<none>\n");
        if (n > 0 && (size_t)n < cap - *len) *len += (size_t)n;
        return;
    }
    int n = snprintf(
        dst + *len, cap - *len,
        "preset:%s\nbg:%s\nrad:%d\nbw:%d\nbc:%s\nisz:%d\npad:%d\noff:%d,%d\nbri:%d\nic:%s\n"
        "tc:%s\nta:%s\ntf:%s\nts:%d\nto:%d,%d\n",
        p->name ? p->name : "",
        p->icon_background_color ? p->icon_background_color : "",
        p->icon_border_radius,
        p->icon_border_width,
        p->icon_border_color ? p->icon_border_color : "",
        p->icon_size,
        p->icon_padding,
        p->icon_offset_x, p->icon_offset_y,
        p->icon_brightness,
        p->icon_color ? p->icon_color : "",
        p->text_color ? p->text_color : "",
        p->text_align ? p->text_align : "",
        p->text_font ? p->text_font : "",
        p->text_size,
        p->text_offset_x, p->text_offset_y
    );
    if (n > 0 && (size_t)n < cap - *len) *len += (size_t)n;
}

static bool cached_or_generated_into(const Options *opt, const Config *cfg, const char *page, size_t item_index, const Item *it,
                                     char *out_path, size_t out_cap) {
    if (!it) return false;
    const char *ic = it->icon ? it->icon : "";
    const char *tx = it->text ? it->text : "";
    const char *pr = it->preset ? it->preset : "";
    if (ic[0] == 0 && tx[0] == 0) return false; // empty => no cache

    const Preset *preset = config_get_preset(cfg, pr);
    if (!preset) preset = config_get_preset(cfg, "default");

    char key[4096];
    size_t key_len = 0;
    int n = snprintf(key + key_len, sizeof(key) - key_len, "page:%s\nidx:%zu\nicon:%s\ntext:%s\n", page, item_index, ic, tx);
    if (n < 0 || (size_t)n >= sizeof(key) - key_len) return false;
    key_len += (size_t)n;
    append_preset_sig(key, sizeof(key), &key_len, preset);
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
        if (generate_icon_pipeline(opt, preset, it, out_path) != 0) {
            (void)copy_file(opt->error_icon, out_path);
        }
        write_hex_u32_file(meta, h);
        return true;
    } else {
        snprintf(out_path, out_cap, "%s/%s/item%zu_%08x.png", opt->cache_root, page, item_index + 1, h);
    }

    if (file_exists(out_path)) return true;

    if (generate_icon_pipeline(opt, preset, it, out_path) != 0) {
        (void)copy_file(opt->error_icon, out_path);
    }
    return true;
}

static const Item *page_item_at(const Page *p, size_t idx) {
    if (!p || idx >= p->count) return NULL;
    return &p->items[idx];
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
    SheetLayout sheet = compute_sheet_layout(p->count, show_back, offset);
    offset = sheet.start;
    size_t item_slots = sheet.cap;
    bool show_prev = sheet.show_prev;
    bool show_next = sheet.show_next;

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
    char btn_label[14][64];
    bool label_set[14] = {0};
    for (int i = 1; i <= 13; i++) {
        snprintf(btn_path[i], sizeof(btn_path[i]), "%s", blank_png);
        btn_set[i] = true;
        btn_label[i][0] = 0;
        label_set[i] = false;
    }

    // Reserve back/prev/next
    bool reserved[14] = {0};
    if (show_back && back_pos >= 1 && back_pos <= 13) reserved[back_pos] = true;
    if (show_prev && prev_pos >= 1 && prev_pos <= 13) reserved[prev_pos] = true;
    if (show_next && next_pos >= 1 && next_pos <= 13) reserved[next_pos] = true;

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
        // name is the device label (no spaces supported by daemon's argv parser)
        if (it && it->name && it->name[0]) {
            size_t w = 0;
            for (size_t i = 0; it->name[i] && w + 1 < sizeof(btn_label[pos]); i++) {
                unsigned char c = (unsigned char)it->name[i];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = '_';
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ) {
                    btn_label[pos][w++] = (char)c;
                } else {
                    btn_label[pos][w++] = '_';
                }
            }
            btn_label[pos][w] = 0;
            if (btn_label[pos][0]) label_set[pos] = true;
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
    if (show_prev && prev_pos >= 1 && prev_pos <= 13) {
        Item it = {.name = NULL, .icon = "mdi:chevron-left", .preset = "$nav", .tap_action = "$page_prev", .tap_data = NULL};
        char tmp[PATH_MAX];
        if (cached_or_generated_into(opt, cfg, "_sys", 1001, &it, tmp, sizeof(tmp))) {
            snprintf(btn_path[prev_pos], sizeof(btn_path[prev_pos]), "%s", tmp);
            btn_set[prev_pos] = true;
        }
    }
    if (show_next && next_pos >= 1 && next_pos <= 13) {
        Item it = {.name = NULL, .icon = "mdi:chevron-right", .preset = "$nav", .tap_action = "$page_next", .tap_data = NULL};
        char tmp[PATH_MAX];
        if (cached_or_generated_into(opt, cfg, "_sys", 1002, &it, tmp, sizeof(tmp))) {
            snprintf(btn_path[next_pos], sizeof(btn_path[next_pos]), "%s", tmp);
            btn_set[next_pos] = true;
        }
    }

    // Build command
    char *cmd = NULL;
    size_t w = 0;
    size_t cap = 0;
    appendf_dyn(&cmd, &w, &cap, "set-buttons-explicit");
    for (int pos = 1; pos <= 13; pos++) {
        if (!btn_set[pos]) snprintf(btn_path[pos], sizeof(btn_path[pos]), "%s", blank_png);
        appendf_dyn(&cmd, &w, &cap, " --button-%d=%s", pos, btn_path[pos]);
        if (label_set[pos]) {
            appendf_dyn(&cmd, &w, &cap, " --label-%d=%s", pos, btn_label[pos]);
        }
    }
    if (!cmd) return;
    if (w > 8000) log_msg("send cmd_len=%zu (was previously truncated at 8192)", w);

    char reply[64] = {0};
    if (send_line_and_read_reply(opt->ulanzi_sock, cmd, reply, sizeof(reply)) != 0) {
        free(cmd);
        log_msg("send failed: connect/write");
        return;
    }
    free(cmd);
    log_msg("send resp='%s'", reply[0] ? reply : "<empty>");
}

static void state_dir(const Options *opt, char *out, size_t cap) {
    // Prefer RAM-backed /dev/shm, fallback to cache_root if not available.
    if (!out || cap == 0) return;
    const char *primary = "/dev/shm/goofydeck/pagging";
    if (try_ensure_dir_parent(primary) == 0 && try_ensure_dir(primary) == 0) {
        snprintf(out, cap, "%s", primary);
        return;
    }
    snprintf(out, cap, "%s/pagging", (opt && opt->cache_root) ? opt->cache_root : ".cache");
    ensure_dir(out);
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

int main(int argc, char **argv) {
    Options opt;
    memset(&opt, 0, sizeof(opt));
    opt.config_path = xstrdup("config/configuration.yml");
    opt.ulanzi_sock = xstrdup("/tmp/ulanzi_device.sock");
    opt.control_sock = xstrdup("/tmp/goofydeck_pagging_control.sock");
    opt.cache_root = xstrdup(".cache");
    opt.error_icon = xstrdup("assets/pregen/error.png");
    opt.sys_pregen_dir = xstrdup("assets/pregen");
    bool dump_config = false;

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
        } else if (strcmp(argv[i], "--control-sock") == 0 && i + 1 < argc) {
            free(opt.control_sock);
            opt.control_sock = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
            free(opt.cache_root);
            opt.cache_root = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--error-icon") == 0 && i + 1 < argc) {
            free(opt.error_icon);
            opt.error_icon = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--sys-pregen-dir") == 0 && i + 1 < argc) {
            free(opt.sys_pregen_dir);
            opt.sys_pregen_dir = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "--dump-config") == 0) {
            dump_config = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--config path] [--ulanzi-sock path] [--control-sock path] [--cache dir]\n", argv[0]);
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
    char *ctl_abs = resolve_path(opt.root_dir, opt.control_sock);
    free(opt.config_path); opt.config_path = cfg_abs;
    free(opt.cache_root); opt.cache_root = cache_abs;
    free(opt.error_icon); opt.error_icon = err_abs;
    free(opt.sys_pregen_dir); opt.sys_pregen_dir = sys_abs;
    free(opt.control_sock); opt.control_sock = ctl_abs;

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
    if (dump_config) {
        fprintf(stderr, "[pagging] dump-config: pages=%zu presets=%zu\n", cfg.page_count, cfg.preset_count);
        for (size_t i = 0; i < cfg.page_count; i++) {
            const Page *p = &cfg.pages[i];
            fprintf(stderr, "[pagging] page '%s' items=%zu\n", p->name ? p->name : "<null>", p->count);
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

    // Subscribe to button events.
    int rb_fd = unix_connect(opt.ulanzi_sock);
    if (rb_fd < 0) die_errno("connect ulanzi socket");
    (void)write(rb_fd, "read-buttons\n", 13);

    int rb_flags = fcntl(rb_fd, F_GETFL, 0);
    if (rb_flags >= 0) (void)fcntl(rb_fd, F_SETFL, rb_flags | O_NONBLOCK);

    int ctl_fd = make_unix_listen_socket(opt.control_sock);
    if (ctl_fd < 0) die_errno("control listen socket");
    log_msg("control socket: %s", opt.control_sock);

    char cur_page[256] = "$root";
    size_t offset = 0;
    char last_sig[256] = {0};
    char page_stack[64][256];
    int page_stack_len = 0;
    bool control_enabled = true;

    // Brightness/sleep state machine (driven by config).
    enum { BR_NORMAL = 0, BR_DIM = 1, BR_SLEEP = 2 } br_state = BR_NORMAL;
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

    // Initial render once.
    render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
    persist_last_page(&opt, cur_page, offset);

    char inbuf[4096];
    size_t inlen = 0;

    while (g_running) {
        struct pollfd fds[2];
        memset(fds, 0, sizeof(fds));
        fds[0].fd = rb_fd;
        fds[0].events = POLLIN;
        fds[1].fd = ctl_fd;
        fds[1].events = POLLIN;

        int pr = poll(fds, 2, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            die_errno("poll");
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
                } else if (strcmp(cmdline, "load-last-page") == 0) {
                    char lp[256] = {0};
                    size_t lo = 0;
                    if (load_last_page(&opt, lp, sizeof(lp), &lo) == 0) {
                        if (config_get_page(&cfg, lp)) {
                            snprintf(cur_page, sizeof(cur_page), "%s", lp);
                            offset = lo;
                            last_sig[0] = 0; // force render
                            render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
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
                log_msg("rx ulanzi: %s", evline);
                trim(evline);
                if (evline[0] == 0) continue;
                if (strcmp(evline, "ok") == 0) continue;

                int btn = 0;
                char evt[32] = {0};
                if (sscanf(evline, "button %d %31s", &btn, evt) != 2) continue;
                if (strcmp(evt, "RELEASED") == 0) snprintf(evt, sizeof(evt), "RELEASE");

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
                if (btn == 14 && strcmp(evt, "LONGHOLD") == 0) {
                    if (!control_enabled) {
                        log_msg("start-control (forced by button 14 LONGHOLD)");
                        control_enabled = true;
                        last_sig[0] = 0; // force refresh
                        render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
                        persist_last_page(&opt, cur_page, offset);
                    }
                    continue;
                }

                if (!control_enabled) continue;
                if (strcmp(evt, "TAP") != 0) continue;

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

                // System button presses
                if (reserved_back && btn == back_pos) {
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
                        render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
                        persist_last_page(&opt, cur_page, offset);
                    }
                    continue;
                }
                if (reserved_prev && btn == prev_pos) {
                    offset = sheet.prev_start;
                    render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
                    persist_last_page(&opt, cur_page, offset);
                    continue;
                }
                if (reserved_next && btn == next_pos) {
                    offset = sheet.next_start;
                    render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
                    persist_last_page(&opt, cur_page, offset);
                    continue;
                }

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
                    if (it && is_action_goto(it->tap_action) && it->tap_data && it->tap_data[0]) {
                        if (page_stack_len < (int)(sizeof(page_stack) / sizeof(page_stack[0]))) {
                            snprintf(page_stack[page_stack_len], sizeof(page_stack[page_stack_len]), "%s", cur_page);
                            page_stack_len++;
                        }
                        snprintf(cur_page, sizeof(cur_page), "%s", it->tap_data);
                        offset = 0;
                        render_and_send(&opt, &cfg, cur_page, offset, blank_png, last_sig, sizeof(last_sig));
                        persist_last_page(&opt, cur_page, offset);
                    }
                }
            }

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
    config_free(&cfg);
    free(opt.config_path);
    free(opt.ulanzi_sock);
    free(opt.control_sock);
    free(opt.cache_root);
    free(opt.error_icon);
    free(opt.sys_pregen_dir);
    free(opt.root_dir);
    return 0;
}
