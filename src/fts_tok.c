#include "fts_tok.h"

#include "text.h"

#include <stdlib.h>
#include <string.h>

/* FTS3/4 tokenizer vtable. The layout matches sqlite's fts3_tokenizer.h.
 * iVersion 0: xLanguageid is not called. */
typedef struct sqlite3_tokenizer_module sqlite3_tokenizer_module;
typedef struct sqlite3_tokenizer sqlite3_tokenizer;
typedef struct sqlite3_tokenizer_cursor sqlite3_tokenizer_cursor;

struct sqlite3_tokenizer_module {
    int iVersion;
    int (*xCreate)(int argc, const char *const *argv, sqlite3_tokenizer **ppTokenizer);
    int (*xDestroy)(sqlite3_tokenizer *pTokenizer);
    int (*xOpen)(sqlite3_tokenizer *pTokenizer, const char *pInput, int nBytes,
                 sqlite3_tokenizer_cursor **ppCursor);
    int (*xClose)(sqlite3_tokenizer_cursor *pCursor);
    int (*xNext)(sqlite3_tokenizer_cursor *pCursor, const char **ppToken, int *pnBytes, int *piStartOffset,
                 int *piEndOffset, int *piPosition);
};

struct sqlite3_tokenizer {
    const sqlite3_tokenizer_module *pModule;
};

struct sqlite3_tokenizer_cursor {
    sqlite3_tokenizer *pTokenizer;
};

typedef struct GroveTok {
    sqlite3_tokenizer base;
} GroveTok;

typedef struct GroveCur {
    sqlite3_tokenizer_cursor base;
    Tokens toks;
    size_t i;
} GroveCur;

static int grove_create(int argc, const char *const *argv, sqlite3_tokenizer **pp) {
    (void)argc;
    (void)argv;
    GroveTok *t = calloc(1, sizeof *t);
    if (!t) {
        return SQLITE_NOMEM;
    }
    *pp = &t->base;
    return SQLITE_OK;
}

static int grove_destroy(sqlite3_tokenizer *p) {
    free(p);
    return SQLITE_OK;
}

static int grove_open(sqlite3_tokenizer *pTokenizer, const char *input, int nBytes, sqlite3_tokenizer_cursor **pp) {
    (void)pTokenizer;
    if (nBytes < 0) {
        nBytes = input ? (int)strlen(input) : 0;
    }
    GroveCur *c = calloc(1, sizeof *c);
    if (!c) {
        return SQLITE_NOMEM;
    }
    tokens_init(&c->toks);
    if (tokenize(&c->toks, input ? input : "", (size_t)nBytes) != 0) {
        tokens_free(&c->toks);
        free(c);
        return SQLITE_NOMEM;
    }
    *pp = &c->base;
    return SQLITE_OK;
}

static int grove_close(sqlite3_tokenizer_cursor *p) {
    GroveCur *c = (GroveCur *)p;
    if (c) {
        tokens_free(&c->toks);
        free(c);
    }
    return SQLITE_OK;
}

static int grove_next(sqlite3_tokenizer_cursor *p, const char **ppToken, int *pnBytes, int *piStart, int *piEnd,
                      int *piPos) {
    GroveCur *c = (GroveCur *)p;
    if (c->i >= c->toks.n) {
        return SQLITE_DONE;
    }
    Token *t = &c->toks.toks[c->i++];
    *ppToken = c->toks.arena.data + t->off;
    *pnBytes = (int)t->len;
    *piStart = (int)t->start;
    *piEnd = (int)t->end;
    *piPos = (int)t->pos;
    return SQLITE_OK;
}

static const sqlite3_tokenizer_module grove_module = {
    0, grove_create, grove_destroy, grove_open, grove_close, grove_next,
};

static const sqlite3_tokenizer_module *grove_module_ptr = &grove_module;

int grove_fts_register(sqlite3 *db, Err *e) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT fts3_tokenizer(?, ?)", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        err_set(e, "fts4 tokenizer: %s", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(st, 1, "grove", -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, &grove_module_ptr, (int)sizeof grove_module_ptr, SQLITE_STATIC);
    rc = sqlite3_step(st);
    int fin = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || fin != SQLITE_OK) {
        err_set(e, "fts4 tokenizer: %s", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}
