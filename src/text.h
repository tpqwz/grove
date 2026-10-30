#ifndef GROVE_TEXT_H
#define GROVE_TEXT_H

#include "util.h"

#include <stddef.h>
#include <stdint.h>

typedef struct Token {
    uint32_t off;   /* byte offset of the folded token in the arena */
    uint32_t len;
    uint32_t pos;
    uint32_t start; /* source byte start */
    uint32_t end;   /* source byte end, exclusive */
} Token;

typedef struct Tokens {
    Buf arena;
    Token *toks;
    size_t n;
    size_t cap;
} Tokens;

void tokens_init(Tokens *t);
void tokens_free(Tokens *t);
void tokens_clear(Tokens *t);
int tokenize(Tokens *t, const char *s, size_t n);

typedef enum QKind { Q_TERM, Q_PHRASE, Q_NOT, Q_AND, Q_OR } QKind;

typedef struct Query {
    QKind kind;
    char *text;
    size_t len;
    int prefix;
    struct Query *a;
    struct Query *b;
    struct Query **kids;
    int nkids;
} Query;

typedef struct ParsedQuery {
    Query *q;
    char *ext;
    char *path_sub;
    char *title_sub;
    /* mtime_year is 0 when the query has no month. Otherwise both are set. */
    int mtime_year;
    int mtime_mon;
    /* Record filters. Several values of one kind are OR. The kinds combine.
     * Author is the creator, form the type, period the coverage, genre the subject. */
    char **language;
    int nlanguage;
    char **author;
    int nauthor;
    char **form;
    int nform;
    char **period;
    int nperiod;
    char **genre;
    int ngenre;
} ParsedQuery;

int query_parse(const char *s, ParsedQuery *out, Err *e);
void parsed_free(ParsedQuery *p);

typedef struct QTerm {
    char *s;
    size_t len;
    int prefix;
    int phrase; /* 0 is a loose word. A positive id is one quoted phrase, in order. */
} QTerm;

int query_positive_terms(const Query *q, QTerm **out, int *n);
void qterms_free(QTerm *t, int n);

int fold_contains(const char *hay, size_t hn, const char *needle, size_t nn);

/* A location in the extracted text. PAGE carries a 1-based page. HEAD carries
 * the heading words and a level (1 is the outermost). */
enum { MARK_PAGE = 1, MARK_HEAD = 2 };

typedef struct Mark {
    uint32_t offset;
    uint8_t kind;
    uint8_t level;
    uint32_t page;
    char *text;
} Mark;

void marks_free(Mark *m, int n);

/* The paragraph around one match, or the nearby sentences when that paragraph
 * is long. offset is a byte index into the extracted text. page is 1-based
 * when a page mark covers it, or when paged is set and there are no marks
 * (form feeds in an older PDF index). section is the heading stack still open
 * at offset, or NULL. */
typedef struct Passage {
    char *text;
    char *section;
    uint32_t offset;
    int page;
} Passage;

/* One tuple from FTS4 offsets(): column, query-term index, byte start, and
 * byte length in the extracted text. A quoted phrase lists each of its tokens.
 * The list keeps the earliest GROVE_OFFSET_MAX tuples. */
enum { GROVE_OFFSET_MAX = 2048, GROVE_PASSAGE_PAGE = 8 };

typedef struct MatchOff {
    uint32_t start;
    uint32_t len;
    int term;
    int column;
} MatchOff;

/* nmatches counts every place. offsets keeps the earliest GROVE_OFFSET_MAX of
 * them, in file order. Passages are one page of GROVE_PASSAGE_PAGE. When
 * resume is set, clusters at or before `after` are skipped. *more_out is 1
 * when another page remains. Offsets and passages are owned by the caller. */
int make_passages(const char *text, size_t n, const QTerm *terms, int nterms, const Mark *marks, int nmarks, int paged,
                  Passage **out, int *nout, MatchOff **offsets, int *noffsets, int *nmatches, uint32_t after,
                  int resume, int *more_out);
void passages_free(Passage *p, int n);

uint32_t casefold_cp(uint32_t cp);
int cp_is_cjk(uint32_t cp);
int cp_is_letter(uint32_t cp);
int cp_is_digit(uint32_t cp);
int utf8_next(const char *s, size_t n, size_t *i, uint32_t *cp);
int utf8_encode(uint32_t cp, char out[4]);

#endif
