#include "../thumbnailer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

void image_free(image_t *img) {
    if (img && img->pixels) {
        free(img->pixels);
        img->pixels = NULL;
    }
}

const char *file_ext_lower(const char *path, char *buf, size_t buflen) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash) || dot[1] == '\0') return NULL;

    size_t len = strlen(dot + 1);
    if (len >= buflen) return NULL;
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)dot[1 + i]);
    buf[len] = '\0';
    return buf;
}
