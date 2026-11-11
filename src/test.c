#include "expand.h"
#include "os.h"
#include "extract.h"
#include "fetch.h"
#include "index.h"
#include "pdf.h"
#include "persona.h"
#include "text.h"
#include "zip.h"

#include <dirent.h>

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
    CHECK(pq.q == NULL && pq.mtime_year == 2026 && pq.mtime_mon == 10 && pq.mtime_end_year == 0);
    parsed_free(&pq);
    CHECK(query_parse("mtime:2020", &pq, &e) == 0);
    CHECK(pq.mtime_year == 2020 && pq.mtime_mon == 0 && pq.mtime_end_year == 0);
    parsed_free(&pq);
    CHECK(query_parse("mtime:2020-01..2020-06", &pq, &e) == 0);
    CHECK(pq.mtime_year == 2020 && pq.mtime_mon == 1 && pq.mtime_end_year == 2020 && pq.mtime_end_mon == 6);
    parsed_free(&pq);
    CHECK(query_parse("mtime:2020-06..2020-01", &pq, &e) != 0);
    CHECK(query_parse("*able", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_TERM && pq.q->wild && pq.q->text && strcmp(pq.q->text, "*able") == 0);
    parsed_free(&pq);
    CHECK(query_parse("p*t", &pq, &e) == 0);
    CHECK(pq.q && pq.q->wild && !pq.q->prefix);
    parsed_free(&pq);
    CHECK(query_parse("*", &pq, &e) != 0);
    CHECK(query_parse("full NEAR/2 text", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_NEAR && pq.q->span == 2);
    parsed_free(&pq);
    CHECK(query_parse("full NEAR text", &pq, &e) == 0);
    CHECK(pq.q && pq.q->kind == Q_NEAR && pq.q->span == 10);
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

typedef struct TItem {
    const char *name;
    const char *long_name;
    const void *data;
    size_t len;
} TItem;

static void tar_put_octal(unsigned char *dst, int digits, unsigned long v) {
    for (int i = digits; i-- > 0;) {
        dst[i] = (unsigned char)('0' + (v & 7ul));
        v >>= 3;
    }
}

static unsigned tar_sum(const unsigned char *h) {
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) {
        sum += (i >= 148 && i < 156) ? (unsigned)' ' : h[i];
    }
    return sum;
}

static int tar_header(unsigned char *h, const char *name, size_t len, char type) {
    size_t nn = strlen(name);
    if (nn > 100) {
        return -1;
    }
    memset(h, 0, 512);
    memcpy(h, name, nn);
    tar_put_octal(h + 100, 7, 0644ul);
    tar_put_octal(h + 124, 11, (unsigned long)len);
    memset(h + 148, ' ', 8);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar", 5);
    memcpy(h + 263, "00", 2);
    tar_put_octal(h + 148, 6, tar_sum(h));
    h[154] = 0;
    h[155] = ' ';
    return 0;
}

static int tar_pad(Buf *b, size_t len) {
    size_t pad = (512 - (len % 512)) % 512;
    unsigned char z[512];
    if (pad == 0) {
        return 0;
    }
    memset(z, 0, pad);
    return buf_append(b, z, pad);
}

static unsigned char *make_tar(const TItem *items, int n, size_t *outn) {
    Buf b;
    buf_init(&b);
    for (int i = 0; i < n; i++) {
        unsigned char h[512];
        if (items[i].long_name && items[i].long_name[0]) {
            size_t ln = strlen(items[i].long_name);
            if (tar_header(h, "././@LongLink", ln, 'L') != 0 || buf_append(&b, h, 512) != 0 ||
                buf_append(&b, items[i].long_name, ln) != 0 || tar_pad(&b, ln) != 0) {
                buf_free(&b);
                return NULL;
            }
        }
        if (!items[i].name || tar_header(h, items[i].name, items[i].len, '0') != 0 || buf_append(&b, h, 512) != 0 ||
            (items[i].len && buf_append(&b, items[i].data, items[i].len) != 0) || tar_pad(&b, items[i].len) != 0) {
            buf_free(&b);
            return NULL;
        }
    }
    {
        unsigned char z[1024];
        memset(z, 0, sizeof z);
        if (buf_append(&b, z, sizeof z) != 0) {
            buf_free(&b);
            return NULL;
        }
    }
    *outn = b.len;
    return (unsigned char *)b.data;
}

static int gzip_wrap(const void *in, size_t n, unsigned char **out, size_t *outn) {
    uLong bound = compressBound((uLong)n);
    size_t cap = (size_t)bound + 64;
    unsigned char *buf = malloc(cap ? cap : 1);
    z_stream zs;
    int z;
    if (!buf) {
        return -1;
    }
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(buf);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)n;
    zs.next_out = buf;
    zs.avail_out = (uInt)cap;
    z = deflate(&zs, Z_FINISH);
    *outn = (size_t)zs.total_out;
    deflateEnd(&zs);
    if (z != Z_STREAM_END) {
        free(buf);
        return -1;
    }
    *out = buf;
    return 0;
}

typedef struct OItem {
    const char *name;
    const void *data;
    size_t len;
} OItem;

static void ole_dent(unsigned char *ent, const char *name, int type, uint32_t start, uint32_t size) {
    size_t n = strlen(name);
    size_t i;
    memset(ent, 0, 128);
    for (i = 0; i < n && i < 31; i++) {
        ent[i * 2] = (unsigned char)name[i];
    }
    wr_u16(ent + 0x40, (uint16_t)((n + 1) * 2));
    ent[0x42] = (unsigned char)type;
    ent[0x43] = 1;
    wr_u32(ent + 0x44, 0xFFFFFFFFu);
    wr_u32(ent + 0x48, 0xFFFFFFFFu);
    wr_u32(ent + 0x4C, 0xFFFFFFFFu);
    wr_u32(ent + 0x74, start);
    wr_u32(ent + 0x78, size);
}

static void ole_chain(uint32_t *fat, uint32_t start, uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        fat[start + i] = (i + 1 < count) ? start + i + 1 : 0xFFFFFFFEu;
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static unsigned char *unhex(const char *s, size_t *outn) {
    size_t n = strlen(s);
    size_t i;
    unsigned char *p;
    if (n % 2) {
        return NULL;
    }
    p = malloc(n / 2 ? n / 2 : 1);
    if (!p) {
        return NULL;
    }
    for (i = 0; i < n; i += 2) {
        int hi = hex_nibble(s[i]);
        int lo = hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            free(p);
            return NULL;
        }
        p[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    *outn = n / 2;
    return p;
}

static unsigned char *make_ole(const OItem *items, int n, size_t *outn) {
    enum { FREE = 0xFFFFFFFFu, END = 0xFFFFFFFEu, FATSECT = 0xFFFFFFFDu };
    struct {
        int mini;
        uint32_t start;
        uint32_t sectors;
    } lay[4];
    uint32_t mini_secs = 0;
    uint32_t reg_secs = 0;
    uint32_t mini_bytes;
    uint32_t ms_sectors;
    uint32_t mf_sectors;
    uint32_t dir_sectors;
    uint32_t total;
    uint32_t cursor;
    uint32_t fat_at;
    uint32_t dir_at;
    uint32_t mf_at;
    uint32_t ms_at;
    uint32_t fat[128];
    unsigned char *file;
    unsigned char *dir;
    unsigned char *mini_buf = NULL;
    int i;
    if (n < 1 || n > 3) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (items[i].len > 0x00FFFFFFu) {
            return NULL;
        }
        if (items[i].len < 4096) {
            lay[i].mini = 1;
            lay[i].sectors = items[i].len ? (uint32_t)((items[i].len + 63) / 64) : 0;
            lay[i].start = mini_secs;
            mini_secs += lay[i].sectors;
        } else {
            lay[i].mini = 0;
            lay[i].sectors = (uint32_t)((items[i].len + 511) / 512);
            reg_secs += lay[i].sectors;
        }
    }
    mini_bytes = mini_secs * 64;
    ms_sectors = mini_bytes ? (mini_bytes + 511) / 512 : 0;
    mf_sectors = mini_secs ? (mini_secs * 4 + 511) / 512 : 0;
    dir_sectors = (uint32_t)(((1 + n) * 128 + 511) / 512);
    total = 1 + dir_sectors + mf_sectors + ms_sectors + reg_secs;
    if (total > 128 || mf_sectors > 1) {
        return NULL;
    }
    cursor = 0;
    fat_at = cursor++;
    dir_at = cursor;
    cursor += dir_sectors;
    mf_at = mf_sectors ? cursor : END;
    cursor += mf_sectors;
    ms_at = ms_sectors ? cursor : END;
    cursor += ms_sectors;
    for (i = 0; i < n; i++) {
        if (!lay[i].mini) {
            lay[i].start = cursor;
            cursor += lay[i].sectors;
        }
    }
    file = calloc(1, (size_t)(total + 1) * 512);
    dir = calloc(1, (size_t)dir_sectors * 512);
    if (mini_bytes) {
        mini_buf = calloc(1, mini_bytes);
    }
    if (!file || !dir || (mini_bytes && !mini_buf)) {
        free(file);
        free(dir);
        free(mini_buf);
        return NULL;
    }
    memcpy(file, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    wr_u16(file + 0x18, 0x003E);
    wr_u16(file + 0x1A, 3);
    wr_u16(file + 0x1C, 0xFFFE);
    wr_u16(file + 0x1E, 9);
    wr_u16(file + 0x20, 6);
    wr_u32(file + 0x2C, 1);
    wr_u32(file + 0x30, dir_at);
    wr_u32(file + 0x38, 4096);
    wr_u32(file + 0x3C, mf_at);
    wr_u32(file + 0x40, mf_sectors);
    wr_u32(file + 0x44, END);
    wr_u32(file + 0x4C, fat_at);
    for (i = 1; i < 109; i++) {
        wr_u32(file + 0x4C + (size_t)i * 4, FREE);
    }
    ole_dent(dir, "Root Entry", 5, ms_sectors ? ms_at : END, mini_bytes);
    for (i = 0; i < n; i++) {
        ole_dent(dir + (size_t)(i + 1) * 128, items[i].name, 2, lay[i].start, (uint32_t)items[i].len);
        if (lay[i].mini && items[i].len) {
            memcpy(mini_buf + (size_t)lay[i].start * 64, items[i].data, items[i].len);
        } else if (!lay[i].mini && items[i].len) {
            memcpy(file + ((size_t)lay[i].start + 1) * 512, items[i].data, items[i].len);
        }
    }
    memcpy(file + ((size_t)dir_at + 1) * 512, dir, (size_t)dir_sectors * 512);
    if (mini_buf) {
        memcpy(file + ((size_t)ms_at + 1) * 512, mini_buf, mini_bytes);
    }
    for (i = 0; i < 128; i++) {
        fat[i] = FREE;
    }
    fat[fat_at] = FATSECT;
    ole_chain(fat, dir_at, dir_sectors);
    if (mf_sectors) {
        ole_chain(fat, mf_at, mf_sectors);
        for (i = 0; i < 128; i++) {
            wr_u32(file + ((size_t)mf_at + 1) * 512 + (size_t)i * 4, FREE);
        }
        for (i = 0; i < n; i++) {
            if (lay[i].mini && lay[i].sectors) {
                uint32_t s;
                for (s = 0; s < lay[i].sectors; s++) {
                    uint32_t next = (s + 1 < lay[i].sectors) ? lay[i].start + s + 1 : END;
                    wr_u32(file + ((size_t)mf_at + 1) * 512 + (size_t)(lay[i].start + s) * 4, next);
                }
            }
        }
    }
    if (ms_sectors) {
        ole_chain(fat, ms_at, ms_sectors);
    }
    for (i = 0; i < n; i++) {
        if (!lay[i].mini && lay[i].sectors) {
            ole_chain(fat, lay[i].start, lay[i].sectors);
        }
    }
    for (i = 0; i < 128; i++) {
        wr_u32(file + ((size_t)fat_at + 1) * 512 + (size_t)i * 4, fat[i]);
    }
    free(dir);
    free(mini_buf);
    *outn = (size_t)(total + 1) * 512;
    return file;
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
    {
        const char *note = "zipneedle lives in the archive\n";
        ZItem member;
        ZItem nest;
        unsigned char *inner = NULL;
        unsigned char *outer = NULL;
        size_t in = 0;
        size_t on = 0;
        char *pack = path_join(root, "pack.zip");
        char *nested = path_join(root, "nested.zip");
        char *sheet = path_join(root, "grid.xlsx");
        char *deck = path_join(root, "deck.pptx");
        const char *shared = "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                             "<si><t>sheetneedle</t></si></sst>";
        const char *slide = "<p:sld><a:t>slideneedle</a:t></p:sld>";
        ZItem cells;
        ZItem slides;
        member.name = "notes/keep.txt";
        member.data = note;
        member.len = strlen(note);
        member.method = 0;
        inner = make_zip(&member, 1, &in);
        nest.name = "inner.zip";
        nest.data = inner;
        nest.len = in;
        nest.method = 0;
        outer = inner ? make_zip(&nest, 1, &on) : NULL;
        CHECK(pack && nested && inner && outer);
        CHECK(write_file(pack, inner, in) == 0);
        CHECK(write_file(nested, outer, on) == 0);
        err_clear(&e);
        xs = extract_path(pack, &ex, &e);
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "zipneedle") && strstr(ex.text, "notes/keep.txt"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(nested, &ex, &e);
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "zipneedle"));
        extract_free(&ex);
        cells.name = "xl/sharedStrings.xml";
        cells.data = shared;
        cells.len = strlen(shared);
        cells.method = 0;
        slides.name = "ppt/slides/slide1.xml";
        slides.data = slide;
        slides.len = strlen(slide);
        slides.method = 0;
        free(inner);
        inner = make_zip(&cells, 1, &in);
        free(outer);
        outer = make_zip(&slides, 1, &on);
        CHECK(sheet && deck && inner && outer);
        CHECK(write_file(sheet, inner, in) == 0);
        CHECK(write_file(deck, outer, on) == 0);
        err_clear(&e);
        xs = extract_path(sheet, &ex, &e);
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "sheetneedle"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(deck, &ex, &e);
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "slideneedle"));
        extract_free(&ex);
        {
            const char *tbody = "tarneedle lives in the archive\n";
            const char *gbody = "gzneedle\n";
            const char *nbody = "nestneedle\n";
            const char *lbody = "longneedle\n";
            const char *hbody = "hiddentar\n";
            TItem deep;
            TItem items[5];
            unsigned char *tiny = NULL;
            unsigned char *tar = NULL;
            unsigned char *gz = NULL;
            size_t tiny_n = 0;
            size_t tar_n = 0;
            size_t gz_n = 0;
            char *ball = path_join(root, "pack.tar");
            char *ballz = path_join(root, "pack.tar.gz");
            char *tgzp = path_join(root, "pack.tgz");
            char *lone = path_join(root, "note.txt.gz");
            deep.name = "deep/keep.txt";
            deep.long_name = NULL;
            deep.data = nbody;
            deep.len = strlen(nbody);
            tiny = make_tar(&deep, 1, &tiny_n);
            CHECK(gzip_wrap(gbody, strlen(gbody), &gz, &gz_n) == 0);
            items[0].name = "notes/keep.txt";
            items[0].long_name = NULL;
            items[0].data = tbody;
            items[0].len = strlen(tbody);
            items[1].name = ".secret.txt";
            items[1].long_name = NULL;
            items[1].data = hbody;
            items[1].len = strlen(hbody);
            items[2].name = "short.txt";
            items[2].long_name = "notes/long-keep.txt";
            items[2].data = lbody;
            items[2].len = strlen(lbody);
            items[3].name = "notes/note.txt.gz";
            items[3].long_name = NULL;
            items[3].data = gz;
            items[3].len = gz_n;
            items[4].name = "inner.tar";
            items[4].long_name = NULL;
            items[4].data = tiny;
            items[4].len = tiny_n;
            tar = (tiny && gz) ? make_tar(items, 5, &tar_n) : NULL;
            CHECK(ball && ballz && tgzp && lone && tar && gz);
            CHECK(tar && write_file(ball, tar, tar_n) == 0);
            free(gz);
            gz = NULL;
            CHECK(tar && gzip_wrap(tar, tar_n, &gz, &gz_n) == 0);
            CHECK(gz && write_file(ballz, gz, gz_n) == 0);
            CHECK(gz && write_file(tgzp, gz, gz_n) == 0);
            err_clear(&e);
            xs = extract_path(ball, &ex, &e);
            if (xs != EX_OK) {
                fprintf(stderr, "tar extract: %s\n", e.msg);
            }
            CHECK(xs == EX_OK && ex.text && strstr(ex.text, "tarneedle") && strstr(ex.text, "notes/keep.txt"));
            CHECK(ex.text && strstr(ex.text, "longneedle") && strstr(ex.text, "notes/long-keep.txt"));
            CHECK(ex.text && !strstr(ex.text, "short.txt") && !strstr(ex.text, "hiddentar"));
            CHECK(ex.text && strstr(ex.text, "gzneedle") && strstr(ex.text, "notes/note.txt") &&
                  !strstr(ex.text, "notes/note.txt.gz"));
            CHECK(ex.text && strstr(ex.text, "nestneedle") && strstr(ex.text, "inner.tar") &&
                  strstr(ex.text, "deep/keep.txt"));
            extract_free(&ex);
            err_clear(&e);
            xs = extract_path(ballz, &ex, &e);
            if (xs != EX_OK) {
                fprintf(stderr, "tar.gz extract: %s\n", e.msg);
            }
            CHECK(xs == EX_OK && ex.text && strstr(ex.text, "tarneedle") && strstr(ex.text, "nestneedle"));
            extract_free(&ex);
            err_clear(&e);
            xs = extract_path(tgzp, &ex, &e);
            if (xs != EX_OK) {
                fprintf(stderr, "tgz extract: %s\n", e.msg);
            }
            CHECK(xs == EX_OK && ex.text && strstr(ex.text, "tarneedle"));
            extract_free(&ex);
            free(gz);
            gz = NULL;
            CHECK(gzip_wrap(gbody, strlen(gbody), &gz, &gz_n) == 0);
            CHECK(lone && gz && write_file(lone, gz, gz_n) == 0);
            err_clear(&e);
            xs = extract_path(lone, &ex, &e);
            if (xs != EX_OK) {
                fprintf(stderr, "gz extract: %s\n", e.msg);
            }
            CHECK(xs == EX_OK && ex.text && strncmp(ex.text, "note.txt\n", 9) == 0 && strstr(ex.text, "gzneedle"));
            extract_free(&ex);
            free(tiny);
            free(tar);
            free(gz);
            free(ball);
            free(ballz);
            free(tgzp);
            free(lone);
        }
        free(inner);
        free(outer);
        free(pack);
        free(nested);
        free(sheet);
        free(deck);
    }
    {
        unsigned char word[0x510];
        unsigned char table[64];
        unsigned char xbook[4096];
        unsigned char slide[128];
        const char *uword = "docneedle";
        const char *aword = "ansineedle";
        const char *xword = "xlsneedle";
        const char *pword = "pptneedle";
        const char *bword = "byteslide";
        OItem doc_items[2];
        OItem xitem;
        OItem pitem;
        unsigned char *docb = NULL;
        unsigned char *xlsb = NULL;
        unsigned char *pptb = NULL;
        unsigned char *zb = NULL;
        size_t docn = 0;
        size_t xln = 0;
        size_t pn = 0;
        size_t zipn = 0;
        size_t i;
        char *docp = path_join(root, "old.doc");
        char *xlsp = path_join(root, "grid.xls");
        char *pptp = path_join(root, "deck.ppt");
        char *zipdoc = path_join(root, "old-doc.zip");
        uint32_t cps[3];
        size_t clx_at = 3;
        ZItem zitem;
        memset(word, 0, sizeof word);
        memset(table, 0, sizeof table);
        memset(xbook, 0, sizeof xbook);
        memset(slide, 0, sizeof slide);
        wr_u16(word + 0x00, 0xA5EC);
        wr_u16(word + 0x02, 0x00C1);
        wr_u16(word + 0x0A, 0x0204);
        wr_u16(word + 0x0C, 0x00BF);
        wr_u16(word + 0x20, 14);
        wr_u16(word + 0x3E, 22);
        wr_u16(word + 0x98, 34);
        for (i = 0; uword[i]; i++) {
            word[0x400 + i * 2] = (unsigned char)uword[i];
        }
        word[0x400 + i * 2] = 0x0D;
        memcpy(word + 0x500, aword, strlen(aword));
        word[0x500 + strlen(aword)] = 0x93;
        word[0x500 + strlen(aword) + 1] = 0x0D;
        cps[0] = 0;
        cps[1] = (uint32_t)(strlen(uword) + 1);
        cps[2] = cps[1] + (uint32_t)(strlen(aword) + 2);
        table[0] = 0x01;
        table[3] = 0x02;
        wr_u32(table + 4, 28);
        for (i = 0; i < 3; i++) {
            wr_u32(table + clx_at + 1 + 4 + i * 4, cps[i]);
        }
        wr_u32(table + clx_at + 1 + 4 + 12 + 2, 0x400);
        wr_u32(table + clx_at + 1 + 4 + 12 + 8 + 2, (0x500u * 2u) | 0x40000000u);
        wr_u32(word + 0x1A2, 0);
        wr_u32(word + 0x1A6, (uint32_t)(clx_at + 1 + 4 + 28));
        doc_items[0].name = "WordDocument";
        doc_items[0].data = word;
        doc_items[0].len = sizeof word;
        doc_items[1].name = "1Table";
        doc_items[1].data = table;
        doc_items[1].len = clx_at + 1 + 4 + 28;
        docb = make_ole(doc_items, 2, &docn);
        wr_u16(xbook, 0x00FC);
        wr_u16(xbook + 2, 29);
        wr_u32(xbook + 4, 1);
        wr_u32(xbook + 8, 1);
        wr_u16(xbook + 12, (uint16_t)strlen(xword));
        xbook[14] = 1;
        for (i = 0; xword[i]; i++) {
            xbook[15 + i * 2] = (unsigned char)xword[i];
        }
        wr_u16(xbook + 33, 0x0203);
        wr_u16(xbook + 35, 14);
        {
            double forty_two = 42.0;
            uint64_t bits;
            memcpy(&bits, &forty_two, sizeof bits);
            wr_u64(xbook + 37 + 6, bits);
        }
        wr_u16(xbook + 51, 0x027E);
        wr_u16(xbook + 53, 10);
        wr_u32(xbook + 61, 30);
        wr_u16(xbook + 65, 0x000A);
        xitem.name = "Workbook";
        xitem.data = xbook;
        xitem.len = sizeof xbook;
        xlsb = make_ole(&xitem, 1, &xln);
        wr_u16(slide, 0x000F);
        wr_u16(slide + 2, 0x03E8);
        wr_u32(slide + 4, 8 + (uint32_t)strlen(pword) * 2 + 8 + (uint32_t)strlen(bword));
        wr_u16(slide + 8, 0);
        wr_u16(slide + 10, 0x0FA0);
        wr_u32(slide + 12, (uint32_t)strlen(pword) * 2);
        for (i = 0; pword[i]; i++) {
            slide[16 + i * 2] = (unsigned char)pword[i];
        }
        {
            size_t at = 16 + strlen(pword) * 2;
            wr_u16(slide + at, 0);
            wr_u16(slide + at + 2, 0x0FA8);
            wr_u32(slide + at + 4, (uint32_t)strlen(bword));
            memcpy(slide + at + 8, bword, strlen(bword));
        }
        pitem.name = "PowerPoint Document";
        pitem.data = slide;
        pitem.len = 8 + rd_u32(slide + 4);
        pptb = make_ole(&pitem, 1, &pn);
        CHECK(docp && xlsp && pptp && zipdoc && docb && xlsb && pptb);
        CHECK(docb && write_file(docp, docb, docn) == 0);
        CHECK(xlsb && write_file(xlsp, xlsb, xln) == 0);
        CHECK(pptb && write_file(pptp, pptb, pn) == 0);
        zitem.name = "notes/old.doc";
        zitem.data = docb;
        zitem.len = docn;
        zitem.method = 0;
        zb = docb ? make_zip(&zitem, 1, &zipn) : NULL;
        CHECK(zb && write_file(zipdoc, zb, zipn) == 0);
        err_clear(&e);
        xs = extract_path(docp, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "doc extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "docneedle") && strstr(ex.text, "ansineedle") &&
              strstr(ex.text, "\xe2\x80\x9c"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(xlsp, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "xls extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "xlsneedle") && strstr(ex.text, "42") && strstr(ex.text, "\n7"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(pptp, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "ppt extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "pptneedle") && strstr(ex.text, "byteslide"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(zipdoc, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "doc zip extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "docneedle") && strstr(ex.text, "notes/old.doc"));
        extract_free(&ex);
        free(docb);
        free(xlsb);
        free(pptb);
        free(zb);
        free(docp);
        free(xlsp);
        free(pptp);
        free(zipdoc);
    }
    {
        const char *pack_hex =
            "377abcaf271c00041d4b399925000000000000005a000000000000001e97a3b1010020736576656e6e6565646c65206c6976657320696e2074686520617263686976650a000104060001092500070b010001212101000c2100080a01b24e71f500000501190c0000000000000000000000001113006b006500650070002e0074007800740000001900140a0100ac7dfbb56171dd01150601002080b4810000";
        const char *squash_hex =
            "377abcaf271c00049a8cbb2627000000000000005a000000000000002495c660e00577001f5d00309b0a67249741cb529dd10540d479b98bf0e3beb686a3a5676f66c449ba92000104060001092700070b010001212101000c857800080a015965fb4900000501190b0000000000000000000000111500740077006f002f0061002e007400780074000000140a01009c52723d6271dd01150601002080b4810000";
        const char *solid_hex =
            "377abcaf271c00048381408f9c000000000000002000000000000000c59e9b8ce0058f002f5d00309b0a67249741cb529dd10540d479b98bf0e3beb686a3a5676f66c491d478a44ef43c39647f483f1ee6199aae47e0000000813307ae0fd16ac5a0a090a0775ec540189fe329ec7e156758567689e8cca9b45310970c43f5cec2a8a45770194775fc3ef0af870c4ec6786bf55b15142c7435de4e441bad82e5f07aaeff3d483318cc82dec01a7d4b0326f00d3d9946b192d900000017063701096500070b01000123030101055d001000000c7e0a01568068650000";
        const char *outer_hex =
            "377abcaf271c0004a2d636808f000000000000005a00000000000000036fb273e0009e00875d001b9e938af138254485ad023677e76ea55471cd7af97f2c8e99f66daeb7d733e6826243bf1afffb2306637fbf08f106a8b601fc7edbe093ccdd776ee0451b3552094830590d748628bdad80db66e3b1ed9b79c5616fed6a5fefabb44ee7e43f38d3502a12841401aec230604061afe007206e33894e9950dcd47de984f41233a2c5c76c8b8e000000010406000109808f00070b010001212101000c809f00080a01ff847f5900000501190a000000000000000000001111007000610063006b002e0037007a00000019020000140a0100d51dfcb56171dd01150601002080b4810000";
        size_t pack_n = 0;
        size_t squash_n = 0;
        size_t solid_n = 0;
        size_t outer_n = 0;
        unsigned char *pack7 = unhex(pack_hex, &pack_n);
        unsigned char *squash7 = unhex(squash_hex, &squash_n);
        unsigned char *solid7 = unhex(solid_hex, &solid_n);
        unsigned char *outer7 = unhex(outer_hex, &outer_n);
        char *pack_path = path_join(root, "pack.7z");
        char *squash_path = path_join(root, "squash.7z");
        char *solid_path = path_join(root, "solid.7z");
        char *outer_path = path_join(root, "outer.7z");
        CHECK(pack7 && squash7 && solid7 && outer7 && pack_path && squash_path && solid_path && outer_path);
        CHECK(pack7 && write_file(pack_path, pack7, pack_n) == 0);
        CHECK(squash7 && write_file(squash_path, squash7, squash_n) == 0);
        CHECK(solid7 && write_file(solid_path, solid7, solid_n) == 0);
        CHECK(outer7 && write_file(outer_path, outer7, outer_n) == 0);
        err_clear(&e);
        xs = extract_path(pack_path, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "7z extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "sevenneedle") && strstr(ex.text, "keep.txt"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(squash_path, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "7z lzma2 extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "needle-a") && strstr(ex.text, "two/a.txt"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(solid_path, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "7z solid extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "needle-a") && strstr(ex.text, "needle-b"));
        extract_free(&ex);
        err_clear(&e);
        xs = extract_path(outer_path, &ex, &e);
        if (xs != EX_OK) {
            fprintf(stderr, "7z nested extract: %s\n", e.msg);
        }
        CHECK(xs == EX_OK && ex.text && strstr(ex.text, "sevenneedle") && strstr(ex.text, "pack.7z"));
        extract_free(&ex);
        free(pack7);
        free(squash7);
        free(solid7);
        free(outer7);
        free(pack_path);
        free(squash_path);
        free(solid_path);
        free(outer_path);
    }

    {
        struct timespec when[2];
        when[0].tv_sec = 1592222400;
        when[0].tv_nsec = 0;
        when[1] = when[0];
        CHECK(utimensat(AT_FDCWD, a, when, 0) == 0);
    }
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

    h = do_search(ix, "*able", NULL, &n);
    expect_hit(h, n, "/a.txt", "suffix");
    hits_free(h, n);
    h = do_search(ix, "p*able", NULL, &n);
    expect_hit(h, n, "/a.txt", "inner star");
    hits_free(h, n);
    h = do_search(ix, "full NEAR/1 text", NULL, &n);
    expect_hit(h, n, "/a.txt", "near");
    hits_free(h, n);
    h = do_search(ix, "hello NEAR/1 stop", NULL, &n);
    expect_miss(h, n, "/a.txt", "near far");
    hits_free(h, n);
    h = do_search(ix, "mtime:2020-06", NULL, &n);
    expect_hit(h, n, "/a.txt", "month");
    expect_miss(h, n, "/b.txt", "month");
    hits_free(h, n);
    h = do_search(ix, "mtime:2020", NULL, &n);
    expect_hit(h, n, "/a.txt", "year");
    hits_free(h, n);
    h = do_search(ix, "mtime:2020-01..2020-12 portable", NULL, &n);
    expect_hit(h, n, "/a.txt", "range");
    hits_free(h, n);
    h = do_search(ix, "mtime:2021 portable", NULL, &n);
    expect_miss(h, n, "/a.txt", "other year");
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
    ts[0] = grove_atim(&keep_st);
    ts[1] = grove_mtim(&keep_st);
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

static Excerpt *excerpt_by_id(Excerpt *rows, int n, const char *id) {
    for (int i = 0; i < n; i++) {
        if (rows[i].id && strcmp(rows[i].id, id) == 0) {
            return &rows[i];
        }
    }
    return NULL;
}

static void test_excerpts(void) {
    char tmpl[] = "/tmp/grove-excerpts-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    char *idx = path_join(root, "idx");
    Index *ix = NULL;
    Excerpt *rows = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (!idx) {
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "excerpts open: %s\n", e.msg);
        fails++;
        goto done;
    }
    CHECK(index_excerpts_list(ix, &rows, &n, &e) == 0);
    CHECK(n == 0);
    ExcerptSpec spec;
    memset(&spec, 0, sizeof spec);
    spec.id = "ex-one";
    spec.path = " /shelf/a.txt ";
    spec.title = " The Poems ";
    spec.creator = "Goethe";
    spec.heading = "DEDICATION.";
    spec.page = 0;
    spec.text = "  a fair— form  ";
    spec.byte_at = 100;
    spec.end_byte = 120;
    spec.created = 50;
    const char *tags[] = {"  Hope ", "hope"};
    spec.tags = tags;
    spec.ntags = 2;
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    const char *kept[] = {"Hope", "night"};
    spec.tags = kept;
    spec.ntags = 2;
    CHECK(index_excerpt_put(ix, &spec, &e) == 0);
    spec.id = "ex-one";
    spec.ntags = 0;
    spec.tags = NULL;
    spec.text = "other words";
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    spec.id = "ex near";
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    spec.id = "ex-empty";
    spec.text = "   ";
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    spec.id = "ex-line";
    spec.text = "two\nlines";
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    spec.id = "ex-page";
    spec.text = "a page";
    spec.page = EXCERPT_PAGE_MAX + 1;
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    spec.page = 0;
    spec.id = "ex-long";
    char *long_text = malloc((size_t)EXCERPT_TEXT_MAX + 2);
    if (!long_text) {
        fails++;
        goto done;
    }
    memset(long_text, 'a', (size_t)EXCERPT_TEXT_MAX + 1);
    long_text[EXCERPT_TEXT_MAX + 1] = 0;
    spec.text = long_text;
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    free(long_text);
    spec.id = "ex-near";
    spec.text = "a fair— form";
    spec.byte_at = 110;
    spec.end_byte = 130;
    spec.created = 40;
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    spec.id = "ex-far";
    spec.byte_at = 400;
    spec.end_byte = 420;
    spec.created = 80;
    spec.ntags = 0;
    CHECK(index_excerpt_put(ix, &spec, &e) == 0);
    const char *renamed[] = {"dawn"};
    CHECK(index_excerpt_tags(ix, "ex-one", renamed, 1, &e) == 0);
    CHECK(index_excerpt_tags(ix, "ex-missing", renamed, 1, &e) != 0);
    CHECK(index_excerpt_drop(ix, "ex-nope", &e) == 0);
    index_excerpts_free(rows, n);
    rows = NULL;
    n = 0;
    CHECK(index_excerpts_list(ix, &rows, &n, &e) == 0);
    CHECK(n == 2);
    Excerpt *one = excerpt_by_id(rows, n, "ex-one");
    CHECK(one && strcmp(one->path, "/shelf/a.txt") == 0);
    CHECK(one && strcmp(one->title, "The Poems") == 0);
    CHECK(one && strcmp(one->text, "a fair— form") == 0);
    CHECK(one && one->byte_at == 100 && one->end_byte == 120 && one->created == 50);
    CHECK(one && one->ntags == 1 && one->tags && strcmp(one->tags[0], "dawn") == 0);
    Excerpt *far = excerpt_by_id(rows, n, "ex-far");
    CHECK(far && far->created == 80 && far->ntags == 0);
    CHECK(index_excerpt_drop(ix, " ex-far ", &e) == 0);
    index_close(ix);
    ix = index_open(idx, 0, &e);
    if (!ix) {
        fprintf(stderr, "excerpts reopen: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_excerpts_free(rows, n);
    rows = NULL;
    n = 0;
    CHECK(index_excerpts_list(ix, &rows, &n, &e) == 0);
    CHECK(n == 1 && excerpt_by_id(rows, n, "ex-one"));
    int have = n;
    index_excerpts_free(rows, n);
    rows = NULL;
    n = 0;
    for (int i = 0; i < EXCERPT_KEEP - have; i++) {
        char id[32];
        char text[32];
        CHECK(snprintf(id, sizeof id, "k%d", i) > 0);
        CHECK(snprintf(text, sizeof text, "word %d", i) > 0);
        memset(&spec, 0, sizeof spec);
        spec.id = id;
        spec.path = "/shelf/cap.txt";
        spec.text = text;
        spec.byte_at = 0;
        spec.end_byte = 4;
        if (index_excerpt_put(ix, &spec, &e) != 0) {
            fprintf(stderr, "excerpt fill %d: %s\n", i, e.msg);
            fails++;
            break;
        }
    }
    memset(&spec, 0, sizeof spec);
    spec.id = "k-over";
    spec.path = "/shelf/cap.txt";
    spec.text = "one more";
    CHECK(index_excerpt_put(ix, &spec, &e) != 0);
    CHECK(strstr(e.msg, "full") != NULL);
    CHECK(index_excerpts_list(ix, &rows, &n, &e) == 0);
    CHECK(n == EXCERPT_KEEP);
done:
    index_excerpts_free(rows, n);
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

    const char *split =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Page /Contents [2 0 R 3 0 R] >>\nendobj\n"
        "2 0 obj\n<< /Length 12 >>\nstream\n(Alpha) Tj\nendstream\nendobj\n"
        "3 0 obj\n<< /Length 11 >>\nstream\n(Beta) Tj\nendstream\nendobj\n"
        "4 0 obj\n<< /Type /Page /Contents 5 0 R >>\nendobj\n"
        "5 0 obj\n<< /Length 13 >>\nstream\n(Gamma) Tj\nendstream\nendobj\n";
    buf_init(&text);
    title = NULL;
    err_clear(&e);
    CHECK(pdf_extract((const unsigned char *)split, strlen(split), &text, &title, &e) == 0);
    CHECK(text.data && strstr(text.data, "Alpha") && strstr(text.data, "Beta") && strstr(text.data, "Gamma"));
    {
        int feeds = 0;
        const char *ff = NULL;
        if (text.data) {
            for (size_t i = 0; text.data[i]; i++) {
                if (text.data[i] == '\f') {
                    feeds++;
                    ff = text.data + i;
                }
            }
        }
        const char *alpha = text.data ? strstr(text.data, "Alpha") : NULL;
        const char *beta = text.data ? strstr(text.data, "Beta") : NULL;
        CHECK(feeds == 1 && ff && alpha && beta && alpha < ff && beta < ff && strstr(ff, "Gamma"));
    }
    buf_free(&text);
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

static void put_persona(Index *ix, const char *path, const char *title, const char *creator, const char *date,
                        const char *source, const char *contributor) {
    Volume rec;
    Err e;
    err_clear(&e);
    memset(&rec, 0, sizeof rec);
    rec.path = (char *)path;
    rec.title = (char *)(title ? title : "");
    rec.creator = (char *)(creator ? creator : "");
    rec.date = (char *)(date ? date : "");
    rec.language = "";
    rec.type = "";
    rec.subject = "";
    rec.coverage = "";
    rec.source = (char *)(source ? source : "");
    rec.description = "";
    rec.publisher = "";
    rec.contributor = (char *)(contributor ? contributor : "");
    rec.format = "";
    rec.identifier = "";
    rec.relation = "";
    rec.rights = "";
    if (index_catalog_put(ix, &rec, &e) != 0) {
        fprintf(stderr, "persona catalog %s: %s\n", path ? path : "", e.msg);
        fails++;
    }
}

static int persona_is(const Personas *p, const char *name, const char *role, const char *title) {
    int i;
    for (i = 0; i < (p ? p->n : 0); i++) {
        if (p->links[i].name && strcmp(p->links[i].name, name) == 0 && p->links[i].role &&
            strcmp(p->links[i].role, role) == 0 &&
            (!title || (p->links[i].title && strcmp(p->links[i].title, title) == 0))) {
            return 1;
        }
    }
    return 0;
}

static int persona_name_n(const Personas *p, const char *name) {
    int i;
    int n = 0;
    for (i = 0; i < (p ? p->n : 0); i++) {
        if (p->links[i].name && strcmp(p->links[i].name, name) == 0) {
            n++;
        }
    }
    return n;
}

static void test_personas(void) {
    char tmpl[] = "/tmp/grove-persona-XXXXXX";
    char *root = mkdtemp(tmpl);
    char *docs = root ? path_join(root, "docs") : NULL;
    char *idx = root ? path_join(root, "idx") : NULL;
    const char *names[] = {"Iliad \xE2\x80\x94 Not Homer.txt", "Essay.txt", "Poem \xE2\x80\x94 Sappho.txt", "Paine.txt",
                           "Ellis.txt"};
    char *paths[5];
    Index *ix = NULL;
    Library lib;
    Personas people;
    Err e;
    int i;
    const PersonaLink *homer = NULL;
    memset(paths, 0, sizeof paths);
    memset(&lib, 0, sizeof lib);
    memset(&people, 0, sizeof people);
    err_clear(&e);
    if (!root || !docs || !idx || mkdir(docs, 0700) != 0) {
        fprintf(stderr, "persona corpus\n");
        fails++;
        goto done;
    }
    for (i = 0; i < 5; i++) {
        paths[i] = path_join(docs, names[i]);
        if (!paths[i] || write_file(paths[i], "a line of the book\n", 19) != 0) {
            fails++;
            goto done;
        }
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "persona open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs, &st, &e) != 0) {
        fprintf(stderr, "persona add: %s\n", e.msg);
        fails++;
        goto done;
    }
    put_persona(ix, paths[0], "The Iliad", "Homer", "c. 8th century BCE", "Translated by Samuel Butler, 1900",
                "Author: Homer");
    put_persona(ix, paths[1], "", "Plato ; Aristotle", "", "Project Gutenberg",
                "Editor: Ada Lovelace; editor: ada lovelace; Speaker: Grace Hopper; Illustrator: Mary Beale");
    put_persona(ix, paths[3], "", "Thomas Paine", "", "Edited by Merwin Roe, 1907",
                "Read by Claire Danes; Spoken by Mark Rylance");
    put_persona(ix, paths[4], "", "", "", "Translated by William Ellis.", "Translator: J. Solomon.");
    CHECK(index_personas(ix, &people, &e) == 0);
    CHECK(people.truncated == 0);
    CHECK(people.n == 14);
    CHECK(people.links && strcmp(people.links[0].name, "Ada Lovelace") == 0);
    CHECK(people.links && strcmp(people.links[0].role, "Editor") == 0);
    CHECK(persona_name_n(&people, "Homer") == 1);
    CHECK(persona_is(&people, "Homer", "Author", "The Iliad"));
    CHECK(!persona_is(&people, "Not Homer", "Author", NULL));
    CHECK(persona_is(&people, "Samuel Butler", "Translator", "The Iliad"));
    CHECK(persona_is(&people, "Plato", "Author", NULL));
    CHECK(persona_is(&people, "Aristotle", "Author", NULL));
    CHECK(persona_is(&people, "Ada Lovelace", "Editor", NULL));
    CHECK(persona_name_n(&people, "ada lovelace") == 0);
    CHECK(persona_is(&people, "Grace Hopper", "Speaker", NULL));
    CHECK(persona_is(&people, "Mary Beale", "Illustrator", NULL));
    CHECK(!persona_is(&people, "Project Gutenberg", "Contributor", NULL));
    CHECK(persona_name_n(&people, "Project Gutenberg") == 0);
    CHECK(persona_is(&people, "Sappho", "Author", NULL));
    CHECK(persona_is(&people, "Thomas Paine", "Author", NULL));
    CHECK(persona_is(&people, "Merwin Roe", "Editor", NULL));
    CHECK(persona_is(&people, "Claire Danes", "Speaker", NULL));
    CHECK(persona_is(&people, "Mark Rylance", "Speaker", NULL));
    CHECK(persona_is(&people, "William Ellis", "Translator", NULL));
    CHECK(persona_is(&people, "J. Solomon", "Translator", NULL));
    CHECK(persona_name_n(&people, "J. Solomon.") == 0);
    for (i = 0; i < people.n; i++) {
        if (people.links[i].name && strcmp(people.links[i].name, "Homer") == 0) {
            homer = &people.links[i];
        }
    }
    CHECK(homer && homer->date && strcmp(homer->date, "c. 8th century BCE") == 0);
    CHECK(index_library_list(ix, "Butler", &lib, &e) == 0);
    CHECK(listed(&lib, "Iliad"));
    library_free(&lib);
    CHECK(index_library_list(ix, "Claire", &lib, &e) == 0);
    CHECK(listed(&lib, "Paine"));
    library_free(&lib);
    CHECK(index_library_list(ix, "Merwin", &lib, &e) == 0);
    CHECK(listed(&lib, "Paine"));
done:
    personas_free(&people);
    library_free(&lib);
    if (ix) {
        index_close(ix);
    }
    for (i = 0; i < 5; i++) {
        free(paths[i]);
    }
    free(docs);
    free(idx);
    rm_rf(root);
}

static const char *persona_hosts[] = {"upload.wikimedia.org", "en.wikipedia.org", "www.wikidata.org", "query.wikidata.org"};

static int persona_body_set(const char *root, const char *folded, const char *body) {
    char *db_path = root ? path_join(root, "grove.db") : NULL;
    char hex[65];
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    if (!db_path || !folded || !body) {
        free(db_path);
        return -1;
    }
    fetch_sha256_hex(folded, strlen(folded), hex);
    rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL);
    free(db_path);
    if (rc != SQLITE_OK || !db) {
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    rc = sqlite3_prepare_v2(db, "UPDATE persona SET body=?, photo=NULL, mime='' WHERE id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, body, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, hex, 64, SQLITE_TRANSIENT);
        rc = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_ERROR;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

static int persona_file(const char *dir, const char *suffix, char **out) {
    DIR *d = opendir(dir);
    struct dirent *de;
    *out = NULL;
    if (!d) {
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        size_t sn = strlen(suffix);
        if (n > sn && strcmp(de->d_name + n - sn, suffix) == 0) {
            free(*out);
            *out = path_join(dir, de->d_name);
        }
    }
    closedir(d);
    return *out ? 0 : -1;
}

static void test_persona_profile(void) {
    Err e;
    char *url = NULL;
    PersonaProfile p;
    char tmpl[] = "/tmp/grove-profile-XXXXXX";
    char *root = mkdtemp(tmpl);
    char *people = root ? path_join(root, "personas") : NULL;
    char *json = NULL;
    char *marker = root ? path_join(root, "gotcha.json") : NULL;
    char *secret = root ? path_join(root, "secret") : NULL;
    const char *search =
        "{\"search\":["
        "{\"id\":\"Q84473023\",\"label\":\"Aristotle\",\"description\":\"male given name\"},"
        "{\"id\":\"Q1191622\",\"label\":\"Aristotle\",\"description\":\"lunar crater\"},"
        "{\"id\":\"Q868\",\"label\":\"Aristotle\",\"description\":\"Greek philosopher\"},"
        "{\"id\":\"Q5\",\"label\":\"Not a person\",\"description\":\"writer\"}"
        "]}";
    const char *entity =
        "{\"entities\":{\"Q868\":{"
        "\"sitelinks\":{\"enwiki\":{\"title\":\"Aristotle\"}},"
        "\"aliases\":{\"en\":[{\"value\":\"Aristotle\"},{\"value\":\"Aristoteles\"},{\"value\":\"The Stagirite\"}]}"
        "}}}";
    const char *summary =
        "{\"extract\":\"Aristotle was an ancient Greek philosopher.\","
        "\"description\":\"Ancient Greek philosopher and polymath (384–322 BC)\","
        "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/commons/thumb/a/a4/Aristotle.jpg\"},"
        "\"content_urls\":{\"desktop\":{\"page\":\"https://en.wikipedia.org/wiki/Aristotle\"}}}";
    const char *sparql =
        "{\"results\":{\"bindings\":["
        "{\"birth\":{\"value\":\"-0384-01-01T00:00:00Z\"},\"death\":{\"value\":\"-0322-01-01T00:00:00Z\"},"
        "\"occLabel\":{\"value\":\"philosopher\"},\"workLabel\":{\"value\":\"Poetics\"},"
        "\"date\":{\"value\":\"-0335-01-01T00:00:00Z\"},"
        "\"article\":{\"value\":\"https://en.wikipedia.org/wiki/Poetics_(Aristotle)\"}},"
        "{\"occLabel\":{\"value\":\"philosopher\"},\"workLabel\":{\"value\":\"Poetics\"},"
        "\"article\":{\"value\":\"https://en.wikipedia.org/wiki/Poetics_(Aristotle)\"}},"
        "{\"occLabel\":{\"value\":\"Q12345\"},\"workLabel\":{\"value\":\"Q999\"},"
        "\"article\":{\"value\":\"http://169.254.169.254/latest\"}},"
        "{\"occLabel\":{\"value\":\"biologist\"},\"workLabel\":{\"value\":\"Metaphysics\"},"
        "\"article\":{\"value\":\"https://evil.example/Metaphysics\"}}"
        "]}}";
    const char *bad_summary =
        "{\"extract\":\"x\",\"description\":\"d\","
        "\"thumbnail\":{\"source\":\"https://evil.example/a.jpg\"},"
        "\"content_urls\":{\"desktop\":{\"page\":\"javascript:alert(1)\"}}}";
    /* Live Wikipedia summaries put the portrait on thumb.wikimedia.org, with a
     * campaign query. The same path on upload.wikimedia.org is the photograph. */
    const char *thumb_summary =
        "{\"extract\":\"Aristotle was an ancient Greek philosopher.\","
        "\"thumbnail\":{\"source\":\"https://thumb.wikimedia.org/wikipedia/commons/thumb/a/ae/"
        "Aristotle_Altemps_Inv8575.jpg/330px-Aristotle_Altemps_Inv8575.jpg"
        "?utm_source=en.wikipedia.org&utm_campaign=api&utm_content=thumbnail\"}}";
    const char *thumb_port =
        "{\"thumbnail\":{\"source\":\"https://THUMB.WIKIMEDIA.ORG:443/wikipedia/commons/thumb/a.jpg\"}}";
    const char *thumb_evil =
        "{\"thumbnail\":{\"source\":\"https://thumb.wikimedia.org.evil/wikipedia/commons/thumb/a.jpg\"}}";
    const char *thumb_http =
        "{\"thumbnail\":{\"source\":\"http://thumb.wikimedia.org/wikipedia/commons/thumb/a.jpg\"}}";
    const char *thumb_user =
        "{\"thumbnail\":{\"source\":\"https://user:pass@thumb.wikimedia.org/wikipedia/a.jpg\"}}";
    const char *thumb_other =
        "{\"thumbnail\":{\"source\":\"https://thumb.wikimedia.org/other/a.jpg\"}}";
    const char *thumb_kept = "https://upload.wikimedia.org/wikipedia/commons/thumb/a/ae/"
                             "Aristotle_Altemps_Inv8575.jpg/330px-Aristotle_Altemps_Inv8575.jpg";
    const char *given = "{\"search\":[{\"id\":\"Q1\",\"label\":\"Homer\",\"description\":\"male given name\"}]}";
    const char *crater = "{\"search\":[{\"id\":\"Q1\",\"label\":\"Aristotle\",\"description\":\"lunar crater\"}]}";
    const char *contains =
        "{\"search\":[{\"id\":\"Q5\",\"label\":\"Aristotle of Stageira\",\"description\":\"ancient writer\"}]}";
    const char *kant =
        "{\"results\":{\"bindings\":[{\"birth\":{\"value\":\"+1724-04-22T00:00:00Z\"},"
        "\"workLabel\":{\"value\":\"Critique\"},\"date\":{\"value\":\"+0000-01-01T00:00:00Z\"}}]}}";
    unsigned char *jpg = NULL;
    PersonaProfile made;
    PersonaProfile hippo;
    PersonaProfile loaded;
    char long_name[202];
    memset(&p, 0, sizeof p);
    memset(&made, 0, sizeof made);
    memset(&hippo, 0, sizeof hippo);
    memset(&loaded, 0, sizeof loaded);
    err_clear(&e);
    memset(long_name, 'a', 201);
    long_name[201] = 0;

    CHECK(fetch_https_allowed("https://upload.wikimedia.org/wikipedia/commons/thumb/a.jpg", persona_hosts, 4) == 1);
    CHECK(fetch_https_allowed("https://upload.wikimedia.org:443/a.jpg", persona_hosts, 4) == 1);
    CHECK(fetch_https_allowed("https://UPLOAD.WIKIMEDIA.ORG/a.jpg", persona_hosts, 4) == 1);
    CHECK(fetch_https_allowed("http://upload.wikimedia.org/a.jpg", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("https://upload.wikimedia.org.evil/a.jpg", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("https://evil.upload.wikimedia.org/a.jpg", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("https://user:pass@upload.wikimedia.org/a.jpg", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("https://127.0.0.1/a.jpg", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("https://upload.wikimedia.org:444/a.jpg", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("https://169.254.169.254/", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("file:///etc/passwd", persona_hosts, 4) == 0);
    CHECK(fetch_https_allowed("https://en.wikipedia.org/wiki/Aristotle", persona_hosts, 4) == 1);
    CHECK(fetch_https_allowed("https://en.wikipedia.org.attacker.net/wiki/Aristotle", persona_hosts, 4) == 0);

    CHECK(persona_lookup_url(PERSONA_FIND, "Aristotle&format=xml", &url, &e) == 0);
    CHECK(url && strstr(url, "https://www.wikidata.org/w/api.php?action=wbsearchentities&search=Aristotle%26format%3Dxml&") == url);
    free(url);
    url = NULL;
    CHECK(persona_lookup_url(PERSONA_FIND, "  ", &url, &e) != 0);
    CHECK(url == NULL);
    CHECK(persona_lookup_url(PERSONA_FIND, long_name, &url, &e) != 0);
    CHECK(persona_lookup_url(PERSONA_ENTITY, "Q868", &url, &e) == 0);
    CHECK(url && strstr(url, "https://www.wikidata.org/") == url);
    CHECK(url && strstr(url, "ids=Q868") != NULL);
    free(url);
    url = NULL;
    CHECK(persona_lookup_url(PERSONA_ENTITY, "Q868;SELECT", &url, &e) != 0);
    CHECK(url == NULL);
    CHECK(persona_lookup_url(PERSONA_SPARQL, "Q868 OR 1", &url, &e) != 0);
    CHECK(persona_lookup_url(PERSONA_SUMMARY, "Foo Bar", &url, &e) == 0);
    CHECK(url && strcmp(url, "https://en.wikipedia.org/api/rest_v1/page/summary/Foo%20Bar") == 0);
    free(url);
    url = NULL;
    CHECK(persona_lookup_url(PERSONA_SUMMARY, "https://evil.example/x", &url, &e) == 0);
    CHECK(url && strncmp(url, "https://en.wikipedia.org/api/rest_v1/page/summary/", 50) == 0);
    CHECK(url && strstr(url, "https://evil") == NULL);
    free(url);
    url = NULL;

    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), summary, strlen(summary),
                                sparql, strlen(sparql), &p, &e) == 0);
    CHECK(p.state == PERSONA_READY);
    CHECK(p.qid && strcmp(p.qid, "Q868") == 0);
    CHECK(p.description && strcmp(p.description, "Ancient Greek philosopher and polymath (384–322 BC)") == 0);
    CHECK(p.summary && strcmp(p.summary, "Aristotle was an ancient Greek philosopher.") == 0);
    CHECK(p.born && strcmp(p.born, "384 BCE") == 0);
    CHECK(p.died && strcmp(p.died, "322 BCE") == 0);
    CHECK(p.occupation && strcmp(p.occupation, "philosopher, biologist") == 0);
    CHECK(p.wikipedia && strcmp(p.wikipedia, "https://en.wikipedia.org/wiki/Aristotle") == 0);
    CHECK(p.photo_url && strcmp(p.photo_url, "https://upload.wikimedia.org/wikipedia/commons/thumb/a/a4/Aristotle.jpg") == 0);
    CHECK(p.alias_n == 2);
    CHECK(p.aliases && strcmp(p.aliases[0], "Aristoteles") == 0);
    CHECK(p.aliases && strcmp(p.aliases[1], "The Stagirite") == 0);
    CHECK(p.work_n == 2);
    CHECK(p.works && strcmp(p.works[0].title, "Poetics") == 0);
    CHECK(p.works && p.works[0].year && strcmp(p.works[0].year, "335 BCE") == 0);
    CHECK(p.works && p.works[0].url && strcmp(p.works[0].url, "https://en.wikipedia.org/wiki/Poetics_(Aristotle)") == 0);
    CHECK(p.works && strcmp(p.works[1].title, "Metaphysics") == 0);
    CHECK(p.works && (!p.works[1].url || !p.works[1].url[0]));
    persona_profile_free(&p);

    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), bad_summary,
                                strlen(bad_summary), NULL, 0, &p, &e) == 0);
    CHECK(p.state == PERSONA_READY);
    CHECK(p.photo_url == NULL);
    CHECK(p.wikipedia == NULL);
    CHECK(p.summary && strcmp(p.summary, "x") == 0);
    persona_profile_free(&p);

    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), thumb_summary,
                                strlen(thumb_summary), NULL, 0, &p, &e) == 0);
    CHECK(p.photo_url && strcmp(p.photo_url, thumb_kept) == 0);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), thumb_port,
                                strlen(thumb_port), NULL, 0, &p, &e) == 0);
    CHECK(p.photo_url && strcmp(p.photo_url, "https://upload.wikimedia.org/wikipedia/commons/thumb/a.jpg") == 0);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), thumb_evil,
                                strlen(thumb_evil), NULL, 0, &p, &e) == 0);
    CHECK(p.photo_url == NULL);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), thumb_http,
                                strlen(thumb_http), NULL, 0, &p, &e) == 0);
    CHECK(p.photo_url == NULL);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), thumb_user,
                                strlen(thumb_user), NULL, 0, &p, &e) == 0);
    CHECK(p.photo_url == NULL);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Aristotle", search, strlen(search), entity, strlen(entity), thumb_other,
                                strlen(thumb_other), NULL, 0, &p, &e) == 0);
    CHECK(p.photo_url == NULL);
    persona_profile_free(&p);

    CHECK(persona_profile_parse("Aristotle", search, strlen(search), NULL, 0, NULL, 0, NULL, 0, &p, &e) == 0);
    CHECK(p.state == PERSONA_READY);
    CHECK(p.qid && strcmp(p.qid, "Q868") == 0);
    CHECK(p.description && strcmp(p.description, "Greek philosopher") == 0);
    CHECK(!p.summary || !p.summary[0]);
    persona_profile_free(&p);

    CHECK(persona_profile_parse("Homer", given, strlen(given), NULL, 0, NULL, 0, NULL, 0, &p, &e) == 0);
    CHECK(p.state == PERSONA_NONE);
    CHECK(p.qid == NULL);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Aristotle", crater, strlen(crater), NULL, 0, NULL, 0, NULL, 0, &p, &e) == 0);
    CHECK(p.state == PERSONA_NONE);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Aristotle", contains, strlen(contains), NULL, 0, NULL, 0, NULL, 0, &p, &e) == 0);
    CHECK(p.state == PERSONA_READY);
    CHECK(p.qid && strcmp(p.qid, "Q5") == 0);
    persona_profile_free(&p);
    CHECK(persona_profile_parse("Kant", "{\"search\":[{\"id\":\"Q5\",\"label\":\"Kant\",\"description\":\"writer\"}]}",
                                strlen("{\"search\":[{\"id\":\"Q5\",\"label\":\"Kant\",\"description\":\"writer\"}]}"),
                                NULL, 0, NULL, 0, kant, strlen(kant), &p, &e) == 0);
    CHECK(p.born && strcmp(p.born, "1724") == 0);
    CHECK(!p.died || !p.died[0]);
    CHECK(p.works && p.work_n == 1 && (!p.works[0].year || !p.works[0].year[0]));
    persona_profile_free(&p);

    jpg = malloc(4);
    if (!root || !people || !marker || !secret || !jpg) {
        fprintf(stderr, "persona profile corpus\n");
        fails++;
        free(jpg);
        goto done;
    }
    memcpy(jpg, "\xff\xd8\xff\x00", 4);
    made.state = PERSONA_READY;
    made.qid = xstrdup("Q1");
    made.description = xstrdup("cached");
    made.summary = xstrdup("from the shelf cache");
    made.photo = jpg;
    made.photo_n = 4;
    made.photo_type = xstrdup("image/jpeg");
    jpg = NULL;
    CHECK(persona_profile_store(root, "Aristotle", &made, &e) == 0);
    CHECK(persona_file(people, ".json", &json) != 0);
    CHECK(json == NULL);
    CHECK(write_file(secret, "SECRET", 6) == 0);
    {
        const char *evil =
            "{\"v\":1,\"qid\":\"Q1\",\"summary\":\"kept\",\"wikipedia\":\"https://evil.example/a\","
            "\"photo\":\"../../secret\",\"works\":[{\"title\":\"Poetics\",\"year\":\"\",\"url\":\"https://127.0.0.1/x\"}]}";
        const char *dropped = "{\"v\":1,\"qid\":\"Q1;drop\",\"summary\":\"no\"}";
        CHECK(persona_body_set(root, "aristotle", evil) == 0);
        CHECK(persona_profile_load(root, "Aristotle", &loaded, &e) == 0);
        CHECK(loaded.state == PERSONA_READY);
        CHECK(loaded.qid && strcmp(loaded.qid, "Q1") == 0);
        CHECK(loaded.summary && strcmp(loaded.summary, "kept") == 0);
        CHECK(loaded.wikipedia == NULL);
        CHECK(loaded.photo_n == 0);
        CHECK(strcmp(loaded.summary, "SECRET") != 0);
        CHECK(loaded.work_n == 1 && loaded.works && (!loaded.works[0].url || !loaded.works[0].url[0]));
        persona_profile_free(&loaded);
        CHECK(persona_body_set(root, "aristotle", dropped) == 0);
        CHECK(persona_profile_load(root, "Aristotle", &loaded, &e) == 0);
        CHECK(loaded.state == PERSONA_NONE);
        persona_profile_free(&loaded);
    }
    CHECK(persona_profile_store(root, "Aristotle", &made, &e) == 0);
    CHECK(persona_profile(root, "  aristotle ", &loaded, &e) == 0);
    CHECK(loaded.state == PERSONA_READY);
    CHECK(loaded.qid && strcmp(loaded.qid, "Q1") == 0);
    CHECK(loaded.summary && strcmp(loaded.summary, "from the shelf cache") == 0);
    CHECK(loaded.photo_n == 4 && loaded.photo && memcmp(loaded.photo, "\xff\xd8\xff\x00", 4) == 0);
    CHECK(loaded.photo_type && strcmp(loaded.photo_type, "image/jpeg") == 0);
    persona_profile_free(&loaded);
    hippo.state = PERSONA_READY;
    hippo.qid = xstrdup("Q9501");
    hippo.summary = xstrdup("a different person");
    CHECK(persona_profile_store(root, "Augustine of Hippo", &hippo, &e) == 0);
    CHECK(persona_profile_store(root, "../gotcha", &made, &e) == 0);
    CHECK(access(marker, F_OK) != 0);
    CHECK(persona_profile_load(root, "Augustine", &loaded, &e) == 0);
    CHECK(loaded.state == PERSONA_NONE);
    persona_profile_free(&loaded);
    CHECK(persona_profile_load(root, "Augustine of Hippo", &loaded, &e) == 0);
    CHECK(loaded.qid && strcmp(loaded.qid, "Q9501") == 0);
    persona_profile_free(&loaded);
    {
        unsigned char *html = malloc(6);
        PersonaProfile bad;
        memset(&bad, 0, sizeof bad);
        if (!html) {
            fails++;
        } else {
            memcpy(html, "<html>", 6);
            bad.state = PERSONA_READY;
            bad.qid = xstrdup("Q2");
            bad.summary = xstrdup("cached poet");
            bad.photo = html;
            bad.photo_n = 6;
            bad.photo_type = xstrdup("image/jpeg");
            CHECK(persona_profile_store(root, "Sappho", &bad, &e) == 0);
            CHECK(persona_profile(root, "Sappho", &loaded, &e) == 0);
            CHECK(loaded.state == PERSONA_READY);
            CHECK(loaded.qid && strcmp(loaded.qid, "Q2") == 0);
            CHECK(loaded.photo_n == 0);
            persona_profile_free(&loaded);
        }
        persona_profile_free(&bad);
    }
    {
        char tmpl2[] = "/tmp/grove-profile-leg-XXXXXX";
        char *leg = mkdtemp(tmpl2);
        char *leg_people = leg ? path_join(leg, "personas") : NULL;
        char hex[65];
        char base[80];
        char *leg_json = NULL;
        char *leg_img = NULL;
        char *bad_json = NULL;
        const char *kept = "{\"v\":1,\"qid\":\"Q868\",\"summary\":\"from the file cache\","
                           "\"wikipedia\":\"https://en.wikipedia.org/wiki/Aristotle\","
                           "\"photo\":\"image/jpeg\",\"aliases\":[\"Aristoteles\"],"
                           "\"works\":[{\"title\":\"Poetics\",\"year\":\"\","
                           "\"url\":\"https://en.wikipedia.org/wiki/Poetics_(Aristotle)\"}]}";
        const char *bad = "{\"v\":1,\"qid\":\"Q1;drop\",\"summary\":\"no\"}";
        const char *newer = "{\"v\":1,\"qid\":\"Q1\",\"summary\":\"replaced\",\"photo\":\"image/jpeg\"}";
        unsigned char jpg4[4] = {0xff, 0xd8, 0xff, 0x00};
        if (!leg || !leg_people || mkdir(leg_people, 0700) != 0) {
            fails++;
        } else {
            fetch_sha256_hex("aristotle", 9, hex);
            CHECK(snprintf(base, sizeof base, "%s.json", hex) > 0);
            leg_json = path_join(leg_people, base);
            CHECK(snprintf(base, sizeof base, "%s.img", hex) > 0);
            leg_img = path_join(leg_people, base);
            fetch_sha256_hex("homer", 5, hex);
            CHECK(snprintf(base, sizeof base, "%s.json", hex) > 0);
            bad_json = path_join(leg_people, base);
            CHECK(leg_json && leg_img && bad_json);
            CHECK(write_file(leg_json, kept, strlen(kept)) == 0);
            CHECK(write_file(leg_img, jpg4, sizeof jpg4) == 0);
            CHECK(write_file(bad_json, bad, strlen(bad)) == 0);
            persona_cache_adopt(leg);
            CHECK(access(leg_json, F_OK) != 0);
            CHECK(access(leg_img, F_OK) != 0);
            CHECK(access(bad_json, F_OK) == 0);
            CHECK(persona_profile_load(leg, "  Aristotle ", &loaded, &e) == 0);
            CHECK(loaded.state == PERSONA_READY);
            CHECK(loaded.qid && strcmp(loaded.qid, "Q868") == 0);
            CHECK(loaded.summary && strcmp(loaded.summary, "from the file cache") == 0);
            CHECK(loaded.wikipedia && strcmp(loaded.wikipedia, "https://en.wikipedia.org/wiki/Aristotle") == 0);
            CHECK(loaded.alias_n == 1 && loaded.aliases && strcmp(loaded.aliases[0], "Aristoteles") == 0);
            CHECK(loaded.photo_n == 4 && loaded.photo && memcmp(loaded.photo, jpg4, 4) == 0);
            CHECK(loaded.work_n == 1 && loaded.works && loaded.works[0].url &&
                  strcmp(loaded.works[0].url, "https://en.wikipedia.org/wiki/Poetics_(Aristotle)") == 0);
            persona_profile_free(&loaded);
            CHECK(persona_profile_load(leg, "Homer", &loaded, &e) == 0);
            CHECK(loaded.state == PERSONA_NONE);
            persona_profile_free(&loaded);
            CHECK(write_file(leg_json, newer, strlen(newer)) == 0);
            CHECK(write_file(leg_img, jpg4, sizeof jpg4) == 0);
            persona_cache_adopt(leg);
            CHECK(access(leg_json, F_OK) == 0);
            CHECK(persona_profile_load(leg, "Aristotle", &loaded, &e) == 0);
            CHECK(loaded.qid && strcmp(loaded.qid, "Q868") == 0);
            CHECK(loaded.summary && strcmp(loaded.summary, "from the file cache") == 0);
            CHECK(loaded.photo_n == 4);
            persona_profile_free(&loaded);
            CHECK(access(bad_json, F_OK) == 0);
        }
        free(leg_json);
        free(leg_img);
        free(bad_json);
        free(leg_people);
        rm_rf(leg);
    }
done:
    persona_profile_free(&made);
    persona_profile_free(&hippo);
    persona_profile_free(&loaded);
    persona_profile_free(&p);
    free(url);
    free(json);
    free(marker);
    free(secret);
    free(people);
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

static void test_alias_root(void) {
#if !defined(_WIN32)
    char real_tmpl[] = "/tmp/grove-real-XXXXXX";
    char via_tmpl[] = "/tmp/grove-via-XXXXXX";
    char *real = mkdtemp(real_tmpl);
    char *via = mkdtemp(via_tmpl);
    char *docs = real ? path_join(real, "docs") : NULL;
    char *note = docs ? path_join(docs, "a.txt") : NULL;
    char *alias = via ? path_join(via, "tree") : NULL;
    char *docs_alias = alias ? path_join(alias, "docs") : NULL;
    char *note_alias = docs_alias ? path_join(docs_alias, "a.txt") : NULL;
    char *idx = real ? path_join(real, "idx") : NULL;
    char *docs2 = NULL;
    Index *ix = NULL;
    Library lib;
    Err e;
    memset(&lib, 0, sizeof lib);
    err_clear(&e);
    if (!real || !via || !docs || !note || !alias || !docs_alias || !note_alias || !idx || mkdir(docs, 0700) != 0 ||
        symlink(real, alias) != 0 || write_file(note, "alias shelf\n", 12) != 0) {
        fprintf(stderr, "alias corpus\n");
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    if (!ix) {
        fprintf(stderr, "alias open: %s\n", e.msg);
        fails++;
        goto done;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (index_add(ix, docs_alias, &st, &e) != 0) {
        fprintf(stderr, "alias add: %s\n", e.msg);
        fails++;
        goto done;
    }
    put_sorted(ix, note_alias, "Ann", "", "", "", "", "", "");
    CHECK(index_library_sorted(ix, NULL, NULL, NULL, NULL, NULL, NULL, LIB_SORT_CREATOR, 0, &lib, &e) == 0);
    CHECK(lib.n == 1 && lib.volumes && lib.volumes[0].creator && strcmp(lib.volumes[0].creator, "Ann") == 0);
    CHECK(lib.n == 1 && lib.volumes && lib.volumes[0].path && strstr(lib.volumes[0].path, "/a.txt"));
    library_free(&lib);
    docs2 = path_join(real, "docs2");
    CHECK(docs2 && rename(docs, docs2) == 0);
    err_clear(&e);
    CHECK(index_add(ix, docs_alias, NULL, &e) != 0);
    CHECK(e.msg[0] && strstr(e.msg, "rebind") != NULL);
    err_clear(&e);
    CHECK(docs2 && index_rebind(ix, docs_alias, docs2, &e) == 0);
done:
    library_free(&lib);
    if (ix) {
        index_close(ix);
    }
    free(docs);
    free(note);
    free(alias);
    free(docs_alias);
    free(note_alias);
    free(idx);
    free(docs2);
    rm_rf(real);
    rm_rf(via);
#else
    (void)0;
#endif
}

static void test_catchup(void) {
    char tmpl[] = "/tmp/grove-catch-XXXXXX";
    char *root = mkdtemp(tmpl);
    char *docs = NULL;
    char *idx = NULL;
    char *a = NULL;
    char *b = NULL;
    char *c = NULL;
    char *side = NULL;
    char *chosen = NULL;
    char *sib = NULL;
    Index *ix = NULL;
    Err e;
    err_clear(&e);
    if (!root) {
        perror("mkdtemp");
        fails++;
        return;
    }
    docs = path_join(root, "docs");
    idx = path_join(root, "idx");
    a = path_join(docs, "a.txt");
    b = path_join(docs, "b.txt");
    c = path_join(docs, "c.txt");
    side = path_join(root, "side");
    chosen = path_join(side, "chosen.txt");
    sib = path_join(side, "sibling.txt");
    if (!docs || !idx || !a || !b || !c || !side || !chosen || !sib || mkdir(docs, 0755) != 0 || mkdir(side, 0755) != 0 ||
        write_file(a, "alpha needle\n", 12) != 0 || write_file(b, "beta needle\n", 11) != 0 ||
        write_file(chosen, "chosen needle\n", 14) != 0 || write_file(sib, "sibling needle\n", 15) != 0) {
        fprintf(stderr, "catchup setup failed\n");
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    IndexStats st;
    memset(&st, 0, sizeof st);
    if (!ix || index_add(ix, docs, &st, &e) != 0 || index_add(ix, chosen, &st, &e) != 0) {
        fprintf(stderr, "catchup index: %s\n", e.msg);
        fails++;
        goto done;
    }
    index_close(ix);
    ix = NULL;
    if (write_file(a, "alpha changedneedle\n", 19) != 0 || unlink(b) != 0 || write_file(c, "gamma newfile\n", 14) != 0 ||
        write_file(chosen, "chosen changedneedle\n", 21) != 0) {
        fprintf(stderr, "catchup rewrite failed\n");
        fails++;
        goto done;
    }
    ix = index_open(idx, 1, &e);
    memset(&st, 0, sizeof st);
    if (!ix || index_catchup(ix, &st, &e) != 0) {
        fprintf(stderr, "catchup: %s\n", e.msg);
        fails++;
        goto done;
    }
    int n = 0;
    Hit *h = do_search(ix, "changedneedle", NULL, &n);
    expect_hit(h, n, "/a.txt", "catchup edit");
    expect_hit(h, n, "/chosen.txt", "catchup chosen");
    hits_free(h, n);
    h = do_search(ix, "newfile", NULL, &n);
    expect_hit(h, n, "/c.txt", "catchup new");
    hits_free(h, n);
    h = do_search(ix, "beta", NULL, &n);
    expect_miss(h, n, "/b.txt", "catchup gone");
    hits_free(h, n);
    h = do_search(ix, "sibling", NULL, &n);
    expect_miss(h, n, "/sibling.txt", "catchup sibling");
    hits_free(h, n);
done:
    index_close(ix);
    free(docs);
    free(idx);
    free(a);
    free(b);
    free(c);
    free(side);
    free(chosen);
    free(sib);
    if (root) {
        rm_rf(root);
    }
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
    test_excerpts();
    test_library();
    test_named_creator();
    test_personas();
    test_persona_profile();
    test_library_sort();
    test_alias_root();
    test_narrow();
    test_alerts();
    test_catchup();
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
