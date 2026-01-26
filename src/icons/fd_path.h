// Shared helpers for icon tools: find project root and resolve paths relative to it.
// Rule A: when given a relative path, treat it as relative to the project root.

#ifndef FD_PATH_H
#define FD_PATH_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(__linux__)
extern ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FD_UNUSED __attribute__((unused))
#else
#define FD_UNUSED
#endif

static FD_UNUSED void fd_die_snprintf(const char *label) {
    fprintf(stderr, "Error: buffer too small for %s\n", label ? label : "snprintf");
    exit(1);
}

static FD_UNUSED void fd_snprintf_checked(char *dst, size_t cap, const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) fd_die_snprintf(label);
}

static FD_UNUSED int fd_file_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static FD_UNUSED int fd_dir_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static FD_UNUSED int fd_mkdir_p(const char *dir) {
    if (!dir || !*dir) return 0;
    if (strcmp(dir, "/") == 0) return 0;

    char tmp[PATH_MAX];
    fd_snprintf_checked(tmp, sizeof(tmp), "mkdir_p(tmp)", "%s", dir);

    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static FD_UNUSED int fd_mkdir_p_parent(const char *path) {
    if (!path || !*path) return 0;
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    if (slash == path) return 0; // parent is "/"

    char dir[PATH_MAX];
    size_t n = (size_t)(slash - path);
    if (n >= sizeof(dir)) fd_die_snprintf("mkdir_parent(dir)");
    memcpy(dir, path, n);
    dir[n] = '\0';
    return fd_mkdir_p(dir);
}

static FD_UNUSED int fd_find_project_root(char *out, size_t outsz) {
    const char *env = getenv("PROJECT_ROOT");
    if (env && *env) {
        fd_snprintf_checked(out, outsz, "PROJECT_ROOT", "%s", env);
        return 0;
    }

    // Best effort: derive root from the executable location.
#if defined(__linux__)
    {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) *slash = '\0'; // exe dir

            const char *base = strrchr(exe, '/');
            base = base ? base + 1 : exe;
            if (strcmp(base, "icons") == 0 || strcmp(base, "lib") == 0 || strcmp(base, "standalone") == 0) {
                char *slash2 = strrchr(exe, '/');
                if (slash2) *slash2 = '\0';
            }

            char marker_make[PATH_MAX];
            char marker_src[PATH_MAX];
            char marker_icons[PATH_MAX];
            fd_snprintf_checked(marker_make, sizeof(marker_make), "marker_make", "%s/Makefile", exe);
            fd_snprintf_checked(marker_src, sizeof(marker_src), "marker_src", "%s/ulanzi_d200_daemon.c", exe);
            fd_snprintf_checked(marker_icons, sizeof(marker_icons), "marker_icons", "%s/icons", exe);
            if (fd_file_exists(marker_make) && fd_file_exists(marker_src) && fd_dir_exists(marker_icons)) {
                fd_snprintf_checked(out, outsz, "root(exe)", "%s", exe);
                return 0;
            }
        }
    }
#endif

    if (!getcwd(out, outsz)) return -1;
    for (;;) {
        char marker_make[PATH_MAX];
        char marker_src[PATH_MAX];
        char marker_icons[PATH_MAX];
        fd_snprintf_checked(marker_make, sizeof(marker_make), "marker_make", "%s/Makefile", out);
        fd_snprintf_checked(marker_src, sizeof(marker_src), "marker_src", "%s/ulanzi_d200_daemon.c", out);
        fd_snprintf_checked(marker_icons, sizeof(marker_icons), "marker_icons", "%s/icons", out);
        if (fd_file_exists(marker_make) && fd_file_exists(marker_src) && fd_dir_exists(marker_icons)) return 0;

        if (strcmp(out, "/") == 0) break;
        char *slash = strrchr(out, '/');
        if (!slash) break;
        if (slash == out) out[1] = '\0';
        else *slash = '\0';
    }
    return -1;
}

static FD_UNUSED int fd_resolve_root_relative(const char *root, const char *in, char *out, size_t outsz) {
    if (!in || !*in) return -1;
    if (in[0] == '/') {
        fd_snprintf_checked(out, outsz, "path(abs)", "%s", in);
        return 0;
    }
    if (!root || !*root) return -1;
    fd_snprintf_checked(out, outsz, "path(root_rel)", "%s/%s", root, in);
    return 0;
}

#endif
