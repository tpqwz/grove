#ifndef GROVE_FETCH_H
#define GROVE_FETCH_H

#include "index.h"

/* A fetched page is one http or https address. The body cap matches the
 * indexer. A longer address is refused. */
enum { FETCH_URL_MAX = 2048, FETCH_BODY_MAX = 32 * 1024 * 1024 };

/* Writes 64 hex digits and a NUL. */
void fetch_sha256_hex(const void *data, size_t n, char out[65]);

/* Normalized address, or an error. The caller frees *url. */
int fetch_normalize(const char *in, char **url, Err *e);

/* Reads fd until EOF. More than cap bytes is an error and *out stays NULL. */
int fetch_read_cap(int fd, unsigned char **out, size_t *n, size_t cap, Err *e);

/* Saves one page as index_dir/web/<sha256 of the address>.html.
 * *saved is the absolute path. *url is the normalized address. */
int fetch_save_cap(const char *index_dir, const char *raw, size_t cap, char **saved, char **url, Err *e);
int fetch_save(const char *index_dir, const char *raw, char **saved, char **url, Err *e);

/* Fetches the page, indexes it, and remembers the address. The identifier
 * is filled in when that field is still empty. */
int grove_fetch_into(Index *ix, const char *index_dir, const char *raw, IndexStats *st, Err *e);

#endif
