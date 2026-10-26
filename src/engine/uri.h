#ifndef URI_H
#define URI_H

/* Canonicalizes `path` (relative or absolute, need not exist... it must
 * exist for stat purposes elsewhere, but this function itself just does
 * lexical path canonicalization) into an absolute file:// URI per RFC 2396
 * canonical form (percent-encoding of reserved/non-ASCII bytes).
 * Returns a malloc'd string the caller must free, or NULL on error
 * (e.g. getcwd failure for a relative path). Does NOT resolve symlinks -
 * only lexically normalizes "." and ".." components, matching what other
 * thumbnailer implementations do (the URI identifies the path used to
 * access the file, not its resolved target). */
char *canonical_file_uri(const char *path);

#endif
