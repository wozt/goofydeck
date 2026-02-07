// Typo-compat wrapper: draw_broder_rectangle -> draw_border_rectangle.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fd_path.h"

int main(int argc, char **argv) {
    char root[PATH_MAX];
    if (fd_find_project_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "Could not locate project root (set PROJECT_ROOT)\n");
        return 1;
    }
    char tool[PATH_MAX];
    fd_snprintf_checked(tool, sizeof(tool), "tool(draw_border_rectangle)", "%s/icons/draw_border_rectangle", root);

    char **nargv = calloc((size_t)argc + 1, sizeof(char *));
    if (!nargv) { perror("calloc"); return 1; }
    nargv[0] = tool;
    for (int i = 1; i < argc; i++) nargv[i] = argv[i];
    nargv[argc] = NULL;

    execv(tool, nargv);
    fprintf(stderr, "Error: exec %s failed: %s\n", tool, strerror(errno));
    return 1;
}

