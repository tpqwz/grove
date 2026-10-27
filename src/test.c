#include "extract.h"
#include "index.h"
#include "text.h"
#include "zip.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    CHECK(query_parse("東京駅", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_PHRASE && pq.q->nkids == 2);
    parsed_free(&pq);
    CHECK(query_parse("text -blown", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_AND && pq.q->b && pq.q->b->kind == Q_NOT);
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
    int rc = index_search(ix, q, 50, at, &h, n, NULL, &e);
    if (rc != 0) {
        fprintf(stderr, "search [%s]: %s\n", q, e.msg);
        fails++;
        *n = 0;
        return NULL;
    }
    return h;
}

static void rm_rf(const char *path) {
    char cmd[1200];
    int n = snprintf(cmd, sizeof cmd, "rm -rf -- %s", path);
    if (n > 0 && (size_t)n < sizeof cmd) {
        int rc = system(cmd);
        (void)rc;
    }
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
    if (!docs || !idx || !sub || !ff || !ruled || !adoc || !lead || !odt || !epub || !outline || !longp || !phrasep ||
        !manyp ||
        mkdir(docs, 0755) != 0 || mkdir(sub, 0755) != 0) {
        fprintf(stderr, "cannot create corpus\n");
        goto done;
    }
    const char *a_text = "Hello there. This is a full text note about portable search. Don't stop.\n";
    CHECK(write_file(a, a_text, strlen(a_text)) == 0);
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
        int pages = 0;
        int more = 1;
        while (more && pages < 20) {
            Passage *ps = NULL;
            int np = 0;
            err_clear(&e);
            CHECK(index_more(ix, "late", h[0].path, prev, &ps, &np, &more, &e) == 0);
            CHECK(np >= 1);
            if (np >= 1 && ps) {
                CHECK(ps[0].offset > prev);
                CHECK(ps[0].text && strstr(ps[0].text, "<<late>>"));
                for (int i = 1; i < np; i++) {
                    CHECK(ps[i].offset > ps[i - 1].offset);
                }
                prev = ps[np - 1].offset;
            }
            pages++;
            passages_free(ps, np);
        }
        CHECK(more == 0);
        CHECK(pages >= 1);
        Passage *none = NULL;
        int nn = 0;
        int extra = 1;
        err_clear(&e);
        CHECK(index_more(ix, "late", h[0].path, prev, &none, &nn, &extra, &e) == 0);
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
    CHECK(index_search(ix, "widget", 1, NULL, &h, &n, &fac, &e) == 0);
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
    CHECK(index_search(ix, "widget path:shelf/", 1, NULL, &h, &n, &fac, &e) == 0);
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
    CHECK(index_search(ix, "widget ext:md", 50, NULL, &h, &n, &fac, &e) == 0);
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
    CHECK(index_search(ix, "silent", 1, NULL, &h, &n, &fac, &e) == 0);
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

int main(void) {
    test_tokens();
    test_query_shape();
    test_zip();
    test_facets();
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
