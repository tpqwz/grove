#include "index.h"
#include "os.h"

#include "extract.h"
#include "fts_tok.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
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
        size_t nn;
        if (size_add(ix->nroots, 1, &nn) != 0 ||
            grow_alloc((void **)&ix->roots, nn, sizeof *ix->roots) != 0 ||
            grow_alloc((void **)&ix->root_ids, nn, sizeof *ix->root_ids) != 0 ||
            grow_alloc((void **)&ix->root_full, nn, sizeof *ix->root_full) != 0) {
            sqlite3_finalize(st);
            err_set(e, "out of memory");
            return -1;
        }
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
    size_t nn;
    if (size_add(ix->nroots, 1, &nn) != 0 || grow_alloc((void **)&ix->roots, nn, sizeof *ix->roots) != 0 ||
        grow_alloc((void **)&ix->root_ids, nn, sizeof *ix->root_ids) != 0 ||
        grow_alloc((void **)&ix->root_full, nn, sizeof *ix->root_full) != 0) {
        err_set(e, "out of memory");
        return -1;
    }
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
        if (s->n == INT_MAX || grow_alloc((void **)&s->a, (size_t)s->n + 1, sizeof(Pend)) != 0) {
            sqlite3_finalize(st);
            scope_free(s);
            err_set(e, "out of memory");
            return -1;
        }
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
    struct timespec mt = grove_mtim(&stt);
    int64_t sec = (int64_t)mt.tv_sec;
    int nsec = (int)mt.tv_nsec;
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
        if (grove_lstat(child, &stt) != 0 || S_ISLNK(stt.st_mode)) {
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
    char *canon = grove_realpath(path, NULL);
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
    char *np = grove_realpath(to, NULL);
    if (!np) {
        err_set(e, "cannot resolve %s: %s", to, strerror(errno));
        return -1;
    }
    char *op = grove_realpath(from, NULL);
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
    size_t n;
    if (!a || !op || !b || size_add(strlen(a), strlen(b), &n) != 0 || size_add(n, strlen(op), &n) != 0 ||
        size_add(n, 8, &n) != 0) {
        return NULL;
    }
    char *s = malloc(n);
    if (!s) {
        return NULL;
    }
    int wrote = snprintf(s, n, "(%s) %s (%s)", a, op, b);
    if (wrote < 0 || (size_t)wrote >= n) {
        free(s);
        return NULL;
    }
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
        if (start < 0 || len < 0 || (uint64_t)start > UINT32_MAX || (uint64_t)len > UINT32_MAX ||
            n >= GROVE_OFFSET_MAX) {
            continue;
        }
        if (n == cap) {
            if (cap >= GROVE_OFFSET_MAX) {
                free(arr);
                return -1;
            }
            int nc;
            if (cap_grow(cap, 32, &nc) != 0) {
                free(arr);
                return -1;
            }
            if (nc > GROVE_OFFSET_MAX) {
                nc = GROVE_OFFSET_MAX;
            }
            if (grow_alloc((void **)&arr, (size_t)nc, sizeof(MatchOff)) != 0) {
                free(arr);
                return -1;
            }
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
        int nc;
        if (cap_grow(*cap, 16, &nc) != 0 || grow_alloc((void **)arr, (size_t)nc, sizeof **arr) != 0) {
            free(owned);
            return -1;
        }
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
    size_t n;
    size_t alloc;
    if (size_add((size_t)(slash - rel), 1, &n) != 0 || size_add(n, 1, &alloc) != 0) {
        return -1;
    }
    char *s = malloc(alloc);
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

static void facet_list_free(Facet *a, int n) {
    if (!a) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(a[i].value);
    }
    free(a);
}

static void narrow_free(Narrow *n) {
    if (!n) {
        return;
    }
    facet_list_free(n->language, n->nlanguage);
    facet_list_free(n->author, n->nauthor);
    facet_list_free(n->form, n->nform);
    facet_list_free(n->period, n->nperiod);
    facet_list_free(n->genre, n->ngenre);
    memset(n, 0, sizeof *n);
}

void facets_free(Facets *f) {
    if (!f) {
        return;
    }
    facet_list_free(f->types, f->ntypes);
    facet_list_free(f->folders, f->nfolders);
    facet_list_free(f->months, f->nmonths);
    narrow_free(&f->narrow);
    memset(f, 0, sizeof *f);
}

static int same_month(int64_t sec, int year, int mon) {
    time_t t = (time_t)sec;
    struct tm tm;
    if (!grove_localtime(&t, &tm)) {
        return 0;
    }
    return tm.tm_year + 1900 == year && tm.tm_mon + 1 == mon;
}

static char *month_key(int64_t sec) {
    time_t t = (time_t)sec;
    struct tm tm;
    if (!grove_localtime(&t, &tm)) {
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
        int nc;
        if (cap_grow(*cap, 32, &nc) != 0 || grow_alloc((void **)arr, (size_t)nc, sizeof(Cand)) != 0) {
            return -1;
        }
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

typedef struct NarrowGate NarrowGate;

static int catalog_narrow_filter(Index *ix, Cand *cands, int *nc, const ParsedQuery *pq, int with_facets,
                                 char ***type_keys, int *ntype, int *type_cap, char ***folder_keys, int *nfolder,
                                 int *folder_cap, char ***month_keys, int *nmonth, int *month_cap, Narrow *narrow,
                                 Err *e);
static int narrow_gate_open(Index *ix, const ParsedQuery *pq, NarrowGate **out, Err *e);
static void narrow_gate_close(NarrowGate *g);
static int narrow_gate_ok(const NarrowGate *g, const char *path);

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
    Narrow record;
    memset(&record, 0, sizeof record);
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
        cands[w++] = cands[i];
        if (w - 1 != i) {
            memset(&cands[i], 0, sizeof cands[i]);
        }
    }
    nc = w;
    if (catalog_narrow_filter(ix, cands, &nc, pq, facets != NULL, &type_keys, &ntype_keys, &type_cap, &folder_keys,
                              &nfolder_keys, &folder_cap, &month_keys, &nmonth_keys, &month_cap, &record, e) != 0) {
        goto fail;
    }
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
        facets->narrow = record;
        memset(&record, 0, sizeof record);
        if (keys_to_facets(type_keys, ntype_keys, &facets->types, &facets->ntypes) != 0 ||
            keys_to_facets(folder_keys, nfolder_keys, &facets->folders, &facets->nfolders) != 0 ||
            keys_to_facets(month_keys, nmonth_keys, &facets->months, &facets->nmonths) != 0) {
            free_key_array(type_keys, ntype_keys);
            free_key_array(folder_keys, nfolder_keys);
            free_key_array(month_keys, nmonth_keys);
            hits_free(arr, nc);
            cands_free(cands, nc);
            facets_free(facets);
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
    narrow_free(&record);
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
        int nc;
        if (cap_grow(*cap, 32, &nc) != 0 || grow_alloc((void **)arr, (size_t)nc, sizeof(Scored)) != 0) {
            return -1;
        }
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

static int parse_for_search(const char *query, const SearchModes *modes, ParsedQuery *pq, Err *e) {
    if (query_parse(query ? query : "", pq, e) != 0) {
        return -1;
    }
    if (query_expand(&pq->q, modes) != 0) {
        parsed_free(pq);
        err_set(e, "out of memory");
        return -1;
    }
    return 0;
}

int index_search(Index *ix, const char *query, int limit, const char *at, Hit **hits, int *nhits, Facets *facets,
                 const SearchModes *modes, Err *e) {
    *hits = NULL;
    *nhits = 0;
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 1000) {
        limit = 1000;
    }
    if (reload_roots(ix, e) != 0) {
        return -1;
    }
    ParsedQuery pq;
    if (parse_for_search(query, modes, &pq, e) != 0) {
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
               const SearchModes *modes, Err *e) {
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
    if (parse_for_search(query, modes, &pq, e) != 0) {
        return -1;
    }
    NarrowGate *gate = NULL;
    if (narrow_gate_open(ix, &pq, &gate, e) != 0) {
        parsed_free(&pq);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT docid, root_id, rel, title, ext, mtime FROM docs", e) != 0) {
        narrow_gate_close(gate);
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
        int eq = shown && strcmp(shown, path) == 0 && narrow_gate_ok(gate, shown);
        free(shown);
        if (eq) {
            found = sqlite3_column_int64(st, 0);
            ext = xstrdup(ex ? ex : "");
            break;
        }
    }
    sqlite3_finalize(st);
    narrow_gate_close(gate);
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

enum { READ_CAP = 48 * 1024 };

static char *cut_dup(const char *s, size_t maxb) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s);
    if (n > maxb) {
        n = utf8_prefix_bytes(s, n, maxb);
    }
    return xstrndup(s, n);
}

static uint32_t boundary_down(const char *text, uint32_t pos) {
    while (pos > 0 && ((unsigned char)text[pos] & 0xc0) == 0x80) {
        pos--;
    }
    return pos;
}

static uint32_t boundary_up(const char *text, uint32_t n, uint32_t pos) {
    while (pos < n && ((unsigned char)text[pos] & 0xc0) == 0x80) {
        pos++;
    }
    return pos;
}

/* Move to a newline within 400 bytes, then sit on a character boundary. */
static uint32_t snap_down(const char *text, uint32_t pos, uint32_t floor) {
    if (pos < floor) {
        pos = floor;
    }
    uint32_t lim = pos > 400 ? pos - 400 : 0;
    if (lim < floor) {
        lim = floor;
    }
    uint32_t i = pos;
    while (i > lim) {
        if (text[i - 1] == '\n') {
            return i;
        }
        i--;
    }
    return boundary_down(text, pos);
}

static uint32_t snap_up(const char *text, uint32_t n, uint32_t pos, uint32_t ceil) {
    if (ceil > n) {
        ceil = n;
    }
    if (pos > ceil) {
        pos = ceil;
    }
    uint32_t lim = pos + 400;
    if (lim > ceil) {
        lim = ceil;
    }
    uint32_t i = pos;
    while (i < lim) {
        if (text[i] == '\n') {
            return i + 1 <= ceil ? i + 1 : ceil;
        }
        i++;
    }
    uint32_t b = boundary_up(text, n, pos);
    return b > ceil ? ceil : b;
}

static int head_mark(const Mark *m) {
    return m->kind == MARK_HEAD && m->text && m->text[0];
}

static void section_bounds(const char *text, uint32_t n, const Mark *marks, int nmarks, uint32_t at, int *have,
                           uint32_t *hs, uint32_t *he, uint8_t *lv) {
    *have = 0;
    *hs = 0;
    *he = n;
    *lv = 1;
    (void)text;
    for (int i = 0; i < nmarks; i++) {
        if (!head_mark(&marks[i]) || marks[i].offset > at) {
            continue;
        }
        if (!*have || marks[i].offset >= *hs) {
            *hs = marks[i].offset;
            *lv = marks[i].level ? marks[i].level : 1;
            *have = 1;
        }
    }
    if (!*have) {
        return;
    }
    for (int i = 0; i < nmarks; i++) {
        if (!head_mark(&marks[i]) || marks[i].offset <= *hs) {
            continue;
        }
        uint8_t level = marks[i].level ? marks[i].level : 1;
        if (level <= *lv && marks[i].offset < *he) {
            *he = marks[i].offset;
        }
    }
}

static void read_bounds(const char *text, uint32_t n, const Mark *marks, int nmarks, uint32_t at, int dir,
                        uint32_t *from, uint32_t *to) {
    if (n == 0) {
        *from = 0;
        *to = 0;
        return;
    }
    if (at > n) {
        at = n;
    }
    if (dir > 0) {
        uint32_t b = at + READ_CAP;
        if (b > n) {
            b = n;
        }
        b = snap_up(text, n, b, n);
        if (b < at) {
            b = at;
        }
        *from = at;
        *to = b;
        return;
    }
    if (dir < 0) {
        uint32_t a = at > (uint32_t)READ_CAP ? at - (uint32_t)READ_CAP : 0;
        a = snap_down(text, a, 0);
        if (a > at) {
            a = at;
        }
        *from = a;
        *to = at;
        return;
    }
    int have = 0;
    uint32_t hs = 0;
    uint32_t he = n;
    uint8_t lv = 1;
    section_bounds(text, n, marks, nmarks, at, &have, &hs, &he, &lv);
    if (have && he > hs && he - hs <= (uint32_t)READ_CAP && at >= hs && at <= he) {
        *from = hs;
        *to = he;
        return;
    }
    uint32_t floor = have ? hs : 0;
    uint32_t ceil = have ? he : n;
    uint32_t back = (uint32_t)READ_CAP / 3;
    uint32_t a = at > back ? at - back : 0;
    if (a < floor) {
        a = floor;
    }
    a = snap_down(text, a, floor);
    uint32_t b = a + (uint32_t)READ_CAP;
    if (b > ceil) {
        b = ceil;
    }
    if (b < at) {
        b = at <= ceil ? at : ceil;
    }
    b = snap_up(text, n, b, ceil);
    if (a > at) {
        a = at;
    }
    if (b < at) {
        b = at <= n ? at : n;
    }
    *from = boundary_down(text, a);
    *to = boundary_up(text, n, b);
    if (*to < *from) {
        *to = *from;
    }
}

static char *section_at(const Mark *marks, int nmarks, uint32_t at) {
    const char *stack[8];
    uint8_t levels[8];
    int sp = 0;
    for (int i = 0; i < nmarks; i++) {
        if (!head_mark(&marks[i]) || marks[i].offset > at) {
            continue;
        }
        uint8_t lv = marks[i].level ? marks[i].level : 1;
        while (sp > 0 && levels[sp - 1] >= lv) {
            sp--;
        }
        if (sp == 8) {
            memmove(stack, stack + 1, 7 * sizeof(char *));
            memmove(levels, levels + 1, 7);
            sp = 7;
        }
        stack[sp] = marks[i].text;
        levels[sp] = lv;
        sp++;
    }
    if (sp == 0) {
        return NULL;
    }
    Buf b;
    buf_init(&b);
    for (int i = 0; i < sp; i++) {
        if (i && buf_puts(&b, " · ") != 0) {
            buf_free(&b);
            return NULL;
        }
        if (buf_puts(&b, stack[i]) != 0) {
            buf_free(&b);
            return NULL;
        }
    }
    if (b.len > 180) {
        size_t keep = utf8_prefix_bytes(b.data, b.len, 180);
        b.len = keep;
        b.data[keep] = 0;
    }
    return b.data;
}

static int page_at_marks(const Mark *marks, int nmarks, uint32_t at) {
    int page = 0;
    for (int i = 0; i < nmarks; i++) {
        if (marks[i].kind != MARK_PAGE || marks[i].offset > at || marks[i].page == 0) {
            continue;
        }
        page = (int)marks[i].page;
    }
    return page;
}

static int count_pages(const Mark *marks, int nmarks) {
    int n = 0;
    for (int i = 0; i < nmarks; i++) {
        if (marks[i].kind == MARK_PAGE && marks[i].page > 0) {
            n++;
        }
    }
    return n;
}

static int lookup_path(Index *ix, const char *path, int64_t *id, char **title, char **ext, Err *e) {
    *id = 0;
    *title = NULL;
    *ext = NULL;
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT docid, root_id, rel, title, ext FROM docs", e) != 0) {
        return -1;
    }
    int step = 0;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(st, 2);
        char *shown = display_path(ix, sqlite3_column_int64(st, 1), rel, NULL);
        int eq = shown && strcmp(shown, path) == 0;
        free(shown);
        if (!eq) {
            continue;
        }
        *id = sqlite3_column_int64(st, 0);
        const char *t = (const char *)sqlite3_column_text(st, 3);
        const char *ex = (const char *)sqlite3_column_text(st, 4);
        *title = xstrdup(t ? t : "");
        *ext = xstrdup(ex ? ex : "");
        if (!*title || !*ext) {
            sqlite3_finalize(st);
            free(*title);
            free(*ext);
            *title = NULL;
            *ext = NULL;
            err_set(e, "out of memory");
            return -1;
        }
        break;
    }
    sqlite3_finalize(st);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        free(*title);
        free(*ext);
        *title = NULL;
        *ext = NULL;
        return db_fail(ix, e, "read");
    }
    if (!*id || !*title || !*ext) {
        free(*title);
        free(*ext);
        *title = NULL;
        *ext = NULL;
        err_set(e, "that file is not in the index");
        return -1;
    }
    return 0;
}

int index_read(Index *ix, const char *path, uint32_t at, int dir, DocRead **out, Err *e) {
    *out = NULL;
    if (!ix || !path || !path[0]) {
        err_set(e, "missing file");
        return -1;
    }
    if (dir > 1 || dir < -1) {
        dir = 0;
    }
    int64_t id = 0;
    char *title = NULL;
    char *ext = NULL;
    if (lookup_path(ix, path, &id, &title, &ext, e) != 0) {
        return -1;
    }
    char *text = NULL;
    Mark *marks = NULL;
    int nmarks = 0;
    if (load_body(ix, id, &text, &marks, &nmarks, e) != 0) {
        free(title);
        free(ext);
        return -1;
    }
    size_t tn = text ? strlen(text) : 0;
    if (tn > UINT32_MAX) {
        free(title);
        free(ext);
        marks_free(marks, nmarks);
        free(text);
        err_set(e, "document is too long");
        return -1;
    }
    uint32_t n = (uint32_t)tn;
    if (at > n) {
        at = n;
    }
    uint32_t from = 0;
    uint32_t to = 0;
    read_bounds(text ? text : "", n, marks, nmarks, at, dir, &from, &to);
    DocRead *rd = calloc(1, sizeof *rd);
    if (!rd) {
        goto oom;
    }
    rd->title = title;
    title = NULL;
    rd->section = section_at(marks, nmarks, at);
    rd->from = from;
    rd->to = to;
    rd->at = at;
    rd->length = n;
    rd->text = xstrndup(text && to >= from ? text + from : "", (size_t)(to - from));
    if (!rd->text) {
        goto oom;
    }
    int npage_marks = count_pages(marks, nmarks);
    if (npage_marks > 0) {
        rd->page = page_at_marks(marks, nmarks, at);
    } else if (ext && ascii_ieq(ext, "pdf")) {
        int page = 1;
        for (uint32_t i = 0; i < at && text && text[i]; i++) {
            if (text[i] == '\f') {
                page++;
            }
        }
        rd->page = page;
    }
    int nh = 0;
    for (int i = 0; i < nmarks; i++) {
        if (head_mark(&marks[i])) {
            nh++;
        }
    }
    if (nh > 0) {
        rd->heads = calloc((size_t)nh, sizeof(DocHead));
        if (!rd->heads) {
            goto oom;
        }
        for (int i = 0; i < nmarks; i++) {
            if (!head_mark(&marks[i])) {
                continue;
            }
            rd->heads[rd->nheads].at = marks[i].offset;
            rd->heads[rd->nheads].level = marks[i].level ? marks[i].level : 1;
            rd->heads[rd->nheads].text = cut_dup(marks[i].text, 180);
            if (!rd->heads[rd->nheads].text) {
                goto oom;
            }
            rd->nheads++;
        }
    }
    int np = 0;
    for (int i = 0; i < nmarks; i++) {
        if (marks[i].kind == MARK_PAGE && marks[i].page > 0 && marks[i].offset >= from && marks[i].offset < to) {
            np++;
        }
    }
    if (np == 0 && rd->page > 0 && npage_marks == 0 && text) {
        for (uint32_t i = from; i < to; i++) {
            if (text[i] == '\f') {
                np++;
            }
        }
    }
    if (np > 0) {
        rd->pages = calloc((size_t)np, sizeof(DocPage));
        if (!rd->pages) {
            goto oom;
        }
        if (npage_marks > 0) {
            for (int i = 0; i < nmarks; i++) {
                if (marks[i].kind != MARK_PAGE || marks[i].page == 0 || marks[i].offset < from ||
                    marks[i].offset >= to) {
                    continue;
                }
                rd->pages[rd->npages].at = marks[i].offset;
                rd->pages[rd->npages].page = (int)marks[i].page;
                rd->npages++;
            }
        } else if (text) {
            int page = 1;
            for (uint32_t i = 0; i < to && text[i]; i++) {
                if (text[i] != '\f') {
                    continue;
                }
                page++;
                if (i >= from) {
                    rd->pages[rd->npages].at = i;
                    rd->pages[rd->npages].page = page;
                    rd->npages++;
                }
            }
        }
    }
    free(ext);
    marks_free(marks, nmarks);
    free(text);
    *out = rd;
    return 0;
oom:
    free(title);
    free(ext);
    marks_free(marks, nmarks);
    free(text);
    docread_free(rd);
    err_set(e, "out of memory");
    return -1;
}

void docread_free(DocRead *d) {
    if (!d) {
        return;
    }
    free(d->title);
    free(d->section);
    free(d->text);
    for (int i = 0; i < d->nheads; i++) {
        free(d->heads[i].text);
    }
    free(d->heads);
    free(d->pages);
    free(d);
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
    char *real = grove_realpath(dir, NULL);
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
    size_t alloc;
    if (size_add(n, 1, &alloc) != 0) {
        err_set(e, "out of memory");
        return NULL;
    }
    char *s = malloc(alloc);
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
        if (!copy || nr == INT_MAX || grow_alloc((void **)&rows, (size_t)nr + 1, sizeof *rows) != 0) {
            free(copy);
            sqlite3_finalize(st);
            index_saved_free(rows, nr);
            err_set(e, "out of memory");
            return -1;
        }
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
    size_t alloc;
    if (size_add(n, 1, &alloc) != 0) {
        err_set(e, "out of memory");
        return NULL;
    }
    char *out = malloc(alloc);
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
        if (off < 0 || off > UINT32_MAX || nr == INT_MAX ||
            grow_alloc((void **)&rows, (size_t)nr + 1, sizeof *rows) != 0) {
            sqlite3_finalize(st);
            index_notes_free(rows, nr);
            err_set(e, off < 0 || off > UINT32_MAX ? "note offset is out of range" : "out of memory");
            return -1;
        }
        char *pcopy = xstrdup(path ? path : "");
        char *bcopy = xstrdup(body ? body : "");
        if (!pcopy || !bcopy) {
            free(pcopy);
            free(bcopy);
            sqlite3_finalize(st);
            index_notes_free(rows, nr);
            err_set(e, "out of memory");
            return -1;
        }
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
        int nc;
        if (cap_grow(*cap, 8, &nc) != 0 || grow_alloc((void **)arr, (size_t)nc, sizeof **arr) != 0) {
            return -1;
        }
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
    NarrowGate *gate = NULL;
    if (narrow_gate_open(ix, &pq, &gate, e) != 0) {
        cands_free(cands, nc);
        mx_free(&mx);
        parsed_free(&pq);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT mtime, nsec, size FROM docs WHERE docid=?", e) != 0) {
        narrow_gate_close(gate);
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
        if (!narrow_gate_ok(gate, path)) {
            free(path);
            continue;
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
    narrow_gate_close(gate);
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
        if (!copy || nq == INT_MAX || grow_alloc((void **)&queries, (size_t)nq + 1, sizeof *queries) != 0) {
            free(copy);
            sqlite3_finalize(st);
            index_saved_free(queries, nq);
            err_set(e, "out of memory");
            return -1;
        }
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

/* docs_shelf holds the columns the shelf lists. A query that uses only those
 * columns does not read the stored text. */
static const char *SHELF_INDEX =
    "CREATE INDEX IF NOT EXISTS docs_shelf ON docs(title, root_id, rel, ext, mtime, size)";

static const char *CATALOG_SQL =
    "CREATE TABLE IF NOT EXISTS catalog("
    " path TEXT PRIMARY KEY,"
    " title TEXT NOT NULL DEFAULT '',"
    " creator TEXT NOT NULL DEFAULT '',"
    " date TEXT NOT NULL DEFAULT '',"
    " language TEXT NOT NULL DEFAULT '',"
    " type TEXT NOT NULL DEFAULT '',"
    " subject TEXT NOT NULL DEFAULT '',"
    " coverage TEXT NOT NULL DEFAULT '',"
    " source TEXT NOT NULL DEFAULT '',"
    " description TEXT NOT NULL DEFAULT '',"
    " publisher TEXT NOT NULL DEFAULT '',"
    " contributor TEXT NOT NULL DEFAULT '',"
    " format TEXT NOT NULL DEFAULT '',"
    " identifier TEXT NOT NULL DEFAULT '',"
    " relation TEXT NOT NULL DEFAULT '',"
    " rights TEXT NOT NULL DEFAULT '')";

/* The address of a fetched page. The table appears on the first fetch. */
static const char *PAGE_SQL =
    "CREATE TABLE IF NOT EXISTS web("
    " path TEXT PRIMARY KEY,"
    " url TEXT NOT NULL)";

enum {
    CF_TITLE = 0,
    CF_CREATOR,
    CF_DATE,
    CF_LANGUAGE,
    CF_TYPE,
    CF_SUBJECT,
    CF_COVERAGE,
    CF_SOURCE,
    CF_DESCRIPTION,
    CF_PUBLISHER,
    CF_CONTRIBUTOR,
    CF_FORMAT,
    CF_IDENTIFIER,
    CF_RELATION,
    CF_RIGHTS,
    CF_N
};

typedef struct Cat {
    char *path;
    char *field[CF_N];
} Cat;

static void cat_free_one(Cat *c) {
    if (!c) {
        return;
    }
    free(c->path);
    for (int i = 0; i < CF_N; i++) {
        free(c->field[i]);
    }
    memset(c, 0, sizeof *c);
}

static void cats_free(Cat *c, int n) {
    if (!c) {
        return;
    }
    for (int i = 0; i < n; i++) {
        cat_free_one(&c[i]);
    }
    free(c);
}

static int cat_cmp(const void *a, const void *b) {
    const Cat *x = a;
    const Cat *y = b;
    return strcmp(x->path ? x->path : "", y->path ? y->path : "");
}

static char *field_copy(const char *s, size_t cap, int newlines, Err *e) {
    if (!s) {
        s = "";
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) {
        n--;
    }
    if (n > cap) {
        err_set(e, "that field is too long");
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (!newlines && (s[i] == '\n' || s[i] == '\r')) {
            err_set(e, "that field is one line");
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

static const char *vol_field(const Volume *v, int i) {
    if (!v) {
        return "";
    }
    switch (i) {
    case CF_TITLE:
        return v->title;
    case CF_CREATOR:
        return v->creator;
    case CF_DATE:
        return v->date;
    case CF_LANGUAGE:
        return v->language;
    case CF_TYPE:
        return v->type;
    case CF_SUBJECT:
        return v->subject;
    case CF_COVERAGE:
        return v->coverage;
    case CF_SOURCE:
        return v->source;
    case CF_DESCRIPTION:
        return v->description;
    case CF_PUBLISHER:
        return v->publisher;
    case CF_CONTRIBUTOR:
        return v->contributor;
    case CF_FORMAT:
        return v->format;
    case CF_IDENTIFIER:
        return v->identifier;
    case CF_RELATION:
        return v->relation;
    case CF_RIGHTS:
        return v->rights;
    default:
        return "";
    }
}

static void vol_set_field(Volume *v, int i, char *owned) {
    char **slot = NULL;
    switch (i) {
    case CF_TITLE:
        slot = &v->title;
        break;
    case CF_CREATOR:
        slot = &v->creator;
        break;
    case CF_DATE:
        slot = &v->date;
        break;
    case CF_LANGUAGE:
        slot = &v->language;
        break;
    case CF_TYPE:
        slot = &v->type;
        break;
    case CF_SUBJECT:
        slot = &v->subject;
        break;
    case CF_COVERAGE:
        slot = &v->coverage;
        break;
    case CF_SOURCE:
        slot = &v->source;
        break;
    case CF_DESCRIPTION:
        slot = &v->description;
        break;
    case CF_PUBLISHER:
        slot = &v->publisher;
        break;
    case CF_CONTRIBUTOR:
        slot = &v->contributor;
        break;
    case CF_FORMAT:
        slot = &v->format;
        break;
    case CF_IDENTIFIER:
        slot = &v->identifier;
        break;
    case CF_RELATION:
        slot = &v->relation;
        break;
    case CF_RIGHTS:
        slot = &v->rights;
        break;
    default:
        free(owned);
        return;
    }
    free(*slot);
    *slot = owned;
}

static void volume_clear(Volume *v) {
    if (!v) {
        return;
    }
    free(v->path);
    free(v->ext);
    for (int i = 0; i < CF_N; i++) {
        vol_set_field(v, i, NULL);
    }
    memset(v, 0, sizeof *v);
}

static void volumes_free(Volume *vols, int n) {
    if (!vols) {
        return;
    }
    for (int i = 0; i < n; i++) {
        volume_clear(&vols[i]);
    }
    free(vols);
}

void library_free(Library *lib) {
    if (!lib) {
        return;
    }
    if (lib->volumes) {
        for (int i = 0; i < lib->n; i++) {
            volume_clear(&lib->volumes[i]);
        }
        free(lib->volumes);
    }
    if (lib->roots) {
        for (int i = 0; i < lib->nroots; i++) {
            free(lib->roots[i]);
        }
        free(lib->roots);
    }
    free(lib->root_full);
    narrow_free(&lib->narrow);
    memset(lib, 0, sizeof *lib);
}

int index_shelf_ready(Index *ix) {
    if (!ix) {
        return 0;
    }
    sqlite3_stmt *st = NULL;
    Err e;
    err_clear(&e);
    if (prep(ix, &st, "SELECT 1 FROM sqlite_master WHERE type='index' AND name='docs_shelf'", &e) != 0) {
        return 0;
    }
    int yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return yes;
}

int index_shelf_build(Index *ix, Err *e) {
    if (!ix || !ix->writable) {
        err_set(e, "index is open read-only");
        return -1;
    }
    if (index_shelf_ready(ix)) {
        return 0;
    }
    return sql_exec(ix, SHELF_INDEX, e);
}

static int write_db(Index *ix, sqlite3 **db, int fts, Err *e) {
    *db = NULL;
    if (!ix || !ix->db_path) {
        err_set(e, "no index");
        return -1;
    }
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
    if (fts && grove_fts_register(*db, e) != 0) {
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    return 0;
}

static int doc_by_path(Index *ix, sqlite3 *db, const char *path, int64_t *id, Err *e) {
    sqlite3_stmt *st = NULL;
    *id = 0;
    if (sqlite3_prepare_v2(db, "SELECT docid, root_id, rel FROM docs", -1, &st, NULL) != SQLITE_OK) {
        return saved_fail(db, e);
    }
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(st, 2);
        char *shown = display_path(ix, sqlite3_column_int64(st, 1), rel, NULL);
        int eq = shown && path && strcmp(shown, path) == 0;
        free(shown);
        if (!eq) {
            continue;
        }
        *id = sqlite3_column_int64(st, 0);
        break;
    }
    sqlite3_finalize(st);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        return saved_fail(db, e);
    }
    if (!*id) {
        err_set(e, "that file is not in the library");
        return -1;
    }
    return 0;
}

static int load_catalog(sqlite3 *db, Cat **out, int *nout, Err *e) {
    *out = NULL;
    *nout = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='catalog'", -1, &st, NULL) !=
        SQLITE_OK) {
        return saved_fail(db, e);
    }
    int yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    st = NULL;
    if (!yes) {
        return 0;
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT path, title, creator, date, language, type, subject, coverage, source,"
                           " description, publisher, contributor, format, identifier, relation, rights"
                           " FROM catalog",
                           -1, &st, NULL) != SQLITE_OK) {
        return saved_fail(db, e);
    }
    Cat *arr = NULL;
    int n = 0;
    int cap = 0;
    int step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            int nc;
            if (cap_grow(cap, 16, &nc) != 0 || grow_alloc((void **)&arr, (size_t)nc, sizeof *arr) != 0) {
                sqlite3_finalize(st);
                cats_free(arr, n);
                err_set(e, "out of memory");
                return -1;
            }
            cap = nc;
        }
        Cat *c = &arr[n];
        memset(c, 0, sizeof *c);
        c->path = xstrdup((const char *)sqlite3_column_text(st, 0));
        int ok = c->path != NULL;
        for (int i = 0; ok && i < CF_N; i++) {
            const char *t = (const char *)sqlite3_column_text(st, i + 1);
            c->field[i] = xstrdup(t ? t : "");
            if (!c->field[i]) {
                ok = 0;
            }
        }
        if (!ok) {
            sqlite3_finalize(st);
            cats_free(arr, n + 1);
            err_set(e, "out of memory");
            return -1;
        }
        n++;
    }
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        cats_free(arr, n);
        return saved_fail(db, e);
    }
    if (n > 1) {
        qsort(arr, (size_t)n, sizeof *arr, cat_cmp);
    }
    *out = arr;
    *nout = n;
    return 0;
}

static const Cat *cat_find(Cat *arr, int n, const char *path) {
    if (!arr || n <= 0 || !path) {
        return NULL;
    }
    Cat key;
    memset(&key, 0, sizeof key);
    key.path = (char *)path;
    return bsearch(&key, arr, (size_t)n, sizeof *arr, cat_cmp);
}

static int shelf_match(const char *needle, const char *path, const char *title, const char *ext, const Cat *cat) {
    if (!needle || !needle[0]) {
        return 1;
    }
    size_t nn = strlen(needle);
    const char *bits[8];
    int nb = 0;
    bits[nb++] = path;
    bits[nb++] = title;
    bits[nb++] = ext;
    if (cat) {
        bits[nb++] = cat->field[CF_CREATOR];
        bits[nb++] = cat->field[CF_SUBJECT];
        bits[nb++] = cat->field[CF_LANGUAGE];
        bits[nb++] = cat->field[CF_TYPE];
        bits[nb++] = cat->field[CF_IDENTIFIER];
    }
    for (int i = 0; i < nb; i++) {
        const char *s = bits[i] ? bits[i] : "";
        if (fold_contains(s, strlen(s), needle, nn)) {
            return 1;
        }
    }
    return 0;
}

static int text_blank(const char *s);

/* A file named "Title — Author.ext" carries the creator in its name when the
 * record has none. The mark is a space, an em dash (U+2014), and a space.
 * out is NULL when the name has no author. -1 is out of memory. */
static int creator_in_name(const char *path, char **out) {
    const char *base;
    const char *hit;
    const char *p;
    const char *who;
    const char *dot;
    size_t n;
    *out = NULL;
    if (!path) {
        return 0;
    }
    base = path;
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    hit = NULL;
    p = base;
    while (*p) {
        if (p[0] == ' ' && (unsigned char)p[1] == 0xE2 && (unsigned char)p[2] == 0x80 &&
            (unsigned char)p[3] == 0x94 && p[4] == ' ') {
            hit = p;
            p += 5;
            continue;
        }
        p++;
    }
    if (!hit) {
        return 0;
    }
    who = hit + 5;
    n = strlen(who);
    dot = NULL;
    for (p = who; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (dot && dot != who) {
        size_t extn = strlen(dot + 1);
        int plain = extn >= 1 && extn <= 8;
        for (size_t i = 0; plain && i < extn; i++) {
            unsigned char c = (unsigned char)dot[1 + i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
                plain = 0;
            }
        }
        if (plain) {
            n = (size_t)(dot - who);
        }
    }
    while (n > 0 && (who[0] == ' ' || who[0] == '\t')) {
        who++;
        n--;
    }
    while (n > 0 && (who[n - 1] == ' ' || who[n - 1] == '\t')) {
        n--;
    }
    if (n == 0) {
        return 0;
    }
    if (n > 400) {
        n = 400;
    }
    *out = xstrndup(who, n);
    return *out ? 0 : -1;
}

static int volume_push(Volume *v, const char *path, const char *title, const char *ext, int64_t mtime, int64_t size,
                       const Cat *cat, const char *named) {
    memset(v, 0, sizeof *v);
    v->path = xstrdup(path ? path : "");
    v->ext = xstrdup(ext ? ext : "");
    v->mtime = mtime;
    v->size = size;
    const char *shown = title ? title : "";
    if (cat && cat->field[CF_TITLE] && cat->field[CF_TITLE][0]) {
        shown = cat->field[CF_TITLE];
    }
    v->title = xstrdup(shown);
    int ok = v->path && v->ext && v->title;
    for (int i = CF_CREATOR; ok && i < CF_N; i++) {
        const char *s = cat && cat->field[i] ? cat->field[i] : "";
        if (i == CF_CREATOR && text_blank(s) && named && named[0]) {
            s = named;
        }
        char *copy = xstrdup(s);
        if (!copy) {
            ok = 0;
            break;
        }
        vol_set_field(v, i, copy);
    }
    if (!ok) {
        volume_clear(v);
        return -1;
    }
    return 0;
}

enum { NG_LANGUAGE = 0, NG_AUTHOR, NG_FORM, NG_PERIOD, NG_GENRE, NG_N, NARROW_SEL_MAX = 64 };

typedef struct NarrowSel {
    char **v[NG_N];
    int n[NG_N];
    int owned;
} NarrowSel;

struct NarrowGate {
    Cat *cats;
    int ncats;
    NarrowSel sel;
};

static void sel_clear(NarrowSel *s) {
    if (!s) {
        return;
    }
    if (s->owned) {
        for (int g = 0; g < NG_N; g++) {
            for (int i = 0; i < s->n[g]; i++) {
                free(s->v[g][i]);
            }
            free(s->v[g]);
        }
    }
    memset(s, 0, sizeof *s);
}

static int sel_any(const NarrowSel *s) {
    if (!s) {
        return 0;
    }
    for (int g = 0; g < NG_N; g++) {
        if (s->n[g] > 0) {
            return 1;
        }
    }
    return 0;
}

static void sel_from_query(NarrowSel *s, const ParsedQuery *pq) {
    memset(s, 0, sizeof *s);
    if (!pq) {
        return;
    }
    s->v[NG_LANGUAGE] = pq->language;
    s->n[NG_LANGUAGE] = pq->nlanguage;
    s->v[NG_AUTHOR] = pq->author;
    s->n[NG_AUTHOR] = pq->nauthor;
    s->v[NG_FORM] = pq->form;
    s->n[NG_FORM] = pq->nform;
    s->v[NG_PERIOD] = pq->period;
    s->n[NG_PERIOD] = pq->nperiod;
    s->v[NG_GENRE] = pq->genre;
    s->n[NG_GENRE] = pq->ngenre;
}

static int sel_add(char ***arr, int *n, char *owned, Err *e) {
    if (*n >= NARROW_SEL_MAX) {
        free(owned);
        err_set(e, "too many values");
        return -1;
    }
    size_t bytes = 0;
    if (size_mul((size_t)*n + 1, sizeof(char *), &bytes) != 0) {
        free(owned);
        err_set(e, "out of memory");
        return -1;
    }
    char **grown = realloc(*arr, bytes);
    if (!grown) {
        free(owned);
        err_set(e, "out of memory");
        return -1;
    }
    *arr = grown;
    (*arr)[(*n)++] = owned;
    return 0;
}

static int parse_sel_lines(const char *s, char ***out, int *nout, Err *e) {
    *out = NULL;
    *nout = 0;
    if (!s || !s[0]) {
        return 0;
    }
    const char *p = s;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') {
            nl++;
        }
        size_t raw = (size_t)(nl - p);
        if (raw > 0) {
            char *tmp = xstrndup(p, raw);
            if (!tmp) {
                err_set(e, "out of memory");
                return -1;
            }
            char *piece = field_copy(tmp, 400, 0, e);
            free(tmp);
            if (!piece) {
                return -1;
            }
            if (piece[0]) {
                if (sel_add(out, nout, piece, e) != 0) {
                    return -1;
                }
            } else {
                free(piece);
            }
        }
        if (!*nl) {
            break;
        }
        p = nl + 1;
        if (*nl == '\r' && *p == '\n') {
            p++;
        }
    }
    return 0;
}

static int narrow_splits(int g) {
    return g == NG_LANGUAGE || g == NG_AUTHOR;
}

static const char *narrow_field(const Cat *cat, int g) {
    int id = CF_LANGUAGE;
    if (!cat) {
        return "";
    }
    switch (g) {
    case NG_LANGUAGE:
        id = CF_LANGUAGE;
        break;
    case NG_AUTHOR:
        id = CF_CREATOR;
        break;
    case NG_FORM:
        id = CF_TYPE;
        break;
    case NG_PERIOD:
        id = CF_COVERAGE;
        break;
    case NG_GENRE:
        id = CF_SUBJECT;
        break;
    default:
        return "";
    }
    return cat->field[id] ? cat->field[id] : "";
}

static void span_trim(const char *s, size_t n, const char **out, size_t *outn) {
    while (n > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) {
        n--;
    }
    *out = s;
    *outn = n;
}

static int token_eq(const char *s, size_t n, const char *want) {
    size_t wn;
    if (!want) {
        return 0;
    }
    wn = strlen(want);
    return n == wn && memcmp(s, want, n) == 0;
}

static int field_has(const char *field, int split, const char *want) {
    const char *p;
    if (!want || !want[0] || !field) {
        return 0;
    }
    if (!split) {
        const char *s = NULL;
        size_t n = 0;
        span_trim(field, strlen(field), &s, &n);
        return token_eq(s, n, want);
    }
    p = field;
    for (;;) {
        const char *semi = strchr(p, ';');
        size_t raw = semi ? (size_t)(semi - p) : strlen(p);
        const char *s = NULL;
        size_t n = 0;
        span_trim(p, raw, &s, &n);
        if (n > 0 && token_eq(s, n, want)) {
            return 1;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

static int group_ok(const char *field, int split, char **sel, int nsel) {
    if (nsel <= 0) {
        return 1;
    }
    for (int i = 0; i < nsel; i++) {
        if (field_has(field, split, sel[i])) {
            return 1;
        }
    }
    return 0;
}

/* skip is one group to ignore, or -1 to apply every group. */
static int narrow_passes(const Cat *cat, const NarrowSel *sel, int skip) {
    for (int g = 0; g < NG_N; g++) {
        if (g == skip || !sel || sel->n[g] <= 0) {
            continue;
        }
        if (!group_ok(narrow_field(cat, g), narrow_splits(g), sel->v[g], sel->n[g])) {
            return 0;
        }
    }
    return 1;
}

typedef struct TokAcc {
    char **keys;
    int n;
    int cap;
    int err;
} TokAcc;

static int tok_keep(const char *s, size_t n, void *ud) {
    TokAcc *a = ud;
    char *copy = xstrndup(s, n);
    if (!copy || push_key(&a->keys, &a->n, &a->cap, copy) != 0) {
        a->err = 1;
        return 1;
    }
    return 0;
}

static int each_token(const char *field, int split, int (*cb)(const char *, size_t, void *), void *ud) {
    const char *p;
    if (!field || !field[0]) {
        return 0;
    }
    if (!split) {
        const char *s = NULL;
        size_t n = 0;
        span_trim(field, strlen(field), &s, &n);
        if (n == 0) {
            return 0;
        }
        return cb(s, n, ud);
    }
    p = field;
    for (;;) {
        const char *semi = strchr(p, ';');
        size_t raw = semi ? (size_t)(semi - p) : strlen(p);
        const char *s = NULL;
        size_t n = 0;
        span_trim(p, raw, &s, &n);
        if (n > 0) {
            int rc = cb(s, n, ud);
            if (rc) {
                return rc;
            }
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

static int facet_keep_selected(Facet **out, int *nout, char **sel, int nsel) {
    for (int i = 0; i < nsel; i++) {
        const char *v = sel ? sel[i] : NULL;
        int found = 0;
        if (!v || !v[0]) {
            continue;
        }
        for (int j = 0; j < *nout; j++) {
            if ((*out)[j].value && strcmp((*out)[j].value, v) == 0) {
                found = 1;
                break;
            }
        }
        if (found) {
            continue;
        }
        if (*nout < 0) {
            return -1;
        }
        size_t bytes = 0;
        Facet *grown;
        char *copy;
        if (size_mul((size_t)*nout + 1, sizeof(Facet), &bytes) != 0) {
            return -1;
        }
        grown = realloc(*out, bytes);
        if (!grown) {
            return -1;
        }
        *out = grown;
        copy = xstrdup(v);
        if (!copy) {
            return -1;
        }
        (*out)[*nout].value = copy;
        (*out)[*nout].count = 0;
        (*nout)++;
    }
    if (*nout > 1) {
        qsort(*out, (size_t)*nout, sizeof(Facet), facet_cmp);
    }
    return 0;
}

static Facet **narrow_slot(Narrow *n, int g, int **count) {
    switch (g) {
    case NG_AUTHOR:
        *count = &n->nauthor;
        return &n->author;
    case NG_FORM:
        *count = &n->nform;
        return &n->form;
    case NG_PERIOD:
        *count = &n->nperiod;
        return &n->period;
    case NG_GENRE:
        *count = &n->ngenre;
        return &n->genre;
    default:
        *count = &n->nlanguage;
        return &n->language;
    }
}

static int fill_narrow(const Cat *const *rows, int nrows, const NarrowSel *sel, Narrow *out, Err *e) {
    memset(out, 0, sizeof *out);
    for (int g = 0; g < NG_N; g++) {
        TokAcc acc;
        int *count = NULL;
        Facet **slot;
        memset(&acc, 0, sizeof acc);
        for (int i = 0; i < nrows; i++) {
            const Cat *cat = rows ? rows[i] : NULL;
            if (!narrow_passes(cat, sel, g)) {
                continue;
            }
            if (each_token(narrow_field(cat, g), narrow_splits(g), tok_keep, &acc) != 0 || acc.err) {
                free_key_array(acc.keys, acc.n);
                narrow_free(out);
                err_set(e, "out of memory");
                return -1;
            }
        }
        slot = narrow_slot(out, g, &count);
        if (keys_to_facets(acc.keys, acc.n, slot, count) != 0) {
            free_key_array(acc.keys, acc.n);
            narrow_free(out);
            err_set(e, "out of memory");
            return -1;
        }
        free_key_array(acc.keys, acc.n);
        if (facet_keep_selected(slot, count, sel ? sel->v[g] : NULL, sel ? sel->n[g] : 0) != 0) {
            narrow_free(out);
            err_set(e, "out of memory");
            return -1;
        }
    }
    return 0;
}

static int push_file_facet(const Cand *c, char ***type_keys, int *ntype, int *type_cap, char ***folder_keys,
                           int *nfolder, int *folder_cap, char ***month_keys, int *nmonth, int *month_cap, Err *e) {
    if (c->ext && c->ext[0]) {
        char *copy = xstrdup(c->ext);
        if (!copy || push_key(type_keys, ntype, type_cap, copy) != 0) {
            err_set(e, "out of memory");
            return -1;
        }
    }
    char *folder = NULL;
    if (parent_folder(c->rel, &folder) != 0) {
        err_set(e, "out of memory");
        return -1;
    }
    if (folder && push_key(folder_keys, nfolder, folder_cap, folder) != 0) {
        err_set(e, "out of memory");
        return -1;
    }
    char *month = month_key(c->mtime);
    if (!month || push_key(month_keys, nmonth, month_cap, month) != 0) {
        err_set(e, "out of memory");
        return -1;
    }
    return 0;
}

static int catalog_narrow_filter(Index *ix, Cand *cands, int *nc, const ParsedQuery *pq, int with_facets,
                                 char ***type_keys, int *ntype, int *type_cap, char ***folder_keys, int *nfolder,
                                 int *folder_cap, char ***month_keys, int *nmonth, int *month_cap, Narrow *narrow,
                                 Err *e) {
    NarrowSel sel;
    Cat *cats = NULL;
    int ncats = 0;
    const Cat **rows = NULL;
    int filter;
    memset(narrow, 0, sizeof *narrow);
    sel_from_query(&sel, pq);
    filter = sel_any(&sel);
    if (!filter && !with_facets) {
        return 0;
    }
    if (load_catalog(ix->db, &cats, &ncats, e) != 0) {
        return -1;
    }
    if (*nc > 0) {
        size_t bytes = 0;
        if (size_mul((size_t)*nc, sizeof *rows, &bytes) != 0) {
            cats_free(cats, ncats);
            err_set(e, "out of memory");
            return -1;
        }
        rows = calloc((size_t)*nc, sizeof *rows);
        if (!rows) {
            cats_free(cats, ncats);
            err_set(e, "out of memory");
            return -1;
        }
        for (int i = 0; i < *nc; i++) {
            rows[i] = cat_find(cats, ncats, cands[i].path);
        }
    }
    if (with_facets) {
        if (fill_narrow(rows, *nc, &sel, narrow, e) != 0) {
            free(rows);
            cats_free(cats, ncats);
            return -1;
        }
        for (int i = 0; i < *nc; i++) {
            if (!narrow_passes(rows ? rows[i] : NULL, &sel, -1)) {
                continue;
            }
            if (push_file_facet(&cands[i], type_keys, ntype, type_cap, folder_keys, nfolder, folder_cap, month_keys,
                                nmonth, month_cap, e) != 0) {
                narrow_free(narrow);
                free(rows);
                cats_free(cats, ncats);
                return -1;
            }
        }
    }
    int w = 0;
    for (int i = 0; i < *nc; i++) {
        const Cat *cat = rows ? rows[i] : NULL;
        if (filter && !narrow_passes(cat, &sel, -1)) {
            cand_free(&cands[i]);
            continue;
        }
        cands[w++] = cands[i];
        if (w - 1 != i) {
            memset(&cands[i], 0, sizeof cands[i]);
        }
    }
    *nc = w;
    free(rows);
    cats_free(cats, ncats);
    return 0;
}

static int narrow_gate_open(Index *ix, const ParsedQuery *pq, NarrowGate **out, Err *e) {
    NarrowGate *g = calloc(1, sizeof *g);
    *out = NULL;
    if (!g) {
        err_set(e, "out of memory");
        return -1;
    }
    sel_from_query(&g->sel, pq);
    if (sel_any(&g->sel) && load_catalog(ix->db, &g->cats, &g->ncats, e) != 0) {
        free(g);
        return -1;
    }
    *out = g;
    return 0;
}

static void narrow_gate_close(NarrowGate *g) {
    if (!g) {
        return;
    }
    cats_free(g->cats, g->ncats);
    free(g);
}

static int narrow_gate_ok(const NarrowGate *g, const char *path) {
    if (!g || !sel_any(&g->sel)) {
        return 1;
    }
    return narrow_passes(cat_find(g->cats, g->ncats, path), &g->sel, -1);
}

typedef struct ShelfRow {
    char *path;
    char *title;
    char *ext;
    char *named;
    int64_t mtime;
    int64_t size;
    const Cat *cat;
} ShelfRow;

static void shelf_rows_free(ShelfRow *rows, int n) {
    if (!rows) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(rows[i].path);
        free(rows[i].title);
        free(rows[i].ext);
        free(rows[i].named);
    }
    free(rows);
}

/* One passing row, ready for qsort. The strings are not owned. */
typedef struct SortRef {
    int index;
    int empty;
    int desc;
    const char *key;
    const char *tie_title;
    const char *tie_path;
} SortRef;

static int text_blank(const char *s) {
    size_t i = 0;
    size_t n;
    if (!s || !s[0]) {
        return 1;
    }
    n = strlen(s);
    while (i < n) {
        uint32_t cp = 0;
        utf8_next(s, n, &i, &cp);
        if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r' && cp != '\f' && cp != '\v') {
            return 0;
        }
    }
    return 1;
}

static int fold_cmp(const char *a, const char *b) {
    const char *sa = a ? a : "";
    const char *sb = b ? b : "";
    size_t ia = 0;
    size_t ib = 0;
    size_t na = strlen(sa);
    size_t nb = strlen(sb);
    while (ia < na && ib < nb) {
        uint32_t ca = 0;
        uint32_t cb = 0;
        utf8_next(sa, na, &ia, &ca);
        utf8_next(sb, nb, &ib, &cb);
        ca = casefold_cp(ca);
        cb = casefold_cp(cb);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (ia >= na && ib >= nb) {
        return 0;
    }
    return ia < na ? 1 : -1;
}

static const char *row_shown_title(const ShelfRow *r) {
    if (!r) {
        return "";
    }
    if (r->cat && r->cat->field[CF_TITLE] && r->cat->field[CF_TITLE][0]) {
        return r->cat->field[CF_TITLE];
    }
    return r->title ? r->title : "";
}

static const char *row_sort_text(const ShelfRow *r, int col) {
    int cf = -1;
    if (!r || col == LIB_SORT_TITLE) {
        return row_shown_title(r);
    }
    switch (col) {
    case LIB_SORT_CREATOR:
        if (r->named && r->named[0] &&
            !(r->cat && r->cat->field[CF_CREATOR] && !text_blank(r->cat->field[CF_CREATOR]))) {
            return r->named;
        }
        cf = CF_CREATOR;
        break;
    case LIB_SORT_DATE:
        cf = CF_DATE;
        break;
    case LIB_SORT_LANGUAGE:
        cf = CF_LANGUAGE;
        break;
    case LIB_SORT_TYPE:
        cf = CF_TYPE;
        break;
    case LIB_SORT_SUBJECT:
        cf = CF_SUBJECT;
        break;
    case LIB_SORT_COVERAGE:
        cf = CF_COVERAGE;
        break;
    case LIB_SORT_SOURCE:
        cf = CF_SOURCE;
        break;
    default:
        return row_shown_title(r);
    }
    if (!r->cat || !r->cat->field[cf]) {
        return "";
    }
    return r->cat->field[cf];
}

static int sortref_cmp(const void *va, const void *vb) {
    const SortRef *a = va;
    const SortRef *b = vb;
    int cmp;
    if (a->empty != b->empty) {
        return a->empty - b->empty;
    }
    cmp = fold_cmp(a->key, b->key);
    if (cmp != 0) {
        return a->desc ? -cmp : cmp;
    }
    cmp = fold_cmp(a->tie_title, b->tie_title);
    if (cmp != 0) {
        return cmp;
    }
    return strcmp(a->tie_path ? a->tie_path : "", b->tie_path ? b->tie_path : "");
}

static int library_fail(char *needle, NarrowSel *sel, Cat *cats, int ncats, ShelfRow *rows, int nrows, const Cat **of,
                        Volume *vols, int nvols, Narrow *built, sqlite3_stmt *st) {
    if (st) {
        sqlite3_finalize(st);
    }
    narrow_free(built);
    volumes_free(vols, nvols);
    shelf_rows_free(rows, nrows);
    free((void *)of);
    cats_free(cats, ncats);
    sel_clear(sel);
    free(needle);
    return -1;
}

static int library_fill(Index *ix, const char *filter, const char *language, const char *author, const char *form,
                        const char *period, const char *genre, int sort_col, int sort_desc, Library *out, Err *e) {
    char *needle = NULL;
    NarrowSel sel;
    Cat *cats = NULL;
    int ncats = 0;
    ShelfRow *rows = NULL;
    int nrows = 0;
    const Cat **of = NULL;
    Volume *vols = NULL;
    int nvols = 0;
    int total = 0;
    Narrow built;
    sqlite3_stmt *st = NULL;
    const char *lines[NG_N];
    const char *list_sql;
    int step = SQLITE_DONE;
    memset(out, 0, sizeof *out);
    memset(&sel, 0, sizeof sel);
    memset(&built, 0, sizeof built);
    sel.owned = 1;
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    if (reload_roots(ix, e) != 0) {
        return -1;
    }
    if (filter && filter[0]) {
        needle = field_copy(filter, 200, 0, e);
        if (!needle) {
            return -1;
        }
    }
    lines[NG_LANGUAGE] = language;
    lines[NG_AUTHOR] = author;
    lines[NG_FORM] = form;
    lines[NG_PERIOD] = period;
    lines[NG_GENRE] = genre;
    for (int g = 0; g < NG_N; g++) {
        if (parse_sel_lines(lines[g], &sel.v[g], &sel.n[g], e) != 0) {
            return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
        }
    }
    if (load_catalog(ix->db, &cats, &ncats, e) != 0) {
        return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
    }
    list_sql = index_shelf_ready(ix) ? "SELECT title, ext, mtime, size, root_id, rel FROM docs INDEXED BY docs_shelf "
                                       "ORDER BY title, rel"
                                     : "SELECT title, ext, mtime, size, root_id, rel FROM docs ORDER BY title, rel";
    if (prep(ix, &st, list_sql, e) != 0) {
        return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
    }
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char *title = (const char *)sqlite3_column_text(st, 0);
        const char *ext = (const char *)sqlite3_column_text(st, 1);
        int64_t mtime = sqlite3_column_int64(st, 2);
        int64_t size = sqlite3_column_int64(st, 3);
        int64_t root_id = sqlite3_column_int64(st, 4);
        const char *rel = (const char *)sqlite3_column_text(st, 5);
        char *path = display_path(ix, root_id, rel, NULL);
        char *title_copy;
        char *ext_copy;
        size_t bytes = 0;
        ShelfRow *grown;
        const Cat *cat;
        if (!path) {
            err_set(e, "out of memory");
            return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
        }
        cat = cat_find(cats, ncats, path);
        if (!shelf_match(needle, path, title, ext, cat)) {
            free(path);
            continue;
        }
        title_copy = xstrdup(title ? title : "");
        ext_copy = xstrdup(ext ? ext : "");
        if (!title_copy || !ext_copy || size_mul((size_t)nrows + 1, sizeof *rows, &bytes) != 0) {
            free(path);
            free(title_copy);
            free(ext_copy);
            err_set(e, "out of memory");
            return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
        }
        grown = realloc(rows, bytes);
        if (!grown) {
            free(path);
            free(title_copy);
            free(ext_copy);
            err_set(e, "out of memory");
            return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
        }
        rows = grown;
        rows[nrows].path = path;
        rows[nrows].title = title_copy;
        rows[nrows].ext = ext_copy;
        rows[nrows].named = NULL;
        rows[nrows].mtime = mtime;
        rows[nrows].size = size;
        rows[nrows].cat = cat;
        if (creator_in_name(path, &rows[nrows].named) != 0) {
            nrows++;
            err_set(e, "out of memory");
            return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
        }
        nrows++;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (step != SQLITE_DONE) {
        db_fail(ix, e, "library");
        return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
    }
    if (nrows > 0) {
        size_t bytes = 0;
        if (size_mul((size_t)nrows, sizeof *of, &bytes) != 0) {
            err_set(e, "out of memory");
            return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
        }
        of = calloc((size_t)nrows, sizeof *of);
        if (!of) {
            err_set(e, "out of memory");
            return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
        }
        for (int i = 0; i < nrows; i++) {
            of[i] = rows[i].cat;
        }
    }
    if (fill_narrow(of, nrows, &sel, &built, e) != 0) {
        return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
    }
    if (sort_col < 0) {
        for (int i = 0; i < nrows; i++) {
            Volume one;
            size_t bytes = 0;
            Volume *grown;
            if (!narrow_passes(rows[i].cat, &sel, -1)) {
                continue;
            }
            total++;
            if (nvols >= LIBRARY_SHOW) {
                continue;
            }
            if (volume_push(&one, rows[i].path, rows[i].title, rows[i].ext, rows[i].mtime, rows[i].size,
                            rows[i].cat, rows[i].named) != 0) {
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            if (size_mul((size_t)nvols + 1, sizeof *vols, &bytes) != 0) {
                volume_clear(&one);
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            grown = realloc(vols, bytes);
            if (!grown) {
                volume_clear(&one);
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            vols = grown;
            vols[nvols++] = one;
        }
    } else {
        /* Order every match, then keep the first LIBRARY_SHOW. */
        SortRef *refs = NULL;
        int nref = 0;
        int take;
        for (int i = 0; i < nrows; i++) {
            if (narrow_passes(rows[i].cat, &sel, -1)) {
                nref++;
            }
        }
        if (nref > 0) {
            size_t bytes = 0;
            int k = 0;
            if (size_mul((size_t)nref, sizeof *refs, &bytes) != 0) {
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            refs = malloc(bytes);
            if (!refs) {
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            for (int i = 0; i < nrows; i++) {
                const char *key;
                if (!narrow_passes(rows[i].cat, &sel, -1)) {
                    continue;
                }
                key = row_sort_text(&rows[i], sort_col);
                refs[k].index = i;
                refs[k].empty = text_blank(key);
                refs[k].desc = sort_desc ? 1 : 0;
                refs[k].key = key ? key : "";
                refs[k].tie_title = row_shown_title(&rows[i]);
                refs[k].tie_path = rows[i].path ? rows[i].path : "";
                k++;
            }
            qsort(refs, (size_t)k, sizeof *refs, sortref_cmp);
            nref = k;
        }
        total = nref;
        take = nref < LIBRARY_SHOW ? nref : LIBRARY_SHOW;
        for (int i = 0; i < take; i++) {
            int ri = refs[i].index;
            Volume one;
            size_t bytes = 0;
            Volume *grown;
            if (volume_push(&one, rows[ri].path, rows[ri].title, rows[ri].ext, rows[ri].mtime, rows[ri].size,
                            rows[ri].cat, rows[ri].named) != 0) {
                free(refs);
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            if (size_mul((size_t)nvols + 1, sizeof *vols, &bytes) != 0) {
                volume_clear(&one);
                free(refs);
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            grown = realloc(vols, bytes);
            if (!grown) {
                volume_clear(&one);
                free(refs);
                err_set(e, "out of memory");
                return library_fail(needle, &sel, cats, ncats, rows, nrows, of, vols, nvols, &built, st);
            }
            vols = grown;
            vols[nvols++] = one;
        }
        free(refs);
    }
    out->volumes = vols;
    out->n = nvols;
    out->total = total;
    out->narrow = built;
    vols = NULL;
    nvols = 0;
    memset(&built, 0, sizeof built);
    shelf_rows_free(rows, nrows);
    rows = NULL;
    free(of);
    of = NULL;
    cats_free(cats, ncats);
    cats = NULL;
    sel_clear(&sel);
    free(needle);
    needle = NULL;
    if (ix->nroots > 0) {
        out->roots = calloc(ix->nroots, sizeof *out->roots);
        out->root_full = calloc(ix->nroots, sizeof *out->root_full);
        if (!out->roots || !out->root_full) {
            library_free(out);
            err_set(e, "out of memory");
            return -1;
        }
        for (uint32_t i = 0; i < ix->nroots; i++) {
            out->roots[i] = xstrdup(ix->roots[i] ? ix->roots[i] : "");
            if (!out->roots[i]) {
                library_free(out);
                err_set(e, "out of memory");
                return -1;
            }
            out->root_full[i] = ix->root_full[i];
            out->nroots++;
        }
    }
    return 0;
}

int index_library_narrow(Index *ix, const char *filter, const char *language, const char *author, const char *form,
                         const char *period, const char *genre, Library *out, Err *e) {
    return library_fill(ix, filter, language, author, form, period, genre, -1, 0, out, e);
}

int index_library_list(Index *ix, const char *filter, Library *out, Err *e) {
    return index_library_narrow(ix, filter, NULL, NULL, NULL, NULL, NULL, out, e);
}

int index_library_sorted(Index *ix, const char *filter, const char *language, const char *author, const char *form,
                         const char *period, const char *genre, int sort_col, int sort_desc, Library *out, Err *e) {
    if (sort_col < LIB_SORT_TITLE || sort_col > LIB_SORT_SOURCE) {
        sort_col = LIB_SORT_TITLE;
    }
    return library_fill(ix, filter, language, author, form, period, genre, sort_col, sort_desc ? 1 : 0, out, e);
}

int index_catalog_put(Index *ix, const Volume *rec, Err *e) {
    if (!ix || !rec || !rec->path || !rec->path[0]) {
        err_set(e, "that file is not in the library");
        return -1;
    }
    char *path = field_copy(rec->path, 4096, 0, e);
    if (!path) {
        return -1;
    }
    char *field[CF_N];
    memset(field, 0, sizeof field);
    for (int i = 0; i < CF_N; i++) {
        int notes = i == CF_DESCRIPTION || i == CF_RIGHTS;
        field[i] = field_copy(vol_field(rec, i), notes ? 4000 : 400, notes, e);
        if (!field[i]) {
            for (int j = 0; j < i; j++) {
                free(field[j]);
            }
            free(path);
            return -1;
        }
    }
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int began = 0;
    if (write_db(ix, &db, 0, e) != 0) {
        goto fail_fields;
    }
    if (saved_exec(db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(db);
        goto fail_fields;
    }
    began = 1;
    if (saved_exec(db, CATALOG_SQL, e) != 0) {
        goto fail;
    }
    int64_t id = 0;
    if (doc_by_path(ix, db, path, &id, e) != 0) {
        goto fail;
    }
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO catalog(path, title, creator, date, language, type, subject, coverage,"
                           " source, description, publisher, contributor, format, identifier, relation, rights)"
                           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                           " ON CONFLICT(path) DO UPDATE SET"
                           " title=excluded.title, creator=excluded.creator, date=excluded.date,"
                           " language=excluded.language, type=excluded.type, subject=excluded.subject,"
                           " coverage=excluded.coverage, source=excluded.source, description=excluded.description,"
                           " publisher=excluded.publisher, contributor=excluded.contributor, format=excluded.format,"
                           " identifier=excluded.identifier, relation=excluded.relation, rights=excluded.rights",
                           -1, &st, NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    for (int i = 0; i < CF_N; i++) {
        sqlite3_bind_text(st, i + 2, field[i], -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (field[CF_TITLE][0]) {
        if (sqlite3_prepare_v2(db, "UPDATE docs SET title=? WHERE docid=?", -1, &st, NULL) != SQLITE_OK) {
            saved_fail(db, e);
            goto fail;
        }
        sqlite3_bind_text(st, 1, field[CF_TITLE], -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        if (sqlite3_step(st) != SQLITE_DONE) {
            saved_fail(db, e);
            goto fail;
        }
        sqlite3_finalize(st);
        st = NULL;
    }
    if (saved_exec(db, "COMMIT", e) != 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        goto fail_fields;
    }
    sqlite3_close(db);
    free(path);
    for (int i = 0; i < CF_N; i++) {
        free(field[i]);
    }
    return 0;
fail:
    if (st) {
        sqlite3_finalize(st);
    }
    if (began) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);
fail_fields:
    free(path);
    for (int i = 0; i < CF_N; i++) {
        free(field[i]);
    }
    return -1;
}

static int page_address_ok(const char *url) {
    const char *rest = NULL;
    if (!url || !url[0]) {
        return 0;
    }
    if (starts_with(url, "https://")) {
        rest = url + 8;
    } else if (starts_with(url, "http://")) {
        rest = url + 7;
    } else {
        return 0;
    }
    if (!rest[0] || strlen(url) > 2048) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f || *p == '\\') {
            return 0;
        }
    }
    return 1;
}

int index_page_remember(Index *ix, const char *path, const char *url, Err *e) {
    if (!ix || !path || !path[0] || !page_address_ok(url)) {
        err_set(e, "that address is not a web page");
        return -1;
    }
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int began = 0;
    if (write_db(ix, &db, 0, e) != 0) {
        return -1;
    }
    if (saved_exec(db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(db);
        return -1;
    }
    began = 1;
    if (saved_exec(db, PAGE_SQL, e) != 0) {
        goto fail;
    }
    int64_t id = 0;
    if (doc_by_path(ix, db, path, &id, e) != 0 || id == 0) {
        goto fail;
    }
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO web(path, url) VALUES(?, ?)"
                           " ON CONFLICT(path) DO UPDATE SET url=excluded.url",
                           -1, &st, NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (strlen(url) <= 400) {
        if (saved_exec(db, CATALOG_SQL, e) != 0) {
            goto fail;
        }
        if (sqlite3_prepare_v2(db,
                               "INSERT INTO catalog(path, identifier) VALUES(?, ?)"
                               " ON CONFLICT(path) DO UPDATE SET identifier=excluded.identifier"
                               " WHERE catalog.identifier = ''",
                               -1, &st, NULL) != SQLITE_OK) {
            saved_fail(db, e);
            goto fail;
        }
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, url, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            saved_fail(db, e);
            goto fail;
        }
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

int index_page_url(Index *ix, const char *path, char **url, Err *e) {
    if (url) {
        *url = NULL;
    }
    if (!url) {
        err_set(e, "out of memory");
        return -1;
    }
    if (!ix || !path || !path[0]) {
        return 0;
    }
    sqlite3_stmt *st = NULL;
    if (prep(ix, &st, "SELECT url FROM web WHERE path=?", e) != 0) {
        if (e->msg[0] && strstr(e->msg, "no such table")) {
            err_clear(e);
            return 0;
        }
        return -1;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        *url = xstrdup(s ? s : "");
        if (!*url) {
            sqlite3_finalize(st);
            err_set(e, "out of memory");
            return -1;
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        free(*url);
        *url = NULL;
        return db_fail(ix, e, "sqlite");
    }
    return 0;
}

int index_library_drop(Index *ix, const char *path, Err *e) {
    if (!ix || !path || !path[0]) {
        err_set(e, "that file is not in the library");
        return -1;
    }
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int began = 0;
    if (write_db(ix, &db, 1, e) != 0) {
        return -1;
    }
    if (saved_exec(db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(db);
        return -1;
    }
    began = 1;
    int64_t id = 0;
    if (doc_by_path(ix, db, path, &id, e) != 0) {
        goto fail;
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM fts WHERE docid=?", -1, &st, NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM docs WHERE docid=?", -1, &st, NULL) != SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        saved_fail(db, e);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    int has = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='catalog'", -1, &st, NULL) !=
        SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    has = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    st = NULL;
    if (has) {
        if (sqlite3_prepare_v2(db, "DELETE FROM catalog WHERE path=?", -1, &st, NULL) != SQLITE_OK) {
            saved_fail(db, e);
            goto fail;
        }
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            saved_fail(db, e);
            goto fail;
        }
        sqlite3_finalize(st);
        st = NULL;
    }
    has = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='web'", -1, &st, NULL) !=
        SQLITE_OK) {
        saved_fail(db, e);
        goto fail;
    }
    has = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    st = NULL;
    if (has) {
        if (sqlite3_prepare_v2(db, "DELETE FROM web WHERE path=?", -1, &st, NULL) != SQLITE_OK) {
            saved_fail(db, e);
            goto fail;
        }
        sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            saved_fail(db, e);
            goto fail;
        }
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
