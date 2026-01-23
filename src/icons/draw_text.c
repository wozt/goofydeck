// Simple text overlay using ImageMagick from C (wrapper around magick annotate).
// Mirrors draw_text.sh behavior and supports fonts in ./fonts.
// Usage: draw_text [--list-ttf] [--text=...] [--text_color=RRGGBB]
//        [--text_align=top|center|bottom] [--text_font=font.ttf]
//        [--text_size=N] [--text_offset=x,y] --filename=foo.png

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <sys/wait.h>
#include <stdarg.h>

#include "fd_path.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void die_snprintf(const char *label) {
    fprintf(stderr, "Error: buffer too small for %s\n", label);
    exit(1);
}

static int snprintf_checked(char *dst, size_t cap, const char *label, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) die_snprintf(label);
    return n;
}

static void appendf_checked(char *dst, size_t cap, size_t *len, const char *label, const char *fmt, ...) {
    if (!len) die_snprintf(label);
    if (*len >= cap) die_snprintf(label);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) die_snprintf(label);
    *len += (size_t)n;
}
static int is_int(const char *s) {
    if (!s || !*s) return 0;
    char *end=NULL;
    strtol(s,&end,10);
    return end && *end=='\0';
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p,&st)==0 && S_ISREG(st.st_mode);
}

static int dir_exists(const char *p) {
    struct stat st;
    return stat(p,&st)==0 && S_ISDIR(st.st_mode);
}

static int validate_font(const char *font_path);

static void list_fonts(const char *font_dir) {
    const char *home = getenv("HOME");
    char home_fonts[PATH_MAX]="";
    char home_local_fonts[PATH_MAX]="";
    if (home) {
        snprintf_checked(home_fonts, sizeof(home_fonts), "home_fonts", "%s/.fonts", home);
        snprintf_checked(home_local_fonts, sizeof(home_local_fonts), "home_local_fonts", "%s/.local/share/fonts", home);
    }

    const char *candidates[] = {
        font_dir,
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        home_fonts[0] ? home_fonts : NULL,
        home_local_fonts[0] ? home_local_fonts : NULL,
        NULL
    };

    char cmd[PATH_MAX*8];
    size_t len = 0;
    int count = 0;
    appendf_checked(cmd, sizeof(cmd), &len, "list_fonts(find)", "%s", "find");
    for (int i=0; candidates[i]; i++) {
        if (!dir_exists(candidates[i])) continue;
        appendf_checked(cmd, sizeof(cmd), &len, "list_fonts(dir)", " '%s'", candidates[i]);
        count++;
    }
    if (!count) {
        fprintf(stderr,"No font directories found\n");
        return;
    }
    appendf_checked(cmd, sizeof(cmd), &len, "list_fonts(tail)",
                    " -maxdepth 5 -type f -iname '*.ttf' ! -user root -printf '%%f\\n' 2>/dev/null | sort -fu");
    system(cmd);
}

static int find_font_in_dirs(const char *name, char *out, size_t outsz) {
    if (!name || !*name) return 0;

    const char *home = getenv("HOME");
    char home_fonts[PATH_MAX]="";
    char home_local_fonts[PATH_MAX]="";
    if (home) {
        snprintf_checked(home_fonts, sizeof(home_fonts), "home_fonts2", "%s/.fonts", home);
        snprintf_checked(home_local_fonts, sizeof(home_local_fonts), "home_local_fonts2", "%s/.local/share/fonts", home);
    }

    const char *candidates[] = {
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        home_fonts[0] ? home_fonts : NULL,
        home_local_fonts[0] ? home_local_fonts : NULL,
        NULL
    };

    char cmd[PATH_MAX*8];
    size_t len = 0;
    int count = 0;
    appendf_checked(cmd, sizeof(cmd), &len, "find_font(find)", "%s", "find");
    for (int i=0; candidates[i]; i++) {
        if (!dir_exists(candidates[i])) continue;
        appendf_checked(cmd, sizeof(cmd), &len, "find_font(dir)", " '%s'", candidates[i]);
        count++;
    }
    if (!count) return 0;
    appendf_checked(cmd, sizeof(cmd), &len, "find_font(query)",
                    " -maxdepth 5 -type f -iname '%s' -print -quit 2>/dev/null", name);

    FILE *fp = popen(cmd,"r");
    if (!fp) return 0;
    int ok=0;
    if (fgets(out,outsz,fp)) {
        size_t l=strlen(out);
        if (l>0 && out[l-1]=='\n') out[l-1]=0;
        if (validate_font(out)) ok=1; else out[0]='\0';
    }
    pclose(fp);
    return ok;
}

static int get_size(const char *path, int *w, int *h) {
    char cmd[PATH_MAX*2];
    snprintf_checked(cmd, sizeof(cmd), "identify_cmd",
                     "magick identify -format \"%%w %%h\" '%s' 2>/dev/null", path);
    FILE *fp = popen(cmd,"r");
    if (!fp) return -1;
    if (fscanf(fp,"%d %d", w, h)!=2) { pclose(fp); return -1; }
    pclose(fp);
    return 0;
}

static int validate_font(const char *font_path) {
    if (!font_path || !*font_path) return 0;
    char tmp[]="/dev/shm/magick-fontcheck-XXXXXX.png";
    int fd = mkstemps(tmp,4); // .png suffix
    if (fd!=-1) close(fd);
    char cmd[PATH_MAX*3];
    snprintf_checked(cmd, sizeof(cmd), "fontcheck_cmd",
                     "magick -size 1x1 xc:none -font '%s' -pointsize 10 -annotate 0 'a' png:'%s' >/dev/null 2>&1",
                     font_path, tmp);
    int ret = system(cmd);
    unlink(tmp);
    return ret==0;
}

static int resolve_font(const char *font_dir, const char *name, char *out, size_t outsz) {
    if (!name || !*name) return 0;
    if (file_exists(name) && validate_font(name)) {
        snprintf_checked(out, outsz, "font_path(direct)", "%s", name);
        return 1;
    }
    char buf[PATH_MAX];
    snprintf_checked(buf, sizeof(buf), "font_path(join)", "%s/%s", font_dir, name);
    if (file_exists(buf) && validate_font(buf)) {
        snprintf_checked(out, outsz, "font_path(out)", "%s", buf);
        return 1;
    }
    if (find_font_in_dirs(name,out,outsz)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *text="";
    const char *text_color="00FF00";
    const char *text_align="center";
    const char *text_font="";
    const char *text_size="16";
    const char *text_offset="0,0";
    const char *filename=NULL;
    int list_ttf=0;

    for (int i=1;i<argc;i++) {
        if (strncmp(argv[i],"--text=",7)==0) text = argv[i]+7;
        else if (strncmp(argv[i],"--text_color=",13)==0) text_color = argv[i]+13;
        else if (strncmp(argv[i],"--text_align=",13)==0) text_align = argv[i]+13;
        else if (strncmp(argv[i],"--text_font=",12)==0) text_font = argv[i]+12;
        else if (strncmp(argv[i],"--text_size=",12)==0) text_size = argv[i]+12;
        else if (strncmp(argv[i],"--text_offset=",14)==0) text_offset = argv[i]+14;
        else if (strncmp(argv[i],"--filename=",11)==0) filename = argv[i]+11;
        else if (strncmp(argv[i],"-f=",3)==0) filename = argv[i]+3;
        else if (strcmp(argv[i],"--list-ttf")==0) list_ttf=1;
        else if (argv[i][0]=='-') { /* ignore unknown */ }
        else filename = argv[i];
    }

    char root[PATH_MAX];
    if (fd_find_project_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "Could not locate project root (set PROJECT_ROOT)\n");
        return 1;
    }
    char font_dir[PATH_MAX];
    snprintf_checked(font_dir, sizeof(font_dir), "font_dir", "%s/fonts", root);

    if (list_ttf) {
        list_fonts(font_dir);
        return 0;
    }

    if (!filename || !strstr(filename,".png")) {
        fprintf(stderr,"Usage: draw_text ... --filename=foo.png\n");
        return 1;
    }

    char target[PATH_MAX];
    if (filename[0] == '/') snprintf_checked(target, sizeof(target), "target(abs)", "%s", filename);
    else snprintf_checked(target, sizeof(target), "target(root_rel)", "%s/%s", root, filename);
    if (!file_exists(target)) {
        fprintf(stderr, "Input not found: %s\n", target);
        return 1;
    }

    if (!is_int(text_size)) text_size="16";

    // parse offset
    int off_x=0, off_y=0;
    {
        char buf[64];
        snprintf_checked(buf, sizeof(buf), "text_offset", "%s", text_offset);
        char *comma = strchr(buf,',');
        if (comma) {
            *comma='\0';
            off_x = atoi(buf);
            off_y = atoi(comma+1);
        } else {
            off_x = atoi(buf);
            off_y = 0;
        }
    }

    int w=0,h=0;
    if (get_size(target,&w,&h)!=0 || w>196 || h>196) {
        fprintf(stderr,"Input exceeds 196x196 or unreadable\n");
        return 1;
    }

    char gravity[16]="Center";
    if (strcmp(text_align,"top")==0) snprintf_checked(gravity, sizeof(gravity), "gravity", "%s", "North");
    else if (strcmp(text_align,"bottom")==0) snprintf_checked(gravity, sizeof(gravity), "gravity", "%s", "South");

    char font_path[PATH_MAX]="";
    if (!resolve_font(font_dir, text_font, font_path, sizeof(font_path))) {
        // fallback to first ttf in font_dir
        if (dir_exists(font_dir)) {
            char cmd[PATH_MAX*2];
            snprintf_checked(cmd, sizeof(cmd), "find_first_font",
                             "find '%s' -maxdepth 1 -type f -iname '*.ttf' | sort -f | head -n1",
                             font_dir);
            FILE *fp = popen(cmd,"r");
            if (fp) {
                if (fgets(font_path,sizeof(font_path),fp)) {
                    size_t len=strlen(font_path);
                    if (len>0 && font_path[len-1]=='\n') font_path[len-1]=0;
                }
                pclose(fp);
                if (!validate_font(font_path)) font_path[0]='\0';
            }
        }
    }

    // temp output
    char tmp_out[PATH_MAX];
    snprintf_checked(tmp_out, sizeof(tmp_out), "tmp_out", "%s.texttmp", target);

    // build magick command
    char annotate[64];
    snprintf_checked(annotate, sizeof(annotate), "annotate", "+%d+%d", off_x, off_y);

    // Prepare argv for exec
    const char *magick = "magick";
    char inputspec[PATH_MAX];
    snprintf_checked(inputspec, sizeof(inputspec), "inputspec", "png32:%s", target);
    int argi=0;
    const char *args[32];
    args[argi++]=magick;
    args[argi++]= inputspec;
    args[argi++]= "-gravity";
    args[argi++]= gravity;
    if (font_path[0]) { args[argi++]= "-font"; args[argi++]= font_path; }
    args[argi++]= "-pointsize"; args[argi++]= text_size;
    char fill[16];
    snprintf_checked(fill, sizeof(fill), "fill", "#%s", text_color);
    args[argi++]= "-fill"; args[argi++]= fill;
    args[argi++]= "-annotate"; args[argi++]= annotate; args[argi++]= text;
    char outspec[PATH_MAX];
    snprintf_checked(outspec, sizeof(outspec), "outspec", "png32:%s", tmp_out);
    args[argi++]= outspec;
    args[argi]=NULL;

    pid_t pid = fork();
    if (pid==0) {
        execvp(magick,(char * const*)args);
        _exit(127);
    }
    int status=0;
    waitpid(pid,&status,0);
    if (status!=0) {
        fprintf(stderr,"magick annotate failed\n");
        unlink(tmp_out);
        return 1;
    }
    rename(tmp_out, target);
    return 0;
}
