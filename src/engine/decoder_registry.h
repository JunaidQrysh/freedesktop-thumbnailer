#ifndef DECODER_REGISTRY_H
#define DECODER_REGISTRY_H

#include "../thumbnailer.h"

/* Looks up the decoder for a lowercase extension (no dot). Returns NULL if
 * the extension isn't registered (thumbnailer.c is expected to have
 * already filtered the work queue to registered extensions during the
 * walk, but callers should still handle NULL defensively). */
decode_fn registry_find(const char *ext_lower);

/* True if ext_lower is a registered/known extension - used by the walk
 * phase to decide whether a file belongs in the work queue. */
int registry_has(const char *ext_lower);

#endif
