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
 * parent of the path relative to the indexed root, with a trailing slash. A
 * month value is YYYY-MM in local time. */
typedef struct Facet {
    char *value;
    int count;
} Facet;

/* Language, author, form, period, and genre from the record. Author is the
 * creator, form the type, period the coverage, and genre the subject.
 * A creator or a language may hold several names separated by semicolons.
 * The others are one value. Counts are files. */
typedef struct Narrow {
    Facet *language;
    int nlanguage;
    Facet *author;
    int nauthor;
    Facet *form;
    int nform;
    Facet *period;
    int nperiod;
    Facet *genre;
    int ngenre;
} Narrow;

typedef struct Facets {
    Facet *types;
    int ntypes;
    Facet *folders;
    int nfolders;
    Facet *months;
    int nmonths;
    Narrow narrow;
    /* Matching files before the limit trim, after the record filters. */
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
/* modes NULL is an exact query, apart from a trailing ~ on a word. */
int index_search(Index *ix, const char *query, int limit, const char *at, Hit **hits, int *nhits, Facets *facets,
                 const SearchModes *modes, Err *e);
void facets_free(Facets *f);
/* Next page of passages for one file already returned by index_search.
 * `after` is the offset of the last passage shown. modes matches that search. */
int index_more(Index *ix, const char *query, const char *path, uint32_t after, Passage **out, int *nout, int *more,
               const SearchModes *modes, Err *e);
void hits_free(Hit *hits, int n);

/* The indexed text of one file, which is the same record for every source
 * type: prose, heading marks, and page marks. dir 0 is the chapter around
 * `at` (or a window of it, when the chapter is long). dir 1 starts at `at`.
 * dir -1 ends at `at`. page is 0 when the file recorded none. heads lists
 * every heading. pages lists page marks inside the slice. */
typedef struct DocHead {
    uint32_t at;
    int level;
    char *text;
} DocHead;

typedef struct DocPage {
    uint32_t at;
    int page;
} DocPage;

typedef struct DocRead {
    char *title;
    char *section;
    char *text;
    uint32_t from;
    uint32_t to;
    uint32_t at;
    uint32_t length;
    int page;
    DocHead *heads;
    int nheads;
    DocPage *pages;
    int npages;
} DocRead;

int index_read(Index *ix, const char *path, uint32_t at, int dir, DocRead **out, Err *e);
void docread_free(DocRead *d);

/* Remembered queries live in the same grove.db. The query text is the identity.
 * List is newest first. A missing table is an empty list. Put and drop open a
 * short write, so a read-only search connection can still remember one. At most
 * 24 are kept. */
int index_saved_list(Index *ix, char ***queries, int *n, Err *e);
void index_saved_free(char **queries, int n);
int index_saved_put(Index *ix, const char *query, Err *e);
int index_saved_drop(Index *ix, const char *query, Err *e);

/* A note is text on one place: the displayed path and the passage offset.
 * The table appears on the first note. List is path, then offset. */
typedef struct Note {
    char *path;
    uint32_t offset;
    char *body;
} Note;

int index_notes_list(Index *ix, Note **notes, int *n, Err *e);
void index_notes_free(Note *notes, int n);
int index_note_put(Index *ix, const char *path, uint32_t offset, const char *body, Err *e);
int index_note_drop(Index *ix, const char *path, uint32_t offset, Err *e);

/* A watch stores the files that match a query: path, mtime, nsec, and size.
 * The list names files that are new or changed since that snapshot. Watching
 * again replaces it. The tables appear on the first watch. A watch is also a
 * saved search, so at most 24 are kept. Forgetting the search drops the watch.
 * Unwatch leaves the saved search. One snapshot holds at most ALERT_FILES_MAX. */
#define ALERT_FILES_MAX 10000

typedef struct AlertFile {
    char *path;
    int64_t mtime;
    int nsec;
    int64_t size;
} AlertFile;

typedef struct Alert {
    char *query;
    AlertFile *files;
    int nfiles;
} Alert;

int index_alert_watch(Index *ix, const char *query, Err *e);
int index_alert_unwatch(Index *ix, const char *query, Err *e);
int index_alerts_list(Index *ix, Alert **alerts, int *n, Err *e);
void index_alerts_free(Alert *alerts, int n);

/* A shelf list reads titles and paths without the stored text. The first
 * build of that list creates an index on those columns. A volume record is
 * the Dublin Core elements, kept beside the file. An empty element is unset.
 * title in the list is the record's title when one was saved, otherwise the
 * title from the file. At most LIBRARY_SHOW volumes are returned; total is
 * the whole match. index_library_list and index_library_narrow keep stored
 * title order. index_library_sorted orders the whole match first. */
#define LIBRARY_SHOW 300

typedef struct Volume {
    char *path;
    char *title;
    char *ext;
    int64_t mtime;
    int64_t size;
    char *creator;
    char *date;
    char *language;
    char *type;
    char *subject;
    char *coverage;
    char *source;
    char *description;
    char *publisher;
    char *contributor;
    char *format;
    char *identifier;
    char *relation;
    char *rights;
} Volume;

typedef struct Library {
    Volume *volumes;
    int n;
    int total;
    char **roots;
    uint8_t *root_full;
    int nroots;
    /* Counts over every volume the text filter matches, past LIBRARY_SHOW.
     * Each group ignores its own selection and keeps a selected name at 0. */
    Narrow narrow;
} Library;

int index_shelf_ready(Index *ix);
int index_shelf_build(Index *ix, Err *e);
int index_library_list(Index *ix, const char *filter, Library *out, Err *e);
/* language, author, form, period, and genre are newline-separated selections.
 * Empty means that group is open. Several names in one group match any of them. */
int index_library_narrow(Index *ix, const char *filter, const char *language, const char *author, const char *form,
                         const char *period, const char *genre, Library *out, Err *e);
/* sort_col is one of the LIB_SORT_* columns. A column outside that range is
 * the title. sort_desc nonzero reverses the column. Empty fields sort last
 * either way. Letters compare without case. The order is applied before
 * LIBRARY_SHOW, so the rows are the first of the whole match. */
enum {
    LIB_SORT_TITLE = 0,
    LIB_SORT_CREATOR,
    LIB_SORT_DATE,
    LIB_SORT_LANGUAGE,
    LIB_SORT_TYPE,
    LIB_SORT_SUBJECT,
    LIB_SORT_COVERAGE,
    LIB_SORT_SOURCE
};
int index_library_sorted(Index *ix, const char *filter, const char *language, const char *author, const char *form,
                         const char *period, const char *genre, int sort_col, int sort_desc, Library *out, Err *e);
void library_free(Library *lib);
/* rec->path is the volume. The other fields are copied. A title updates the
 * title search uses. The file on disk is left where it is. */
int index_catalog_put(Index *ix, const Volume *rec, Err *e);
/* Remembers the address of a fetched page. The identifier is set to that
 * address when it is still empty and the address fits in the record.
 * A saved identifier is left as it is. */
int index_page_remember(Index *ix, const char *path, const char *url, Err *e);
/* *url is NULL when this volume is not a fetched page. The caller frees it. */
int index_page_url(Index *ix, const char *path, char **url, Err *e);
/* Drops the volume from the index. The file on disk stays. */
int index_library_drop(Index *ix, const char *path, Err *e);

#endif

