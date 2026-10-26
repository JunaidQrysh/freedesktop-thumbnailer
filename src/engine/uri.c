#include "uri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

/* Lexically normalize an absolute, slash-separated path: collapse "." and
 * ".." components and repeated slashes, without touching the filesystem
 * or resolving symlinks. Returns a malloc'd string. */
static char *lexical_normalize(const char *abspath) {
    /* abspath is guaranteed to start with '/' */
    size_t len = strlen(abspath);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, abspath, len + 1);

    /* components stack */
    char **stack = malloc(sizeof(char *) * (len / 1 + 1));
    if (!stack) { free(copy); return NULL; }
    size_t nstack = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(copy, "/", &saveptr);
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (nstack > 0) nstack--;
            /* else: ".." past root -> ignore (stay at root) */
        } else if (tok[0] != '\0') {
            stack[nstack++] = tok;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }

    size_t outcap = len + 2;
    char *out = malloc(outcap);
    if (!out) { free(copy); free(stack); return NULL; }
    size_t pos = 0;
    out[pos++] = '/';
    for (size_t i = 0; i < nstack; i++) {
        size_t clen = strlen(stack[i]);
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, stack[i], clen);
        pos += clen;
    }
    out[pos] = '\0';

    free(copy);
    free(stack);
    return out;
}

static int is_unreserved(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    switch (c) {
        case '-': case '_': case '.': case '~':
            return 1;
        default:
            return 0;
    }
}

/* Percent-encode all bytes except unreserved chars and '/' (path separator
 * kept literal so the result still reads as a path). */
static char *percent_encode_path(const char *path) {
    size_t len = strlen(path);
    /* worst case every byte becomes %XX -> 3x */
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '/' || is_unreserved(c)) {
            out[pos++] = (char)c;
        } else {
            snprintf(out + pos, 4, "%%%02X", c);
            pos += 3;
        }
    }
    out[pos] = '\0';
    return out;
}

char *canonical_file_uri(const char *path) {
    char abspath[PATH_MAX];

    if (path[0] == '/') {
        if (strlen(path) >= sizeof(abspath)) return NULL;
        strcpy(abspath, path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return NULL;
        if (strlen(cwd) + 1 + strlen(path) >= sizeof(abspath)) return NULL;
        snprintf(abspath, sizeof(abspath), "%s/%s", cwd, path);
    }

    char *normalized = lexical_normalize(abspath);
    if (!normalized) return NULL;

    char *encoded = percent_encode_path(normalized);
    free(normalized);
    if (!encoded) return NULL;

    size_t total = strlen("file://") + strlen(encoded) + 1;
    char *uri = malloc(total);
    if (!uri) { free(encoded); return NULL; }
    snprintf(uri, total, "file://%s", encoded);
    free(encoded);
    return uri;
}
