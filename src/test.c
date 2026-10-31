#include "expand.h"
#include "extract.h"
#include "fetch.h"
#include "index.h"
#include "pdf.h"
#include "text.h"
#include "zip.h"

#include "sqlite3.h"

#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static int fails = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static int tok_is(const Tokens *t, size_t i, const char *s) {
    if (i >= t->n) {
        return 0;
    }
    size_t n = strlen(s);
    return t->toks[i].len == n && memcmp(t->arena.data + t->toks[i].off, s, n) == 0;
}

static void test_tokens(void) {
    Tokens t;
    tokens_init(&t);
    CHECK(tokenize(&t, "Hello", 5) == 0);
    CHECK(t.n == 1 && tok_is(&t, 0, "hello") && t.toks[0].pos == 0);
    tokens_clear(&t);
    CHECK(tokenize(&t, "don't", 5) == 0);
    CHECK(t.n == 1 && tok_is(&t, 0, "dont"));
    CHECK(t.toks[0].start == 0 && t.toks[0].end == 5);
    tokens_clear(&t);
    CHECK(casefold_cp(0x1f08) == 0x1f00);
    CHECK(casefold_cp(0x1f00) == 0x1f00);
    CHECK(casefold_cp(0x1fbc) == 0x1fb3);
    CHECK(cp_is_letter(0x1f00));
    CHECK(!cp_is_letter(0x1fbd));
    const char *poly = "\u1F00\u03BD\u03B5\u03B2\u03AC\u03BB\u03BB\u03B5\u03C4\u03BF";
    CHECK(tokenize(&t, poly, strlen(poly)) == 0);
    CHECK(t.n == 1 && tok_is(&t, 0, poly));
    CHECK(t.toks[0].start == 0 && t.toks[0].end == (uint32_t)strlen(poly));
    tokens_clear(&t);
    const char *arch = "\u1F08\u03C1\u03C7\u03AE";
    const char *arch_fold = "\u1F00\u03C1\u03C7\u03AE";
    CHECK(tokenize(&t, arch, strlen(arch)) == 0);
    CHECK(t.n == 1 && tok_is(&t, 0, arch_fold));
    tokens_clear(&t);
    const char *split = "\u1F00\u1FBD\u1F00";
    CHECK(tokenize(&t, split, strlen(split)) == 0);
    CHECK(t.n == 2 && tok_is(&t, 0, "\u1F00") && tok_is(&t, 1, "\u1F00"));
    tokens_clear(&t);
    CHECK(tokenize(&t, "full-blown", 10) == 0);
    CHECK(t.n == 2 && tok_is(&t, 0, "full") && tok_is(&t, 1, "blown"));
    CHECK(t.toks[1].pos == t.toks[0].pos + 1);
    CHECK(t.toks[0].start == 0 && t.toks[0].end == 4);
    CHECK(t.toks[1].start == 5 && t.toks[1].end == 10);
    tokens_clear(&t);
    const char *cjk = "東京駅";
    CHECK(tokenize(&t, cjk, strlen(cjk)) == 0);
    CHECK(t.n == 2 && tok_is(&t, 0, "東京") && tok_is(&t, 1, "京駅"));
    CHECK(t.toks[0].pos == 0 && t.toks[1].pos == 1);
    CHECK(t.toks[0].start == 0 && t.toks[0].end == 6);
    CHECK(t.toks[1].start == 3 && t.toks[1].end == 9);
    tokens_free(&t);
}

static void test_query_shape(void) {
    ParsedQuery pq;
    Err e;
    err_clear(&e);
    CHECK(query_parse("", &pq, &e) != 0);
    CHECK(query_parse("hello OR", &pq, &e) != 0);
    CHECK(query_parse("ext:txt", &pq, &e) == 0);
    CHECK(pq.q == NULL && pq.ext && strcmp(pq.ext, "txt") == 0);
    parsed_free(&pq);
    CHECK(query_parse("ext:.PDF", &pq, &e) == 0);
    CHECK(pq.ext && strcmp(pq.ext, "pdf") == 0);
    parsed_free(&pq);
    CHECK(query_parse("or", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_TERM);
    parsed_free(&pq);
    CHECK(query_parse("a OR b", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_OR);
    parsed_free(&pq);
    CHECK(query_parse("\"full text\"", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_PHRASE && pq.q->nkids == 2);
    parsed_free(&pq);
    CHECK(query_parse("port*", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_TERM && pq.q->prefix);
    parsed_free(&pq);
    CHECK(query_parse("mtime:2026-10", &pq, &e) == 0);
    CHECK(pq.q == NULL && pq.mtime_year == 2026 && pq.mtime_mon == 10);
    parsed_free(&pq);
    CHECK(query_parse("hello mtime:2026-10", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_TERM && pq.mtime_year == 2026 && pq.mtime_mon == 10);
    parsed_free(&pq);
    CHECK(query_parse("mtime:2026-13", &pq, &e) != 0);
    CHECK(query_parse("mtime:nope", &pq, &e) != 0);
    CHECK(query_parse("language:English language:Greek", &pq, &e) == 0);
    CHECK(pq.q == NULL && pq.nlanguage == 2);
    CHECK(pq.language && strcmp(pq.language[0], "English") == 0 && strcmp(pq.language[1], "Greek") == 0);
    parsed_free(&pq);
    CHECK(query_parse("author:\" A Reader \"", &pq, &e) == 0);
    CHECK(pq.nauthor == 1 && pq.author && strcmp(pq.author[0], "A Reader") == 0);
    parsed_free(&pq);
    CHECK(query_parse("language:English author:Freud form:Prose period:1900s genre:Essay", &pq, &e) == 0);
    CHECK(pq.nlanguage == 1 && pq.nauthor == 1 && pq.nform == 1 && pq.nperiod == 1 && pq.ngenre == 1);
    parsed_free(&pq);
    CHECK(query_parse("language:\"\"", &pq, &e) != 0);
    CHECK(query_parse("東京駅", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_PHRASE && pq.q->nkids == 2);
    parsed_free(&pq);
    CHECK(query_parse("text -blown", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_AND && pq.q->b && pq.q->b->kind == Q_NOT);
    parsed_free(&pq);
    CHECK(query_parse("azrue~", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_TERM && pq.q->fuzzy && pq.q->len == 5);
    CHECK(pq.q->text && strcmp(pq.q->text, "azrue") == 0);
    parsed_free(&pq);
    CHECK(query_parse("port*", &pq, &e) == 0);
    CHECK(pq.q && pq.q->prefix && !pq.q->fuzzy);
    parsed_free(&pq);
    char stemmed[32];
    size_t sn = porter_stem("running", 7, stemmed);
    CHECK(sn == 3 && memcmp(stemmed, "run", 3) == 0);
    sn = porter_stem("love", 4, stemmed);
    CHECK(sn == 4 && memcmp(stemmed, "love", 4) == 0);
    SearchModes stem_on;
    memset(&stem_on, 0, sizeof stem_on);
    stem_on.stem = 1;
    CHECK(query_parse("runs", &pq, &e) == 0);
    CHECK(query_expand(&pq.q, &stem_on) == 0);
    CHECK(pq.q && pq.q->kind == Q_OR);
    QTerm *qt = NULL;
    int nq = 0;
    CHECK(query_positive_terms(pq.q, &qt, &nq) == 0);
    int saw_running = 0;
    for (int i = 0; i < nq; i++) {
        if (qt[i].len == 7 && memcmp(qt[i].s, "running", 7) == 0 && !qt[i].kin) {
            saw_running = 1;
        }
    }
    CHECK(saw_running);
    qterms_free(qt, nq);
    parsed_free(&pq);
    CHECK(query_parse("runs", &pq, &e) == 0);
    CHECK(query_expand(&pq.q, NULL) == 0);
    CHECK(pq.q && pq.q->kind == Q_TERM);
    parsed_free(&pq);
}

static int raw_deflate(const void *in, size_t n, unsigned char **out, size_t *outn) {
    size_t cap = compressBound((uLong)n) + 64;
    unsigned char *buf = malloc(cap ? cap : 1);
    if (!buf) {
        return -1;
    }
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(buf);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)n;
    zs.next_out = buf;
    zs.avail_out = (uInt)cap;
    int z = deflate(&zs, Z_FINISH);
    *outn = zs.total_out;
    deflateEnd(&zs);
    if (z != Z_STREAM_END) {
        free(buf);
        return -1;
    }
    *out = buf;
    return 0;
}

typedef struct ZItem {
    const char *name;
    const void *data;
    size_t len;
    int method;
} ZItem;

static unsigned char *make_zip(const ZItem *items, int n, size_t *outn) {
    Buf local, central;
    buf_init(&local);
    buf_init(&central);
    for (int i = 0; i < n; i++) {
        const unsigned char *payload = items[i].data;
        size_t plen = items[i].len;
        unsigned char *owned = NULL;
        if (items[i].method == 8) {
            if (raw_deflate(items[i].data, items[i].len, &owned, &plen) != 0) {
                buf_free(&local);
                buf_free(&central);
                return NULL;
            }
            payload = owned;
        }
        uint32_t local_off = (uint32_t)local.len;
        unsigned char h[30];
        memset(h, 0, sizeof h);
        memcpy(h, "PK\003\004", 4);
        wr_u16(h + 4, 20);
        wr_u16(h + 8, (uint16_t)items[i].method);
        wr_u32(h + 18, (uint32_t)plen);
        wr_u32(h + 22, (uint32_t)items[i].len);
        wr_u16(h + 26, (uint16_t)strlen(items[i].name));
        buf_append(&local, h, 30);
        buf_append(&local, items[i].name, strlen(items[i].name));
        buf_append(&local, payload, plen);
        free(owned);
        unsigned char c[46];
        memset(c, 0, sizeof c);
        memcpy(c, "PK\001\002", 4);
        wr_u16(c + 4, 20);
        wr_u16(c + 6, 20);
        wr_u16(c + 10, (uint16_t)items[i].method);
        wr_u32(c + 20, (uint32_t)plen);
        wr_u32(c + 24, (uint32_t)items[i].len);
        wr_u16(c + 28, (uint16_t)strlen(items[i].name));
        wr_u32(c + 42, local_off);
        buf_append(&central, c, 46);
        buf_append(&central, items[i].name, strlen(items[i].name));
    }
    unsigned char eocd[22];
    memset(eocd, 0, sizeof eocd);
    memcpy(eocd, "PK\005\006", 4);
    wr_u16(eocd + 8, (uint16_t)n);
    wr_u16(eocd + 10, (uint16_t)n);
    wr_u32(eocd + 12, (uint32_t)central.len);
    wr_u32(eocd + 16, (uint32_t)local.len);
    Buf all;
    buf_init(&all);
    buf_append(&all, local.data, local.len);
    buf_append(&all, central.data, central.len);
    buf_append(&all, eocd, 22);
    buf_free(&local);
    buf_free(&central);
    *outn = all.len;
    unsigned char *p = (unsigned char *)all.data;
    return p;
}

static void test_zip(void) {
    const char *stored = "stored-bytes";
    const char *deflated = "deflate-bytes";
    ZItem items[2];
    items[0].name = "hello.txt";
    items[0].data = stored;
    items[0].len = strlen(stored);
    items[0].method = 0;
    items[1].name = "deflate.txt";
    items[1].data = deflated;
    items[1].len = strlen(deflated);
    items[1].method = 8;
    size_t n = 0;
    unsigned char *zip = make_zip(items, 2, &n);
    CHECK(zip != NULL);
    if (!zip) {
        return;
    }
    Zip z;
    Err e;
    err_clear(&e);
    int rc = zip_read(zip, n, &z, &e);
    if (rc != 0) {
        fprintf(stderr, "zip_read: %s\n", e.msg);
    }
    CHECK(rc == 0);
    if (rc == 0) {
        ZipFile *a = zip_find(&z, "hello.txt");
        ZipFile *b = zip_find(&z, "deflate.txt");
        CHECK(a && a->len == strlen(stored) && memcmp(a->data, stored, a->len) == 0);
        CHECK(b && b->len == strlen(deflated) && memcmp(b->data, deflated, b->len) == 0);
        zip_free(&z);
    }
    free(zip);
}

static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    size_t w = n ? fwrite(data, 1, n, f) : 0;
    int c = fclose(f);
    return (c == 0 && w == n) ? 0 : -1;
}

static int write_pdf(const char *path, const char *stream, int flate, const char *title) {
    const unsigned char *raw = (const unsigned char *)stream;
    size_t raw_n = strlen(stream);
    unsigned char *payload = NULL;
    size_t pn = raw_n;
    if (flate) {
        uLongf cap = compressBound((uLong)raw_n);
        payload = malloc(cap);
        if (!payload || compress(payload, &cap, raw, (uLong)raw_n) != Z_OK) {
            free(payload);
            return -1;
        }
        pn = (size_t)cap;
    }
    char head[256];
    int hn = snprintf(head, sizeof head, "%%PDF-1.4\n1 0 obj\n<< /Length %zu%s >>\nstream\n", pn,
                      flate ? " /Filter /FlateDecode" : "");
    char tail[256];
    int tn = snprintf(tail, sizeof tail, "\nendstream\nendobj\n/Title (%s)\n", title);
    if (hn < 0 || tn < 0 || (size_t)hn >= sizeof head || (size_t)tn >= sizeof tail) {
        free(payload);
        return -1;
    }
    size_t total = (size_t)hn + pn + (size_t)tn;
    unsigned char *buf = malloc(total);
    if (!buf) {
        free(payload);
        return -1;
    }
    memcpy(buf, head, (size_t)hn);
    memcpy(buf + hn, flate ? payload : raw, pn);
    memcpy(buf + hn + pn, tail, (size_t)tn);
    int rc = write_file(path, buf, total);
    free(buf);
    free(payload);
    return rc;
}

static int write_two_page_pdf(const char *path) {
    const char *s1 = "(alpha onepage marker.) Tj\n";
    const char *s2 = "(alpha twopage marker.) Tj\n";
    char head[128];
    char mid[160];
    int h1 = snprintf(head, sizeof head, "%%PDF-1.4\n1 0 obj\n<< /Length %zu >>\nstream\n", strlen(s1));
    int h2 = snprintf(mid, sizeof mid, "\nendstream\nendobj\n2 0 obj\n<< /Length %zu >>\nstream\n", strlen(s2));
    const char *tail = "\nendstream\nendobj\n";
    if (h1 < 0 || h2 < 0 || (size_t)h1 >= sizeof head || (size_t)h2 >= sizeof mid) {
        return -1;
    }
    size_t total = (size_t)h1 + strlen(s1) + (size_t)h2 + strlen(s2) + strlen(tail);
    char *buf = malloc(total + 1);
    if (!buf) {
        return -1;
    }
    size_t at = 0;
    memcpy(buf + at, head, (size_t)h1);
    at += (size_t)h1;
    memcpy(buf + at, s1, strlen(s1));
    at += strlen(s1);
    memcpy(buf + at, mid, (size_t)h2);
    at += (size_t)h2;
    memcpy(buf + at, s2, strlen(s2));
    at += strlen(s2);
    memcpy(buf + at, tail, strlen(tail));
    at += strlen(tail);
    int rc = write_file(path, buf, at);
    free(buf);
    return rc;
}

static int write_outline_pdf(const char *path) {
    const char *stream = "(outlinemarker lives here.) Tj\n";
    const char *front = "%PDF-1.4\n"
                        "/Title (Outline Notes)\n"
                        "1 0 obj\n"
                        "<< /Type /Page >>\n"
                        "endobj\n";
    char mid[128];
    int mn = snprintf(mid, sizeof mid, "2 0 obj\n<< /Length %zu >>\nstream\n", strlen(stream));
    const char *tail = "\nendstream\nendobj\n"
                       "3 0 obj\n"
                       "<< /Type /Outlines /First 4 0 R >>\n"
                       "endobj\n"
                       "4 0 obj\n"
                       "<< /Title (Opening) /Dest [1 0 R /Fit] >>\n"
                       "endobj\n"
                       "/Outlines 3 0 R\n";
    if (mn < 0 || (size_t)mn >= sizeof mid) {
        return -1;
    }
    size_t total = strlen(front) + (size_t)mn + strlen(stream) + strlen(tail);
    char *buf = malloc(total + 1);
    if (!buf) {
        return -1;
    }
    size_t at = 0;
    memcpy(buf + at, front, strlen(front));
    at += strlen(front);
    memcpy(buf + at, mid, (size_t)mn);
    at += (size_t)mn;
    memcpy(buf + at, stream, strlen(stream));
    at += strlen(stream);
    memcpy(buf + at, tail, strlen(tail));
    at += strlen(tail);
    int rc = write_file(path, buf, at);
    free(buf);
    return rc;
}

static int write_mapped_pdf(const char *path) {
    const char *s1 = "beginbfchar\n<0001> <0048>\nendbfchar\n";
    const char *s2 = "<0001> Tj\n";
    char head1[160];
    char mid[160];
    int h1 = snprintf(head1, sizeof head1, "%%PDF-1.4\n1 0 obj\n<< /Length %zu >>\nstream\n", strlen(s1));
    int h2 = snprintf(mid, sizeof mid, "endstream\nendobj\n2 0 obj\n<< /Length %zu >>\nstream\n", strlen(s2));
    const char *tail = "endstream\nendobj\n/Title (Mapped Glyph)\n";
    if (h1 < 0 || h2 < 0) {
        return -1;
    }
    size_t total = (size_t)h1 + strlen(s1) + (size_t)h2 + strlen(s2) + strlen(tail);
    char *buf = malloc(total + 1);
    if (!buf) {
        return -1;
    }
    size_t at = 0;
    memcpy(buf + at, head1, (size_t)h1);
    at += (size_t)h1;
    memcpy(buf + at, s1, strlen(s1));
    at += strlen(s1);
    memcpy(buf + at, mid, (size_t)h2);
    at += (size_t)h2;
    memcpy(buf + at, s2, strlen(s2));
    at += strlen(s2);
    memcpy(buf + at, tail, strlen(tail));
    at += strlen(tail);
    int rc = write_file(path, buf, at);
    free(buf);
    return rc;
}

static Hit *find_hit(Hit *h, int n, const char *suffix) {
    size_t sn = strlen(suffix);
    for (int i = 0; i < n; i++) {
        if (!h || !h[i].path) {
            continue;
        }
        size_t pn = strlen(h[i].path);
        if (pn >= sn && memcmp(h[i].path + pn - sn, suffix, sn) == 0) {
            return &h[i];
        }
    }
    return NULL;
}

static int hits_have(Hit *h, int n, const char *suffix) {
    size_t sn = strlen(suffix);
    for (int i = 0; i < n; i++) {
        if (!h[i].path) {
            continue;
        }
        size_t pn = strlen(h[i].path);
        if (pn >= sn && memcmp(h[i].path + pn - sn, suffix, sn) == 0) {
            return 1;
        }
    }
    return 0;
}

static void dump_hits(Hit *h, int n) {
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "  %s | %s | %s (%d passages)\n", h[i].path ? h[i].path : "", h[i].title ? h[i].title : "",
                h[i].snippet ? h[i].snippet : "", h[i].npassages);
    }
}

static void expect_hit(Hit *h, int n, const char *suffix, const char *q) {
    if (!hits_have(h, n, suffix)) {
        fprintf(stderr, "missing %s for [%s] (%d hits)\n", suffix, q, n);
        dump_hits(h, n);
        fails++;
    }
}

static void expect_miss(Hit *h, int n, const char *suffix, const char *q) {
    if (hits_have(h, n, suffix)) {
        fprintf(stderr, "unexpected %s for [%s]\n", suffix, q);
        dump_hits(h, n);
        fails++;
    }
}

static Hit *do_search(Index *ix, const char *q, const char *at, int *n) {
    Err e;
    err_clear(&e);
    Hit *h = NULL;
    int rc = index_search(ix, q, 50, at, &h, n, NULL, NULL, &e);
    if (rc != 0) {
        fprintf(stderr, "search [%s]: %s\n", q, e.msg);
        fails++;
        *n = 0;
        return NULL;
    }
    return h;
}

static int rm_one(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)ftwbuf;
    if (typeflag == FTW_DP) {
        rmdir(fpath);
    } else {
        unlink(fpath);
    }
    return 0;
}

static void rm_rf(const char *path) {
    if (path && path[0]) {
        nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
    }
}

static void test_bounds(void) {
    size_t n = 1;
    CHECK(size_add(SIZE_MAX, 1, &n) != 0);
    CHECK(size_add(3, 4, &n) == 0 && n == 7);
    CHECK(size_mul(SIZE_MAX, 2, &n) != 0);
    CHECK(size_mul(0, 5, &n) == 0 && n == 0);
    CHECK(size_fits(10, 8, 2));
    CHECK(!size_fits(10, 8, 3));
    CHECK(!size_fits(10, SIZE_MAX, 1));
    int cap = 0;
    CHECK(cap_grow(0, 16, &cap) == 0 && cap == 16);
    CHECK(cap_grow(INT_MAX / 2 + 1, 16, &cap) != 0);
    void *block = NULL;
    CHECK(grow_alloc(&block, 3, sizeof(int)) == 0 && block);
    int *nums = block;
    nums[0] = 1;
    CHECK(grow_alloc(&block, SIZE_MAX, sizeof(int)) != 0);
    CHECK(nums[0] == 1);
    free(block);

    uint32_t v = 0;
    const unsigned char *next = NULL;
    const unsigned char fit[] = {0xff, 0xff, 0xff, 0xff, 0x0f};
    CHECK(varint_get(fit, fit + 5, &v, &next) == 0 && v == 0xffffffffu);
    const unsigned char over[] = {0x80, 0x80, 0x80, 0x80, 0x10};
    CHECK(varint_get(over, over + 5, &v, &next) != 0);

    char dir[] = "/tmp/grove-safe-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *path = path_join(dir, "note.txt");
    char *link = path_join(dir, "link.txt");
    Err e;
    err_clear(&e);
    if (!path || !link || write_file_atomic(path, "kelp", 4, &e) != 0) {
        fprintf(stderr, "FAIL write_file_atomic %s\n", e.msg);
        fails++;
    } else {
        unsigned char *buf = NULL;
        size_t got = 0;
        CHECK(read_entire_file(path, &buf, &got, 32, &e) == 0);
        CHECK(buf && got == 4 && memcmp(buf, "kelp", 4) == 0 && buf[4] == 0);
        free(buf);
        buf = NULL;
        CHECK(symlink(path, link) == 0);
        CHECK(read_entire_file(link, &buf, &got, 32, &e) != 0);
        free(buf);
    }
    free(path);
    free(link);
    rm_rf(dir);
}

static int test_corpus(const char *root) {
    enum { NP = 32 };
    char *owned[NP];
    int np = 0;
    memset(owned, 0, sizeof owned);
    Index *ix = NULL;
    int rc = -1;
#define KEEP(expr) (np < NP ? (owned[np++] = (expr)) : NULL)

    char *docs = KEEP(path_join(root, "docs"));
    char *idx = KEEP(path_join(root, "idx"));
    char *sub = KEEP(path_join(docs, "sub"));
    char *a = KEEP(path_join(docs, "a.txt"));
    char *greek = KEEP(path_join(docs, "greek.txt"));
    char *b = KEEP(path_join(docs, "b.txt"));
    char *c = KEEP(path_join(sub, "c.md"));
    char *tokyo = KEEP(path_join(docs, "tokyo.txt"));
    char *html = KEEP(path_join(docs, "note.html"));
    char *docx = KEEP(path_join(docs, "page.docx"));
    char *pdf = KEEP(path_join(docs, "page.pdf"));
    char *flate = KEEP(path_join(docs, "flate.pdf"));
    char *mapped = KEEP(path_join(docs, "mapped.pdf"));
    char *keep = KEEP(path_join(docs, "keep.txt"));
    char *ortxt = KEEP(path_join(docs, "or.txt"));
    char *bin = KEEP(path_join(docs, "skip.bin"));
    char *places = KEEP(path_join(docs, "places.txt"));
    char *pages = KEEP(path_join(docs, "pages.pdf"));
    char *ff = KEEP(path_join(docs, "breaks.txt"));
    char *ruled = KEEP(path_join(docs, "ruled.txt"));
    char *adoc = KEEP(path_join(docs, "note.adoc"));
    char *lead = KEEP(path_join(docs, "lead.docx"));
    char *odt = KEEP(path_join(docs, "note.odt"));
    char *epub = KEEP(path_join(docs, "book.epub"));
    char *outline = KEEP(path_join(docs, "outline.pdf"));
    char *longp = KEEP(path_join(docs, "long.txt"));
    char *phrasep = KEEP(path_join(docs, "phrase.txt"));
    char *manyp = KEEP(path_join(docs, "many.txt"));
    if (!docs || !idx || !sub || !a || !greek || !ff || !ruled || !adoc || !lead || !odt || !epub || !outline ||
        !longp || !phrasep || !manyp ||
        mkdir(docs, 0755) != 0 || mkdir(sub, 0755) != 0) {
        fprintf(stderr, "cannot create corpus\n");
        goto done;
    }
    const char *a_text = "Hello there. This is a full text note about portable search. Don't stop.\n";
    CHECK(write_file(a, a_text, strlen(a_text)) == 0);
    const char *greek_text = "\u1F00\u03BD\u03B5\u03B2\u03AC\u03BB\u03BB\u03B5\u03C4\u03BF \u1F08\u03C1\u03C7\u03AE.\n";
    CHECK(write_file(greek, greek_text, strlen(greek_text)) == 0);
    const char *b_text = "A full-blown text about something else.\n";
    CHECK(write_file(b, b_text, strlen(b_text)) == 0);
    const char *c_text = "# Gamma Note\ngamma lives here with alpha\n";
    CHECK(write_file(c, c_text, strlen(c_text)) == 0);
    const char *tokyo_text = "東京駅に行く\n";
    CHECK(write_file(tokyo, tokyo_text, strlen(tokyo_text)) == 0);
    const char *html_text =
        "<html><head><title>Mango Page</title></head><body><h1>Citrus Box</h1><p>html body citrus</p>"
        "<script>secretnoise</script></body></html>\n";
    CHECK(write_file(html, html_text, strlen(html_text)) == 0);
    const char *keep_text = "alpha beta\n";
    CHECK(write_file(keep, keep_text, strlen(keep_text)) == 0);
    const char *or_text = "the operator or stays a word\n";
    CHECK(write_file(ortxt, or_text, strlen(or_text)) == 0);
    CHECK(write_file(bin, "nope", 4) == 0);
    CHECK(write_pdf(pdf, "(Groonga portable search.) Tj\n", 0, "Pdf Notes") == 0);
    CHECK(write_pdf(flate, "(Groonga flate needle.) Tj\n", 1, "Flate Notes") == 0);
    CHECK(write_mapped_pdf(mapped) == 0);
    char places_text[420];
    memset(places_text, 'x', sizeof places_text - 1);
    places_text[sizeof places_text - 1] = 0;
    memcpy(places_text, "quartz front ", 13);
    memcpy(places_text + 280, " quartz back\n", 13);
    CHECK(write_file(places, places_text, strlen(places_text)) == 0);
    char long_text[2048];
    size_t lp = 0;
    const char *begin = "Begin starts the letter. ";
    const char *pad = "Padding fills this sentence. ";
    const char *anchor = "The quartzanchor sits in this sentence. ";
    const char *omega = "Omega closes the thought.\n";
    memcpy(long_text + lp, begin, strlen(begin));
    lp += strlen(begin);
    for (int k = 0; k < 40; k++) {
        memcpy(long_text + lp, pad, strlen(pad));
        lp += strlen(pad);
    }
    memcpy(long_text + lp, anchor, strlen(anchor));
    lp += strlen(anchor);
    memcpy(long_text + lp, omega, strlen(omega));
    lp += strlen(omega);
    long_text[lp] = 0;
    CHECK(write_file(longp, long_text, lp) == 0);
    const char *phrase_text = "I shall go there tomorrow and think.\n\nShall I compare thee to a summer day and stop.\n";
    CHECK(write_file(phrasep, phrase_text, strlen(phrase_text)) == 0);
    enum { NLATE = 2100 };
    char many_text[6 + NLATE * 5 + 1];
    memcpy(many_text, "early\n", 6);
    for (int k = 0; k < NLATE; k++) {
        memcpy(many_text + 6 + (size_t)k * 5, "late ", 5);
    }
    many_text[6 + NLATE * 5] = 0;
    CHECK(write_file(manyp, many_text, 6 + (size_t)NLATE * 5) == 0);
    CHECK(write_two_page_pdf(pages) == 0);
    CHECK(write_outline_pdf(outline) == 0);
    const char *ff_text = "formfeedalpha\f formfeedbeta\n";
    CHECK(write_file(ff, ff_text, strlen(ff_text)) == 0);
    const char *ruled_text = "==== Not A Heading\nrulemarker lives here\n";
    CHECK(write_file(ruled, ruled_text, strlen(ruled_text)) == 0);
    const char *adoc_text = "= Adoc Title\nadocmarker lives here\n";
    CHECK(write_file(adoc, adoc_text, strlen(adoc_text)) == 0);

    const char *docxml = "<?xml version=\"1.0\"?>"
                         "<w:document><w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr>"
                         "<w:r><w:t>Intro</w:t></w:r></w:p>"
                         "<w:p><w:r><w:t>hello</w:t><w:tab/><w:t>world</w:t></w:r></w:p>"
                         "<w:p><w:r><w:lastRenderedPageBreak/></w:r><w:r><w:t>second page dock</w:t></w:r></w:p>"
                         "<w:p><w:r><w:t>portable docx search</w:t></w:r></w:p></w:document>";
    const char *core = "<cp:coreProperties><dc:title>Docx Title</dc:title></cp:coreProperties>";
    ZItem docitems[2];
    docitems[0].name = "word/document.xml";
    docitems[0].data = docxml;
    docitems[0].len = strlen(docxml);
    docitems[0].method = 0;
    docitems[1].name = "docProps/core.xml";
    docitems[1].data = core;
    docitems[1].len = strlen(core);
    docitems[1].method = 0;
    size_t zn = 0;
    unsigned char *zbytes = make_zip(docitems, 2, &zn);
    CHECK(zbytes && write_file(docx, zbytes, zn) == 0);
    free(zbytes);

    const char *leadxml = "<?xml version=\"1.0\"?><w:document><w:p><w:r><w:br w:type=\"page\"/></w:r>"
                          "<w:r><w:t>leaddock stays first</w:t></w:r></w:p></w:document>";
    ZItem leaditems[1];
    leaditems[0].name = "word/document.xml";
    leaditems[0].data = leadxml;
    leaditems[0].len = strlen(leadxml);
    leaditems[0].method = 0;
    zbytes = make_zip(leaditems, 1, &zn);
    CHECK(zbytes && write_file(lead, zbytes, zn) == 0);
    free(zbytes);

    const char *odtxml = "<office:document-content><text:h text:outline-level=\"1\">Grain Chapter</text:h>"
                         "<text:p>odt paragraph quinoa</text:p><text:soft-page-break/>"
                         "<text:p>grainpage follows</text:p></office:document-content>";
    const char *odtmeta = "<office:meta><dc:title>Odt Title</dc:title></office:meta>";
    ZItem odtitems[2];
    odtitems[0].name = "content.xml";
    odtitems[0].data = odtxml;
    odtitems[0].len = strlen(odtxml);
    odtitems[0].method = 0;
    odtitems[1].name = "meta.xml";
    odtitems[1].data = odtmeta;
    odtitems[1].len = strlen(odtmeta);
    odtitems[1].method = 0;
    zbytes = make_zip(odtitems, 2, &zn);
    CHECK(zbytes && write_file(odt, zbytes, zn) == 0);
    free(zbytes);

    const char *opf = "<package><metadata><dc:title>Epub Title</dc:title></metadata></package>";
    const char *ch1 = "<html><body><h2>Zebra Chapter</h2><p>beforezebra word</p>"
                      "<span epub:type=\"pagebreak\" title=\"4\"></span><p>afterzebra word</p></body></html>";
    const char *plain = "<html><body><p>stemword here</p></body></html>";
    ZItem epitems[3];
    epitems[0].name = "OEBPS/content.opf";
    epitems[0].data = opf;
    epitems[0].len = strlen(opf);
    epitems[0].method = 0;
    epitems[1].name = "OEBPS/ch1.xhtml";
    epitems[1].data = ch1;
    epitems[1].len = strlen(ch1);
    epitems[1].method = 0;
    epitems[2].name = "OEBPS/plain.xhtml";
    epitems[2].data = plain;
    epitems[2].len = strlen(plain);
    epitems[2].method = 0;
    zbytes = make_zip(epitems, 3, &zn);
    CHECK(zbytes && write_file(epub, zbytes, zn) == 0);
    free(zbytes);

    Err e;
    err_clear(&e);
    Extract ex;
    memset(&ex, 0, sizeof ex);
    int xs = extract_path(pdf, &ex, &e);
    if (xs != EX_OK) {
        fprintf(stderr, "pdf extract: %s\n", e.msg);
    }
    CHECK(xs == EX_OK && ex.text && strstr(ex.text, "Groonga portable search."));
    CHECK(ex.title && strstr(ex.title, "Pdf Notes"));
    extract_free(&ex);
    xs = extract_path(flate, &ex, &e);
    if (xs != EX_OK) {
        fprintf(stderr, "flate extract: %s\n", e.msg);
    }
    CHECK(xs == EX_OK && ex.text && strstr(ex.text, "flate needle"));
    extract_free(&ex);
    xs = extract_path(mapped, &ex, &e);
    if (xs != EX_OK) {
        fprintf(stderr, "mapped extract: %s text=%s\n", e.msg, ex.text ? ex.text : "");
    }
    CHECK(xs == EX_OK && ex.text && strchr(ex.text, 'H'));
    extract_free(&ex);
    xs = extract_path(pages, &ex, &e);
    if (xs != EX_OK) {
        fprintf(stderr, "pages extract: %s\n", e.msg);
    }
    CHECK(xs == EX_OK && ex.text && strstr(ex.text, "onepage") && strstr(ex.text, "twopage") && strchr(ex.text, '\f'));
    extract_free(&ex);
    xs = extract_path(docx, &ex, &e);
    if (xs != EX_OK) {
        fprintf(stderr, "docx extract: %s\n", e.msg);
    }
    CHECK(xs == EX_OK && ex.text && strstr(ex.text, "hello world"));
    CHECK(ex.title && strstr(ex.title, "Docx Title"));
    extract_free(&ex);
    xs = extract_path(html, &ex, &e);
    CHECK(xs == EX_OK && ex.text && strstr(ex.text, "citrus") && !strstr(ex.text, "secretnoise"));
    extract_free(&ex);

    err_clear(&e);
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "open: %s\n", e.msg);
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "index: %s\n", e.msg);
        goto done;
    }
    CHECK(st.indexed >= 8);
    CHECK(st.skipped >= 1);

    int n = 0;
    Hit *h = do_search(ix, "\"full text\"", NULL, &n);
    expect_hit(h, n, "/a.txt", "phrase full text");
    expect_miss(h, n, "/b.txt", "phrase full text");
    CHECK(n == 1);
    if (n == 1) {
        CHECK(h[0].score == 100);
        CHECK(h[0].snippet && strstr(h[0].snippet, "<<"));
        CHECK(h[0].snippet && strstr(h[0].snippet, "full"));
        CHECK(h[0].snippet && strstr(h[0].snippet, "<<full text>>"));
        CHECK(h[0].snippet && strstr(h[0].snippet, "Hello there"));
        CHECK(h[0].snippet && strstr(h[0].snippet, "Don't stop"));
        CHECK(h[0].snippet && strncmp(h[0].snippet, "...", 3) != 0);
        CHECK(h[0].npassages >= 1);
        CHECK(h[0].more == 0);
        CHECK(h[0].nmatches == 2);
        CHECK(h[0].noffsets == 2);
        CHECK(h[0].offsets && h[0].offsets[0].start == h[0].passages[0].offset);
        CHECK(h[0].offsets[0].len == 4 && h[0].offsets[0].term == 0 && h[0].offsets[0].column == 0);
        CHECK(h[0].offsets[1].len == 4 && h[0].offsets[1].term == 1 && h[0].offsets[1].column == 0);
        CHECK(h[0].offsets[1].start == h[0].offsets[0].start + 5);
        CHECK(h[0].matchinfo.phrases == 1 && h[0].matchinfo.columns == 1);
        CHECK(h[0].matchinfo.phrase && h[0].matchinfo.phrase[0].hits == 1);
        CHECK(h[0].matchinfo.phrase[0].docs >= 1);
        CHECK(h[0].passages[0].page == 0);
        CHECK(h[0].passages[0].offset == (uint32_t)(strstr(a_text, "full") - a_text));
        CHECK(h[0].snippet == h[0].passages[0].text);
    }
    hits_free(h, n);

    h = do_search(ix, "quartz", NULL, &n);
    expect_hit(h, n, "/places.txt", "two places");
    CHECK(n == 1);
    if (n == 1) {
        CHECK(h[0].npassages == 1);
        CHECK(h[0].more == 0);
        CHECK(h[0].nmatches == 2);
        CHECK(h[0].noffsets == 2);
        if (h[0].npassages == 1 && h[0].offsets) {
            CHECK(h[0].passages[0].page == 0);
            CHECK(memcmp(places_text + h[0].passages[0].offset, "quartz", 6) == 0);
            CHECK(h[0].passages[0].text && strstr(h[0].passages[0].text, "<<quartz>> front"));
            CHECK(h[0].passages[0].text && strstr(h[0].passages[0].text, "<<quartz>> back"));
            CHECK(h[0].offsets[0].start == h[0].passages[0].offset);
            CHECK(h[0].offsets[0].len == 6 && h[0].offsets[0].term == 0);
            CHECK(h[0].offsets[1].len == 6 && h[0].offsets[1].term == 0);
            CHECK(h[0].offsets[1].start > h[0].offsets[0].start);
            CHECK(memcmp(places_text + h[0].offsets[1].start, "quartz", 6) == 0);
            CHECK(h[0].offsets[0].column == 0 && h[0].offsets[1].column == 0);
            CHECK(h[0].matchinfo.phrases >= 1 && h[0].matchinfo.columns == 1);
            CHECK(h[0].matchinfo.rows >= 1);
            CHECK(h[0].matchinfo.phrase && h[0].matchinfo.phrase[0].hits == 2);
        }
    }
    hits_free(h, n);

    h = do_search(ix, "\"shall i compare thee\"", NULL, &n);
    expect_hit(h, n, "/phrase.txt", "phrase place");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        const char *t = h[0].passages[0].text ? h[0].passages[0].text : "";
        CHECK(h[0].npassages == 1);
        CHECK(h[0].more == 0);
        CHECK(h[0].nmatches == 4);
        CHECK(h[0].noffsets == 4);
        CHECK(h[0].offsets && h[0].offsets[0].start == h[0].passages[0].offset);
        CHECK(h[0].offsets[0].len == 5 && h[0].offsets[0].term == 0 && h[0].offsets[0].column == 0);
        CHECK(h[0].offsets[1].len == 1 && h[0].offsets[1].term == 1 && h[0].offsets[1].column == 0);
        CHECK(h[0].offsets[2].len == 7 && h[0].offsets[2].term == 2 && h[0].offsets[2].column == 0);
        CHECK(h[0].offsets[3].len == 4 && h[0].offsets[3].term == 3 && h[0].offsets[3].column == 0);
        CHECK(h[0].offsets[1].start == h[0].offsets[0].start + 6);
        CHECK(h[0].offsets[2].start == h[0].offsets[1].start + 2);
        CHECK(h[0].offsets[3].start == h[0].offsets[2].start + 8);
        CHECK(h[0].matchinfo.phrases == 1 && h[0].matchinfo.columns == 1);
        CHECK(memcmp(phrase_text + h[0].passages[0].offset, "Shall I compare", 15) == 0);
        CHECK(strstr(t, "<<Shall I compare thee>>"));
        CHECK(!strstr(t, "tomorrow"));
    }
    hits_free(h, n);

    h = do_search(ix, "late early", NULL, &n);
    expect_hit(h, n, "/many.txt", "offset cap");
    CHECK(n == 1);
    if (n == 1 && h && h[0].offsets) {
        CHECK(h[0].nmatches == NLATE + 1);
        CHECK(h[0].noffsets == GROVE_OFFSET_MAX);
        CHECK(h[0].npassages >= 1 && h[0].npassages < h[0].nmatches);
        CHECK(h[0].offsets[0].start == 0);
        CHECK(h[0].offsets[0].len == 5);
        CHECK(h[0].offsets[0].term == 1);
        CHECK(h[0].offsets[1].start == 6);
        CHECK(h[0].offsets[1].len == 4);
        CHECK(h[0].offsets[1].term == 0);
        CHECK(h[0].offsets[GROVE_OFFSET_MAX - 1].start == 6 + (uint32_t)(GROVE_OFFSET_MAX - 2) * 5);
        int ordered = 1;
        for (int i = 1; i < h[0].noffsets; i++) {
            if (h[0].offsets[i].start <= h[0].offsets[i - 1].start) {
                ordered = 0;
                break;
            }
        }
        CHECK(ordered);
    }
    hits_free(h, n);

    h = do_search(ix, "late", NULL, &n);
    expect_hit(h, n, "/many.txt", "late pages");
    CHECK(n == 1);
    if (n == 1 && h && h[0].passages) {
        CHECK(h[0].npassages == GROVE_PASSAGE_PAGE);
        CHECK(h[0].more == 1);
        CHECK(h[0].nmatches == NLATE);
        uint32_t prev = h[0].passages[h[0].npassages - 1].offset;
        int page_fetches = 0;
        int more = 1;
        while (more && page_fetches < 20) {
            Passage *ps = NULL;
            int nmore = 0;
            err_clear(&e);
            CHECK(index_more(ix, "late", h[0].path, prev, &ps, &nmore, &more, NULL, &e) == 0);
            CHECK(nmore >= 1);
            if (nmore >= 1 && ps) {
                CHECK(ps[0].offset > prev);
                CHECK(ps[0].text && strstr(ps[0].text, "<<late>>"));
                for (int i = 1; i < nmore; i++) {
                    CHECK(ps[i].offset > ps[i - 1].offset);
                }
                prev = ps[nmore - 1].offset;
            }
            page_fetches++;
            passages_free(ps, nmore);
        }
        CHECK(more == 0);
        CHECK(page_fetches >= 1);
        Passage *none = NULL;
        int nn = 0;
        int extra = 1;
        err_clear(&e);
        CHECK(index_more(ix, "late", h[0].path, prev, &none, &nn, &extra, NULL, &e) == 0);
        CHECK(nn == 0 && extra == 0);
        passages_free(none, nn);
    }
    hits_free(h, n);

    h = do_search(ix, "early", NULL, &n);
    expect_hit(h, n, "/many.txt", "early once");
    CHECK(n == 1);
    if (n == 1 && h) {
        CHECK(h[0].more == 0);
        CHECK(h[0].npassages == 1);
        CHECK(h[0].nmatches == 1);
    }
    hits_free(h, n);

    h = do_search(ix, "quartzanchor", NULL, &n);
    expect_hit(h, n, "/long.txt", "long paragraph");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        const char *t = h[0].passages[0].text ? h[0].passages[0].text : "";
        CHECK(memcmp(long_text + h[0].passages[0].offset, "quartzanchor", 12) == 0);
        CHECK(strstr(t, "<<quartzanchor>>"));
        CHECK(strstr(t, "Omega closes the thought."));
        CHECK(strstr(t, "sits in this sentence."));
        CHECK(!strstr(t, "Begin starts"));
        CHECK(strncmp(t, "... ", 4) == 0);
        CHECK(!strstr(t, " ..."));
    }
    hits_free(h, n);

    h = do_search(ix, "onepage", NULL, &n);
    expect_hit(h, n, "/pages.pdf", "pdf page 1");
    if (n >= 1 && hits_have(h, n, "/pages.pdf")) {
        int i;
        for (i = 0; i < n; i++) {
            if (h[i].path && strstr(h[i].path, "/pages.pdf")) {
                CHECK(h[i].npassages >= 1);
                CHECK(h[i].passages[0].page == 1);
                CHECK(h[i].title && strstr(h[i].title, "onepage") && !strchr(h[i].title, '\f'));
                CHECK(h[i].title && !strstr(h[i].title, "twopage"));
            }
        }
    }
    hits_free(h, n);

    h = do_search(ix, "twopage", NULL, &n);
    expect_hit(h, n, "/pages.pdf", "pdf page 2");
    if (n >= 1 && hits_have(h, n, "/pages.pdf")) {
        int i;
        for (i = 0; i < n; i++) {
            if (h[i].path && strstr(h[i].path, "/pages.pdf")) {
                CHECK(h[i].npassages >= 1);
                CHECK(h[i].passages[0].page == 2);
                CHECK(h[i].passages[0].text && strstr(h[i].passages[0].text, "<<twopage>>"));
            }
        }
    }
    hits_free(h, n);

    h = do_search(ix, "\"full-blown text\"", NULL, &n);
    expect_hit(h, n, "/b.txt", "phrase full-blown");
    expect_miss(h, n, "/a.txt", "phrase full-blown");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "東京駅", NULL, &n);
    expect_hit(h, n, "/tokyo.txt", "cjk");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "don't", NULL, &n);
    expect_hit(h, n, "/a.txt", "dont");
    {
        Hit *dh = find_hit(h, n, "/a.txt");
        CHECK(dh && dh->npassages >= 1 && dh->passages[0].text && strstr(dh->passages[0].text, "<<Don't>>"));
    }
    CHECK(n == 1);
    hits_free(h, n);
    h = do_search(ix, "\u1F00\u03BD\u03B5\u03B2\u03AC\u03BB\u03BB\u03B5\u03C4\u03BF", NULL, &n);
    expect_hit(h, n, "/greek.txt", "polytonic");
    {
        Hit *gh = find_hit(h, n, "/greek.txt");
        CHECK(gh && gh->npassages >= 1 && gh->passages[0].text &&
              strstr(gh->passages[0].text, "<<\u1F00\u03BD\u03B5\u03B2\u03AC\u03BB\u03BB\u03B5\u03C4\u03BF>>"));
    }
    CHECK(n == 1);
    hits_free(h, n);
    h = do_search(ix, "\u1F00\u03C1\u03C7\u03AE", NULL, &n);
    expect_hit(h, n, "/greek.txt", "polytonic case");
    {
        Hit *gh = find_hit(h, n, "/greek.txt");
        CHECK(gh && gh->npassages >= 1 && gh->passages[0].text &&
              strstr(gh->passages[0].text, "<<\u1F08\u03C1\u03C7\u03AE>>"));
    }
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "or", NULL, &n);
    expect_hit(h, n, "/or.txt", "or");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "text -blown", NULL, &n);
    expect_hit(h, n, "/a.txt", "not");
    expect_miss(h, n, "/b.txt", "not");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "hello OR gamma", NULL, &n);
    expect_hit(h, n, "/a.txt", "or");
    expect_hit(h, n, "/c.md", "or");
    expect_hit(h, n, "/page.docx", "or");
    expect_miss(h, n, "/b.txt", "or");
    CHECK(n == 3);
    hits_free(h, n);

    h = do_search(ix, "(hello OR gamma) ext:txt", NULL, &n);
    expect_hit(h, n, "/a.txt", "paren");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "(hello OR gamma) AND (portable)", NULL, &n);
    expect_hit(h, n, "/a.txt", "and line");
    expect_hit(h, n, "/page.docx", "and line");
    expect_miss(h, n, "/c.md", "and line");
    expect_miss(h, n, "/page.pdf", "and line");
    CHECK(n == 2);
    hits_free(h, n);

    h = do_search(ix, "ext:md gamma", NULL, &n);
    expect_hit(h, n, "/c.md", "ext");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "path:sub gamma", NULL, &n);
    expect_hit(h, n, "/c.md", "path");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Gamma Note") == 0);
        CHECK(h[0].passages[0].page == 0);
    }
    hits_free(h, n);

    h = do_search(ix, "title:Mango", NULL, &n);
    expect_hit(h, n, "/note.html", "title");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "secretnoise", NULL, &n);
    CHECK(n == 0);
    hits_free(h, n);

    h = do_search(ix, "citrus", NULL, &n);
    expect_hit(h, n, "/note.html", "html");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Citrus Box") == 0);
        CHECK(h[0].title && strstr(h[0].title, "Mango"));
    }
    hits_free(h, n);

    h = do_search(ix, "\"hello world\"", NULL, &n);
    expect_hit(h, n, "/page.docx", "docx phrase");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].page == 1);
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Intro") == 0);
    }
    hits_free(h, n);

    h = do_search(ix, "dock", NULL, &n);
    expect_hit(h, n, "/page.docx", "docx page");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].page == 2);
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Intro") == 0);
        DocRead *rd = NULL;
        err_clear(&e);
        CHECK(index_read(ix, h[0].path, h[0].passages[0].offset, 0, &rd, &e) == 0);
        CHECK(rd && rd->page == 2);
        CHECK(rd && rd->text && strstr(rd->text, "second page dock"));
        CHECK(rd && rd->from <= h[0].passages[0].offset && rd->to > h[0].passages[0].offset);
        CHECK(rd && rd->section && strcmp(rd->section, "Intro") == 0);
        docread_free(rd);
    }
    hits_free(h, n);

    h = do_search(ix, "leaddock", NULL, &n);
    expect_hit(h, n, "/lead.docx", "leading break");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].page == 1);
    }
    hits_free(h, n);

    h = do_search(ix, "formfeedalpha", NULL, &n);
    expect_hit(h, n, "/breaks.txt", "form feed page 1");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].page == 1);
        CHECK(!h[0].passages[0].section);
    }
    hits_free(h, n);

    h = do_search(ix, "formfeedbeta", NULL, &n);
    expect_hit(h, n, "/breaks.txt", "form feed page 2");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].page == 2);
    }
    hits_free(h, n);

    h = do_search(ix, "rulemarker", NULL, &n);
    expect_hit(h, n, "/ruled.txt", "plain rule");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].page == 0);
        CHECK(!h[0].passages[0].section);
    }
    hits_free(h, n);

    h = do_search(ix, "adocmarker", NULL, &n);
    expect_hit(h, n, "/note.adoc", "adoc heading");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Adoc Title") == 0);
        CHECK(h[0].passages[0].page == 0);
        DocRead *rd = NULL;
        err_clear(&e);
        CHECK(index_read(ix, h[0].path, h[0].passages[0].offset, 0, &rd, &e) == 0);
        CHECK(rd && rd->text && strstr(rd->text, "adocmarker"));
        CHECK(rd && rd->nheads >= 1 && rd->heads[0].text && strcmp(rd->heads[0].text, "Adoc Title") == 0);
        CHECK(rd && rd->page == 0);
        docread_free(rd);
        err_clear(&e);
        CHECK(index_read(ix, "/no/such/file", 0, 0, &rd, &e) != 0);
        docread_free(rd);
    }
    hits_free(h, n);

    h = do_search(ix, "quinoa", NULL, &n);
    expect_hit(h, n, "/note.odt", "odt section");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Grain Chapter") == 0);
        CHECK(h[0].passages[0].page == 1);
        CHECK(h[0].title && strstr(h[0].title, "Odt Title"));
    }
    hits_free(h, n);

    h = do_search(ix, "grainpage", NULL, &n);
    expect_hit(h, n, "/note.odt", "odt page 2");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].page == 2);
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Grain Chapter") == 0);
    }
    hits_free(h, n);

    h = do_search(ix, "beforezebra", NULL, &n);
    expect_hit(h, n, "/book.epub", "epub before page");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Zebra Chapter") == 0);
        CHECK(h[0].passages[0].page == 0);
        CHECK(h[0].title && strstr(h[0].title, "Epub Title"));
    }
    hits_free(h, n);

    h = do_search(ix, "afterzebra", NULL, &n);
    expect_hit(h, n, "/book.epub", "epub page");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Zebra Chapter") == 0);
        CHECK(h[0].passages[0].page == 4);
    }
    hits_free(h, n);

    h = do_search(ix, "stemword", NULL, &n);
    expect_hit(h, n, "/book.epub", "epub file section");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "plain") == 0);
        CHECK(h[0].passages[0].page == 4);
    }
    hits_free(h, n);

    h = do_search(ix, "outlinemarker", NULL, &n);
    expect_hit(h, n, "/outline.pdf", "pdf outline");
    CHECK(n == 1);
    if (n == 1 && h && h[0].npassages >= 1) {
        CHECK(h[0].passages[0].section && strcmp(h[0].passages[0].section, "Opening") == 0);
        CHECK(h[0].passages[0].page == 1);
        CHECK(h[0].title && strstr(h[0].title, "Outline Notes"));
        CHECK(h[0].title && !strstr(h[0].title, "Opening"));
    }
    hits_free(h, n);

    h = do_search(ix, "port*", NULL, &n);
    expect_hit(h, n, "/a.txt", "prefix");
    expect_hit(h, n, "/page.pdf", "prefix");
    expect_hit(h, n, "/page.docx", "prefix");
    if (n > 0) {
        CHECK(h[0].score == 100);
    }
    hits_free(h, n);

    h = do_search(ix, "ext:pdf groonga", NULL, &n);
    expect_hit(h, n, "/page.pdf", "pdf");
    expect_hit(h, n, "/flate.pdf", "pdf");
    CHECK(n == 2);
    hits_free(h, n);

    h = do_search(ix, "flate", NULL, &n);
    expect_hit(h, n, "/flate.pdf", "flate");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "h", NULL, &n);
    expect_hit(h, n, "/mapped.pdf", "cmap");
    CHECK(n == 1);
    hits_free(h, n);

    h = do_search(ix, "Hello", NULL, &n);
    expect_hit(h, n, "/a.txt", "case");
    if (hits_have(h, n, "/a.txt")) {
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (h[i].snippet && strstr(h[i].snippet, "<<Hello>>")) {
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "snippet did not mark Hello\n");
            dump_hits(h, n);
            fails++;
        }
    }
    hits_free(h, n);

    struct stat keep_st;
    CHECK(stat(keep, &keep_st) == 0);
    const char *zw = "ZZZZZ beta\n";
    CHECK(strlen(zw) == strlen(keep_text));
    CHECK(write_file(keep, zw, strlen(zw)) == 0);
    struct timespec ts[2];
    ts[0].tv_sec = keep_st.st_atim.tv_sec;
    ts[0].tv_nsec = keep_st.st_atim.tv_nsec;
    ts[1].tv_sec = keep_st.st_mtim.tv_sec;
    ts[1].tv_nsec = keep_st.st_mtim.tv_nsec;
    CHECK(utimensat(AT_FDCWD, keep, ts, 0) == 0);
    memset(&st, 0, sizeof st);
    CHECK(index_add(ix, docs, &st, &e) == 0);
    CHECK(st.unchanged >= 1);
    h = do_search(ix, "ZZZZZ", NULL, &n);
    CHECK(n == 0);
    hits_free(h, n);
    h = do_search(ix, "alpha", NULL, &n);
    expect_hit(h, n, "/keep.txt", "stale alpha");
    hits_free(h, n);
    index_set_force(ix, 1);
    memset(&st, 0, sizeof st);
    CHECK(index_add(ix, docs, &st, &e) == 0);
    index_set_force(ix, 0);
    h = do_search(ix, "ZZZZZ", NULL, &n);
    expect_hit(h, n, "/keep.txt", "force");
    CHECK(n == 1);
    hits_free(h, n);
    h = do_search(ix, "alpha", NULL, &n);
    expect_hit(h, n, "/c.md", "alpha remains");
    expect_miss(h, n, "/keep.txt", "alpha forced out");
    hits_free(h, n);

    const char *a2 = "gamma replaced the previous note\n";
    CHECK(write_file(a, a2, strlen(a2)) == 0);
    memset(&st, 0, sizeof st);
    CHECK(index_add(ix, docs, &st, &e) == 0);
    CHECK(st.indexed >= 1);
    h = do_search(ix, "\"full text\"", NULL, &n);
    CHECK(n == 0);
    hits_free(h, n);
    h = do_search(ix, "hello", NULL, &n);
    expect_miss(h, n, "/a.txt", "rewritten hello");
    expect_hit(h, n, "/page.docx", "docx hello remains");
    hits_free(h, n);
    h = do_search(ix, "gamma", NULL, &n);
    expect_hit(h, n, "/a.txt", "new gamma");
    expect_hit(h, n, "/c.md", "old gamma");
    hits_free(h, n);
    h = do_search(ix, "don't", NULL, &n);
    CHECK(n == 0);
    hits_free(h, n);

    CHECK(unlink(b) == 0);
    memset(&st, 0, sizeof st);
    CHECK(index_purge(ix, &st, &e) == 0);
    CHECK(st.removed >= 1);
    h = do_search(ix, "\"full-blown text\"", NULL, &n);
    CHECK(n == 0);
    hits_free(h, n);
    h = do_search(ix, "something", NULL, &n);
    CHECK(n == 0);
    hits_free(h, n);

    CHECK(index_compact(ix, &e) == 0);
    index_close(ix);
    ix = NULL;
    err_clear(&e);
    ix = index_open(idx, 0, &e);
    if (!ix) {
        fprintf(stderr, "reopen: %s\n", e.msg);
        goto done;
    }
    IndexInfo info;
    CHECK(index_info(ix, &info, &e) == 0);
    CHECK(info.n_segments == 1);
    CHECK(info.n_docs >= 8);
    h = do_search(ix, "gamma", NULL, &n);
    expect_hit(h, n, "/a.txt", "after compact");
    expect_hit(h, n, "/c.md", "after compact");
    {
        Hit *cm = find_hit(h, n, "/c.md");
        Hit *am = find_hit(h, n, "/a.txt");
        CHECK(cm && cm->npassages >= 1 && cm->passages[0].section &&
              strcmp(cm->passages[0].section, "Gamma Note") == 0);
        CHECK(am && am->npassages >= 1 && !am->passages[0].section);
    }
    hits_free(h, n);
    h = do_search(ix, "東京駅", NULL, &n);
    expect_hit(h, n, "/tokyo.txt", "cjk after compact");
    hits_free(h, n);
    index_close(ix);
    ix = NULL;

    char *docs2 = KEEP(malloc(strlen(docs) + 2));
    if (!docs2) {
        goto done;
    }
    memcpy(docs2, docs, strlen(docs));
    memcpy(docs2 + strlen(docs), "2", 2);
    CHECK(rename(docs, docs2) == 0);
    err_clear(&e);
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "reopen writable: %s\n", e.msg);
        goto done;
    }
    h = do_search(ix, "gamma", docs2, &n);
    if (n > 0) {
        int shown = 0;
        for (int i = 0; i < n; i++) {
            if (h[i].path && strstr(h[i].path, docs2)) {
                shown = 1;
            }
        }
        if (!shown) {
            fprintf(stderr, "--at did not remap\n");
            dump_hits(h, n);
            fails++;
        }
    }
    hits_free(h, n);
    err_clear(&e);
    int add_rc = index_add(ix, docs, NULL, &e);
    CHECK(add_rc != 0);
    CHECK(strstr(e.msg, "rebind") != NULL);
    err_clear(&e);
    CHECK(index_rebind(ix, docs, docs2, &e) == 0);
    h = do_search(ix, "gamma", NULL, &n);
    expect_hit(h, n, "/a.txt", "rebind");
    if (n > 0) {
        CHECK(h[0].path && strstr(h[0].path, docs2));
    }
    hits_free(h, n);
    memset(&st, 0, sizeof st);
    CHECK(index_add(ix, docs2, &st, &e) == 0);
    h = do_search(ix, "ZZZZZ", NULL, &n);
    expect_hit(h, n, "/keep.txt", "after rebind index");
    hits_free(h, n);

    rc = 0;
done:
    index_close(ix);
    for (int i = 0; i < np; i++) {
        free(owned[i]);
    }
#undef KEEP
    return rc;
}

static int facet_n(const Facet *a, int n, const char *value) {
    for (int i = 0; i < n; i++) {
        if (a[i].value && strcmp(a[i].value, value) == 0) {
            return a[i].count;
        }
    }
    return -1;
}

static void test_facets(void) {
    char tmpl[] = "/tmp/grove-facet-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *docs = path_join(root, "docs");
    char *idx = path_join(root, "idx");
    char *shelf = docs ? path_join(docs, "shelf") : NULL;
    char *other = docs ? path_join(docs, "other") : NULL;
    char *notes = docs ? path_join(docs, "notebooks") : NULL;
    char *paths[8];
    memset(paths, 0, sizeof paths);
    const char *names[] = {"a.txt", "b.txt", "c.txt", "quiet.txt"};
    if (!docs || !idx || !shelf || !other || !notes || mkdir(docs, 0755) != 0 || mkdir(shelf, 0755) != 0 ||
        mkdir(other, 0755) != 0 || mkdir(notes, 0755) != 0) {
        fprintf(stderr, "facet corpus\n");
        fails++;
        goto done;
    }
    for (int i = 0; i < 4; i++) {
        paths[i] = path_join(shelf, names[i]);
        const char *text = i == 3 ? "silent page\n" : "widget alpha\n";
        if (!paths[i] || write_file(paths[i], text, strlen(text)) != 0) {
            fails++;
            goto done;
        }
    }
    paths[4] = path_join(other, "d.md");
    paths[5] = path_join(other, "e.md");
    paths[6] = path_join(notes, "n.txt");
    paths[7] = path_join(docs, "f.txt");
    const char *beta = "widget beta\n";
    const char *gamma = "widget gamma\n";
    const char *delta = "widget delta\n";
    if (!paths[4] || !paths[5] || !paths[6] || !paths[7] || write_file(paths[4], beta, strlen(beta)) != 0 ||
        write_file(paths[5], beta, strlen(beta)) != 0 || write_file(paths[6], gamma, strlen(gamma)) != 0 ||
        write_file(paths[7], delta, strlen(delta)) != 0) {
        fails++;
        goto done;
    }
    Err e;
    err_clear(&e);
    Index *ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "facet index: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "facet add: %s\n", e.msg);
        fails++;
        index_close(ix);
        goto done;
    }
    Facets fac;
    memset(&fac, 0, sizeof fac);
    Hit *h = NULL;
    int n = 0;
    CHECK(index_search(ix, "widget", 1, NULL, &h, &n, &fac, NULL, &e) == 0);
    CHECK(n == 1);
    CHECK(fac.nfiles == 7);
    CHECK(facet_n(fac.types, fac.ntypes, "txt") == 5);
    CHECK(facet_n(fac.types, fac.ntypes, "md") == 2);
    CHECK(fac.ntypes == 2 && fac.types && fac.types[0].count >= fac.types[1].count);
    CHECK(facet_n(fac.folders, fac.nfolders, "shelf/") == 3);
    CHECK(facet_n(fac.folders, fac.nfolders, "other/") == 2);
    CHECK(facet_n(fac.folders, fac.nfolders, "notebooks/") == 1);
    CHECK(fac.nfolders == 3);
    hits_free(h, n);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    n = 0;
    CHECK(index_search(ix, "widget path:shelf/", 1, NULL, &h, &n, &fac, NULL, &e) == 0);
    CHECK(n == 1);
    CHECK(fac.nfiles == 3);
    CHECK(facet_n(fac.types, fac.ntypes, "txt") == 3);
    CHECK(facet_n(fac.types, fac.ntypes, "md") == -1);
    CHECK(facet_n(fac.folders, fac.nfolders, "shelf/") == 3);
    CHECK(facet_n(fac.folders, fac.nfolders, "other/") == -1);
    CHECK(facet_n(fac.folders, fac.nfolders, "notebooks/") == -1);
    hits_free(h, n);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    n = 0;
    CHECK(index_search(ix, "widget ext:md", 50, NULL, &h, &n, &fac, NULL, &e) == 0);
    CHECK(n == 2);
    CHECK(fac.nfiles == 2);
    CHECK(facet_n(fac.types, fac.ntypes, "md") == 2);
    CHECK(facet_n(fac.types, fac.ntypes, "txt") == -1);
    CHECK(facet_n(fac.folders, fac.nfolders, "other/") == 2);
    CHECK(facet_n(fac.folders, fac.nfolders, "shelf/") == -1);
    hits_free(h, n);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    n = 0;
    CHECK(index_search(ix, "silent", 1, NULL, &h, &n, &fac, NULL, &e) == 0);
    CHECK(fac.nfiles == 1);
    CHECK(facet_n(fac.folders, fac.nfolders, "shelf/") == 1);
    CHECK(facet_n(fac.types, fac.ntypes, "txt") == 1);
    hits_free(h, n);
    facets_free(&fac);
    index_close(ix);
done:
    free(docs);
    free(idx);
    free(shelf);
    free(other);
    free(notes);
    for (int i = 0; i < 8; i++) {
        free(paths[i]);
    }
    rm_rf(root);
}

static time_t local_month(int year, int mon, int day) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static int set_mtime(const char *path, time_t sec) {
    struct timespec ts[2];
    ts[0].tv_sec = sec;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = sec;
    ts[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, path, ts, 0);
}

static void test_mtime(void) {
    char tmpl[] = "/tmp/grove-mtime-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *docs = path_join(root, "docs");
    char *idx = path_join(root, "idx");
    char *oct = docs ? path_join(docs, "oct.txt") : NULL;
    char *sep = docs ? path_join(docs, "sep.txt") : NULL;
    Index *ix = NULL;
    if (!docs || !idx || !oct || !sep || mkdir(docs, 0755) != 0) {
        fprintf(stderr, "mtime corpus\n");
        fails++;
        goto done;
    }
    const char *text = "widget monthmark\n";
    if (write_file(oct, text, strlen(text)) != 0 || write_file(sep, text, strlen(text)) != 0 ||
        set_mtime(oct, local_month(2026, 10, 15)) != 0 || set_mtime(sep, local_month(2026, 9, 15)) != 0) {
        fails++;
        goto done;
    }
    Err e;
    err_clear(&e);
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "mtime open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "mtime index: %s\n", e.msg);
        fails++;
        goto done;
    }
    Facets fac;
    memset(&fac, 0, sizeof fac);
    Hit *h = NULL;
    int n = 0;
    CHECK(index_search(ix, "widget", 10, NULL, &h, &n, &fac, NULL, &e) == 0);
    CHECK(n == 2);
    CHECK(fac.nfiles == 2);
    CHECK(facet_n(fac.months, fac.nmonths, "2026-10") == 1);
    CHECK(facet_n(fac.months, fac.nmonths, "2026-09") == 1);
    hits_free(h, n);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    n = 0;
    CHECK(index_search(ix, "widget mtime:2026-09", 10, NULL, &h, &n, &fac, NULL, &e) == 0);
    CHECK(n == 1);
    CHECK(fac.nfiles == 1);
    expect_hit(h, n, "/sep.txt", "september");
    expect_miss(h, n, "/oct.txt", "september");
    CHECK(facet_n(fac.months, fac.nmonths, "2026-09") == 1);
    CHECK(facet_n(fac.months, fac.nmonths, "2026-10") == -1);
    hits_free(h, n);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    n = 0;
    CHECK(index_search(ix, "mtime:2026-10", 10, NULL, &h, &n, &fac, NULL, &e) == 0);
    CHECK(n == 1);
    CHECK(fac.nfiles == 1);
    expect_hit(h, n, "/oct.txt", "october only");
    expect_miss(h, n, "/sep.txt", "october only");
    hits_free(h, n);
    facets_free(&fac);
    index_close(ix);
    ix = NULL;
done:
    if (ix) {
        index_close(ix);
    }
    free(docs);
    free(idx);
    free(oct);
    free(sep);
    rm_rf(root);
}

static int saved_has(char **q, int n, const char *s) {
    for (int i = 0; i < n; i++) {
        if (strcmp(q[i], s) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_saved(void) {
    char tmpl[] = "/tmp/grove-saved-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *idx = path_join(root, "idx");
    Index *ix = NULL;
    char **q = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (!idx) {
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "saved open: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_saved_list(ix, &q, &n, &e) == 0);
    CHECK(n == 0);
    CHECK(index_saved_put(ix, "  shall  ", &e) == 0);
    CHECK(index_saved_put(ix, "shall", &e) == 0);
    CHECK(index_saved_put(ix, "mtime:2026-13", &e) != 0);
    CHECK(index_saved_put(ix, "", &e) != 0);
    CHECK(index_saved_put(ix, "shall\nmore", &e) != 0);
    index_saved_free(q, n);
    q = NULL;
    n = 0;
    CHECK(index_saved_list(ix, &q, &n, &e) == 0);
    CHECK(n == 1 && q && strcmp(q[0], "shall") == 0);
    CHECK(index_saved_put(ix, "other", &e) == 0);
    index_saved_free(q, n);
    q = NULL;
    n = 0;
    CHECK(index_saved_list(ix, &q, &n, &e) == 0);
    CHECK(n == 2 && strcmp(q[0], "other") == 0 && strcmp(q[1], "shall") == 0);
    index_close(ix);
    ix = index_open(idx, 0, &e);
    if (!ix) {
        fprintf(stderr, "saved reopen: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_saved_put(ix, "third", &e) == 0);
    index_saved_free(q, n);
    q = NULL;
    n = 0;
    CHECK(index_saved_list(ix, &q, &n, &e) == 0);
    CHECK(n == 3 && strcmp(q[0], "third") == 0);
    CHECK(index_saved_drop(ix, " shall ", &e) == 0);
    CHECK(index_saved_drop(ix, "shall", &e) == 0);
    index_saved_free(q, n);
    q = NULL;
    n = 0;
    CHECK(index_saved_list(ix, &q, &n, &e) == 0);
    CHECK(n == 2 && !saved_has(q, n, "shall") && saved_has(q, n, "other") && saved_has(q, n, "third"));
    for (int i = 0; i < 25; i++) {
        char name[16];
        snprintf(name, sizeof name, "n%02d", i);
        if (index_saved_put(ix, name, &e) != 0) {
            fprintf(stderr, "saved put %s: %s\n", name, e.msg);
            fails++;
            goto done;
        }
    }
    index_saved_free(q, n);
    q = NULL;
    n = 0;
    CHECK(index_saved_list(ix, &q, &n, &e) == 0);
    CHECK(n == 24);
    CHECK(saved_has(q, n, "n24") && saved_has(q, n, "n01"));
    CHECK(!saved_has(q, n, "n00") && !saved_has(q, n, "other") && !saved_has(q, n, "third"));
    CHECK(q && strcmp(q[0], "n24") == 0);
done:
    index_saved_free(q, n);
    if (ix) {
        index_close(ix);
    }
    free(idx);
    rm_rf(root);
}

static void test_notes(void) {
    char tmpl[] = "/tmp/grove-notes-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *idx = path_join(root, "idx");
    Index *ix = NULL;
    Note *notes = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (!idx) {
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "notes open: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_notes_list(ix, &notes, &n, &e) == 0);
    CHECK(n == 0);
    CHECK(index_note_put(ix, "/shelf/a.txt", 40, "  margin  ", &e) == 0);
    CHECK(index_note_put(ix, "/shelf/a.txt", 40, "", &e) != 0);
    CHECK(index_note_put(ix, "/shelf/a.txt", 40, "two\nlines", &e) != 0);
    CHECK(index_note_put(ix, "/shelf/a.txt", 40, "kept", &e) == 0);
    CHECK(index_note_put(ix, "/shelf/b.txt", 8, "other", &e) == 0);
    index_notes_free(notes, n);
    notes = NULL;
    n = 0;
    CHECK(index_notes_list(ix, &notes, &n, &e) == 0);
    CHECK(n == 2);
    CHECK(notes && strcmp(notes[0].path, "/shelf/a.txt") == 0 && notes[0].offset == 40 && strcmp(notes[0].body, "kept") == 0);
    CHECK(strcmp(notes[1].path, "/shelf/b.txt") == 0 && notes[1].offset == 8);
    index_close(ix);
    ix = index_open(idx, 0, &e);
    if (!ix) {
        fprintf(stderr, "notes reopen: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_note_drop(ix, " /shelf/a.txt ", 40, &e) == 0);
    index_notes_free(notes, n);
    notes = NULL;
    n = 0;
    CHECK(index_notes_list(ix, &notes, &n, &e) == 0);
    CHECK(n == 1 && notes && strcmp(notes[0].path, "/shelf/b.txt") == 0);
done:
    index_notes_free(notes, n);
    if (ix) {
        index_close(ix);
    }
    free(idx);
    rm_rf(root);
}

static int alert_nfiles(Alert *a, int n, const char *query) {
    for (int i = 0; i < n; i++) {
        if (a[i].query && strcmp(a[i].query, query) == 0) {
            return a[i].nfiles;
        }
    }
    return -1;
}

static int alert_has_path(Alert *a, int n, const char *query, const char *suffix) {
    size_t m = strlen(suffix);
    for (int i = 0; i < n; i++) {
        if (!a[i].query || strcmp(a[i].query, query) != 0) {
            continue;
        }
        for (int j = 0; j < a[i].nfiles; j++) {
            const char *p = a[i].files[j].path ? a[i].files[j].path : "";
            size_t pn = strlen(p);
            if (pn >= m && memcmp(p + pn - m, suffix, m) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int add_tree(Index *ix, const char *docs, Err *e) {
    IndexStats st;
    memset(&st, 0, sizeof st);
    return index_add(ix, docs, &st, e);
}

static void test_alerts(void) {
    char tmpl[] = "/tmp/grove-alerts-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *docs = path_join(root, "docs");
    char *idx = path_join(root, "idx");
    char *a = docs ? path_join(docs, "a.txt") : NULL;
    char *b = docs ? path_join(docs, "b.txt") : NULL;
    char *c = docs ? path_join(docs, "c.txt") : NULL;
    char *d = docs ? path_join(docs, "d.txt") : NULL;
    Index *ix = NULL;
    Alert *al = NULL;
    char **saved = NULL;
    int n = 0;
    int ns = 0;
    Err e;
    err_clear(&e);
    if (!docs || !idx || !a || !b || !c || !d || mkdir(docs, 0755) != 0) {
        fprintf(stderr, "alert corpus\n");
        fails++;
        goto done;
    }
    if (write_file(a, "alpha alertword\n", 16) != 0 || write_file(b, "beta alertword\n", 15) != 0) {
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "alert open: %s\n", e.msg);
        fails++;
        goto done;
    }
    if (add_tree(ix, docs, &e) != 0) {
        fprintf(stderr, "alert index: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(n == 0);
    CHECK(index_alert_watch(ix, "", &e) != 0);
    CHECK(index_alert_watch(ix, "alertword\nmore", &e) != 0);
    CHECK(index_alert_watch(ix, "mtime:2026-13", &e) != 0);
    if (index_alert_watch(ix, "  alertword  ", &e) != 0) {
        fprintf(stderr, "watch: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_saved_list(ix, &saved, &ns, &e) == 0);
    CHECK(ns == 1 && saved && strcmp(saved[0], "alertword") == 0);
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(n == 1 && alert_nfiles(al, n, "alertword") == 0);
    CHECK(index_alert_unwatch(ix, " alertword ", &e) == 0);
    CHECK(index_alert_unwatch(ix, "alertword", &e) == 0);
    index_saved_free(saved, ns);
    saved = NULL;
    ns = 0;
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_saved_list(ix, &saved, &ns, &e) == 0);
    CHECK(ns == 1 && saved_has(saved, ns, "alertword"));
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(n == 0);
    if (index_alert_watch(ix, "alertword", &e) != 0) {
        fprintf(stderr, "rewatch: %s\n", e.msg);
        fails++;
        goto done;
    }
    if (write_file(c, "gamma alertword\n", 16) != 0 || add_tree(ix, docs, &e) != 0) {
        fprintf(stderr, "alert add c: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(alert_nfiles(al, n, "alertword") == 1);
    CHECK(alert_has_path(al, n, "alertword", "/c.txt"));
    if (write_file(a, "alpha alertword changed more\n", 29) != 0 || add_tree(ix, docs, &e) != 0) {
        fprintf(stderr, "alert change a: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(alert_nfiles(al, n, "alertword") == 2);
    CHECK(alert_has_path(al, n, "alertword", "/a.txt"));
    CHECK(alert_has_path(al, n, "alertword", "/c.txt"));
    CHECK(!alert_has_path(al, n, "alertword", "/b.txt"));
    if (index_alert_watch(ix, "alertword", &e) != 0) {
        fprintf(stderr, "seen: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(alert_nfiles(al, n, "alertword") == 0);
    if (unlink(c) != 0 || add_tree(ix, docs, &e) != 0) {
        fprintf(stderr, "alert drop c: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(alert_nfiles(al, n, "alertword") == 0);
    CHECK(index_saved_drop(ix, "alertword", &e) == 0);
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    index_saved_free(saved, ns);
    saved = NULL;
    ns = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(n == 0);
    CHECK(index_saved_list(ix, &saved, &ns, &e) == 0);
    CHECK(!saved_has(saved, ns, "alertword"));
    if (index_alert_watch(ix, "zzznomatch", &e) != 0) {
        fprintf(stderr, "empty watch: %s\n", e.msg);
        fails++;
        goto done;
    }
    if (write_file(d, "zzznomatch\n", 11) != 0 || add_tree(ix, docs, &e) != 0) {
        fprintf(stderr, "alert add d: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_close(ix);
    ix = index_open(idx, 0, &e);
    if (!ix) {
        fprintf(stderr, "alert reopen: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(alert_nfiles(al, n, "zzznomatch") == 1);
    CHECK(alert_has_path(al, n, "zzznomatch", "/d.txt"));
    if (index_alert_watch(ix, "zzznomatch", &e) != 0) {
        fprintf(stderr, "readonly seen: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(alert_nfiles(al, n, "zzznomatch") == 0);
    for (int i = 0; i < 24; i++) {
        char name[16];
        snprintf(name, sizeof name, "n%02d", i);
        if (index_saved_put(ix, name, &e) != 0) {
            fprintf(stderr, "alert cap %s: %s\n", name, e.msg);
            fails++;
            goto done;
        }
    }
    index_alerts_free(al, n);
    al = NULL;
    n = 0;
    index_saved_free(saved, ns);
    saved = NULL;
    ns = 0;
    CHECK(index_alerts_list(ix, &al, &n, &e) == 0);
    CHECK(n == 0);
    CHECK(index_saved_list(ix, &saved, &ns, &e) == 0);
    CHECK(ns == 24);
    CHECK(!saved_has(saved, ns, "zzznomatch"));
    CHECK(saved_has(saved, ns, "n00") && saved_has(saved, ns, "n23"));
done:
    index_alerts_free(al, n);
    index_saved_free(saved, ns);
    if (ix) {
        index_close(ix);
    }
    free(docs);
    free(idx);
    free(a);
    free(b);
    free(c);
    free(d);
    rm_rf(root);
}

static void test_reader(void) {
    char tmpl[] = "/tmp/grove-read-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *docs = path_join(root, "docs");
    char *idx = path_join(root, "idx");
    char *file = docs ? path_join(docs, "long.adoc") : NULL;
    Index *ix = NULL;
    char *body = NULL;
    Err e;
    err_clear(&e);
    if (!docs || !idx || !file || mkdir(docs, 0755) != 0) {
        fails++;
        goto done;
    }
    size_t cap = 100 * 1024;
    body = malloc(cap + 64);
    if (!body) {
        fails++;
        goto done;
    }
    const char *head = "= Chapter\n";
    size_t n = strlen(head);
    memcpy(body, head, n);
    const char *line = "alpha beta gamma delta epsilon zeta\n";
    size_t ln = strlen(line);
    while (n + ln + 16 < cap) {
        memcpy(body + n, line, ln);
        n += ln;
    }
    const char *mark = "\nuniquemarker\n";
    memcpy(body + 50000, mark, strlen(mark));
    body[n] = 0;
    if (write_file(file, body, n) != 0) {
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "reader open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "reader add: %s\n", e.msg);
        fails++;
        goto done;
    }
    int nh = 0;
    Hit *h = do_search(ix, "uniquemarker", NULL, &nh);
    CHECK(nh == 1 && h && h[0].npassages >= 1);
    if (nh == 1 && h && h[0].npassages >= 1) {
        DocRead *rd = NULL;
        err_clear(&e);
        CHECK(index_read(ix, h[0].path, h[0].passages[0].offset, 0, &rd, &e) == 0);
        CHECK(rd && rd->text && strstr(rd->text, "uniquemarker"));
        CHECK(rd && rd->from <= h[0].passages[0].offset && rd->to > h[0].passages[0].offset);
        CHECK(rd && rd->length > rd->to);
        CHECK(rd && rd->to - rd->from <= 48u * 1024u + 512u);
        CHECK(rd && rd->nheads >= 1 && strcmp(rd->heads[0].text, "Chapter") == 0);
        uint32_t mid = rd->to;
        uint32_t start = rd->from;
        docread_free(rd);
        rd = NULL;
        err_clear(&e);
        CHECK(index_read(ix, h[0].path, mid, 1, &rd, &e) == 0);
        CHECK(rd && rd->from == mid);
        CHECK(rd && rd->to > rd->from);
        CHECK(rd && !strstr(rd->text, "uniquemarker"));
        docread_free(rd);
        rd = NULL;
        err_clear(&e);
        CHECK(index_read(ix, h[0].path, start, -1, &rd, &e) == 0);
        CHECK(rd && rd->to == start);
        CHECK(rd && rd->from < rd->to);
        CHECK(rd && rd->text && strstr(rd->text, "= Chapter"));
        docread_free(rd);
    }
    hits_free(h, nh);
done:
    if (ix) {
        index_close(ix);
    }
    free(body);
    free(file);
    free(docs);
    free(idx);
    rm_rf(root);
}

static void test_cert(void) {
    Err e;
    err_clear(&e);
    const char *body = "[(Hello) -120 (World)] TJ\n";
    char kern[256];
    int kn = snprintf(kern, sizeof kern,
                      "%%PDF-1.4\n1 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                      strlen(body), body);
    CHECK(kn > 0 && (size_t)kn < sizeof kern);
    Buf text;
    buf_init(&text);
    char *title = NULL;
    CHECK(pdf_extract((const unsigned char *)kern, (size_t)kn, &text, &title, &e) == 0);
    CHECK(text.data && strstr(text.data, "Hello World"));
    buf_free(&text);
    free(title);

    char digits[48];
    memset(digits, '9', 40);
    digits[40] = 0;
    char big[512];
    int bn = snprintf(big, sizeof big,
                      "%%PDF-1.4\n1 0 obj\n<< /Length %s >>\nstream\n(kelpbound) Tj\nendstream\nendobj\n",
                      digits);
    CHECK(bn > 0 && (size_t)bn < sizeof big);
    buf_init(&text);
    title = NULL;
    err_clear(&e);
    CHECK(pdf_extract((const unsigned char *)big, (size_t)bn, &text, &title, &e) == 0);
    CHECK(text.data && strstr(text.data, "kelpbound"));
    buf_free(&text);
    free(title);

    const char *tight =
        "%PDF-1.4\n1 0 obj\n<< /Length 13 >>stream\r\n(Scrooge) Tj\nendstream\nendobj\n";
    buf_init(&text);
    title = NULL;
    err_clear(&e);
    CHECK(pdf_extract((const unsigned char *)tight, strlen(tight), &text, &title, &e) == 0);
    CHECK(text.data && strstr(text.data, "Scrooge"));
    buf_free(&text);
    free(title);

    const char *longer =
        "%PDF-1.4\n1 0 obj\n<< /Length 18 >>streaming\r\n(notastream) Tj\nendstream\nendobj\n";
    buf_init(&text);
    title = NULL;
    err_clear(&e);
    CHECK(pdf_extract((const unsigned char *)longer, strlen(longer), &text, &title, &e) == 0);
    CHECK(!text.data || !strstr(text.data, "notastream"));
    buf_free(&text);
    free(title);

    const char *onesign =
        "%PDF-1.4\n1 0 obj\n<< /Length 11 >stream\n(onesign) Tj\nendstream\nendobj\n";
    buf_init(&text);
    title = NULL;
    err_clear(&e);
    CHECK(pdf_extract((const unsigned char *)onesign, strlen(onesign), &text, &title, &e) == 0);
    CHECK(!text.data || !strstr(text.data, "onesign"));
    buf_free(&text);
    free(title);

    const char *show = "BT /F1 12 Tf (Hello) Tj /F1 12 Tf (\014) Tj /F2 12 Tf (\014) Tj ET\n";
    const char *map_a = "beginbfchar\n<0C> <0041>\nendbfchar\n";
    const char *map_b = "beginbfchar\n<0C> <0042>\nendbfchar\n";
    const char *noise = "(notpage) Tj\n";
    Buf fonts;
    buf_init(&fonts);
    CHECK(buf_puts(&fonts, "%PDF-1.4\n") == 0);
    CHECK(buf_puts(&fonts,
                   "1 0 obj\n<< /Type /Page /Contents 2 0 R /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> "
                   ">>\nendobj\n") == 0);
    CHECK(buf_printf(&fonts, "2 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n", strlen(show), show) == 0);
    CHECK(buf_puts(&fonts, "3 0 obj\n<< /Type /Font /ToUnicode 5 0 R >>\nendobj\n") == 0);
    CHECK(buf_puts(&fonts, "4 0 obj\n<< /Type /Font /ToUnicode 6 0 R >>\nendobj\n") == 0);
    CHECK(buf_printf(&fonts, "5 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n", strlen(map_a), map_a) == 0);
    CHECK(buf_printf(&fonts, "6 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n", strlen(map_b), map_b) == 0);
    CHECK(buf_printf(&fonts, "7 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n", strlen(noise), noise) == 0);
    buf_init(&text);
    title = NULL;
    err_clear(&e);
    if (fonts.data) {
        CHECK(pdf_extract((const unsigned char *)fonts.data, fonts.len, &text, &title, &e) == 0);
        CHECK(text.data && strstr(text.data, "Hello A B"));
        CHECK(!strstr(text.data ? text.data : "", "notpage"));
    }
    buf_free(&text);
    buf_free(&fonts);
    free(title);

    Buf doc;
    buf_init(&doc);
    CHECK(buf_puts(&doc, "%PDF-1.4\n1 0 obj\n<< /Type /Page >>\nendobj\n") == 0);
    for (int i = 0; i < 129; i++) {
        int obj = i + 2;
        char line[160];
        int ln;
        if (i + 1 < 129) {
            ln = snprintf(line, sizeof line,
                          "%d 0 obj\n<< /Title (S%d) /Dest [1 0 R /Fit] /Next %d 0 R >>\nendobj\n", obj,
                          i, obj + 1);
        } else {
            ln = snprintf(line, sizeof line,
                          "%d 0 obj\n<< /Title (S%d) /Dest [1 0 R /Fit] >>\nendobj\n", obj, i);
        }
        CHECK(ln > 0 && (size_t)ln < sizeof line);
        CHECK(buf_puts(&doc, line) == 0);
    }
    CHECK(buf_puts(&doc, "200 0 obj\n<< /Type /Outlines /First 2 0 R >>\nendobj\n/Outlines 200 0 R\n") ==
          0);
    PdfSection *secs = NULL;
    int ns = 0;
    if (doc.data) {
        CHECK(pdf_sections((const unsigned char *)doc.data, doc.len, &secs, &ns) == 0);
        CHECK(ns == 128);
    }
    pdf_sections_free(secs, ns);
    buf_free(&doc);

    const char *wrapped = "%PDF-1.4\n/Outlines 99999999999999999999 0 R\n";
    secs = NULL;
    ns = -1;
    CHECK(pdf_sections((const unsigned char *)wrapped, strlen(wrapped), &secs, &ns) == 0);
    CHECK(ns == 0 && !secs);

    char dir[] = "/tmp/grove-cert-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *html = path_join(dir, "ent.html");
    char *page = path_join(dir, "page.html");
    char *over = path_join(dir, "over.html");
    const char *ent = "<html><body><p>amp&#38;ok &#x41; &#x110000;end</p></body></html>";
    const char *okpage =
        "<html><body><span epub:type=\"pagebreak\" title=\"4\"></span><p>pagedigit</p></body></html>";
    const char *badpage =
        "<html><body><span epub:type=\"pagebreak\" title=\"999999999999\"></span><p>pagedigit</p></body></html>";
    if (!html || !page || !over || write_file(html, ent, strlen(ent)) != 0 ||
        write_file(page, okpage, strlen(okpage)) != 0 || write_file(over, badpage, strlen(badpage)) != 0) {
        fprintf(stderr, "FAIL cert fixtures\n");
        fails++;
    } else {
        Extract ex;
        memset(&ex, 0, sizeof ex);
        err_clear(&e);
        CHECK(extract_path(html, &ex, &e) == EX_OK);
        CHECK(ex.text && strstr(ex.text, "amp&ok A"));
        CHECK(ex.text && strstr(ex.text, "&#x110000;end"));
        extract_free(&ex);
        err_clear(&e);
        CHECK(extract_path(page, &ex, &e) == EX_OK);
        int saw4 = 0;
        for (int i = 0; ex.marks && i < ex.nmarks; i++) {
            if (ex.marks[i].kind == MARK_PAGE && ex.marks[i].page == 4) {
                saw4 = 1;
            }
        }
        CHECK(saw4);
        extract_free(&ex);
        err_clear(&e);
        CHECK(extract_path(over, &ex, &e) == EX_OK);
        int huge_page = 0;
        for (int i = 0; ex.marks && i < ex.nmarks; i++) {
            if (ex.marks[i].kind == MARK_PAGE && ex.marks[i].page > 1000000u) {
                huge_page = 1;
            }
        }
        CHECK(!huge_page);
        extract_free(&ex);
    }
    free(html);
    free(page);
    free(over);
    rm_rf(dir);
}

static int plan_covers(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return 0;
    }
    char *q = NULL;
    if (sqlite3_exec(db, "SELECT 1", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    size_t n = strlen(sql) + 32;
    q = malloc(n);
    if (!q || snprintf(q, n, "EXPLAIN QUERY PLAN %s", sql) < 0) {
        free(q);
        sqlite3_close(db);
        return 0;
    }
    int rc = sqlite3_prepare_v2(db, q, -1, &st, NULL);
    free(q);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    int cover = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *detail = (const char *)sqlite3_column_text(st, 3);
        if (detail && strstr(detail, "COVERING") && strstr(detail, "docs_shelf")) {
            cover = 1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return cover;
}

static int facet_at(const Facet *a, int n, int i, const char *value, int count) {
    return a && i >= 0 && i < n && a[i].value && strcmp(a[i].value, value) == 0 && a[i].count == count;
}

static void put_record(Index *ix, const char *path, const char *creator, const char *language, const char *type,
                       const char *coverage, const char *subject) {
    Volume rec;
    Err e;
    err_clear(&e);
    memset(&rec, 0, sizeof rec);
    rec.path = (char *)path;
    rec.creator = (char *)creator;
    rec.language = (char *)language;
    rec.type = (char *)type;
    rec.coverage = (char *)coverage;
    rec.subject = (char *)subject;
    if (index_catalog_put(ix, &rec, &e) != 0) {
        fprintf(stderr, "catalog %s: %s\n", path ? path : "", e.msg);
        fails++;
    }
}

static const Volume *vol_suffix(const Library *lib, const char *suffix) {
    for (int i = 0; i < lib->n; i++) {
        if (lib->volumes[i].path && strstr(lib->volumes[i].path, suffix)) {
            return &lib->volumes[i];
        }
    }
    return NULL;
}

static void test_narrow(void) {
    char tmpl[] = "/tmp/grove-narrow-XXXXXX";
    char *root = mkdtemp(tmpl);
    char *docs = root ? path_join(root, "docs") : NULL;
    char *idx = root ? path_join(root, "idx") : NULL;
    char *paths[5];
    const char *names[] = {"a.txt", "b.md", "c.txt", "d.txt", "e.txt"};
    const char *texts[] = {"widget freudessay\n", "widget aristotlepage\n", "widget freudverse\n", "widget nocat\n",
                           "widget hybridform\n"};
    Index *ix = NULL;
    Library lib;
    memset(paths, 0, sizeof paths);
    memset(&lib, 0, sizeof lib);
    if (!root || !docs || !idx || mkdir(docs, 0700) != 0) {
        fprintf(stderr, "narrow corpus\n");
        fails++;
        goto done;
    }
    for (int i = 0; i < 5; i++) {
        paths[i] = path_join(docs, names[i]);
        if (!paths[i] || write_file(paths[i], texts[i], strlen(texts[i])) != 0) {
            fails++;
            goto done;
        }
    }
    Err e;
    err_clear(&e);
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "narrow open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "narrow add: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_library_list(ix, NULL, &lib, &e) == 0);
    CHECK(lib.total == 5);
    CHECK(lib.narrow.nlanguage == 0 && lib.narrow.nauthor == 0 && lib.narrow.nform == 0);
    const Volume *a = vol_suffix(&lib, "/a.txt");
    const Volume *b = vol_suffix(&lib, "/b.md");
    const Volume *c = vol_suffix(&lib, "/c.txt");
    const Volume *evol = vol_suffix(&lib, "/e.txt");
    CHECK(a && b && c && evol);
    put_record(ix, a ? a->path : "", "Freud; Jung", "English", "Prose", "1900s", "Essay");
    put_record(ix, b ? b->path : "", "Aristotle", "Greek", "Prose", "Ancient", "Philosophy");
    put_record(ix, c ? c->path : "", "Freud", "English", "Verse", "1900s", "Poem");
    put_record(ix, evol ? evol->path : "", "Only One", "English ; Latin", "Prose; Verse", "1900s", "Essay");
    library_free(&lib);
    CHECK(index_library_narrow(ix, NULL, NULL, NULL, NULL, NULL, NULL, &lib, &e) == 0);
    CHECK(lib.total == 5 && lib.n == 5);
    CHECK(facet_at(lib.narrow.language, lib.narrow.nlanguage, 0, "English", 3));
    CHECK(facet_at(lib.narrow.language, lib.narrow.nlanguage, 1, "Greek", 1));
    CHECK(facet_at(lib.narrow.language, lib.narrow.nlanguage, 2, "Latin", 1));
    CHECK(facet_at(lib.narrow.author, lib.narrow.nauthor, 0, "Freud", 2));
    CHECK(facet_at(lib.narrow.author, lib.narrow.nauthor, 1, "Aristotle", 1));
    CHECK(facet_at(lib.narrow.author, lib.narrow.nauthor, 2, "Jung", 1));
    CHECK(facet_at(lib.narrow.author, lib.narrow.nauthor, 3, "Only One", 1));
    CHECK(facet_n(lib.narrow.form, lib.narrow.nform, "Prose") == 2);
    CHECK(facet_n(lib.narrow.form, lib.narrow.nform, "Verse") == 1);
    CHECK(facet_n(lib.narrow.form, lib.narrow.nform, "Prose; Verse") == 1);
    CHECK(facet_n(lib.narrow.form, lib.narrow.nform, "Prose;Verse") == -1);
    CHECK(facet_at(lib.narrow.period, lib.narrow.nperiod, 0, "1900s", 3));
    CHECK(facet_n(lib.narrow.genre, lib.narrow.ngenre, "Essay") == 2);
    library_free(&lib);

    CHECK(index_library_narrow(ix, NULL, "English\nGreek", NULL, NULL, NULL, NULL, &lib, &e) == 0);
    CHECK(lib.total == 4);
    CHECK(vol_suffix(&lib, "/d.txt") == NULL);
    CHECK(facet_at(lib.narrow.author, lib.narrow.nauthor, 0, "Freud", 2));
    CHECK(facet_n(lib.narrow.author, lib.narrow.nauthor, "Aristotle") == 1);
    CHECK(facet_n(lib.narrow.language, lib.narrow.nlanguage, "English") == 3);
    CHECK(facet_n(lib.narrow.language, lib.narrow.nlanguage, "Greek") == 1);
    library_free(&lib);

    CHECK(index_library_narrow(ix, NULL, "English", "Aristotle", NULL, NULL, NULL, &lib, &e) == 0);
    CHECK(lib.total == 0 && lib.n == 0);
    CHECK(facet_at(lib.narrow.language, lib.narrow.nlanguage, 0, "Greek", 1));
    CHECK(facet_n(lib.narrow.language, lib.narrow.nlanguage, "English") == 0);
    CHECK(facet_n(lib.narrow.author, lib.narrow.nauthor, "Aristotle") == 0);
    CHECK(facet_n(lib.narrow.author, lib.narrow.nauthor, "Freud") == 2);
    library_free(&lib);

    CHECK(index_library_narrow(ix, "freud", NULL, NULL, NULL, NULL, NULL, &lib, &e) == 0);
    CHECK(lib.total == 2);
    CHECK(facet_n(lib.narrow.language, lib.narrow.nlanguage, "English") == 2);
    CHECK(facet_n(lib.narrow.language, lib.narrow.nlanguage, "Greek") == -1);
    CHECK(facet_n(lib.narrow.author, lib.narrow.nauthor, "Freud") == 2);
    CHECK(facet_n(lib.narrow.author, lib.narrow.nauthor, "Jung") == 1);
    library_free(&lib);

    char *many = NULL;
    size_t many_n = 0;
    for (int i = 0; i < 65; i++) {
        char line[32];
        int nw = snprintf(line, sizeof line, "Name%d\n", i);
        if (nw < 0 || (size_t)nw >= sizeof line) {
            fails++;
            break;
        }
        char *grown = realloc(many, many_n + (size_t)nw + 1);
        if (!grown) {
            fails++;
            break;
        }
        memcpy(grown + many_n, line, (size_t)nw + 1);
        many = grown;
        many_n += (size_t)nw;
    }
    CHECK(index_library_narrow(ix, NULL, many, NULL, NULL, NULL, NULL, &lib, &e) != 0);
    free(many);
    library_free(&lib);

    Facets fac;
    memset(&fac, 0, sizeof fac);
    Hit *h = NULL;
    int nh = 0;
    CHECK(index_search(ix, "widget language:English", 10, NULL, &h, &nh, &fac, NULL, &e) == 0);
    CHECK(nh == 3 && fac.nfiles == 3);
    CHECK(facet_n(fac.types, fac.ntypes, "txt") == 3);
    CHECK(facet_n(fac.types, fac.ntypes, "md") == -1);
    CHECK(facet_at(fac.narrow.author, fac.narrow.nauthor, 0, "Freud", 2));
    CHECK(facet_n(fac.narrow.author, fac.narrow.nauthor, "Jung") == 1);
    CHECK(facet_n(fac.narrow.author, fac.narrow.nauthor, "Aristotle") == -1);
    CHECK(facet_n(fac.narrow.form, fac.narrow.nform, "Prose") == 1);
    CHECK(facet_n(fac.narrow.form, fac.narrow.nform, "Prose; Verse") == 1);
    expect_miss(h, nh, "/b.md", "english");
    expect_miss(h, nh, "/d.txt", "english");
    hits_free(h, nh);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    nh = 0;
    CHECK(index_search(ix, "language:English language:Greek", 10, NULL, &h, &nh, &fac, NULL, &e) == 0);
    CHECK(fac.nfiles == 4);
    expect_miss(h, nh, "/d.txt", "either language");
    hits_free(h, nh);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    nh = 0;
    CHECK(index_search(ix, "author:Freud author:Aristotle", 10, NULL, &h, &nh, &fac, NULL, &e) == 0);
    CHECK(fac.nfiles == 3);
    expect_hit(h, nh, "/b.md", "or author");
    expect_miss(h, nh, "/e.txt", "or author");
    hits_free(h, nh);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    nh = 0;
    CHECK(index_search(ix, "author:Jung language:Greek", 10, NULL, &h, &nh, &fac, NULL, &e) == 0);
    CHECK(nh == 0 && fac.nfiles == 0);
    CHECK(facet_n(fac.narrow.language, fac.narrow.nlanguage, "Greek") == 0);
    CHECK(facet_n(fac.narrow.language, fac.narrow.nlanguage, "English") == 1);
    CHECK(facet_n(fac.narrow.author, fac.narrow.nauthor, "Jung") == 0);
    CHECK(facet_n(fac.narrow.author, fac.narrow.nauthor, "Aristotle") == 1);
    hits_free(h, nh);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    nh = 0;
    CHECK(index_search(ix, "form:Prose", 10, NULL, &h, &nh, &fac, NULL, &e) == 0);
    CHECK(fac.nfiles == 2);
    expect_miss(h, nh, "/e.txt", "whole form");
    expect_hit(h, nh, "/a.txt", "whole form");
    hits_free(h, nh);
    facets_free(&fac);

    memset(&fac, 0, sizeof fac);
    h = NULL;
    nh = 0;
    CHECK(index_search(ix, "language:Latin", 10, NULL, &h, &nh, &fac, NULL, &e) == 0);
    CHECK(nh == 1 && fac.nfiles == 1);
    expect_hit(h, nh, "/e.txt", "latin");
    CHECK(facet_n(fac.narrow.language, fac.narrow.nlanguage, "Latin") == 1);
    CHECK(facet_n(fac.narrow.language, fac.narrow.nlanguage, "English") == 3);
    hits_free(h, nh);
    facets_free(&fac);
    index_close(ix);
    ix = NULL;
done:
    library_free(&lib);
    if (ix) {
        index_close(ix);
    }
    for (int i = 0; i < 5; i++) {
        free(paths[i]);
    }
    free(docs);
    free(idx);
    rm_rf(root);
}

static void test_library(void) {
    char tmpl[] = "/tmp/grove-lib-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *docs = path_join(root, "docs");
    char *idx = path_join(root, "idx");
    char *file = docs ? path_join(docs, "a.txt") : NULL;
    char *other = docs ? path_join(docs, "b.txt") : NULL;
    char *db = idx ? path_join(idx, "grove.db") : NULL;
    Index *ix = NULL;
    Library lib;
    memset(&lib, 0, sizeof lib);
    Err e;
    err_clear(&e);
    if (!docs || !idx || !file || !other || !db || mkdir(docs, 0700) != 0) {
        fails++;
        goto done;
    }
    FILE *f = fopen(file, "w");
    if (!f) {
        fails++;
        goto done;
    }
    fputs("alpha garden shelf\n", f);
    fclose(f);
    f = fopen(other, "w");
    if (!f) {
        fails++;
        goto done;
    }
    fputs("other kelp text\n", f);
    fclose(f);
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "library open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "library add: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_library_list(ix, NULL, &lib, &e) == 0);
    CHECK(lib.total == 2 && lib.n == 2 && lib.nroots == 1);
    const Volume *garden = NULL;
    for (int i = 0; i < lib.n; i++) {
        if (lib.volumes[i].path && strstr(lib.volumes[i].path, "a.txt")) {
            garden = &lib.volumes[i];
        }
    }
    CHECK(garden && garden->creator && garden->creator[0] == 0);
    Volume rec;
    memset(&rec, 0, sizeof rec);
    rec.path = garden ? garden->path : "";
    rec.title = "Garden Notes";
    rec.creator = "A Reader";
    rec.date = "2026";
    rec.language = "English";
    rec.type = "Prose";
    rec.subject = "Notes";
    rec.coverage = "Imported";
    rec.source = "A shelf";
    rec.description = "A bed.\nAnother.";
    rec.publisher = "";
    rec.contributor = "";
    rec.format = "text";
    rec.identifier = "ISBN 000";
    rec.relation = "";
    rec.rights = "Kept by the reader.";
    CHECK(garden && index_catalog_put(ix, &rec, &e) == 0);
    rec.creator = "Line\nbreak";
    CHECK(index_catalog_put(ix, &rec, &e) != 0);
    char long_title[500];
    memset(long_title, 'a', sizeof long_title - 1);
    long_title[sizeof long_title - 1] = 0;
    rec.creator = "A Reader";
    rec.title = long_title;
    CHECK(index_catalog_put(ix, &rec, &e) != 0);
    library_free(&lib);
    CHECK(index_library_list(ix, "reader", &lib, &e) == 0);
    CHECK(lib.total == 1 && lib.n == 1);
    CHECK(lib.volumes && strcmp(lib.volumes[0].title, "Garden Notes") == 0);
    CHECK(strcmp(lib.volumes[0].creator, "A Reader") == 0);
    CHECK(strcmp(lib.volumes[0].identifier, "ISBN 000") == 0);
    CHECK(strstr(lib.volumes[0].description, "Another."));
    library_free(&lib);
    int nh = 0;
    Hit *hits = do_search(ix, "title:garden", NULL, &nh);
    CHECK(nh == 1);
    hits_free(hits, nh);
    CHECK(index_shelf_build(ix, &e) == 0);
    CHECK(index_shelf_ready(ix) == 1);
    CHECK(index_shelf_build(ix, &e) == 0);
    CHECK(plan_covers(db, "SELECT title, ext, mtime, size, root_id, rel FROM docs INDEXED BY docs_shelf ORDER BY title, rel"));
    index_close(ix);
    err_clear(&e);
    ix = index_open(idx, 0, &e);
    CHECK(ix != NULL);
    CHECK(ix && index_shelf_ready(ix) == 1);
    const char *drop_path = NULL;
    CHECK(ix && index_library_list(ix, "a.txt", &lib, &e) == 0);
    CHECK(lib.n == 1 && lib.volumes && lib.volumes[0].path);
    CHECK(lib.volumes && strcmp(lib.volumes[0].title, "Garden Notes") == 0);
    drop_path = lib.volumes && lib.volumes[0].path ? lib.volumes[0].path : "";
    CHECK(ix && index_library_drop(ix, drop_path, &e) == 0);
    struct stat sb;
    CHECK(stat(file, &sb) == 0);
    library_free(&lib);
    CHECK(ix && index_library_list(ix, NULL, &lib, &e) == 0);
    CHECK(lib.total == 1);
    CHECK(ix && index_library_drop(ix, file, &e) != 0);
done:
    library_free(&lib);
    if (ix) {
        index_close(ix);
    }
    free(db);
    free(other);
    free(file);
    free(docs);
    free(idx);
    rm_rf(root);
}

static int order_is(const Library *lib, const char *a, const char *b, const char *c, const char *d) {
    const char *want[4];
    if (!lib || lib->n != 4) {
        return 0;
    }
    want[0] = a;
    want[1] = b;
    want[2] = c;
    want[3] = d;
    for (int i = 0; i < 4; i++) {
        if (!lib->volumes[i].path || !strstr(lib->volumes[i].path, want[i])) {
            return 0;
        }
    }
    return 1;
}

static int listed(const Library *lib, const char *suffix) {
    for (int i = 0; i < (lib ? lib->n : 0); i++) {
        if (lib->volumes[i].path && strstr(lib->volumes[i].path, suffix)) {
            return 1;
        }
    }
    return 0;
}

static void put_sorted(Index *ix, const char *path, const char *creator, const char *date, const char *language,
                       const char *type, const char *subject, const char *coverage, const char *source) {
    Volume rec;
    Err e;
    err_clear(&e);
    memset(&rec, 0, sizeof rec);
    rec.path = (char *)path;
    rec.title = "";
    rec.creator = (char *)creator;
    rec.date = (char *)date;
    rec.language = (char *)language;
    rec.type = (char *)type;
    rec.subject = (char *)subject;
    rec.coverage = (char *)coverage;
    rec.source = (char *)source;
    rec.description = "";
    rec.publisher = "";
    rec.contributor = "";
    rec.format = "";
    rec.identifier = "ITEM-1";
    rec.relation = "";
    rec.rights = "";
    if (index_catalog_put(ix, &rec, &e) != 0) {
        fprintf(stderr, "sort catalog %s: %s\n", path ? path : "", e.msg);
        fails++;
    }
}

static const Volume *vol_by(const Library *lib, const char *suffix) {
    for (int i = 0; i < (lib ? lib->n : 0); i++) {
        if (lib->volumes[i].path && strstr(lib->volumes[i].path, suffix)) {
            return &lib->volumes[i];
        }
    }
    return NULL;
}

static void test_named_creator(void) {
    char tmpl[] = "/tmp/grove-named-XXXXXX";
    char *root = mkdtemp(tmpl);
    char *docs = root ? path_join(root, "docs") : NULL;
    char *idx = root ? path_join(root, "idx") : NULL;
    const char *names[] = {"Rose \xE2\x80\x94 Ada.txt", "Pine \xE2\x80\x94 Bev.txt", "Plain.txt",
                           "Oak \xE2\x80\x94 Ida.txt", "Pooh \xE2\x80\x94 A. A. Milne.txt", "Hyphen - Ann.txt"};
    char *paths[6];
    Index *ix = NULL;
    Library lib;
    Err e;
    const Volume *v;
    memset(paths, 0, sizeof paths);
    memset(&lib, 0, sizeof lib);
    err_clear(&e);
    if (!root || !docs || !idx || mkdir(docs, 0700) != 0) {
        fprintf(stderr, "named corpus\n");
        fails++;
        goto done;
    }
    for (int i = 0; i < 6; i++) {
        paths[i] = path_join(docs, names[i]);
        if (!paths[i] || write_file(paths[i], "named shelf\n", 12) != 0) {
            fails++;
            goto done;
        }
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "named open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "named add: %s\n", e.msg);
        fails++;
        goto done;
    }
    put_sorted(ix, paths[3], "Zed", "", "", "", "", "", "");
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 0, &lib, &e) == 0);
    CHECK(lib.total == 6 && lib.n == 6);
    CHECK(lib.volumes && strstr(lib.volumes[0].path, "Pooh"));
    CHECK(lib.volumes[1].path && strstr(lib.volumes[1].path, "Rose"));
    CHECK(lib.volumes[2].path && strstr(lib.volumes[2].path, "Pine"));
    CHECK(lib.volumes[3].path && strstr(lib.volumes[3].path, "Oak"));
    CHECK(lib.volumes[4].path && strstr(lib.volumes[4].path, "Hyphen"));
    CHECK(lib.volumes[5].path && strstr(lib.volumes[5].path, "Plain"));
    v = vol_by(&lib, "Pooh");
    CHECK(v && strcmp(v->creator, "A. A. Milne") == 0);
    v = vol_by(&lib, "Rose");
    CHECK(v && strcmp(v->creator, "Ada") == 0);
    v = vol_by(&lib, "Oak");
    CHECK(v && strcmp(v->creator, "Zed") == 0);
    v = vol_by(&lib, "Plain");
    CHECK(v && v->creator && v->creator[0] == 0);
    v = vol_by(&lib, "Hyphen");
    CHECK(v && v->creator && v->creator[0] == 0);
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 1, &lib, &e) == 0);
    CHECK(lib.volumes && strstr(lib.volumes[0].path, "Oak"));
    CHECK(lib.volumes[3].path && strstr(lib.volumes[3].path, "Pooh"));
    CHECK(lib.volumes[5].path && strstr(lib.volumes[5].path, "Plain"));
done:
    library_free(&lib);
    if (ix) {
        index_close(ix);
    }
    for (int i = 0; i < 6; i++) {
        free(paths[i]);
    }
    free(docs);
    free(idx);
    rm_rf(root);
}

static void test_library_sort(void) {
    char tmpl[] = "/tmp/grove-sort-XXXXXX";
    char *root = mkdtemp(tmpl);
    char *docs = root ? path_join(root, "docs") : NULL;
    char *idx = root ? path_join(root, "idx") : NULL;
    const char *names[] = {"a.txt", "b.txt", "m.txt", "z.txt"};
    const char *texts[] = {"alpha file\n", "Blank author\n", "Middling\n", "zebra file\n"};
    char *paths[4];
    Index *ix = NULL;
    Library lib;
    Err e;
    memset(paths, 0, sizeof paths);
    memset(&lib, 0, sizeof lib);
    err_clear(&e);
    if (!root || !docs || !idx || mkdir(docs, 0700) != 0) {
        fprintf(stderr, "sort corpus\n");
        fails++;
        goto done;
    }
    for (int i = 0; i < 4; i++) {
        paths[i] = path_join(docs, names[i]);
        if (!paths[i] || write_file(paths[i], texts[i], strlen(texts[i])) != 0) {
            fails++;
            goto done;
        }
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "sort open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "sort add: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_library_list(ix, NULL, &lib, &e) == 0);
    CHECK(lib.total == 4 && lib.n == 4);
    CHECK(order_is(&lib, "/b.txt", "/m.txt", "/a.txt", "/z.txt"));
    library_free(&lib);
    put_sorted(ix, paths[0], "Ann", "", "", "Prose", "", "1900s", "");
    put_sorted(ix, paths[1], "", "Ancient", "Latin", "", "Notes", "Rome", "Gutenberg");
    put_sorted(ix, paths[2], "Plato", "1922", "English", "Prose", "Essay", "Athens", "Shelf");
    put_sorted(ix, paths[3], "ann", "c. 340 BCE", "Greek", "Verse", "Poem", "", "Delphi");

    CHECK(index_library_narrow(ix, NULL, NULL, NULL, NULL, NULL, NULL, &lib, &e) == 0);
    CHECK(order_is(&lib, "/b.txt", "/m.txt", "/a.txt", "/z.txt"));
    library_free(&lib);

    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_TITLE, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/a.txt", "/b.txt", "/m.txt", "/z.txt"));
    CHECK(lib.volumes && strcmp(lib.volumes[0].identifier, "ITEM-1") == 0);
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, 99, 2, &lib, &e) == 0);
    CHECK(order_is(&lib, "/z.txt", "/m.txt", "/b.txt", "/a.txt"));
    library_free(&lib);

    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/a.txt", "/z.txt", "/m.txt", "/b.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 1, &lib, &e) == 0);
    CHECK(order_is(&lib, "/m.txt", "/a.txt", "/z.txt", "/b.txt"));
    library_free(&lib);

    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_DATE, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/m.txt", "/b.txt", "/z.txt", "/a.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_DATE, 1, &lib, &e) == 0);
    CHECK(order_is(&lib, "/z.txt", "/b.txt", "/m.txt", "/a.txt"));
    library_free(&lib);

    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_LANGUAGE, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/m.txt", "/z.txt", "/b.txt", "/a.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_TYPE, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/a.txt", "/m.txt", "/z.txt", "/b.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_SUBJECT, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/m.txt", "/b.txt", "/z.txt", "/a.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_COVERAGE, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/a.txt", "/m.txt", "/b.txt", "/z.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_SOURCE, 0, &lib, &e) == 0);
    CHECK(order_is(&lib, "/z.txt", "/b.txt", "/m.txt", "/a.txt"));
    library_free(&lib);

    CHECK(index_library_sorted(ix, NULL, "English", NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 0, &lib, &e) == 0);
    CHECK(lib.total == 1 && lib.n == 1);
    CHECK(lib.volumes && lib.volumes[0].path && strstr(lib.volumes[0].path, "/m.txt"));
    library_free(&lib);

done:
    library_free(&lib);
    if (ix) {
        index_close(ix);
    }
    for (int i = 0; i < 4; i++) {
        free(paths[i]);
    }
    free(docs);
    free(idx);
    rm_rf(root);

    char cap_tmpl[] = "/tmp/grove-sortcap-XXXXXX";
    char *cap_root = mkdtemp(cap_tmpl);
    char *cap_docs = cap_root ? path_join(cap_root, "docs") : NULL;
    char *cap_idx = cap_root ? path_join(cap_root, "idx") : NULL;
    char *late = cap_docs ? path_join(cap_docs, "T300.txt") : NULL;
    char *early = cap_docs ? path_join(cap_docs, "T000.txt") : NULL;
    Index *cap = NULL;
    if (!cap_root || !cap_docs || !cap_idx || !late || !early || mkdir(cap_docs, 0700) != 0) {
        fprintf(stderr, "sort cap corpus\n");
        fails++;
        goto cap_done;
    }
    for (int i = 0; i < LIBRARY_SHOW + 1; i++) {
        char name[32];
        char body[64];
        char *path;
        int nw = snprintf(name, sizeof name, "T%03d.txt", i);
        int nb = snprintf(body, sizeof body, "T%03d word\n", i);
        if (nw < 0 || nb < 0 || (size_t)nw >= sizeof name || (size_t)nb >= sizeof body) {
            fails++;
            goto cap_done;
        }
        path = path_join(cap_docs, name);
        if (!path || write_file(path, body, strlen(body)) != 0) {
            free(path);
            fails++;
            goto cap_done;
        }
        free(path);
    }
    err_clear(&e);
    cap = index_open(cap_idx, 1, &e);
    if (!cap) {
        fprintf(stderr, "sort cap open: %s\n", e.msg);
        fails++;
        goto cap_done;
    }
    memset(&st, 0, sizeof st);
    if (index_add(cap, cap_docs, &st, &e) != 0) {
        fprintf(stderr, "sort cap add: %s\n", e.msg);
        fails++;
        goto cap_done;
    }
    put_sorted(cap, early, "Zzz", "", "", "", "", "", "");
    put_sorted(cap, late, "Aaa", "", "", "", "", "", "");
    CHECK(index_library_list(cap, NULL, &lib, &e) == 0);
    CHECK(lib.total == LIBRARY_SHOW + 1 && lib.n == LIBRARY_SHOW);
    CHECK(lib.volumes && lib.volumes[0].path && strstr(lib.volumes[0].path, "/T000.txt"));
    CHECK(!listed(&lib, "/T300.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(cap, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_TITLE, 0, &lib, &e) == 0);
    CHECK(lib.n == LIBRARY_SHOW && lib.volumes && strstr(lib.volumes[0].path, "/T000.txt"));
    CHECK(!listed(&lib, "/T300.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(cap, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 0, &lib, &e) == 0);
    CHECK(lib.total == LIBRARY_SHOW + 1 && lib.n == LIBRARY_SHOW);
    CHECK(lib.volumes && strstr(lib.volumes[0].path, "/T300.txt"));
    CHECK(lib.n > 1 && strstr(lib.volumes[1].path, "/T000.txt"));
    CHECK(!listed(&lib, "/T299.txt"));
    CHECK(listed(&lib, "/T001.txt"));
    library_free(&lib);
    CHECK(index_library_sorted(cap, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 1, &lib, &e) == 0);
    CHECK(lib.volumes && strstr(lib.volumes[0].path, "/T000.txt"));
    CHECK(lib.n > 1 && strstr(lib.volumes[1].path, "/T300.txt"));
    CHECK(!listed(&lib, "/T299.txt"));
    library_free(&lib);

cap_done:
    library_free(&lib);
    if (cap) {
        index_close(cap);
    }
    free(late);
    free(early);
    free(cap_docs);
    free(cap_idx);
    rm_rf(cap_root);
}

static void test_pages(void) {
    Err e;
    err_clear(&e);
    char hex[65];
    fetch_sha256_hex("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    fetch_sha256_hex("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    char *url = NULL;
    CHECK(fetch_normalize("  HTTPS://Example.COM:443/A#frag ", &url, &e) == 0);
    CHECK(url && strcmp(url, "https://example.com/A") == 0);
    free(url);
    url = NULL;
    CHECK(fetch_normalize("http://example.com:80/a", &url, &e) == 0);
    CHECK(url && strcmp(url, "http://example.com/a") == 0);
    free(url);
    url = NULL;
    CHECK(fetch_normalize("http://Example.com:8080/a?B=c", &url, &e) == 0);
    CHECK(url && strcmp(url, "http://example.com:8080/a?B=c") == 0);
    free(url);
    url = NULL;
    CHECK(fetch_normalize("http://[::1]/a", &url, &e) == 0);
    CHECK(url && strcmp(url, "http://[::1]/a") == 0);
    free(url);
    url = NULL;
    err_clear(&e);
    CHECK(fetch_normalize("   ", &url, &e) != 0);
    CHECK(strstr(e.msg, "type a page"));
    CHECK(url == NULL);
    err_clear(&e);
    CHECK(fetch_normalize("file:///etc/passwd", &url, &e) != 0);
    CHECK(fetch_normalize("http://a:b@example.com/", &url, &e) != 0);
    CHECK(fetch_normalize("javascript:alert(1)", &url, &e) != 0);
    CHECK(fetch_normalize("http://example.com/a\nb", &url, &e) != 0);
    char big[2200];
    memset(big, 'a', sizeof big - 1);
    memcpy(big, "http://", 7);
    big[sizeof big - 1] = 0;
    err_clear(&e);
    CHECK(fetch_normalize(big, &url, &e) != 0);
    CHECK(strstr(e.msg, "too long"));

    int fds[2];
    CHECK(pipe(fds) == 0);
    CHECK(write(fds[1], "0123456789", 10) == 10);
    close(fds[1]);
    unsigned char *buf = NULL;
    size_t n = 0;
    err_clear(&e);
    CHECK(fetch_read_cap(fds[0], &buf, &n, 40, &e) == 0);
    CHECK(n == 10 && buf && memcmp(buf, "0123456789", 10) == 0);
    free(buf);
    close(fds[0]);
    CHECK(pipe(fds) == 0);
    char over[41];
    memset(over, 'a', sizeof over);
    CHECK(write(fds[1], over, sizeof over) == (ssize_t)sizeof over);
    close(fds[1]);
    buf = NULL;
    n = 0;
    err_clear(&e);
    CHECK(fetch_read_cap(fds[0], &buf, &n, 40, &e) != 0);
    CHECK(buf == NULL);
    CHECK(strstr(e.msg, "too large"));
    close(fds[0]);

    char tmpl[] = "/tmp/grove-page-XXXXXX";
    char *root = mkdtemp(tmpl);
    char *docs = NULL;
    char *idx = NULL;
    char *file = NULL;
    char *path = NULL;
    char *got = NULL;
    Index *ix = NULL;
    Library lib;
    memset(&lib, 0, sizeof lib);
    if (!root) {
        fails++;
        return;
    }
    docs = path_join(root, "docs");
    idx = path_join(root, "idx");
    file = docs ? path_join(docs, "a.html") : NULL;
    if (!docs || !idx || !file || mkdir(docs, 0700) != 0) {
        fails++;
        goto done;
    }
    FILE *f = fopen(file, "w");
    if (!f) {
        fails++;
        goto done;
    }
    fputs("<html><head><title>Page Title</title></head><body><p>page body token</p></body></html>\n", f);
    fclose(f);
    err_clear(&e);
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "page open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, file, &st, &e) != 0) {
        fprintf(stderr, "page add: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_library_list(ix, NULL, &lib, &e) == 0);
    CHECK(lib.n == 1 && lib.volumes && lib.volumes[0].path);
    path = lib.volumes && lib.volumes[0].path ? xstrdup(lib.volumes[0].path) : NULL;
    if (!path) {
        fails++;
        goto done;
    }
    CHECK(index_page_url(ix, path, &got, &e) == 0);
    CHECK(got == NULL);
    CHECK(index_page_remember(ix, path, "file:///etc/passwd", &e) != 0);
    CHECK(index_page_remember(ix, path, "http://example.com/a", &e) == 0);
    free(got);
    got = NULL;
    CHECK(index_page_url(ix, path, &got, &e) == 0);
    CHECK(got && strcmp(got, "http://example.com/a") == 0);
    library_free(&lib);
    CHECK(index_library_list(ix, NULL, &lib, &e) == 0);
    CHECK(lib.n == 1 && lib.volumes);
    CHECK(lib.volumes && strcmp(lib.volumes[0].title, "Page Title") == 0);
    CHECK(lib.volumes && strcmp(lib.volumes[0].identifier, "http://example.com/a") == 0);
    Volume rec;
    memset(&rec, 0, sizeof rec);
    rec.path = lib.volumes ? lib.volumes[0].path : "";
    rec.title = "Edited";
    rec.creator = "";
    rec.date = "";
    rec.language = "";
    rec.type = "";
    rec.subject = "";
    rec.coverage = "";
    rec.source = "";
    rec.description = "";
    rec.publisher = "";
    rec.contributor = "";
    rec.format = "";
    rec.identifier = "Kept mark";
    rec.relation = "";
    rec.rights = "";
    CHECK(lib.volumes && index_catalog_put(ix, &rec, &e) == 0);
    CHECK(index_page_remember(ix, path, "http://example.com/other", &e) == 0);
    free(got);
    got = NULL;
    CHECK(index_page_url(ix, path, &got, &e) == 0);
    CHECK(got && strcmp(got, "http://example.com/other") == 0);
    library_free(&lib);
    CHECK(index_library_list(ix, NULL, &lib, &e) == 0);
    CHECK(lib.volumes && strcmp(lib.volumes[0].title, "Edited") == 0);
    CHECK(lib.volumes && strcmp(lib.volumes[0].identifier, "Kept mark") == 0);
    const char *drop_path = lib.volumes && lib.volumes[0].path ? lib.volumes[0].path : "";
    CHECK(index_library_drop(ix, drop_path, &e) == 0);
    struct stat sb;
    CHECK(stat(file, &sb) == 0);
    free(got);
    got = NULL;
    CHECK(index_page_url(ix, drop_path, &got, &e) == 0);
    CHECK(got == NULL);
    free(got);
    got = NULL;
done:
    free(got);
    library_free(&lib);
    if (ix) {
        index_close(ix);
    }
    free(path);
    free(file);
    free(docs);
    free(idx);
    rm_rf(root);
}

static Hit *search_mode(Index *ix, const char *q, const SearchModes *modes, int *n) {
    Err e;
    err_clear(&e);
    Hit *h = NULL;
    int rc = index_search(ix, q, 50, NULL, &h, n, NULL, modes, &e);
    if (rc != 0) {
        fprintf(stderr, "search [%s]: %s\n", q, e.msg);
        fails++;
        *n = 0;
        return NULL;
    }
    return h;
}

static const char *snip_of(Hit *h, int n, const char *suffix) {
    size_t sn = strlen(suffix);
    for (int i = 0; i < n; i++) {
        if (!h[i].path) {
            continue;
        }
        size_t pn = strlen(h[i].path);
        if (pn >= sn && memcmp(h[i].path + pn - sn, suffix, sn) == 0) {
            return h[i].snippet ? h[i].snippet : "";
        }
    }
    return NULL;
}

static void test_modes(void) {
    char tmpl[] = "/tmp/grove-modes-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *docs = path_join(root, "docs");
    char *idx = path_join(root, "idx");
    char *wn = path_join(root, "wn.txt");
    char *running = docs ? path_join(docs, "running.txt") : NULL;
    char *run = docs ? path_join(docs, "run.txt") : NULL;
    char *azure = docs ? path_join(docs, "azure.txt") : NULL;
    char *eros = docs ? path_join(docs, "eros.txt") : NULL;
    char *flag = docs ? path_join(docs, "flag.txt") : NULL;
    Index *ix = NULL;
    Alert *al = NULL;
    int nal = 0;
    Err e;
    err_clear(&e);
    if (!docs || !idx || !wn || !running || !run || !azure || !eros || !flag || mkdir(docs, 0755) != 0) {
        fprintf(stderr, "modes corpus\n");
        fails++;
        goto done;
    }
    if (write_file(running, "the running horse\n", 18) != 0 || write_file(run, "run along the road\n", 19) != 0 ||
        write_file(azure, "azure banner\n", 13) != 0 || write_file(eros, "only eros remains\n", 18) != 0 ||
        write_file(flag, "raise the flag\n", 15) != 0 ||
        write_file(wn, "banner\tflag,pennant\n", 20) != 0) {
        fails++;
        goto done;
    }
    if (setenv("GROVE_WORDNET", wn, 1) != 0) {
        perror("setenv");
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    if (!ix || add_tree(ix, docs, &e) != 0) {
        fprintf(stderr, "modes index: %s\n", e.msg);
        fails++;
        goto done;
    }
    SearchModes off;
    memset(&off, 0, sizeof off);
    SearchModes stem;
    memset(&stem, 0, sizeof stem);
    stem.stem = 1;
    SearchModes fuzzy;
    memset(&fuzzy, 0, sizeof fuzzy);
    fuzzy.fuzzy = 1;
    SearchModes concept;
    memset(&concept, 0, sizeof concept);
    concept.concept = 1;

    int n = 0;
    Hit *h = search_mode(ix, "runs", &off, &n);
    expect_miss(h, n, "running.txt", "runs exact");
    hits_free(h, n);
    h = search_mode(ix, "runs", &stem, &n);
    expect_hit(h, n, "running.txt", "runs stem");
    const char *snip = snip_of(h, n, "running.txt");
    CHECK(snip && strstr(snip, "<<running>>"));
    CHECK(snip && !strstr(snip, "{{"));
    hits_free(h, n);
    h = search_mode(ix, "running", &stem, &n);
    expect_hit(h, n, "run.txt", "running stem");
    hits_free(h, n);
    h = search_mode(ix, "\"runs horse\"", &stem, &n);
    expect_miss(h, n, "running.txt", "phrase stem");
    hits_free(h, n);
    h = search_mode(ix, "runs horse", &stem, &n);
    expect_hit(h, n, "running.txt", "and stem");
    hits_free(h, n);

    h = search_mode(ix, "azrue", &off, &n);
    expect_miss(h, n, "azure.txt", "azrue exact");
    hits_free(h, n);
    h = search_mode(ix, "azrue", &fuzzy, &n);
    expect_hit(h, n, "azure.txt", "azrue fuzzy");
    snip = snip_of(h, n, "azure.txt");
    CHECK(snip && strstr(snip, "<<azure>>"));
    hits_free(h, n);
    h = search_mode(ix, "azrue~", &off, &n);
    expect_hit(h, n, "azure.txt", "azrue tilde");
    hits_free(h, n);

    h = search_mode(ix, "love", &off, &n);
    expect_miss(h, n, "eros.txt", "love exact");
    hits_free(h, n);
    h = search_mode(ix, "love", &concept, &n);
    expect_hit(h, n, "eros.txt", "love concept");
    snip = snip_of(h, n, "eros.txt");
    CHECK(snip && strstr(snip, "{{eros}}"));
    CHECK(snip && !strstr(snip, "<<"));
    hits_free(h, n);
    h = search_mode(ix, "banner", &concept, &n);
    expect_hit(h, n, "flag.txt", "banner concept");
    snip = snip_of(h, n, "flag.txt");
    CHECK(snip && strstr(snip, "{{flag}}"));
    hits_free(h, n);
    h = search_mode(ix, "banner", &off, &n);
    expect_miss(h, n, "flag.txt", "banner exact");
    hits_free(h, n);

    if (index_alert_watch(ix, "runs", &e) != 0) {
        fprintf(stderr, "modes watch: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_alerts_list(ix, &al, &nal, &e) == 0);
    CHECK(alert_nfiles(al, nal, "runs") == 0);

done:
    index_alerts_free(al, nal);
    if (ix) {
        index_close(ix);
    }
    unsetenv("GROVE_WORDNET");
    free(docs);
    free(idx);
    free(wn);
    free(running);
    free(run);
    free(azure);
    free(eros);
    free(flag);
    rm_rf(root);
}

int main(void) {
    test_bounds();
    test_cert();
    test_tokens();
    test_query_shape();
    test_modes();
    test_zip();
    test_facets();
    test_mtime();
    test_saved();
    test_notes();
    test_library();
    test_named_creator();
    test_library_sort();
    test_narrow();
    test_alerts();
    test_reader();
    test_pages();
    char tmpl[] = "/tmp/grove-test-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        return 1;
    }
    int rc = test_corpus(root);
    rm_rf(root);
    if (fails || rc != 0) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
