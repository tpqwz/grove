#ifndef GROVE_INDEX_H
#define GROVE_INDEX_H

#include "text.h"
#include "util.h"

#include <stdint.h>

typedef struct Index Index;

typedef struct IndexStats {
    uint32_t indexed;
    uint32_t unchanged;
    uint32_t removed;
    uint32_t skipped;
} IndexStats;

typedef struct IndexInfo {
    const char *dir;
    uint32_t n_docs;
    uint32_t n_segments;
    uint32_t n_roots;
    const char *const *roots;
    const uint8_t *root_full;
    uint64_t text_bytes;
} IndexInfo;

/* One phrase column from FTS4 matchinfo('pcnalx'), column 0 (the body). */
typedef struct PhraseStat {
    uint32_t hits;  /* occurrences in this row */
    uint32_t total; /* occurrences in the whole index */
    uint32_t docs;  /* rows that contain the phrase */
} PhraseStat;

/* Parsed matchinfo('pcnalx'). phrase is NULL when the query did not use MATCH.
 * Ranking uses these counts; callers can read them and rank differently. */
typedef struct MatchInfo {
    uint32_t phrases;    /* p */
    uint32_t columns;    /* c */
    uint32_t rows;       /* n, documents in the FTS index */
    uint32_t avg_tokens; /* average tokens per document */
    uint32_t tokens;     /* tokens in this document */
    PhraseStat *phrase;
    int nphrase;
} MatchInfo;

typedef struct Hit {
    char *path;
    char *title;
    char *ext;
    int score;
    /* First passage's text, or NULL. Owned by passages[0]. */
    char *snippet;
    /* FTS4 offsets(), earliest GROVE_OFFSET_MAX, in file order. */
    MatchOff *offsets;
    int noffsets;
    int nmatches;
    MatchInfo matchinfo;
    Passage *passages;
    int npassages;
    /* 1 when another page of passages remains after these. */
    int more;
} Hit;

/* One file-count bucket. A type value is the extension. A folder value is the
 * parent of the path relative to the indexed root, with a trailing slash. */
typedef struct Facet {
    char *value;
    int count;
} Facet;

typedef struct Facets {
    Facet *types;
    int ntypes;
    Facet *folders;
    int nfolders;
    /* Matching files before the limit trim. */
    int nfiles;
} Facets;

Index *index_open(const char *dir, int writable, Err *e);
void index_close(Index *ix);
void index_set_quiet(Index *ix, int quiet);
void index_set_force(Index *ix, int force);

int index_add(Index *ix, const char *path, IndexStats *st, Err *e);
int index_reindex(Index *ix, IndexStats *st, Err *e);
int index_rebind(Index *ix, const char *from, const char *to, Err *e);
int index_purge(Index *ix, IndexStats *st, Err *e);
int index_compact(Index *ix, Err *e);
int index_info(Index *ix, IndexInfo *info, Err *e);

/* at is NULL, a replacement root (one indexed root), or OLD=NEW.
 * facets may be NULL. When set, counts cover every matching file, before limit
 * drops the hit list. Pass a zeroed Facets; free it with facets_free. */
int index_search(Index *ix, const char *query, int limit, const char *at, Hit **hits, int *nhits, Facets *facets,
                 Err *e);
void facets_free(Facets *f);
/* Next page of passages for one file already returned by index_search.
 * `after` is the offset of the last passage shown. */
int index_more(Index *ix, const char *query, const char *path, uint32_t after, Passage **out, int *nout, int *more,
               Err *e);
void hits_free(Hit *hits, int n);

#endif
