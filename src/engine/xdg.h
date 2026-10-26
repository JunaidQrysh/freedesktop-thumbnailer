#ifndef XDG_H
#define XDG_H

/* Returns malloc'd path: $XDG_CACHE_HOME/thumbnails if XDG_CACHE_HOME is
 * set and non-blank, else $HOME/.cache/thumbnails. Returns NULL if HOME
 * is unset in the fallback case (nothing we can do). */
char *thumbnails_base_dir(void);

#endif
