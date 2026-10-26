#include "xdg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_blank(const char *s) {
    if (!s) return 1;
    while (*s) {
        if (*s != ' ' && *s != '\t') return 0;
        s++;
    }
    return 1;
}

char *thumbnails_base_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && !is_blank(xdg) && xdg[0] != '\0') {
        size_t n = strlen(xdg) + strlen("/thumbnails") + 1;
        char *out = malloc(n);
        if (!out) return NULL;
        snprintf(out, n, "%s/thumbnails", xdg);
        return out;
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return NULL;
    size_t n = strlen(home) + strlen("/.cache/thumbnails") + 1;
    char *out = malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s/.cache/thumbnails", home);
    return out;
}
