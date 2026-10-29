#include "index.h"

#include "extract.h"
#include "fts_tok.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* One grove.db per index directory. External-content FTS4 stores the body in
 * docs and the inverted index in fts. Ranking reads matchinfo('pcnalx').
 * Places read offsets(). The journal mode is DELETE so the directory stays
 * one copyable database file. */

enum { MX_ERR = 0, MX_POS = 1, MX_NEG = 2, MX_SHAPE = 3 };

struct Index {
    char *dir;
    char *realdir;
    char *db_path;
    sqlite3 *db;
    int writable;
    int quiet;
    int force;
    char **roots;
    int64_t *root_ids;
    uint8_t *root_full;
    uint32_t nroots;
};

typedef struct MExpr {
    int kind;
    char *sql;
} MExpr;

typedef struct Pend {
    int64_t id;
    char *rel;
    int drop;
} Pend;

typedef struct Scope {
    Pend *a;
    int n;
} Scope;

typedef struct Cand {
    int64_t id;
    int64_t root_id;
    double raw;
    int pct;
    char *rel;
    char *title;
    char *ext;
    char *path;
    int64_t mtime;
    MatchInfo mi;
} Cand;

typedef struct Scored {
    int64_t id;
    double score;
    MatchInfo mi;
} Scored;

static int db_fail(Index *ix, Err *e, const char *what) {
    int rc = sqlite3_extended_errcode(ix->db);
    int primary = rc & 0xff;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
        err_set(e, "index is in use");
        return -1;
    }
    err_set(e, "%s: %s", what, sqlite3_errmsg(ix->db));
    return -1;
}

static int sql_exec(Index *ix, const char *sql, Err *e) {
    char *msg = NULL;
    int rc = sqlite3_exec(ix->db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        int primary = rc & 0xff;
        if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
            err_set(e, "index is in use");
        } else {
            err_set(e, "%s", msg ? msg : sqlite3_errmsg(ix->db));
        }
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

static int prep(Index *ix, sqlite3_stmt **st, const char *sql, Err *e) {
    int rc = sqlite3_prepare_v2(ix->db, sql, -1, st, NULL);
    if (rc != SQLITE_OK) {
        return db_fail(ix, e, "sqlite");
    }
    return 0;
}

static void mi_clear(MatchInfo *m) {
    if (!m) {
        return;
    }
    free(m->phrase);
    memset(m, 0, sizeof *m);
}

static int tx_begin(Index *ix, Err *e) {
    return sql_exec(ix, "BEGIN IMMEDIATE", e);
}

static int tx_commit(Index *ix, Err *e) {
    return sql_exec(ix, "COMMIT", e);
}

static void tx_rollback(Index *ix) {
    sqlite3_exec(ix->db, "ROLLBACK", NULL, NULL, NULL);
}

static int path_within(const char *parent, const char *path) {
    size_t n = strlen(parent);
    if (strncmp(parent, path, n) != 0) {
        return 0;
    }
    return path[n] == 0 || path[n] == '/';
}

static int file_there(const char *path) {
    return path && access(path, F_OK) == 0;
}

static int is_old_index(const char *dir) {
    char *catalog = path_join(dir, "catalog");
    char *text = path_join(dir, "text.dat");
    char *seg = path_join(dir, "seg");
    int yes = file_there(catalog) || file_there(text) || file_there(seg);
    free(catalog);
    free(text);
    free(seg);
    return yes;
}

static void roots_clear(Index *ix) {
    for (uint32_t i = 0; i < ix->nroots; i++) {
        free(ix->roots[i]);
    }
    free(ix->roots);
    free(ix->root_ids);
    free(ix->root_full);
    ix->roots = NULL;
    ix->root_ids = NULL;
    ix->root_full = NULL;
    ix->nroots = 0;
}

static int reload_roots(Index *ix, Err *e) {
    sqlite3_stmt *st = NULL;
    roots_clear(ix);
    if (prep(ix, &st, "SELECT id, path, full FROM roots ORDER BY id", e) != 0) {
        return -1;
    }
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        char **nr = realloc(ix->roots, (ix->nroots + 1) * sizeof(char *));
        int64_t *ni = realloc(ix->root_ids, (ix->nroots + 1) * sizeof(int64_t));
        uint8_t *nf = realloc(ix->root_full, (ix->nroots + 1) * sizeof(uint8_t));
        if (!nr || !ni || !nf) {
            free(nr);
            free(ni);
            free(nf);
            sqlite3_finalize(st);
            err_set(e, "out of memory");
            return -1;
        }
        ix->roots = nr;
        ix->root_ids = ni;
        ix->root_full = nf;
        const char *path = (const char *)sqlite3_column_text(st, 1);
        ix->roots[ix->nroots] = xstrdup(path ? path : "");
        ix->root_ids[ix->nroots] = sqlite3_column_int64(st, 0);
        ix->root_full[ix->nroots] = (uint8_t)sqlite3_column_int(st, 2);
        if (!ix->roots[ix->nroots]) {
            sqlite3_finalize(st);
            err_set(e, "out of memory");
            return -1;
        }
        ix->nroots++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return db_fail(ix, e, "roots");
    }
    return 0;
}

static int root_slot(const Index *ix, int64_t id) {
    for (uint32_t i = 0; i < ix->nroots; i++) {
        if (ix->root_ids[i] == id) {
            return (int)i;
        }
    }
    return -1;
}

static int root_exact(const Index *ix, const char *canon) {
    for (uint32_t i = 0; i < ix->nroots; i++) {
        if (strcmp(ix->roots[i], canon) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int root_longest(const Index *ix, const char *canon) {
    int best = -1;
    size_t bestn = 0;
    for (uint32_t i = 0; i < ix->nroots; i++) {
        if (!path_within(ix->roots[i], canon)) {
            continue;
        }
        size_t n = strlen(ix->roots[i]);
        if (n >= bestn) {
            bestn = n;
            best = (int)i;
        }
    }
    return best;
}

static int root_insert(Index *ix, const char *path, int full, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "INSERT INTO roots(path, full) VALUES(?, ?)", e) != 0) {
        return -1;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, full ? 1 : 0);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return db_fail(ix, e, "root");
    }
    int64_t id = sqlite3_last_insert_rowid(ix->db);
    char **nr = realloc(ix->roots, (ix->nroots + 1) * sizeof(char *));
    int64_t *ni = realloc(ix->root_ids, (ix->nroots + 1) * sizeof(int64_t));
    uint8_t *nf = realloc(ix->root_full, (ix->nroots + 1) * sizeof(uint8_t));
    if (!nr || !ni || !nf) {
        err_set(e, "out of memory");
        return -1;
    }
    ix->roots = nr;
    ix->root_ids = ni;
    ix->root_full = nf;
    ix->roots[ix->nroots] = xstrdup(path);
    ix->root_ids[ix->nroots] = id;
    ix->root_full[ix->nroots] = (uint8_t)(full ? 1 : 0);
    if (!ix->roots[ix->nroots]) {
        err_set(e, "out of memory");
        return -1;
    }
    ix->nroots++;
    return (int)(ix->nroots - 1);
}

static int root_set_full(Index *ix, int slot, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "UPDATE roots SET full=1 WHERE id=?", e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, ix->root_ids[slot]);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return db_fail(ix, e, "root");
    }
    ix->root_full[slot] = 1;
    return 0;
}

static const char *base_name(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

static char *ext_dup(const char *rel) {
    const char *base = base_name(rel);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) {
        return xstrdup("");
    }
    char *e = xstrdup(dot + 1);
    if (e) {
        ascii_lower_inplace(e);
    }
    return e;
}

static char *make_title(const char *extracted, const char *rel) {
    if (extracted && extracted[0]) {
        char *copy = xstrdup(extracted);
        if (!copy) {
            return NULL;
        }
        for (char *p = copy; *p; p++) {
            if (*p == '\n' || *p == '\r' || *p == '\t' || *p == '\f') {
                *p = ' ';
            }
        }
        size_t tn = utf8_prefix_bytes(copy, strlen(copy), 180);
        if (tn < strlen(copy)) {
            copy[tn] = 0;
        }
        return copy;
    }
    return xstrdup(base_name(rel));
}

typedef struct Rdr {
    const unsigned char *p;
    const unsigned char *end;
    int err;
} Rdr;

static int rneed(Rdr *r, size_t n) {
    if (r->err || (size_t)(r->end - r->p) < n) {
        r->err = 1;
        return -1;
    }
    return 0;
}

static uint32_t ru32(Rdr *r) {
    if (rneed(r, 4)) {
        return 0;
    }
    uint32_t v = rd_u32(r->p);
    r->p += 4;
    return v;
}

static char *rstr(Rdr *r) {
    uint32_t n = ru32(r);
    if (r->err || n > 10000000 || rneed(r, n)) {
        r->err = 1;
        return NULL;
    }
    char *s = xstrndup((const char *)r->p, n);
    r->p += n;
    if (!s) {
        r->err = 1;
    }
    return s;
}

static void bu32(Buf *b, uint32_t v) {
    unsigned char p[4];
    wr_u32(p, v);
    buf_append(b, p, 4);
}

static void bstr(Buf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    bu32(b, (uint32_t)n);
    if (n) {
        buf_append(b, s, n);
    }
}

static int encode_marks(const Mark *m, int n, unsigned char **out, uint32_t *outn) {
    *out = NULL;
    *outn = 0;
    if (n <= 0) {
        return 0;
    }
    Buf b;
    buf_init(&b);
    bu32(&b, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        bu32(&b, m[i].offset);
        unsigned char kv[4] = {m[i].kind, m[i].level, 0, 0};
        if (buf_append(&b, kv, 4) != 0) {
            buf_free(&b);
            return -1;
        }
        bu32(&b, m[i].page);
        bstr(&b, m[i].text);
    }
    if (!b.data) {
        return -1;
    }
    *out = (unsigned char *)b.data;
    *outn = (uint32_t)b.len;
    return 0;
}

static int decode_marks(const unsigned char *buf, size_t n, Mark **out, int *nout) {
    *out = NULL;
    *nout = 0;
    Rdr r = {buf, buf + n, 0};
    uint32_t count = ru32(&r);
    if (r.err || count > 10000) {
        return -1;
    }
    Mark *m = calloc(count ? count : 1, sizeof(Mark));
    if (!m) {
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        m[i].offset = ru32(&r);
        if (rneed(&r, 4)) {
            break;
        }
        m[i].kind = r.p[0];
        m[i].level = r.p[1];
        r.p += 4;
        m[i].page = ru32(&r);
        m[i].text = rstr(&r);
        if (r.err) {
            break;
        }
    }
    if (r.err) {
        marks_free(m, (int)count);
        return -1;
    }
    if (!count) {
        free(m);
        return 0;
    }
    *out = m;
    *nout = (int)count;
    return 0;
}

static int delete_doc(Index *ix, int64_t id, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "DELETE FROM fts WHERE docid=?", e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return db_fail(ix, e, "index");
    }
    if (prep(ix, &st, "DELETE FROM docs WHERE docid=?", e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return db_fail(ix, e, "index");
    }
    return 0;
}

static int fts_insert(Index *ix, int64_t id, const char *body, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "INSERT INTO fts(docid, body) VALUES(?, ?)", e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, body ? body : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return db_fail(ix, e, "index");
    }
    return 0;
}

static int rel_in_prefix(const char *rel, const char *prefix) {
    if (!prefix || !prefix[0]) {
        return 1;
    }
    size_t n = strlen(prefix);
    if (strcmp(rel, prefix) == 0) {
        return 1;
    }
    return strncmp(rel, prefix, n) == 0 && rel[n] == '/';
}

static int pend_cmp(const void *a, const void *b) {
    const char *x = ((const Pend *)a)->rel;
    const char *y = ((const Pend *)b)->rel;
    return strcmp(x ? x : "", y ? y : "");
}

static void scope_free(Scope *s) {
    if (!s) {
        return;
    }
    for (int i = 0; i < s->n; i++) {
        free(s->a[i].rel);
    }
    free(s->a);
    s->a = NULL;
    s->n = 0;
}

static int scope_load(Index *ix, int64_t root, const char *prefix, Scope *s, Err *e) {
    memset(s, 0, sizeof *s);
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT docid, rel FROM docs WHERE root_id=?", e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, root);
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(st, 1);
        if (!rel_in_prefix(rel ? rel : "", prefix)) {
            continue;
        }
        Pend *p = realloc(s->a, (size_t)(s->n + 1) * sizeof(Pend));
        if (!p) {
            sqlite3_finalize(st);
            scope_free(s);
            err_set(e, "out of memory");
            return -1;
        }
        s->a = p;
        s->a[s->n].id = sqlite3_column_int64(st, 0);
        s->a[s->n].rel = xstrdup(rel ? rel : "");
        s->a[s->n].drop = 1;
        if (!s->a[s->n].rel) {
            sqlite3_finalize(st);
            scope_free(s);
            err_set(e, "out of memory");
            return -1;
        }
        s->n++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        scope_free(s);
        return db_fail(ix, e, "index");
    }
    if (s->n > 1) {
        qsort(s->a, (size_t)s->n, sizeof(Pend), pend_cmp);
    }
    return 0;
}

static void scope_keep(Scope *s, const char *rel) {
    if (!s || !s->n || !rel) {
        return;
    }
    Pend key;
    key.rel = (char *)rel;
    Pend *found = bsearch(&key, s->a, (size_t)s->n, sizeof(Pend), pend_cmp);
    if (found) {
        found->drop = 0;
    }
}

static int scope_sweep(Index *ix, Scope *s, IndexStats *st, Err *e) {
    if (!s) {
        return 0;
    }
    for (int i = 0; i < s->n; i++) {
        if (!s->a[i].drop) {
            continue;
        }
        if (delete_doc(ix, s->a[i].id, e) != 0) {
            return -1;
        }
        if (st) {
            st->removed++;
        }
    }
    return 0;
}

static int bind_doc(sqlite3_stmt *st, int64_t root, const char *rel, const char *title, const char *ext, int64_t mtime,
                    int nsec, int64_t size, const char *body, const void *marks, int marks_n) {
    sqlite3_bind_int64(st, 1, root);
    sqlite3_bind_text(st, 2, rel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, ext ? ext : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, mtime);
    sqlite3_bind_int(st, 6, nsec);
    sqlite3_bind_int64(st, 7, size);
    sqlite3_bind_text(st, 8, body ? body : "", -1, SQLITE_TRANSIENT);
    if (marks && marks_n > 0) {
        sqlite3_bind_blob(st, 9, marks, marks_n, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 9);
    }
    return 0;
}

static int consider_file(Index *ix, int64_t root, const char *rel, const char *abspath, Scope *scope, IndexStats *st,
                         Err *e) {
    struct stat stt;
    if (stat(abspath, &stt) != 0 || !S_ISREG(stt.st_mode)) {
        return 0;
    }
    sqlite3_stmt *stq = NULL;
    if (prep(ix, &stq,
             "SELECT docid, mtime, nsec, size, length(CAST(body AS BLOB)) FROM docs WHERE root_id=? AND rel=?",
             e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(stq, 1, root);
    sqlite3_bind_text(stq, 2, rel, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stq);
    int64_t existing = 0;
    int have = 0;
    int64_t omtime = 0, osize = 0, obytes = 0;
    int onsec = 0;
    if (rc == SQLITE_ROW) {
        have = 1;
        existing = sqlite3_column_int64(stq, 0);
        omtime = sqlite3_column_int64(stq, 1);
        onsec = sqlite3_column_int(stq, 2);
        osize = sqlite3_column_int64(stq, 3);
        obytes = sqlite3_column_int64(stq, 4);
    } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(stq);
        return db_fail(ix, e, "index");
    }
    sqlite3_finalize(stq);
    int64_t sec = (int64_t)stt.st_mtim.tv_sec;
    int nsec = (int)stt.st_mtim.tv_nsec;
    int64_t size = (int64_t)stt.st_size;
    if (have && !ix->force && omtime == sec && onsec == nsec && osize == size && obytes > 0) {
        scope_keep(scope, rel);
        if (st) {
            st->unchanged++;
        }
        return 0;
    }
    Extract ex;
    memset(&ex, 0, sizeof ex);
    ExStatus xs = extract_path(abspath, &ex, e);
    if (xs == EX_ERR) {
        if (!ix->quiet) {
            fprintf(stderr, "grove: %s\n", e->msg[0] ? e->msg : abspath);
        }
        err_clear(e);
        if (have) {
            scope_keep(scope, rel);
        }
        if (st) {
            st->skipped++;
        }
        extract_free(&ex);
        return 0;
    }
    if (xs == EX_SKIP) {
        if (have) {
            if (delete_doc(ix, existing, e) != 0) {
                extract_free(&ex);
                return -1;
            }
            scope_keep(scope, rel);
            if (st) {
                st->removed++;
            }
        } else if (st) {
            st->skipped++;
        }
        extract_free(&ex);
        return 0;
    }
    unsigned char *blob = NULL;
    uint32_t blen = 0;
    if (ex.nmarks > 0 && encode_marks(ex.marks, ex.nmarks, &blob, &blen) != 0) {
        extract_free(&ex);
        err_set(e, "out of memory");
        return -1;
    }
    char *title = make_title(ex.title, rel);
    char *ext = ext_dup(rel);
    const char *body = ex.text ? ex.text : "";
    if (!title || !ext) {
        free(title);
        free(ext);
        free(blob);
        extract_free(&ex);
        err_set(e, "out of memory");
        return -1;
    }
    int64_t id = existing;
    if (have) {
        sqlite3_stmt *del = NULL;
        if (prep(ix, &del, "DELETE FROM fts WHERE docid=?", e) != 0) {
            free(title);
            free(ext);
            free(blob);
            extract_free(&ex);
            return -1;
        }
        sqlite3_bind_int64(del, 1, existing);
        rc = sqlite3_step(del);
        sqlite3_finalize(del);
        if (rc != SQLITE_DONE) {
            free(title);
            free(ext);
            free(blob);
            extract_free(&ex);
            return db_fail(ix, e, "index");
        }
        sqlite3_stmt *up = NULL;
        if (prep(ix, &up,
                 "UPDATE docs SET title=?, ext=?, mtime=?, nsec=?, size=?, body=?, marks=? WHERE docid=?", e) != 0) {
            free(title);
            free(ext);
            free(blob);
            extract_free(&ex);
            return -1;
        }
        sqlite3_bind_text(up, 1, title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 2, ext, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(up, 3, sec);
        sqlite3_bind_int(up, 4, nsec);
        sqlite3_bind_int64(up, 5, size);
        sqlite3_bind_text(up, 6, body, -1, SQLITE_TRANSIENT);
        if (blob && blen) {
            sqlite3_bind_blob(up, 7, blob, (int)blen, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(up, 7);
        }
        sqlite3_bind_int64(up, 8, existing);
        rc = sqlite3_step(up);
        sqlite3_finalize(up);
        if (rc != SQLITE_DONE) {
            free(title);
            free(ext);
            free(blob);
            extract_free(&ex);
            return db_fail(ix, e, "index");
        }
    } else {
        sqlite3_stmt *ins = NULL;
        if (prep(ix, &ins,
                 "INSERT INTO docs(root_id, rel, title, ext, mtime, nsec, size, body, marks) VALUES(?,?,?,?,?,?,?,?,?)",
                 e) != 0) {
            free(title);
            free(ext);
            free(blob);
            extract_free(&ex);
            return -1;
        }
        bind_doc(ins, root, rel, title, ext, sec, nsec, size, body, blob, (int)blen);
        rc = sqlite3_step(ins);
        sqlite3_finalize(ins);
        if (rc != SQLITE_DONE) {
            free(title);
            free(ext);
            free(blob);
            extract_free(&ex);
            return db_fail(ix, e, "index");
        }
        id = sqlite3_last_insert_rowid(ix->db);
    }
    int irc = fts_insert(ix, id, body, e);
    free(title);
    free(ext);
    free(blob);
    extract_free(&ex);
    if (irc != 0) {
        return -1;
    }
    scope_keep(scope, rel);
    if (st) {
        st->indexed++;
    }
    return 0;
}

static int walk(Index *ix, int64_t root, int slot, const char *rel, int depth, Scope *scope, IndexStats *st, Err *e) {
    if (depth > 48) {
        if (!ix->quiet) {
            fprintf(stderr, "grove: not descending past %s (too deep)\n", rel && rel[0] ? rel : ix->roots[slot]);
        }
        return 0;
    }
    char *dirpath = rel && rel[0] ? path_join(ix->roots[slot], rel) : xstrdup(ix->roots[slot]);
    if (!dirpath) {
        err_set(e, "out of memory");
        return -1;
    }
    if (ix->realdir && path_within(ix->realdir, dirpath)) {
        free(dirpath);
        return 0;
    }
    DIR *d = opendir(dirpath);
    if (!d) {
        err_set(e, "cannot open %s: %s", dirpath, strerror(errno));
        free(dirpath);
        return -1;
    }
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char *child = path_join(dirpath, de->d_name);
        char *crel = rel && rel[0] ? path_join(rel, de->d_name) : xstrdup(de->d_name);
        if (!child || !crel) {
            free(child);
            free(crel);
            err_set(e, "out of memory");
            rc = -1;
            break;
        }
        struct stat stt;
        if (lstat(child, &stt) != 0 || S_ISLNK(stt.st_mode)) {
            free(child);
            free(crel);
            continue;
        }
        if (S_ISDIR(stt.st_mode)) {
            rc = walk(ix, root, slot, crel, depth + 1, scope, st, e);
        } else if (S_ISREG(stt.st_mode)) {
            rc = consider_file(ix, root, crel, child, scope, st, e);
        }
        free(child);
        free(crel);
        if (rc != 0) {
            break;
        }
    }
    closedir(d);
    free(dirpath);
    return rc;
}

static int add_inner(Index *ix, const char *path, IndexStats *st, Err *e) {
    char *canon = realpath(path, NULL);
    if (!canon) {
        if (root_exact(ix, path) >= 0) {
            err_set(e, "root %s is not available; rebind it if the drive moved", path);
        } else {
            err_set(e, "cannot resolve %s: %s", path, strerror(errno));
        }
        return -1;
    }
    struct stat stt;
    if (stat(canon, &stt) != 0) {
        err_set(e, "cannot stat %s", canon);
        free(canon);
        return -1;
    }
    int rc = 0;
    if (S_ISDIR(stt.st_mode)) {
        int slot = root_longest(ix, canon);
        char *prefix = NULL;
        if (slot >= 0 && strcmp(ix->roots[slot], canon) != 0) {
            size_t n = strlen(ix->roots[slot]);
            const char *rest = canon + n;
            if (*rest == '/') {
                rest++;
            }
            prefix = xstrdup(rest);
        } else if (slot < 0) {
            slot = root_insert(ix, canon, 1, e);
            prefix = slot >= 0 ? xstrdup("") : NULL;
        } else {
            if (root_set_full(ix, slot, e) != 0) {
                free(canon);
                return -1;
            }
            prefix = xstrdup("");
        }
        if (!prefix || slot < 0) {
            free(prefix);
            free(canon);
            if (!e->msg[0]) {
                err_set(e, "out of memory");
            }
            return -1;
        }
        if (access(ix->roots[slot], F_OK) != 0) {
            err_set(e, "root %s is not available; rebind it if the drive moved", ix->roots[slot]);
            free(prefix);
            free(canon);
            return -1;
        }
        Scope scope;
        if (scope_load(ix, ix->root_ids[slot], prefix, &scope, e) != 0) {
            free(prefix);
            free(canon);
            return -1;
        }
        rc = walk(ix, ix->root_ids[slot], slot, prefix, 0, &scope, st, e);
        if (rc == 0) {
            rc = scope_sweep(ix, &scope, st, e);
        }
        scope_free(&scope);
        free(prefix);
    } else if (S_ISREG(stt.st_mode)) {
        char *parent = xstrdup(canon);
        char *slash = parent ? strrchr(parent, '/') : NULL;
        const char *base = slash ? slash + 1 : canon;
        if (slash && slash != parent) {
            *slash = 0;
        }
        int slot = root_longest(ix, canon);
        char *rel = NULL;
        if (slot >= 0) {
            size_t n = strlen(ix->roots[slot]);
            const char *rest = canon + n;
            if (*rest == '/') {
                rest++;
            }
            rel = xstrdup(rest[0] ? rest : base);
        } else {
            slot = root_insert(ix, parent ? parent : canon, 0, e);
            rel = slot >= 0 ? xstrdup(base) : NULL;
        }
        free(parent);
        if (!rel || slot < 0) {
            free(rel);
            free(canon);
            if (!e->msg[0]) {
                err_set(e, "out of memory");
            }
            return -1;
        }
        rc = consider_file(ix, ix->root_ids[slot], rel, canon, NULL, st, e);
        free(rel);
    } else {
        err_set(e, "%s is not a file or directory", canon);
        rc = -1;
    }
    free(canon);
    return rc;
}

int index_add(Index *ix, const char *path, IndexStats *st, Err *e) {
    if (!ix->writable) {
        err_set(e, "index is open read-only");
        return -1;
    }
    if (tx_begin(ix, e) != 0) {
        return -1;
    }
    int rc = add_inner(ix, path, st, e);
    if (rc == 0) {
        rc = tx_commit(ix, e);
    }
    if (rc != 0) {
        tx_rollback(ix);
        Err ignore;
        err_clear(&ignore);
        reload_roots(ix, &ignore);
    }
    return rc;
}

int index_reindex(Index *ix, IndexStats *st, Err *e) {
    if (ix->nroots == 0) {
        err_set(e, "no directories have been indexed yet");
        return -1;
    }
    char **copy = calloc(ix->nroots, sizeof(char *));
    uint8_t *full = malloc(ix->nroots);
    if (!copy || !full) {
        free(copy);
        free(full);
        err_set(e, "out of memory");
        return -1;
    }
    uint32_t n = ix->nroots;
    for (uint32_t i = 0; i < n; i++) {
        copy[i] = xstrdup(ix->roots[i]);
        full[i] = ix->root_full[i];
    }
    int rc = 0;
    for (uint32_t i = 0; i < n && rc == 0; i++) {
        if (!full[i] || !copy[i]) {
            if (full[i] && !copy[i]) {
                err_set(e, "out of memory");
                rc = -1;
            }
            continue;
        }
        rc = index_add(ix, copy[i], st, e);
    }
    for (uint32_t i = 0; i < n; i++) {
        free(copy[i]);
    }
    free(copy);
    free(full);
    return rc;
}

int index_rebind(Index *ix, const char *from, const char *to, Err *e) {
    if (!ix->writable) {
        err_set(e, "index is open read-only");
        return -1;
    }
    char *np = realpath(to, NULL);
    if (!np) {
        err_set(e, "cannot resolve %s: %s", to, strerror(errno));
        return -1;
    }
    char *op = realpath(from, NULL);
    const char *key = op ? op : from;
    int slot = root_exact(ix, key);
    if (slot < 0) {
        slot = root_exact(ix, from);
    }
    if (slot < 0) {
        free(np);
        free(op);
        err_set(e, "no indexed root matches %s", from);
        return -1;
    }
    int other = root_exact(ix, np);
    if (other >= 0 && other != slot) {
        free(np);
        free(op);
        err_set(e, "%s is already an indexed root", to);
        return -1;
    }
    if (tx_begin(ix, e) != 0) {
        free(np);
        free(op);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int rc = prep(ix, &st, "UPDATE roots SET path=? WHERE id=?", e);
    if (rc == 0) {
        sqlite3_bind_text(st, 1, np, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, ix->root_ids[slot]);
        int step = sqlite3_step(st);
        sqlite3_finalize(st);
        if (step != SQLITE_DONE) {
            rc = db_fail(ix, e, "rebind");
        }
    }
    if (rc == 0) {
        rc = tx_commit(ix, e);
    }
    if (rc != 0) {
        tx_rollback(ix);
        free(np);
        free(op);
        return -1;
    }
    free(ix->roots[slot]);
    ix->roots[slot] = np;
    free(op);
    return 0;
}

int index_purge(Index *ix, IndexStats *st, Err *e) {
    if (!ix->writable) {
        err_set(e, "index is open read-only");
        return -1;
    }
    for (uint32_t r = 0; r < ix->nroots; r++) {
        if (access(ix->roots[r], F_OK) != 0 && !ix->quiet) {
            fprintf(stderr, "grove: keeping files under missing root %s\n", ix->roots[r]);
        }
    }
    if (tx_begin(ix, e) != 0) {
        return -1;
    }
    int rc = 0;
    for (uint32_t r = 0; rc == 0 && r < ix->nroots; r++) {
        if (access(ix->roots[r], F_OK) != 0) {
            continue;
        }
        sqlite3_stmt *stq = NULL;
        if (prep(ix, &stq, "SELECT docid, rel FROM docs WHERE root_id=?", e) != 0) {
            rc = -1;
            break;
        }
        sqlite3_bind_int64(stq, 1, ix->root_ids[r]);
        int step = 0;
        while ((step = sqlite3_step(stq)) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(stq, 0);
            const char *rel = (const char *)sqlite3_column_text(stq, 1);
            char *p = path_join(ix->roots[r], rel ? rel : "");
            struct stat stt;
            int gone = !p || stat(p, &stt) != 0;
            free(p);
            if (!gone) {
                continue;
            }
            if (delete_doc(ix, id, e) != 0) {
                rc = -1;
                break;
            }
            if (st) {
                st->removed++;
            }
        }
        sqlite3_finalize(stq);
        if (rc == 0 && step != SQLITE_DONE) {
            rc = db_fail(ix, e, "purge");
        }
    }
    if (rc == 0) {
        rc = tx_commit(ix, e);
    }
    if (rc != 0) {
        tx_rollback(ix);
    }
    return rc;
}

int index_compact(Index *ix, Err *e) {
    if (!ix->writable) {
        err_set(e, "index is open read-only");
        return -1;
    }
    if (sql_exec(ix, "INSERT INTO fts(fts) VALUES('rebuild')", e) != 0) {
        return -1;
    }
    if (sql_exec(ix, "VACUUM", e) != 0) {
        return -1;
    }
    return 0;
}

int index_info(Index *ix, IndexInfo *info, Err *e) {
    memset(info, 0, sizeof *info);
    info->dir = ix->dir;
    info->n_segments = 1;
    info->n_roots = ix->nroots;
    info->roots = (const char *const *)ix->roots;
    info->root_full = ix->root_full;
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT COUNT(*), COALESCE(SUM(length(CAST(body AS BLOB))), 0) FROM docs", e) != 0) {
        return -1;
    }
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        info->n_docs = (uint32_t)sqlite3_column_int64(st, 0);
        info->text_bytes = (uint64_t)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        return db_fail(ix, e, "status");
    }
    return 0;
}

static void mx_free(MExpr *m) {
    free(m->sql);
    m->sql = NULL;
    m->kind = MX_ERR;
}

static MExpr mx_make(int kind, char *sql) {
    MExpr m;
    m.kind = sql ? kind : MX_ERR;
    m.sql = sql;
    return m;
}

static char *mx_wrap(const char *a, const char *op, const char *b) {
    size_t n = strlen(a) + strlen(b) + strlen(op) + 8;
    char *s = malloc(n);
    if (!s) {
        return NULL;
    }
    snprintf(s, n, "(%s) %s (%s)", a, op, b);
    return s;
}

static MExpr emit_query(const Query *q) {
    if (!q) {
        return (MExpr){MX_SHAPE, NULL};
    }
    if (q->kind == Q_TERM || q->kind == Q_PHRASE) {
        Buf b;
        buf_init(&b);
        int rc = 0;
        /* FTS4 sets the prefix flag only when '*' is the next source byte
         * after the token, inside the quoted string. "port"* does not. */
        rc = buf_putc(&b, '"');
        if (q->kind == Q_TERM) {
            const char *s = q->text ? q->text : "";
            size_t n = q->len;
            for (size_t k = 0; rc == 0 && k < n; k++) {
                if (s[k] == '"') {
                    rc = buf_putc(&b, '"');
                }
                if (rc == 0) {
                    rc = buf_putc(&b, (unsigned char)s[k]);
                }
            }
            if (rc == 0 && q->prefix) {
                rc = buf_putc(&b, '*');
            }
        } else {
            for (int i = 0; rc == 0 && i < q->nkids; i++) {
                if (i) {
                    rc = buf_putc(&b, ' ');
                }
                const char *s = q->kids[i] && q->kids[i]->text ? q->kids[i]->text : "";
                size_t n = q->kids[i] ? q->kids[i]->len : 0;
                for (size_t k = 0; rc == 0 && k < n; k++) {
                    if (s[k] == '"') {
                        rc = buf_putc(&b, '"');
                    }
                    if (rc == 0) {
                        rc = buf_putc(&b, (unsigned char)s[k]);
                    }
                }
                if (rc == 0 && i + 1 == q->nkids && q->kids[i] && q->kids[i]->prefix) {
                    rc = buf_putc(&b, '*');
                }
            }
        }
        if (rc == 0) {
            rc = buf_putc(&b, '"');
        }
        if (rc != 0 || !b.data) {
            buf_free(&b);
            return (MExpr){MX_ERR, NULL};
        }
        return (MExpr){MX_POS, b.data};
    }
    if (q->kind == Q_NOT) {
        MExpr inner = emit_query(q->a);
        if (inner.kind == MX_POS) {
            inner.kind = MX_NEG;
        } else if (inner.kind == MX_NEG) {
            inner.kind = MX_POS;
        }
        return inner;
    }
    MExpr L = emit_query(q->a);
    MExpr R = emit_query(q->b);
    if (L.kind == MX_ERR || R.kind == MX_ERR) {
        mx_free(&L);
        mx_free(&R);
        return (MExpr){MX_ERR, NULL};
    }
    if (L.kind == MX_SHAPE || R.kind == MX_SHAPE || L.kind == MX_ERR || R.kind == MX_ERR) {
        mx_free(&L);
        mx_free(&R);
        return (MExpr){MX_SHAPE, NULL};
    }
    const char *op = NULL;
    int kind = MX_POS;
    if (q->kind == Q_AND) {
        if (L.kind == MX_POS && R.kind == MX_POS) {
            op = "AND";
        } else if (L.kind == MX_POS && R.kind == MX_NEG) {
            op = "NOT";
        } else if (L.kind == MX_NEG && R.kind == MX_POS) {
            MExpr tmp = L;
            L = R;
            R = tmp;
            op = "NOT";
        } else {
            op = "OR";
            kind = MX_NEG;
        }
    } else if (q->kind == Q_OR) {
        if (L.kind == MX_POS && R.kind == MX_POS) {
            op = "OR";
        } else if (L.kind == MX_NEG && R.kind == MX_NEG) {
            op = "AND";
            kind = MX_NEG;
        } else {
            mx_free(&L);
            mx_free(&R);
            return (MExpr){MX_SHAPE, NULL};
        }
    } else {
        mx_free(&L);
        mx_free(&R);
        return (MExpr){MX_SHAPE, NULL};
    }
    char *sql = mx_wrap(L.sql, op, R.sql);
    mx_free(&L);
    mx_free(&R);
    return mx_make(kind, sql);
}

static int parse_matchinfo(const void *blob, int nbytes, MatchInfo *m) {
    memset(m, 0, sizeof *m);
    if (!blob || nbytes < (int)(3 * sizeof(uint32_t))) {
        return 0;
    }
    const uint32_t *u = blob;
    int n = nbytes / (int)sizeof(uint32_t);
    uint32_t p = u[0];
    uint32_t c = u[1] ? u[1] : 1;
    uint64_t need = 3ull + (uint64_t)c + (uint64_t)c + 3ull * (uint64_t)c * (uint64_t)p;
    if ((uint64_t)n < need) {
        return -1;
    }
    m->phrases = p;
    m->columns = u[1];
    m->rows = u[2];
    m->avg_tokens = u[3];
    m->tokens = u[3 + c];
    if (p == 0) {
        return 0;
    }
    m->phrase = calloc(p, sizeof(PhraseStat));
    if (!m->phrase) {
        return -1;
    }
    m->nphrase = (int)p;
    const uint32_t *x = u + 3 + c + c;
    for (uint32_t i = 0; i < p; i++) {
        const uint32_t *cell = x + (size_t)i * c * 3;
        m->phrase[i].hits = cell[0];
        m->phrase[i].total = cell[1];
        m->phrase[i].docs = cell[2];
    }
    return 0;
}

static double bm25_of(const MatchInfo *m) {
    if (!m || !m->rows || m->nphrase <= 0) {
        return 0;
    }
    double adl = m->avg_tokens > 0 ? (double)m->avg_tokens : 1;
    double dl = (double)m->tokens;
    double sum = 0;
    const double k = 1.2;
    const double b = 0.75;
    for (int i = 0; i < m->nphrase; i++) {
        uint32_t tf = m->phrase[i].hits;
        uint32_t df = m->phrase[i].docs;
        if (!tf) {
            continue;
        }
        double idf = log(((double)m->rows - (double)df + 0.5) / ((double)df + 0.5));
        if (idf < 0.05) {
            idf = 0.05;
        }
        double norm = k * (1.0 - b + b * (dl / adl));
        sum += idf * ((double)tf * (k + 1.0)) / ((double)tf + norm);
    }
    return sum;
}

static int parse_offsets(const char *s, MatchOff **out, int *nout, int *nmatches) {
    *out = NULL;
    *nout = 0;
    *nmatches = 0;
    if (!s || !s[0]) {
        return 0;
    }
    MatchOff *arr = NULL;
    int n = 0;
    int cap = 0;
    int total = 0;
    const char *p = s;
    while (*p) {
        char *end = NULL;
        long col = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        long term = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        long start = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        long len = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        total++;
        if (start < 0 || len < 0 || start > UINT32_MAX || len > UINT32_MAX || n >= GROVE_OFFSET_MAX) {
            continue;
        }
        if (n == cap) {
            int nc = cap ? cap * 2 : 32;
            if (nc > GROVE_OFFSET_MAX) {
                nc = GROVE_OFFSET_MAX;
            }
            MatchOff *np = realloc(arr, (size_t)nc * sizeof(MatchOff));
            if (!np) {
                free(arr);
                return -1;
            }
            arr = np;
            cap = nc;
        }
        arr[n].column = (int)col;
        arr[n].term = (int)term;
        arr[n].start = (uint32_t)start;
        arr[n].len = (uint32_t)len;
        n++;
    }
    *out = arr;
    *nout = n;
    *nmatches = total;
    return 0;
}

static char *display_path(Index *ix, int64_t root_id, const char *rel, const char *at) {
    int slot = root_slot(ix, root_id);
    const char *root = slot >= 0 ? ix->roots[slot] : "";
    const char *use = root;
    if (at && *at) {
        const char *eq = strchr(at, '=');
        if (eq) {
            size_t n = (size_t)(eq - at);
            if (strlen(root) == n && memcmp(root, at, n) == 0) {
                use = eq + 1;
            }
        } else if (ix->nroots == 1) {
            use = at;
        }
    }
    return path_join(use, rel ? rel : "");
}

static void free_key_array(char **keys, int n) {
    if (!keys) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(keys[i]);
    }
    free(keys);
}

static int push_key(char ***arr, int *n, int *cap, char *owned) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        char **p = realloc(*arr, (size_t)nc * sizeof(*p));
        if (!p) {
            free(owned);
            return -1;
        }
        *arr = p;
        *cap = nc;
    }
    (*arr)[(*n)++] = owned;
    return 0;
}

static int parent_folder(const char *rel, char **out) {
    *out = NULL;
    if (!rel || !rel[0]) {
        return 0;
    }
    const char *slash = strrchr(rel, '/');
    if (!slash || slash == rel) {
        return 0;
    }
    size_t n = (size_t)(slash - rel) + 1;
    char *s = malloc(n + 1);
    if (!s) {
        return -1;
    }
    memcpy(s, rel, n);
    s[n] = 0;
    *out = s;
    return 0;
}

static int key_ptr_cmp(const void *a, const void *b) {
    const char *x = *(char *const *)a;
    const char *y = *(char *const *)b;
    return strcmp(x ? x : "", y ? y : "");
}

static int facet_cmp(const void *a, const void *b) {
    const Facet *x = a;
    const Facet *y = b;
    if (x->count != y->count) {
        return (y->count > x->count) - (y->count < x->count);
    }
    return strcmp(x->value ? x->value : "", y->value ? y->value : "");
}

static int keys_to_facets(char **keys, int n, Facet **out, int *nout) {
    *out = NULL;
    *nout = 0;
    if (n <= 0) {
        return 0;
    }
    qsort(keys, (size_t)n, sizeof(char *), key_ptr_cmp);
    int groups = 1;
    for (int i = 1; i < n; i++) {
        if (strcmp(keys[i] ? keys[i] : "", keys[i - 1] ? keys[i - 1] : "") != 0) {
            groups++;
        }
    }
    Facet *f = calloc((size_t)groups, sizeof(Facet));
    if (!f) {
        return -1;
    }
    int nf = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && strcmp(keys[i], keys[j]) == 0) {
            j++;
        }
        f[nf].value = keys[i];
        keys[i] = NULL;
        f[nf].count = j - i;
        for (int k = i + 1; k < j; k++) {
            free(keys[k]);
            keys[k] = NULL;
        }
        nf++;
        i = j;
    }
    qsort(f, (size_t)nf, sizeof(Facet), facet_cmp);
    *out = f;
    *nout = nf;
    return 0;
}

void facets_free(Facets *f) {
    if (!f) {
        return;
    }
    for (int i = 0; i < f->ntypes; i++) {
        free(f->types[i].value);
    }
    for (int i = 0; i < f->nfolders; i++) {
        free(f->folders[i].value);
    }
    for (int i = 0; i < f->nmonths; i++) {
        free(f->months[i].value);
    }
    free(f->types);
    free(f->folders);
    free(f->months);
    memset(f, 0, sizeof *f);
}

static int same_month(int64_t sec, int year, int mon) {
    time_t t = (time_t)sec;
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        return 0;
    }
    return tm.tm_year + 1900 == year && tm.tm_mon + 1 == mon;
}

static char *month_key(int64_t sec) {
    time_t t = (time_t)sec;
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        return NULL;
    }
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
    if (n < 7 || (size_t)n >= sizeof buf) {
        return NULL;
    }
    return xstrdup(buf);
}

static int doc_passes(const char *rel, const char *title, const char *ext, int64_t mtime, const ParsedQuery *pq) {
    if (pq->ext && !ascii_ieq(ext ? ext : "", pq->ext)) {
        return 0;
    }
    if (pq->path_sub && !fold_contains(rel ? rel : "", rel ? strlen(rel) : 0, pq->path_sub, strlen(pq->path_sub))) {
        return 0;
    }
    if (pq->title_sub &&
        !fold_contains(title ? title : "", title ? strlen(title) : 0, pq->title_sub, strlen(pq->title_sub))) {
        return 0;
    }
    if (pq->mtime_year && !same_month(mtime, pq->mtime_year, pq->mtime_mon)) {
        return 0;
    }
    return 1;
}

static void cand_free(Cand *c) {
    free(c->rel);
    free(c->title);
    free(c->ext);
    free(c->path);
    mi_clear(&c->mi);
    memset(c, 0, sizeof *c);
}

static void cands_free(Cand *c, int n) {
    for (int i = 0; i < n; i++) {
        cand_free(&c[i]);
    }
    free(c);
}

static int cand_push(Cand **arr, int *n, int *cap, Cand *src) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 32;
        Cand *p = realloc(*arr, (size_t)nc * sizeof(Cand));
        if (!p) {
            return -1;
        }
        *arr = p;
        *cap = nc;
    }
    (*arr)[(*n)++] = *src;
    memset(src, 0, sizeof *src);
    return 0;
}

static char *col_text(sqlite3_stmt *st, int i) {
    const char *p = (const char *)sqlite3_column_text(st, i);
    return xstrdup(p ? p : "");
}

static int take_row(sqlite3_stmt *st, int with_mi, double raw, Cand *c, Err *e) {
    memset(c, 0, sizeof *c);
    c->id = sqlite3_column_int64(st, 0);
    c->root_id = sqlite3_column_int64(st, 1);
    c->rel = col_text(st, 2);
    c->title = col_text(st, 3);
    c->ext = col_text(st, 4);
    c->mtime = sqlite3_column_int64(st, 5);
    c->raw = raw;
    if (!c->rel || !c->title || !c->ext) {
        cand_free(c);
        err_set(e, "out of memory");
        return -1;
    }
    if (with_mi) {
        const void *blob = sqlite3_column_blob(st, 6);
        int n = sqlite3_column_bytes(st, 6);
        if (parse_matchinfo(blob, n, &c->mi) != 0) {
            cand_free(c);
            err_set(e, "matchinfo is short");
            return -1;
        }
        c->raw = bm25_of(&c->mi);
    }
    return 0;
}

static int load_offsets(Index *ix, int64_t id, const char *match, MatchOff **out, int *nout, int *nmatches, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT offsets(fts) FROM fts WHERE docid=? AND fts MATCH ?", e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, match, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    int pr = 0;
    if (rc == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        pr = parse_offsets(s, out, nout, nmatches);
    } else if (rc == SQLITE_DONE) {
        *out = NULL;
        *nout = 0;
        *nmatches = 0;
    }
    sqlite3_finalize(st);
    if (pr != 0) {
        err_set(e, "out of memory");
        return -1;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        return db_fail(ix, e, "offsets");
    }
    return 0;
}

static int load_body(Index *ix, int64_t id, char **text, Mark **marks, int *nmarks, Err *e) {
    *text = NULL;
    *marks = NULL;
    *nmarks = 0;
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT body, marks FROM docs WHERE docid=?", e) != 0) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        if (rc == SQLITE_DONE) {
            err_set(e, "document disappeared");
        } else {
            db_fail(ix, e, "document");
        }
        return -1;
    }
    const void *tb = sqlite3_column_blob(st, 0);
    int tn = sqlite3_column_bytes(st, 0);
    if (tn < 0) {
        tn = 0;
    }
    char *copy = malloc((size_t)tn + 1);
    if (!copy) {
        sqlite3_finalize(st);
        err_set(e, "out of memory");
        return -1;
    }
    if (tn && tb) {
        memcpy(copy, tb, (size_t)tn);
    }
    copy[tn] = 0;
    const void *mb = sqlite3_column_blob(st, 1);
    int mn = sqlite3_column_bytes(st, 1);
    unsigned char *mcopy = NULL;
    if (mb && mn > 0) {
        mcopy = malloc((size_t)mn);
        if (!mcopy) {
            free(copy);
            sqlite3_finalize(st);
            err_set(e, "out of memory");
            return -1;
        }
        memcpy(mcopy, mb, (size_t)mn);
    }
    sqlite3_finalize(st);
    if (mcopy && decode_marks(mcopy, (size_t)mn, marks, nmarks) != 0) {
        free(mcopy);
        free(copy);
        err_set(e, "marks are corrupt");
        return -1;
    }
    free(mcopy);
    *text = copy;
    return 0;
}

static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a;
    const Cand *y = b;
    if (x->pct != y->pct) {
        return (y->pct > x->pct) - (y->pct < x->pct);
    }
    return strcmp(x->path ? x->path : "", y->path ? y->path : "");
}

static int finish_search(Index *ix, Cand *cands, int nc, const ParsedQuery *pq, int limit, const char *at,
                         const char *match, Hit **hits, int *nhits, Facets *facets, Err *e) {
    char **type_keys = NULL;
    int ntype_keys = 0;
    int type_cap = 0;
    char **folder_keys = NULL;
    int nfolder_keys = 0;
    int folder_cap = 0;
    char **month_keys = NULL;
    int nmonth_keys = 0;
    int month_cap = 0;
    int w = 0;
    for (int i = 0; i < nc; i++) {
        if (!doc_passes(cands[i].rel, cands[i].title, cands[i].ext, cands[i].mtime, pq)) {
            cand_free(&cands[i]);
            continue;
        }
        cands[i].path = display_path(ix, cands[i].root_id, cands[i].rel, at);
        if (!cands[i].path) {
            err_set(e, "out of memory");
            goto fail;
        }
        if (facets) {
            if (cands[i].ext && cands[i].ext[0]) {
                char *copy = xstrdup(cands[i].ext);
                if (!copy || push_key(&type_keys, &ntype_keys, &type_cap, copy) != 0) {
                    err_set(e, "out of memory");
                    goto fail;
                }
            }
            char *folder = NULL;
            if (parent_folder(cands[i].rel, &folder) != 0 ||
                (folder && push_key(&folder_keys, &nfolder_keys, &folder_cap, folder) != 0)) {
                err_set(e, "out of memory");
                goto fail;
            }
            char *month = month_key(cands[i].mtime);
            if (!month || push_key(&month_keys, &nmonth_keys, &month_cap, month) != 0) {
                free(month);
                err_set(e, "out of memory");
                goto fail;
            }
        }
        cands[w++] = cands[i];
        if (w - 1 != i) {
            memset(&cands[i], 0, sizeof cands[i]);
        }
    }
    nc = w;
    double best = 0;
    for (int i = 0; i < nc; i++) {
        if (cands[i].raw > best) {
            best = cands[i].raw;
        }
    }
    for (int i = 0; i < nc; i++) {
        int pct = best > 0 ? (int)(100.0 * cands[i].raw / best + 0.5) : 100;
        if (pct < 1) {
            pct = 1;
        }
        if (pct > 100) {
            pct = 100;
        }
        cands[i].pct = pct;
    }
    if (nc > 1) {
        qsort(cands, (size_t)nc, sizeof(Cand), cand_cmp);
    }
    int nfiles = nc > 1000000000 ? 1000000000 : nc;
    if (nc > limit) {
        for (int i = limit; i < nc; i++) {
            cand_free(&cands[i]);
        }
        nc = limit;
    }
    QTerm *terms = NULL;
    int nt = 0;
    if (pq->q && query_positive_terms(pq->q, &terms, &nt) != 0) {
        err_set(e, "out of memory");
        goto fail;
    }
    Hit *arr = calloc(nc ? (size_t)nc : 1, sizeof(Hit));
    if (!arr) {
        qterms_free(terms, nt);
        err_set(e, "out of memory");
        goto fail;
    }
    for (int i = 0; i < nc; i++) {
        char *text = NULL;
        Mark *marks = NULL;
        int nmarks = 0;
        if (load_body(ix, cands[i].id, &text, &marks, &nmarks, e) != 0) {
            hits_free(arr, i);
            qterms_free(terms, nt);
            goto fail;
        }
        Passage *ps = NULL;
        int np = 0;
        MatchOff *offs = NULL;
        int noffs = 0;
        int nmatches = 0;
        int more = 0;
        int paged = cands[i].ext && ascii_ieq(cands[i].ext, "pdf");
        if (make_passages(text, strlen(text), terms, nt, marks, nmarks, paged, &ps, &np, &offs, &noffs, &nmatches, 0,
                          0, &more) != 0) {
            marks_free(marks, nmarks);
            free(text);
            hits_free(arr, i);
            qterms_free(terms, nt);
            err_set(e, "out of memory");
            goto fail;
        }
        marks_free(marks, nmarks);
        free(text);
        if (match) {
            MatchOff *fo = NULL;
            int nfo = 0;
            int nall = 0;
            if (load_offsets(ix, cands[i].id, match, &fo, &nfo, &nall, e) != 0) {
                free(offs);
                passages_free(ps, np);
                hits_free(arr, i);
                qterms_free(terms, nt);
                goto fail;
            }
            free(offs);
            offs = fo;
            noffs = nfo;
            nmatches = nall;
        }
        arr[i].path = cands[i].path;
        arr[i].title = cands[i].title;
        arr[i].ext = cands[i].ext;
        cands[i].path = NULL;
        cands[i].title = NULL;
        cands[i].ext = NULL;
        arr[i].score = cands[i].pct;
        arr[i].offsets = offs;
        arr[i].noffsets = noffs;
        arr[i].nmatches = nmatches;
        arr[i].matchinfo = cands[i].mi;
        memset(&cands[i].mi, 0, sizeof cands[i].mi);
        arr[i].passages = ps;
        arr[i].npassages = np;
        arr[i].more = more;
        arr[i].snippet = np ? ps[0].text : NULL;
    }
    qterms_free(terms, nt);
    if (facets) {
        memset(facets, 0, sizeof *facets);
        facets->nfiles = nfiles;
        if (keys_to_facets(type_keys, ntype_keys, &facets->types, &facets->ntypes) != 0 ||
            keys_to_facets(folder_keys, nfolder_keys, &facets->folders, &facets->nfolders) != 0 ||
            keys_to_facets(month_keys, nmonth_keys, &facets->months, &facets->nmonths) != 0) {
            free_key_array(type_keys, ntype_keys);
            free_key_array(folder_keys, nfolder_keys);
            free_key_array(month_keys, nmonth_keys);
            hits_free(arr, nc);
            cands_free(cands, nc);
            err_set(e, "out of memory");
            return -1;
        }
    }
    free_key_array(type_keys, ntype_keys);
    free_key_array(folder_keys, nfolder_keys);
    free_key_array(month_keys, nmonth_keys);
    cands_free(cands, nc);
    *hits = arr;
    *nhits = nc;
    return 0;
fail:
    free_key_array(type_keys, ntype_keys);
    free_key_array(folder_keys, nfolder_keys);
    free_key_array(month_keys, nmonth_keys);
    cands_free(cands, nc);
    return -1;
}

static int query_rows(Index *ix, const char *sql, const char *match, const char *ext, int with_mi, double raw,
                      Cand **out, int *nout, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, sql, e) != 0) {
        return -1;
    }
    int bind = 1;
    if (match) {
        sqlite3_bind_text(st, bind++, match, -1, SQLITE_TRANSIENT);
    }
    if (ext) {
        sqlite3_bind_text(st, bind++, ext, -1, SQLITE_TRANSIENT);
    }
    Cand *arr = NULL;
    int n = 0;
    int cap = 0;
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        Cand c;
        if (take_row(st, with_mi, raw, &c, e) != 0 || cand_push(&arr, &n, &cap, &c) != 0) {
            cand_free(&c);
            sqlite3_finalize(st);
            cands_free(arr, n);
            if (!e->msg[0]) {
                err_set(e, "out of memory");
            }
            return -1;
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        cands_free(arr, n);
        return db_fail(ix, e, "search");
    }
    *out = arr;
    *nout = n;
    return 0;
}

static const char *MATCH_SQL =
    "SELECT d.docid, d.root_id, d.rel, d.title, d.ext, d.mtime, matchinfo(fts, 'pcnalx') "
    "FROM fts JOIN docs d ON d.docid = fts.docid WHERE fts MATCH ?";
static const char *MATCH_EXT_SQL =
    "SELECT d.docid, d.root_id, d.rel, d.title, d.ext, d.mtime, matchinfo(fts, 'pcnalx') "
    "FROM fts JOIN docs d ON d.docid = fts.docid WHERE fts MATCH ? AND d.ext=?";
static const char *ALL_SQL = "SELECT docid, root_id, rel, title, ext, mtime FROM docs";
static const char *ALL_EXT_SQL = "SELECT docid, root_id, rel, title, ext, mtime FROM docs WHERE ext=?";
static const char *EXCLUDE_SQL =
    "SELECT docid, root_id, rel, title, ext, mtime FROM docs WHERE docid NOT IN "
    "(SELECT docid FROM fts WHERE fts MATCH ?)";
static const char *EXCLUDE_EXT_SQL =
    "SELECT docid, root_id, rel, title, ext, mtime FROM docs WHERE docid NOT IN "
    "(SELECT docid FROM fts WHERE fts MATCH ?) AND ext=?";

static void scored_free(Scored *a, int n) {
    for (int i = 0; i < n; i++) {
        mi_clear(&a[i].mi);
    }
    free(a);
}

static int scored_cmp(const void *a, const void *b) {
    int64_t x = ((const Scored *)a)->id;
    int64_t y = ((const Scored *)b)->id;
    return (x > y) - (x < y);
}

static int scored_push(Scored **arr, int *n, int *cap, Scored *src) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 32;
        Scored *p = realloc(*arr, (size_t)nc * sizeof(Scored));
        if (!p) {
            return -1;
        }
        *arr = p;
        *cap = nc;
    }
    (*arr)[(*n)++] = *src;
    memset(src, 0, sizeof *src);
    return 0;
}

static int match_ids(Index *ix, const char *match, Scored **out, int *nout, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT docid, matchinfo(fts, 'pcnalx') FROM fts WHERE fts MATCH ?", e) != 0) {
        return -1;
    }
    sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
    Scored *arr = NULL;
    int n = 0;
    int cap = 0;
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        Scored s;
        memset(&s, 0, sizeof s);
        s.id = sqlite3_column_int64(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int nb = sqlite3_column_bytes(st, 1);
        if (parse_matchinfo(blob, nb, &s.mi) != 0) {
            sqlite3_finalize(st);
            scored_free(arr, n);
            err_set(e, "matchinfo is short");
            return -1;
        }
        s.score = bm25_of(&s.mi);
        if (scored_push(&arr, &n, &cap, &s) != 0) {
            mi_clear(&s.mi);
            sqlite3_finalize(st);
            scored_free(arr, n);
            err_set(e, "out of memory");
            return -1;
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        scored_free(arr, n);
        return db_fail(ix, e, "search");
    }
    *out = arr;
    *nout = n;
    return 0;
}

static int scored_has(Scored *a, int n, int64_t id) {
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid].id == id) {
            return 1;
        }
        if (a[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static void mi_take(MatchInfo *dst, MatchInfo *src) {
    *dst = *src;
    memset(src, 0, sizeof *src);
}

static int scored_and(Scored *L, int nl, Scored *R, int nr, Scored **out, int *nout) {
    if (nl > 1) {
        qsort(L, (size_t)nl, sizeof(Scored), scored_cmp);
    }
    if (nr > 1) {
        qsort(R, (size_t)nr, sizeof(Scored), scored_cmp);
    }
    Scored *arr = NULL;
    int n = 0;
    int cap = 0;
    int i = 0;
    int j = 0;
    while (i < nl && j < nr) {
        if (L[i].id == R[j].id) {
            Scored s;
            memset(&s, 0, sizeof s);
            s.id = L[i].id;
            s.score = L[i].score + R[j].score;
            if (L[i].mi.nphrase) {
                mi_take(&s.mi, &L[i].mi);
            } else if (R[j].mi.nphrase) {
                mi_take(&s.mi, &R[j].mi);
            }
            if (scored_push(&arr, &n, &cap, &s) != 0) {
                mi_clear(&s.mi);
                scored_free(arr, n);
                return -1;
            }
            i++;
            j++;
        } else if (L[i].id < R[j].id) {
            i++;
        } else {
            j++;
        }
    }
    *out = arr;
    *nout = n;
    return 0;
}

static int scored_or(Scored *L, int nl, Scored *R, int nr, Scored **out, int *nout) {
    if (nl > 1) {
        qsort(L, (size_t)nl, sizeof(Scored), scored_cmp);
    }
    if (nr > 1) {
        qsort(R, (size_t)nr, sizeof(Scored), scored_cmp);
    }
    Scored *arr = NULL;
    int n = 0;
    int cap = 0;
    int i = 0;
    int j = 0;
    while (i < nl || j < nr) {
        Scored s;
        memset(&s, 0, sizeof s);
        int take_l = j >= nr || (i < nl && L[i].id < R[j].id);
        int take_r = i >= nl || (j < nr && R[j].id < L[i].id);
        if (take_l) {
            s.id = L[i].id;
            s.score = L[i].score;
            mi_take(&s.mi, &L[i].mi);
            i++;
        } else if (take_r) {
            s.id = R[j].id;
            s.score = R[j].score;
            mi_take(&s.mi, &R[j].mi);
            j++;
        } else {
            s.id = L[i].id;
            s.score = L[i].score + R[j].score;
            if (L[i].mi.nphrase) {
                mi_take(&s.mi, &L[i].mi);
            } else {
                mi_take(&s.mi, &R[j].mi);
            }
            i++;
            j++;
        }
        if (scored_push(&arr, &n, &cap, &s) != 0) {
            mi_clear(&s.mi);
            scored_free(arr, n);
            return -1;
        }
    }
    *out = arr;
    *nout = n;
    return 0;
}

static int all_except(Index *ix, Scored *inner, int ni, Scored **out, int *nout, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT docid FROM docs", e) != 0) {
        return -1;
    }
    Scored *arr = NULL;
    int n = 0;
    int cap = 0;
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(st, 0);
        if (scored_has(inner, ni, id)) {
            continue;
        }
        Scored s;
        memset(&s, 0, sizeof s);
        s.id = id;
        s.score = 0.1;
        if (scored_push(&arr, &n, &cap, &s) != 0) {
            sqlite3_finalize(st);
            scored_free(arr, n);
            err_set(e, "out of memory");
            return -1;
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        scored_free(arr, n);
        return db_fail(ix, e, "search");
    }
    *out = arr;
    *nout = n;
    return 0;
}

static int collect_query(Index *ix, const Query *q, Scored **out, int *nout, Err *e) {
    *out = NULL;
    *nout = 0;
    if (!q) {
        return 0;
    }
    if (q->kind == Q_TERM || q->kind == Q_PHRASE) {
        MExpr mx = emit_query(q);
        if (mx.kind != MX_POS) {
            mx_free(&mx);
            err_set(e, "out of memory");
            return -1;
        }
        int rc = match_ids(ix, mx.sql, out, nout, e);
        mx_free(&mx);
        return rc;
    }
    if (q->kind == Q_NOT) {
        Scored *inner = NULL;
        int ni = 0;
        if (collect_query(ix, q->a, &inner, &ni, e) != 0) {
            return -1;
        }
        if (ni > 1) {
            qsort(inner, (size_t)ni, sizeof(Scored), scored_cmp);
        }
        int rc = all_except(ix, inner, ni, out, nout, e);
        scored_free(inner, ni);
        return rc;
    }
    Scored *L = NULL;
    Scored *R = NULL;
    int nl = 0;
    int nr = 0;
    if (collect_query(ix, q->a, &L, &nl, e) != 0) {
        return -1;
    }
    if (collect_query(ix, q->b, &R, &nr, e) != 0) {
        scored_free(L, nl);
        return -1;
    }
    int rc = q->kind == Q_AND ? scored_and(L, nl, R, nr, out, nout) : scored_or(L, nl, R, nr, out, nout);
    scored_free(L, nl);
    scored_free(R, nr);
    if (rc != 0) {
        err_set(e, "out of memory");
    }
    return rc;
}

static int cands_from_scored(Index *ix, Scored *s, int n, Cand **out, int *nout, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT root_id, rel, title, ext, mtime FROM docs WHERE docid=?", e) != 0) {
        return -1;
    }
    Cand *arr = NULL;
    int nc = 0;
    int cap = 0;
    for (int i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, s[i].id);
        int rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(st);
            cands_free(arr, nc);
            if (rc == SQLITE_DONE) {
                err_set(e, "document disappeared");
            } else {
                db_fail(ix, e, "search");
            }
            return -1;
        }
        Cand c;
        memset(&c, 0, sizeof c);
        c.id = s[i].id;
        c.root_id = sqlite3_column_int64(st, 0);
        c.rel = col_text(st, 1);
        c.title = col_text(st, 2);
        c.ext = col_text(st, 3);
        c.mtime = sqlite3_column_int64(st, 4);
        c.raw = s[i].score;
        c.mi = s[i].mi;
        memset(&s[i].mi, 0, sizeof s[i].mi);
        if (!c.rel || !c.title || !c.ext || cand_push(&arr, &nc, &cap, &c) != 0) {
            cand_free(&c);
            sqlite3_finalize(st);
            cands_free(arr, nc);
            err_set(e, "out of memory");
            return -1;
        }
    }
    sqlite3_finalize(st);
    *out = arr;
    *nout = nc;
    return 0;
}

static int run_finish(Index *ix, Cand *cands, int nc, const ParsedQuery *pq, int limit, const char *at,
                      const char *match, Hit **hits, int *nhits, Facets *facets, Err *e) {
    return finish_search(ix, cands, nc, pq, limit, at, match, hits, nhits, facets, e);
}

/* Every candidate for a parsed query, before the limit trim and before bodies
 * are read. *match is the positive MATCH text when ranking used one, and it
 * points into *mx. */
static int gather_cands(Index *ix, const ParsedQuery *pq, Cand **cands, int *nc, const char **match, MExpr *mx,
                        Err *e) {
    *cands = NULL;
    *nc = 0;
    *match = NULL;
    mx->kind = MX_ERR;
    mx->sql = NULL;
    int rc = 0;
    if (!pq->q) {
        rc = query_rows(ix, pq->ext ? ALL_EXT_SQL : ALL_SQL, NULL, pq->ext, 0, 1, cands, nc, e);
    } else {
        *mx = emit_query(pq->q);
        if (mx->kind == MX_ERR) {
            err_set(e, "out of memory");
            return -1;
        }
        if (mx->kind == MX_POS) {
            *match = mx->sql;
            rc = query_rows(ix, pq->ext ? MATCH_EXT_SQL : MATCH_SQL, mx->sql, pq->ext, 1, 0, cands, nc, e);
        } else if (mx->kind == MX_NEG) {
            rc = query_rows(ix, pq->ext ? EXCLUDE_EXT_SQL : EXCLUDE_SQL, mx->sql, pq->ext, 0, 0.1, cands, nc, e);
        } else {
            Scored *sc = NULL;
            int ns = 0;
            rc = collect_query(ix, pq->q, &sc, &ns, e);
            if (rc == 0) {
                rc = cands_from_scored(ix, sc, ns, cands, nc, e);
            }
            scored_free(sc, ns);
        }
    }
    if (rc != 0) {
        cands_free(*cands, *nc);
        *cands = NULL;
        *nc = 0;
    }
    return rc;
}

int index_search(Index *ix, const char *query, int limit, const char *at, Hit **hits, int *nhits, Facets *facets,
                 Err *e) {
    *hits = NULL;
    *nhits = 0;
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 1000) {
        limit = 1000;
    }
    ParsedQuery pq;
    if (query_parse(query, &pq, e) != 0) {
        return -1;
    }
    if (at && *at && !strchr(at, '=') && ix->nroots > 1) {
        parsed_free(&pq);
        err_set(e, "this index has multiple roots; use --at OLD=NEW");
        return -1;
    }
    Cand *cands = NULL;
    int nc = 0;
    const char *match = NULL;
    MExpr mx = {MX_ERR, NULL};
    int rc = gather_cands(ix, &pq, &cands, &nc, &match, &mx, e);
    if (rc == 0) {
        rc = run_finish(ix, cands, nc, &pq, limit, at, match, hits, nhits, facets, e);
    }
    mx_free(&mx);
    parsed_free(&pq);
    return rc;
}

int index_more(Index *ix, const char *query, const char *path, uint32_t after, Passage **out, int *nout, int *more,
               Err *e) {
    *out = NULL;
    *nout = 0;
    if (more) {
        *more = 0;
    }
    if (!ix || !path || !path[0]) {
        err_set(e, "missing file");
        return -1;
    }
    ParsedQuery pq;
    if (query_parse(query ? query : "", &pq, e) != 0) {
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT docid, root_id, rel, title, ext, mtime FROM docs", e) != 0) {
        parsed_free(&pq);
        return -1;
    }
    int64_t found = 0;
    char *ext = NULL;
    int step = 0;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(st, 2);
        const char *title = (const char *)sqlite3_column_text(st, 3);
        const char *ex = (const char *)sqlite3_column_text(st, 4);
        int64_t mtime = sqlite3_column_int64(st, 5);
        if (!doc_passes(rel, title, ex, mtime, &pq)) {
            continue;
        }
        char *shown = display_path(ix, sqlite3_column_int64(st, 1), rel, NULL);
        int eq = shown && strcmp(shown, path) == 0;
        free(shown);
        if (eq) {
            found = sqlite3_column_int64(st, 0);
            ext = xstrdup(ex ? ex : "");
            break;
        }
    }
    sqlite3_finalize(st);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        parsed_free(&pq);
        free(ext);
        return db_fail(ix, e, "search");
    }
    if (!found || !ext) {
        parsed_free(&pq);
        free(ext);
        err_set(e, "that file is not in the index");
        return -1;
    }
    QTerm *terms = NULL;
    int nt = 0;
    if (pq.q && query_positive_terms(pq.q, &terms, &nt) != 0) {
        parsed_free(&pq);
        free(ext);
        err_set(e, "out of memory");
        return -1;
    }
    char *text = NULL;
    Mark *marks = NULL;
    int nmarks = 0;
    if (load_body(ix, found, &text, &marks, &nmarks, e) != 0) {
        qterms_free(terms, nt);
        parsed_free(&pq);
        free(ext);
        return -1;
    }
    MatchOff *offs = NULL;
    int noffs = 0;
    int nmatches = 0;
    int page_more = 0;
    int paged = ascii_ieq(ext, "pdf");
    int rc = make_passages(text, strlen(text), terms, nt, marks, nmarks, paged, out, nout, &offs, &noffs, &nmatches,
                           after, 1, &page_more);
    marks_free(marks, nmarks);
    free(text);
    free(offs);
    free(ext);
    qterms_free(terms, nt);
    parsed_free(&pq);
    if (rc != 0) {
        passages_free(*out, *nout);
        *out = NULL;
        *nout = 0;
        err_set(e, "out of memory");
        return -1;
    }
    if (more) {
        *more = page_more;
    }
    return 0;
}

void hits_free(Hit *hits, int n) {
    if (!hits) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(hits[i].path);
        free(hits[i].title);
        free(hits[i].ext);
        free(hits[i].offsets);
        mi_clear(&hits[i].matchinfo);
        passages_free(hits[i].passages, hits[i].npassages);
    }
    free(hits);
}

static const char *SCHEMA =
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE roots(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, full INTEGER NOT NULL);"
    "CREATE TABLE docs("
    " docid INTEGER PRIMARY KEY,"
    " root_id INTEGER NOT NULL,"
    " rel TEXT NOT NULL,"
    " title TEXT NOT NULL,"
    " ext TEXT NOT NULL,"
    " mtime INTEGER NOT NULL,"
    " nsec INTEGER NOT NULL,"
    " size INTEGER NOT NULL,"
    " body TEXT NOT NULL,"
    " marks BLOB,"
    " UNIQUE(root_id, rel));"
    "CREATE VIRTUAL TABLE fts USING fts4(content='docs', body, tokenize=grove);"
    "INSERT INTO meta(key, value) VALUES('format', 'fts4');"
    "INSERT INTO meta(key, value) VALUES('version', '1');";

static int ensure_schema(Index *ix, Err *e) {
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT value FROM meta WHERE key='format'", e) != 0) {
        const char *msg = sqlite3_errmsg(ix->db);
        int missing = msg && strstr(msg, "no such table");
        if (!missing) {
            return -1;
        }
        err_clear(e);
        if (!ix->writable) {
            err_set(e, "no index at %s", ix->dir);
            return -1;
        }
        if (sql_exec(ix, SCHEMA, e) != 0) {
            return -1;
        }
        return 0;
    }
    int rc = sqlite3_step(st);
    const char *fmt = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
    int ok = fmt && strcmp(fmt, "fts4") == 0;
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW || !ok) {
        err_set(e, "unrecognized index in %s", ix->dir);
        return -1;
    }
    return 0;
}

Index *index_open(const char *dir, int writable, Err *e) {
    Index *ix = calloc(1, sizeof *ix);
    if (!ix) {
        err_set(e, "out of memory");
        return NULL;
    }
    ix->writable = writable;
    ix->quiet = 1;
    ix->dir = xstrdup(dir ? dir : "");
    ix->db_path = ix->dir ? path_join(ix->dir, "grove.db") : NULL;
    if (!ix->dir || !ix->db_path) {
        err_set(e, "out of memory");
        index_close(ix);
        return NULL;
    }
    if (writable && mkdir_p(dir) != 0) {
        err_set(e, "cannot create %s: %s", dir, strerror(errno));
        index_close(ix);
        return NULL;
    }
    int db_exists = file_there(ix->db_path);
    if (!db_exists && is_old_index(dir)) {
        err_set(e, "this index is the previous format; remove it and index again");
        index_close(ix);
        return NULL;
    }
    if (!db_exists && !writable) {
        err_set(e, "no index at %s", dir);
        index_close(ix);
        return NULL;
    }
    int flags = writable ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    int rc = sqlite3_open_v2(ix->db_path, &ix->db, flags, NULL);
    if (rc != SQLITE_OK) {
        err_set(e, "cannot open %s: %s", ix->db_path, ix->db ? sqlite3_errmsg(ix->db) : "sqlite");
        index_close(ix);
        return NULL;
    }
    sqlite3_busy_timeout(ix->db, 20000);
    sqlite3_extended_result_codes(ix->db, 1);
    if (grove_fts_register(ix->db, e) != 0) {
        index_close(ix);
        return NULL;
    }
    if (writable) {
        if (sql_exec(ix, "PRAGMA journal_mode=DELETE", e) != 0 || sql_exec(ix, "PRAGMA synchronous=NORMAL", e) != 0) {
            index_close(ix);
            return NULL;
        }
    }
    if (ensure_schema(ix, e) != 0 || reload_roots(ix, e) != 0) {
        index_close(ix);
        return NULL;
    }
    char *real = realpath(dir, NULL);
    ix->realdir = real ? real : xstrdup(dir);
    return ix;
}

void index_set_quiet(Index *ix, int quiet) {
    if (ix) {
        ix->quiet = quiet;
    }
}

void index_set_force(Index *ix, int force) {
    if (ix) {
        ix->force = force;
    }
}

void index_close(Index *ix) {
    if (!ix) {
        return;
    }
    if (ix->db) {
        sqlite3_close(ix->db);
    }
    roots_clear(ix);
    free(ix->dir);
    free(ix->realdir);
    free(ix->db_path);
    free(ix);
}

/* Saved searches are not part of the fts4 format row. The table appears on the
 * first remember, so an older grove.db does not have to be indexed again. */
#define SAVED_KEEP 24
#define SAVED_QUERY_MAX 400
#define SAVED_STR(x) SAVED_STR2(x)
#define SAVED_STR2(x) #x

static int saved_exec(sqlite3 *db, const char *sql, Err *e) {
    char *msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        int primary = rc & 0xff;
        if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
            err_set(e, "index is in use");
        } else {
            err_set(e, "%s", msg ? msg : sqlite3_errmsg(db));
        }
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

static int saved_fail(sqlite3 *db, Err *e) {
    int primary = sqlite3_extended_errcode(db) & 0xff;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
        err_set(e, "index is in use");
    } else {
        err_set(e, "sqlite: %s", sqlite3_errmsg(db));
    }
    return -1;
}

static char *saved_text(const char *query, int check_parse, Err *e) {
    if (!query) {
        query = "";
    }
    while (*query == ' ' || *query == '\t') {
        query++;
    }
    size_t n = strlen(query);
    while (n > 0 && (query[n - 1] == ' ' || query[n - 1] == '\t')) {
        n--;
    }
    if (n == 0) {
        err_set(e, "empty query");
        return NULL;
    }
    if (n > SAVED_QUERY_MAX) {
        err_set(e, "that search is too long");
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (query[i] == '\n' || query[i] == '\r') {
            err_set(e, "a saved search is one line");
            return NULL;
        }
    }
    char *s = malloc(n + 1);
    if (!s) {
        err_set(e, "out of memory");
        return NULL;
    }
    memcpy(s, query, n);
    s[n] = 0;
    if (check_parse) {
        ParsedQuery pq;
        if (query_parse(s, &pq, e) != 0) {
            free(s);
            return NULL;
        }
        parsed_free(&pq);
    }
    return s;
}

static int alert_table(sqlite3 *db, const char *name, int *yes, Err *e) {
    sqlite3_stmt *st = NULL;
    *yes = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, NULL) !=
        SQLITE_OK) {
        return saved_fail(db, e);
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *yes = 1;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        return saved_fail(db, e);
    }
    return 0;
}

static int alert_forget_query(sqlite3 *db, const char *query, Err *e) {
    int yes = 0;
    sqlite3_stmt *st = NULL;
    if (alert_table(db, "alert", &yes, e) != 0) {
        return -1;
    }
    if (!yes) {
        return 0;
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM alert WHERE query=?", -1, &st, NULL) != SQLITE_OK) {
        return saved_fail(db, e);
    }
    sqlite3_bind_text(st, 1, query, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    st = NULL;
    if (rc != SQLITE_DONE) {
        return saved_fail(db, e);
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM alert_seen WHERE query=?", -1, &st, NULL) != SQLITE_OK) {
        return saved_fail(db, e);
    }
    sqlite3_bind_text(st, 1, query, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return saved_fail(db, e);
    }
    return 0;
}

static int alert_forget_orphans(sqlite3 *db, Err *e) {
    int yes = 0;
    if (alert_table(db, "alert", &yes, e) != 0) {
        return -1;
    }
    if (!yes) {
        return 0;
    }
    return saved_exec(db,
                      "DELETE FROM alert WHERE query NOT IN (SELECT query FROM saved);"
                      "DELETE FROM alert_seen WHERE query NOT IN (SELECT query FROM saved);",
                      e);
}

static int saved_touch(Index *ix, const char *canon, int drop, Err *e) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int began = 0;
    int rc = sqlite3_open_v2(ix->db_path, &db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK || !db) {
        err_set(e, "cannot open %s: %s", ix->db_path, db ? sqlite3_errmsg(db) : "sqlite");
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    sqlite3_busy_timeout(db, 20000);
    sqlite3_extended_result_codes(db, 1);
    if (saved_exec(db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(db);
        return -1;
    }
    began = 1;
    if (saved_exec(db,
                   "CREATE TABLE IF NOT EXISTS saved("
                   " query TEXT PRIMARY KEY,"
                   " created INTEGER NOT NULL)",
                   e) != 0) {
        goto fail;
    }
    rc = sqlite3_prepare_v2(db,
                            drop ? "DELETE FROM saved WHERE query=?"
                                 : "INSERT INTO saved(query, created) VALUES(?, ?)"
                                   " ON CONFLICT(query) DO UPDATE SET created=excluded.created",
                            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_text(st, 1, canon, -1, SQLITE_TRANSIENT);
    if (!drop) {
        int64_t now = (int64_t)time(NULL);
        sqlite3_stmt *mx = NULL;
        if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(created), 0) FROM saved", -1, &mx, NULL) == SQLITE_OK) {
            if (sqlite3_step(mx) == SQLITE_ROW) {
                int64_t m = sqlite3_column_int64(mx, 0);
                if (m >= now) {
                    now = m + 1;
                }
            }
        }
        sqlite3_finalize(mx);
        sqlite3_bind_int64(st, 2, now);
    }
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    st = NULL;
    if (rc != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    if (!drop &&
        saved_exec(db,
                   "DELETE FROM saved WHERE query IN ("
                   "SELECT query FROM saved ORDER BY created DESC, query LIMIT -1 OFFSET " SAVED_STR(SAVED_KEEP) ")",
                   e) != 0) {
        goto fail;
    }
    if (drop) {
        if (alert_forget_query(db, canon, e) != 0) {
            goto fail;
        }
    } else if (alert_forget_orphans(db, e) != 0) {
        goto fail;
    }
    if (saved_exec(db, "COMMIT", e) != 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_close(db);
    return 0;
fail:
    if (st) {
        sqlite3_finalize(st);
    }
    if (began) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    return -1;
}

int index_saved_list(Index *ix, char ***queries, int *n, Err *e) {
    if (queries) {
        *queries = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!ix || !queries || !n) {
        err_set(e, "no index");
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT query FROM saved ORDER BY created DESC, query", e) != 0) {
        if (e->msg[0] && strstr(e->msg, "no such table")) {
            err_clear(e);
            return 0;
        }
        return -1;
    }
    char **rows = NULL;
    int nr = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        char *copy = xstrdup(s ? s : "");
        char **next = copy ? realloc(rows, (size_t)(nr + 1) * sizeof(*rows)) : NULL;
        if (!copy || !next) {
            free(copy);
            sqlite3_finalize(st);
            index_saved_free(rows, nr);
            err_set(e, "out of memory");
            return -1;
        }
        rows = next;
        rows[nr++] = copy;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        index_saved_free(rows, nr);
        return db_fail(ix, e, "sqlite");
    }
    *queries = rows;
    *n = nr;
    return 0;
}

void index_saved_free(char **queries, int n) {
    if (!queries) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(queries[i]);
    }
    free(queries);
}

int index_saved_put(Index *ix, const char *query, Err *e) {
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    char *canon = saved_text(query, 1, e);
    if (!canon) {
        return -1;
    }
    int rc = saved_touch(ix, canon, 0, e);
    free(canon);
    return rc;
}

int index_saved_drop(Index *ix, const char *query, Err *e) {
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    char *canon = saved_text(query, 0, e);
    if (!canon) {
        return -1;
    }
    int rc = saved_touch(ix, canon, 1, e);
    free(canon);
    return rc;
}

#define NOTE_KEEP 200
#define NOTE_BODY_MAX 400
#define NOTE_PATH_MAX 1024

static char *note_line(const char *s, size_t max, const char *empty_msg, const char *long_msg, Err *e) {
    if (!s) {
        s = "";
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    if (n == 0) {
        err_set(e, "%s", empty_msg);
        return NULL;
    }
    if (n > max) {
        err_set(e, "%s", long_msg);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') {
            err_set(e, "a note is one line");
            return NULL;
        }
    }
    char *out = malloc(n + 1);
    if (!out) {
        err_set(e, "out of memory");
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static int note_touch(Index *ix, const char *path, uint32_t offset, const char *body, int drop, Err *e) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int began = 0;
    int rc = sqlite3_open_v2(ix->db_path, &db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK || !db) {
        err_set(e, "cannot open %s: %s", ix->db_path, db ? sqlite3_errmsg(db) : "sqlite");
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    sqlite3_busy_timeout(db, 20000);
    sqlite3_extended_result_codes(db, 1);
    if (saved_exec(db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(db);
        return -1;
    }
    began = 1;
    if (saved_exec(db,
                   "CREATE TABLE IF NOT EXISTS notes("
                   " path TEXT NOT NULL,"
                   " offset INTEGER NOT NULL,"
                   " body TEXT NOT NULL,"
                   " created INTEGER NOT NULL,"
                   " PRIMARY KEY(path, offset))",
                   e) != 0) {
        goto fail;
    }
    if (!drop) {
        sqlite3_stmt *cnt = NULL;
        int have = 0;
        int total = 0;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM notes", -1, &cnt, NULL) != SQLITE_OK) {
            saved_fail(db, e);
            goto fail;
        }
        if (sqlite3_step(cnt) == SQLITE_ROW) {
            total = sqlite3_column_int(cnt, 0);
        }
        sqlite3_finalize(cnt);
        cnt = NULL;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM notes WHERE path=? AND offset=?", -1, &cnt, NULL) != SQLITE_OK) {
            saved_fail(db, e);
            goto fail;
        }
        sqlite3_bind_text(cnt, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(cnt, 2, (int64_t)offset);
        have = sqlite3_step(cnt) == SQLITE_ROW;
        sqlite3_finalize(cnt);
        if (!have && total >= NOTE_KEEP) {
            err_set(e, "too many notes");
            goto fail;
        }
    }
    rc = sqlite3_prepare_v2(db,
                            drop ? "DELETE FROM notes WHERE path=? AND offset=?"
                                 : "INSERT INTO notes(path, offset, body, created) VALUES(?, ?, ?, ?)"
                                   " ON CONFLICT(path, offset) DO UPDATE SET body=excluded.body, created=excluded.created",
                            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)offset);
    if (!drop) {
        sqlite3_bind_text(st, 3, body, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, (int64_t)time(NULL));
    }
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    st = NULL;
    if (rc != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    if (saved_exec(db, "COMMIT", e) != 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_close(db);
    return 0;
fail:
    if (st) {
        sqlite3_finalize(st);
    }
    if (began) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    return -1;
}

int index_notes_list(Index *ix, Note **notes, int *n, Err *e) {
    if (notes) {
        *notes = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!ix || !notes || !n) {
        err_set(e, "no index");
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT path, offset, body FROM notes ORDER BY path, offset", e) != 0) {
        if (e->msg[0] && strstr(e->msg, "no such table")) {
            err_clear(e);
            return 0;
        }
        return -1;
    }
    Note *rows = NULL;
    int nr = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(st, 0);
        const char *body = (const char *)sqlite3_column_text(st, 2);
        int64_t off = sqlite3_column_int64(st, 1);
        Note *next = realloc(rows, (size_t)(nr + 1) * sizeof(*rows));
        char *pcopy = xstrdup(path ? path : "");
        char *bcopy = xstrdup(body ? body : "");
        if (!next || !pcopy || !bcopy || off < 0 || off > UINT32_MAX) {
            free(pcopy);
            free(bcopy);
            sqlite3_finalize(st);
            index_notes_free(next ? next : rows, nr);
            err_set(e, "out of memory");
            return -1;
        }
        rows = next;
        rows[nr].path = pcopy;
        rows[nr].offset = (uint32_t)off;
        rows[nr].body = bcopy;
        nr++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        index_notes_free(rows, nr);
        return db_fail(ix, e, "sqlite");
    }
    *notes = rows;
    *n = nr;
    return 0;
}

void index_notes_free(Note *notes, int n) {
    if (!notes) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(notes[i].path);
        free(notes[i].body);
    }
    free(notes);
}

int index_note_put(Index *ix, const char *path, uint32_t offset, const char *body, Err *e) {
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    char *canon_path = note_line(path, NOTE_PATH_MAX, "note needs a path", "that path is too long", e);
    if (!canon_path) {
        return -1;
    }
    char *canon_body = note_line(body, NOTE_BODY_MAX, "empty note", "that note is too long", e);
    if (!canon_body) {
        free(canon_path);
        return -1;
    }
    int rc = note_touch(ix, canon_path, offset, canon_body, 0, e);
    free(canon_path);
    free(canon_body);
    return rc;
}

int index_note_drop(Index *ix, const char *path, uint32_t offset, Err *e) {
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    char *canon_path = note_line(path, NOTE_PATH_MAX, "note needs a path", "that path is too long", e);
    if (!canon_path) {
        return -1;
    }
    int rc = note_touch(ix, canon_path, offset, "", 1, e);
    free(canon_path);
    return rc;
}

static void alert_files_free(AlertFile *files, int n) {
    if (!files) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(files[i].path);
    }
    free(files);
}

static int alert_file_push(AlertFile **arr, int *n, int *cap, AlertFile *src) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        AlertFile *p = realloc(*arr, (size_t)nc * sizeof(*p));
        if (!p) {
            return -1;
        }
        *arr = p;
        *cap = nc;
    }
    (*arr)[(*n)++] = *src;
    src->path = NULL;
    return 0;
}

static int alert_file_cmp(const void *a, const void *b) {
    const char *x = ((const AlertFile *)a)->path;
    const char *y = ((const AlertFile *)b)->path;
    return strcmp(x ? x : "", y ? y : "");
}

/* Matching files for a query, with no passage text. Paths are the stored roots. */
static int match_files(Index *ix, const char *query, AlertFile **out, int *nout, Err *e) {
    *out = NULL;
    *nout = 0;
    ParsedQuery pq;
    if (query_parse(query, &pq, e) != 0) {
        return -1;
    }
    Cand *cands = NULL;
    int nc = 0;
    const char *match = NULL;
    MExpr mx = {MX_ERR, NULL};
    if (gather_cands(ix, &pq, &cands, &nc, &match, &mx, e) != 0) {
        mx_free(&mx);
        parsed_free(&pq);
        return -1;
    }
    (void)match;
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT mtime, nsec, size FROM docs WHERE docid=?", e) != 0) {
        cands_free(cands, nc);
        mx_free(&mx);
        parsed_free(&pq);
        return -1;
    }
    AlertFile *arr = NULL;
    int n = 0;
    int cap = 0;
    int failed = 0;
    for (int i = 0; i < nc; i++) {
        if (!doc_passes(cands[i].rel, cands[i].title, cands[i].ext, cands[i].mtime, &pq)) {
            continue;
        }
        char *path = display_path(ix, cands[i].root_id, cands[i].rel, NULL);
        if (!path) {
            err_set(e, "out of memory");
            failed = 1;
            break;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, cands[i].id);
        if (sqlite3_step(st) != SQLITE_ROW) {
            free(path);
            err_set(e, "document disappeared");
            failed = 1;
            break;
        }
        AlertFile f;
        memset(&f, 0, sizeof f);
        f.path = path;
        f.mtime = sqlite3_column_int64(st, 0);
        f.nsec = sqlite3_column_int(st, 1);
        f.size = sqlite3_column_int64(st, 2);
        if (alert_file_push(&arr, &n, &cap, &f) != 0) {
            free(f.path);
            err_set(e, "out of memory");
            failed = 1;
            break;
        }
    }
    sqlite3_finalize(st);
    cands_free(cands, nc);
    mx_free(&mx);
    parsed_free(&pq);
    if (failed) {
        alert_files_free(arr, n);
        return -1;
    }
    *out = arr;
    *nout = n;
    return 0;
}

static int alert_open(Index *ix, sqlite3 **db, Err *e) {
    int rc = sqlite3_open_v2(ix->db_path, db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK || !*db) {
        err_set(e, "cannot open %s: %s", ix->db_path, *db ? sqlite3_errmsg(*db) : "sqlite");
        if (*db) {
            sqlite3_close(*db);
            *db = NULL;
        }
        return -1;
    }
    sqlite3_busy_timeout(*db, 20000);
    sqlite3_extended_result_codes(*db, 1);
    return 0;
}

static int alert_schema(sqlite3 *db, Err *e) {
    if (saved_exec(db,
                   "CREATE TABLE IF NOT EXISTS saved("
                   " query TEXT PRIMARY KEY,"
                   " created INTEGER NOT NULL)",
                   e) != 0) {
        return -1;
    }
    if (saved_exec(db,
                   "CREATE TABLE IF NOT EXISTS alert("
                   " query TEXT PRIMARY KEY,"
                   " created INTEGER NOT NULL)",
                   e) != 0) {
        return -1;
    }
    return saved_exec(db,
                      "CREATE TABLE IF NOT EXISTS alert_seen("
                      " query TEXT NOT NULL,"
                      " path TEXT NOT NULL,"
                      " mtime INTEGER NOT NULL,"
                      " nsec INTEGER NOT NULL,"
                      " size INTEGER NOT NULL,"
                      " PRIMARY KEY(query, path))",
                      e);
}

static int alert_created(sqlite3 *db, int64_t *now, Err *e) {
    sqlite3_stmt *mx = NULL;
    *now = (int64_t)time(NULL);
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(created), 0) FROM saved", -1, &mx, NULL) != SQLITE_OK) {
        return saved_fail(db, e);
    }
    if (sqlite3_step(mx) == SQLITE_ROW) {
        int64_t m = sqlite3_column_int64(mx, 0);
        if (m >= *now) {
            *now = m + 1;
        }
    }
    sqlite3_finalize(mx);
    return 0;
}

static int alert_store(Index *ix, const char *canon, AlertFile *files, int n, Err *e) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int began = 0;
    if (alert_open(ix, &db, e) != 0) {
        return -1;
    }
    if (saved_exec(db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(db);
        return -1;
    }
    began = 1;
    if (alert_schema(db, e) != 0) {
        goto fail;
    }
    int64_t now = 0;
    if (alert_created(db, &now, e) != 0) {
        goto fail;
    }
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO saved(query, created) VALUES(?, ?)"
                           " ON CONFLICT(query) DO UPDATE SET created=excluded.created",
                           -1, &st, NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_text(st, 1, canon, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, now);
    if (sqlite3_step(st) != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (saved_exec(db,
                   "DELETE FROM saved WHERE query IN ("
                   "SELECT query FROM saved ORDER BY created DESC, query LIMIT -1 OFFSET " SAVED_STR(SAVED_KEEP) ")",
                   e) != 0) {
        goto fail;
    }
    if (alert_forget_orphans(db, e) != 0) {
        goto fail;
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM alert_seen WHERE query=?", -1, &st, NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_text(st, 1, canon, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO alert(query, created) VALUES(?, ?)"
                           " ON CONFLICT(query) DO UPDATE SET created=excluded.created",
                           -1, &st, NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_text(st, 1, canon, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, now);
    if (sqlite3_step(st) != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (n > 0 &&
        sqlite3_prepare_v2(db, "INSERT INTO alert_seen(query, path, mtime, nsec, size) VALUES(?, ?, ?, ?, ?)", -1, &st,
                           NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    for (int i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, canon, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, files[i].path ? files[i].path : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, files[i].mtime);
        sqlite3_bind_int(st, 4, files[i].nsec);
        sqlite3_bind_int64(st, 5, files[i].size);
        if (sqlite3_step(st) != SQLITE_DONE) {
            saved_fail(db, e);
            goto fail;
        }
    }
    if (st) {
        sqlite3_finalize(st);
        st = NULL;
    }
    if (saved_exec(db, "COMMIT", e) != 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_close(db);
    return 0;
fail:
    if (st) {
        sqlite3_finalize(st);
    }
    if (began) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    return -1;
}

int index_alert_watch(Index *ix, const char *query, Err *e) {
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    char *canon = saved_text(query, 1, e);
    if (!canon) {
        return -1;
    }
    AlertFile *files = NULL;
    int n = 0;
    if (match_files(ix, canon, &files, &n, e) != 0) {
        free(canon);
        return -1;
    }
    if (n > ALERT_FILES_MAX) {
        alert_files_free(files, n);
        free(canon);
        err_set(e, "too many files to watch");
        return -1;
    }
    int rc = alert_store(ix, canon, files, n, e);
    alert_files_free(files, n);
    free(canon);
    return rc;
}

int index_alert_unwatch(Index *ix, const char *query, Err *e) {
    sqlite3 *db = NULL;
    int began = 0;
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    char *canon = saved_text(query, 0, e);
    if (!canon) {
        return -1;
    }
    if (alert_open(ix, &db, e) != 0) {
        free(canon);
        return -1;
    }
    if (saved_exec(db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(db);
        free(canon);
        return -1;
    }
    began = 1;
    if (alert_forget_query(db, canon, e) != 0) {
        goto fail;
    }
    if (saved_exec(db, "COMMIT", e) != 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        free(canon);
        return -1;
    }
    sqlite3_close(db);
    free(canon);
    return 0;
fail:
    if (began) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    free(canon);
    return -1;
}

static int alert_load_seen(Index *ix, const char *query, AlertFile **out, int *nout, Err *e) {
    sqlite3_stmt *st = NULL;
    *out = NULL;
    *nout = 0;
    if (prep(ix, &st, "SELECT path, mtime, nsec, size FROM alert_seen WHERE query=? ORDER BY path", e) != 0) {
        return -1;
    }
    sqlite3_bind_text(st, 1, query, -1, SQLITE_TRANSIENT);
    AlertFile *arr = NULL;
    int n = 0;
    int cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(st, 0);
        AlertFile f;
        memset(&f, 0, sizeof f);
        f.path = xstrdup(path ? path : "");
        f.mtime = sqlite3_column_int64(st, 1);
        f.nsec = sqlite3_column_int(st, 2);
        f.size = sqlite3_column_int64(st, 3);
        if (!f.path || alert_file_push(&arr, &n, &cap, &f) != 0) {
            free(f.path);
            sqlite3_finalize(st);
            alert_files_free(arr, n);
            err_set(e, "out of memory");
            return -1;
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        alert_files_free(arr, n);
        return db_fail(ix, e, "sqlite");
    }
    *out = arr;
    *nout = n;
    return 0;
}

static int alert_changed(Index *ix, const char *query, AlertFile **out, int *nout, Err *e) {
    AlertFile *cur = NULL;
    AlertFile *seen = NULL;
    AlertFile *neu = NULL;
    int nc = 0;
    int ns = 0;
    int nn = 0;
    int cap = 0;
    *out = NULL;
    *nout = 0;
    if (match_files(ix, query, &cur, &nc, e) != 0) {
        return -1;
    }
    if (alert_load_seen(ix, query, &seen, &ns, e) != 0) {
        alert_files_free(cur, nc);
        return -1;
    }
    for (int i = 0; i < nc; i++) {
        AlertFile key;
        memset(&key, 0, sizeof key);
        key.path = cur[i].path;
        AlertFile *found = ns ? bsearch(&key, seen, (size_t)ns, sizeof(AlertFile), alert_file_cmp) : NULL;
        if (found && found->mtime == cur[i].mtime && found->nsec == cur[i].nsec && found->size == cur[i].size) {
            continue;
        }
        if (alert_file_push(&neu, &nn, &cap, &cur[i]) != 0) {
            err_set(e, "out of memory");
            alert_files_free(cur, nc);
            alert_files_free(seen, ns);
            alert_files_free(neu, nn);
            return -1;
        }
    }
    alert_files_free(cur, nc);
    alert_files_free(seen, ns);
    if (nn > 1) {
        qsort(neu, (size_t)nn, sizeof(AlertFile), alert_file_cmp);
    }
    *out = neu;
    *nout = nn;
    return 0;
}

int index_alerts_list(Index *ix, Alert **alerts, int *n, Err *e) {
    sqlite3_stmt *st = NULL;
    char **queries = NULL;
    int nq = 0;
    Alert *rows = NULL;
    if (alerts) {
        *alerts = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!ix || !alerts || !n) {
        err_set(e, "no index");
        return -1;
    }
    if (prep(ix, &st, "SELECT query FROM alert ORDER BY created DESC, query", e) != 0) {
        if (e->msg[0] && strstr(e->msg, "no such table")) {
            err_clear(e);
            return 0;
        }
        return -1;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        char *copy = xstrdup(s ? s : "");
        char **next = copy ? realloc(queries, (size_t)(nq + 1) * sizeof(*queries)) : NULL;
        if (!copy || !next) {
            free(copy);
            sqlite3_finalize(st);
            index_saved_free(queries, nq);
            err_set(e, "out of memory");
            return -1;
        }
        queries = next;
        queries[nq++] = copy;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        index_saved_free(queries, nq);
        return db_fail(ix, e, "sqlite");
    }
    rows = calloc(nq ? (size_t)nq : 1, sizeof(Alert));
    if (!rows) {
        index_saved_free(queries, nq);
        err_set(e, "out of memory");
        return -1;
    }
    for (int i = 0; i < nq; i++) {
        rows[i].query = queries[i];
        queries[i] = NULL;
        if (alert_changed(ix, rows[i].query, &rows[i].files, &rows[i].nfiles, e) != 0) {
            index_alerts_free(rows, i + 1);
            index_saved_free(queries, nq);
            return -1;
        }
    }
    index_saved_free(queries, nq);
    *alerts = rows;
    *n = nq;
    return 0;
}

void index_alerts_free(Alert *alerts, int n) {
    if (!alerts) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(alerts[i].query);
        alert_files_free(alerts[i].files, alerts[i].nfiles);
    }
    free(alerts);
}
