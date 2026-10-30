#include "pdf.h"

#include "text.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { OUTLINE_SEEN_MAX = 128, OUTLINE_CAP = 256 };

static int u_accum(unsigned *v, unsigned digit) {
    if (*v > UINT_MAX / 10 || (*v == UINT_MAX / 10 && digit > UINT_MAX % 10)) {
        return -1;
    }
    *v = *v * 10u + digit;
    return 0;
}

/* Windows-1252 for bytes 0x80..0x9F. 0xA0..0xFF match Unicode. */
static const uint16_t CP1252_80[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178};

typedef struct CmapEnt {
    uint32_t src;
    char *dst;
    size_t dstlen;
} CmapEnt;

typedef struct Cmap {
    CmapEnt *e;
    size_t n, cap;
} Cmap;

static void cmap_free(Cmap *c) {
    for (size_t i = 0; i < c->n; i++) {
        free(c->e[i].dst);
    }
    free(c->e);
    c->e = NULL;
    c->n = c->cap = 0;
}

static int cmap_cmp(const void *a, const void *b) {
    const CmapEnt *x = a;
    const CmapEnt *y = b;
    if (x->src < y->src) {
        return -1;
    }
    if (x->src > y->src) {
        return 1;
    }
    return 0;
}

static int cmap_add(Cmap *c, uint32_t src, const char *dst, size_t n) {
    if (c->n == c->cap) {
        size_t cap;
        if (cap_grow_size(c->cap, 32, &cap) != 0 || grow_alloc((void **)&c->e, cap, sizeof(CmapEnt)) != 0) {
            return -1;
        }
        c->cap = cap;
    }
    char *s = xstrndup(dst, n);
    if (!s) {
        return -1;
    }
    c->e[c->n].src = src;
    c->e[c->n].dst = s;
    c->e[c->n].dstlen = n;
    c->n++;
    return 0;
}

static CmapEnt *cmap_find(Cmap *c, uint32_t src) {
    size_t lo = 0, hi = c->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->e[mid].src < src) {
            lo = mid + 1;
        } else if (c->e[mid].src > src) {
            hi = mid;
        } else {
            return &c->e[mid];
        }
    }
    return NULL;
}

static int hex_nibble(int c) {
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

static int read_hex_string(const unsigned char *p, const unsigned char *end, Buf *out, const unsigned char **next) {
    if (p >= end || *p != '<') {
        return -1;
    }
    p++;
    buf_clear(out);
    int hi = -1;
    while (p < end && *p != '>') {
        int v = hex_nibble(*p);
        if (v >= 0) {
            if (hi < 0) {
                hi = v;
            } else {
                if (buf_putc(out, (char)((hi << 4) | v)) != 0) {
                    return -1;
                }
                hi = -1;
            }
        }
        p++;
    }
    if (p >= end || *p != '>') {
        return -1;
    }
    if (hi >= 0) {
        if (buf_putc(out, (char)(hi << 4)) != 0) {
            return -1;
        }
    }
    *next = p + 1;
    return 0;
}

static void append_cp(Buf *out, uint32_t cp) {
    /* U+0000 would cut the text at the first stored C string. */
    if (cp == 0 || cp > 0x10FFFF) {
        return;
    }
    char enc[4];
    int k = utf8_encode(cp, enc);
    if (k <= 0) {
        return;
    }
    buf_append(out, enc, (size_t)k);
}

static void append_winansi(Buf *out, const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == 0) {
            continue;
        }
        if (c < 0x80) {
            buf_putc(out, (char)c);
        } else if (c < 0xa0) {
            append_cp(out, CP1252_80[c - 0x80]);
        } else {
            append_cp(out, c);
        }
    }
}

static void append_utf16be(Buf *out, const unsigned char *s, size_t n) {
    size_t i = 0;
    if (n >= 2 && s[0] == 0xfe && s[1] == 0xff) {
        i = 2;
    }
    while (i + 1 < n) {
        uint32_t u = ((uint32_t)s[i] << 8) | s[i + 1];
        i += 2;
        if (u >= 0xd800 && u <= 0xdbff && i + 1 < n) {
            uint32_t u2 = ((uint32_t)s[i] << 8) | s[i + 1];
            if (u2 >= 0xdc00 && u2 <= 0xdfff) {
                u = 0x10000 + (((u - 0xd800) << 10) | (u2 - 0xdc00));
                i += 2;
            }
        }
        append_cp(out, u);
    }
}

static uint32_t load_be(const unsigned char *s, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && i < 4; i++) {
        v = (v << 8) | s[i];
    }
    return v;
}

static int decode_mapped(Cmap *cmap, const unsigned char *s, size_t n, int width, Buf *out) {
    if (width <= 0 || (n % (size_t)width) != 0) {
        return 0;
    }
    size_t hits = 0;
    size_t codes = n / (size_t)width;
    for (size_t i = 0; i < n; i += (size_t)width) {
        if (cmap_find(cmap, load_be(s + i, (size_t)width))) {
            hits++;
        }
    }
    if (codes == 0 || hits * 2 < codes) {
        return 0;
    }
    for (size_t i = 0; i < n; i += (size_t)width) {
        CmapEnt *e = cmap_find(cmap, load_be(s + i, (size_t)width));
        if (e) {
            buf_append(out, e->dst, e->dstlen);
        }
    }
    return 1;
}

static void decode_pdf_string(Cmap *cmap, const unsigned char *s, size_t n, Buf *out) {
    if (n >= 2 && s[0] == 0xfe && s[1] == 0xff) {
        append_utf16be(out, s, n);
        return;
    }
    int printable = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127)) {
            printable++;
        }
    }
    if (n == 0 || printable * 5 >= (int)n * 4) {
        append_winansi(out, s, n);
        return;
    }
    if (cmap && cmap->n) {
        if (decode_mapped(cmap, s, n, 2, out)) {
            return;
        }
        if (decode_mapped(cmap, s, n, 1, out)) {
            return;
        }
    }
    append_winansi(out, s, n);
}

static int read_lit(const unsigned char *p, const unsigned char *end, Buf *raw, const unsigned char **next) {
    if (p >= end || *p != '(') {
        return -1;
    }
    p++;
    buf_clear(raw);
    int depth = 1;
    while (p < end && depth) {
        unsigned char c = *p++;
        if (c == '\\' && p < end) {
            unsigned char e = *p++;
            if (e == 'n') {
                c = '\n';
            } else if (e == 'r') {
                c = '\r';
            } else if (e == 't') {
                c = '\t';
            } else if (e == 'b') {
                c = '\b';
            } else if (e == 'f') {
                c = '\f';
            } else if (e >= '0' && e <= '7') {
                int v = e - '0';
                for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; k++) {
                    v = (v << 3) | (*p++ - '0');
                }
                c = (unsigned char)v;
            } else {
                c = e;
            }
            if (buf_putc(raw, (char)c) != 0) {
                return -1;
            }
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
        }
        if (buf_putc(raw, (char)c) != 0) {
            return -1;
        }
    }
    *next = p;
    return 0;
}

static int cmap_put_range(Cmap *c, uint32_t src, uint32_t dst_cp) {
    char enc[4];
    int k = utf8_encode(dst_cp, enc);
    return cmap_add(c, src, enc, (size_t)k);
}

static void parse_cmap(const unsigned char *s, size_t n, Cmap *c) {
    const char *bf = "beginbfchar";
    const char *br = "beginbfrange";
    size_t i = 0;
    while (i + 11 < n) {
        if (i + 11 <= n && memcmp(s + i, bf, 11) == 0) {
            i += 11;
            while (i < n && memcmp(s + i, "endbfchar", 9) != 0) {
                while (i < n && s[i] != '<' && !(i + 9 <= n && memcmp(s + i, "endbfchar", 9) == 0)) {
                    i++;
                }
                if (i + 9 <= n && memcmp(s + i, "endbfchar", 9) == 0) {
                    break;
                }
                if (i >= n || s[i] != '<') {
                    break;
                }
                size_t hs = ++i;
                while (i < n && s[i] != '>') {
                    i++;
                }
                Buf srcb, dstb;
                buf_init(&srcb);
                buf_init(&dstb);
                const unsigned char *np = NULL;
                /* reparse with surrounding brackets */
                if (hs > 0 && read_hex_string(s + hs - 1, s + n, &srcb, &np) == 0) {
                    i = (size_t)(np - s);
                    while (i < n && s[i] != '<') {
                        i++;
                    }
                    if (i < n && read_hex_string(s + i, s + n, &dstb, &np) == 0) {
                        i = (size_t)(np - s);
                        uint32_t src = load_be((unsigned char *)srcb.data, srcb.len);
                        Buf u8;
                        buf_init(&u8);
                        append_utf16be(&u8, (unsigned char *)dstb.data, dstb.len);
                        if (u8.len) {
                            cmap_add(c, src, u8.data, u8.len);
                        }
                        buf_free(&u8);
                    }
                }
                buf_free(&srcb);
                buf_free(&dstb);
            }
            continue;
        }
        if (i + 12 <= n && memcmp(s + i, br, 12) == 0) {
            i += 12;
            while (i < n && memcmp(s + i, "endbfrange", 11) != 0) {
                while (i < n && s[i] != '<' && !(i + 11 <= n && memcmp(s + i, "endbfrange", 11) == 0)) {
                    i++;
                }
                if (i + 11 <= n && memcmp(s + i, "endbfrange", 11) == 0) {
                    break;
                }
                Buf a, b, d;
                buf_init(&a);
                buf_init(&b);
                buf_init(&d);
                const unsigned char *np = NULL;
                if (read_hex_string(s + i, s + n, &a, &np) != 0) {
                    buf_free(&a);
                    buf_free(&b);
                    buf_free(&d);
                    break;
                }
                i = (size_t)(np - s);
                while (i < n && s[i] != '<') {
                    i++;
                }
                if (i >= n || read_hex_string(s + i, s + n, &b, &np) != 0) {
                    buf_free(&a);
                    buf_free(&b);
                    buf_free(&d);
                    break;
                }
                i = (size_t)(np - s);
                while (i < n && s[i] != '<' && s[i] != '[') {
                    i++;
                }
                uint32_t src0 = load_be((unsigned char *)a.data, a.len);
                uint32_t src1 = load_be((unsigned char *)b.data, b.len);
                if (src1 < src0 || src1 - src0 > 512) {
                    buf_free(&a);
                    buf_free(&b);
                    buf_free(&d);
                    continue;
                }
                if (i < n && s[i] == '[') {
                    i++;
                    for (uint32_t src = src0; src <= src1 && i < n && s[i] != ']'; src++) {
                        while (i < n && s[i] != '<' && s[i] != ']') {
                            i++;
                        }
                        if (i >= n || s[i] != '<') {
                            break;
                        }
                        buf_clear(&d);
                        if (read_hex_string(s + i, s + n, &d, &np) != 0) {
                            break;
                        }
                        i = (size_t)(np - s);
                        Buf u8;
                        buf_init(&u8);
                        append_utf16be(&u8, (unsigned char *)d.data, d.len);
                        if (u8.len) {
                            cmap_add(c, src, u8.data, u8.len);
                        }
                        buf_free(&u8);
                    }
                } else if (i < n && s[i] == '<') {
                    if (read_hex_string(s + i, s + n, &d, &np) == 0) {
                        i = (size_t)(np - s);
                        uint32_t cp = 0;
                        if (d.len >= 2) {
                            cp = ((unsigned char)d.data[0] << 8) | (unsigned char)d.data[1];
                        } else if (d.len == 1) {
                            cp = (unsigned char)d.data[0];
                        }
                        for (uint32_t src = src0; src <= src1; src++) {
                            cmap_put_range(c, src, cp + (src - src0));
                        }
                    }
                }
                buf_free(&a);
                buf_free(&b);
                buf_free(&d);
            }
            continue;
        }
        i++;
    }
    if (c->n > 1) {
        qsort(c->e, c->n, sizeof(CmapEnt), cmap_cmp);
        /* drop duplicate src, keep the first after sort is unstable — compact equals */
        size_t w = 0;
        for (size_t r = 0; r < c->n; r++) {
            if (w && c->e[w - 1].src == c->e[r].src) {
                free(c->e[r].dst);
                continue;
            }
            c->e[w++] = c->e[r];
        }
        c->n = w;
    }
}

static int inflate_zlib(const unsigned char *src, size_t n, unsigned char **out, size_t *outn) {
    if (n > (size_t)UINT_MAX) {
        return -1;
    }
    size_t cap;
    if (size_mul(n, 4, &cap) != 0 || size_add(cap, 64, &cap) != 0 || cap > 32u * 1024u * 1024u) {
        cap = 32u * 1024u * 1024u;
    }
    if (cap < 256) {
        cap = 256;
    }
    unsigned char *dst = malloc(cap);
    if (!dst) {
        return -1;
    }
    z_stream strm;
    memset(&strm, 0, sizeof strm);
    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)n;
    if (inflateInit(&strm) != Z_OK) {
        free(dst);
        return -1;
    }
    int rc;
    for (;;) {
        if (strm.total_out >= cap || cap - (size_t)strm.total_out < 64) {
            if (cap >= 32u * 1024u * 1024u) {
                inflateEnd(&strm);
                free(dst);
                return -1;
            }
            size_t ncap;
            if (size_mul(cap, 2, &ncap) != 0) {
                inflateEnd(&strm);
                free(dst);
                return -1;
            }
            if (ncap > 32u * 1024u * 1024u) {
                ncap = 32u * 1024u * 1024u;
            }
            unsigned char *nd = realloc(dst, ncap);
            if (!nd) {
                inflateEnd(&strm);
                free(dst);
                return -1;
            }
            dst = nd;
            cap = ncap;
        }
        strm.next_out = dst + strm.total_out;
        strm.avail_out = (uInt)(cap - strm.total_out);
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK) {
            inflateEnd(&strm);
            free(dst);
            return -1;
        }
    }
    inflateEnd(&strm);
    *out = dst;
    *outn = strm.total_out;
    return 0;
}

static void take_title(const unsigned char *s, size_t n, char **title) {
    if (*title) {
        return;
    }
    const char *key = "/Title";
    for (size_t i = 0; i + 6 < n; i++) {
        if (memcmp(s + i, key, 6) != 0) {
            continue;
        }
        size_t j = i + 6;
        while (j < n && (s[j] == ' ' || s[j] == '\n' || s[j] == '\r' || s[j] == '\t')) {
            j++;
        }
        Buf raw;
        buf_init(&raw);
        const unsigned char *np = NULL;
        if (j < n && s[j] == '(' && read_lit(s + j, s + n, &raw, &np) == 0) {
            Buf u;
            buf_init(&u);
            decode_pdf_string(NULL, (unsigned char *)raw.data, raw.len, &u);
            if (u.len) {
                *title = xstrndup(u.data, u.len);
            }
            buf_free(&u);
        } else if (j < n && s[j] == '<' && j + 1 < n && s[j + 1] != '<') {
            if (read_hex_string(s + j, s + n, &raw, &np) == 0 && raw.len) {
                Buf u;
                buf_init(&u);
                decode_pdf_string(NULL, (unsigned char *)raw.data, raw.len, &u);
                if (u.len) {
                    *title = xstrndup(u.data, u.len);
                }
                buf_free(&u);
            }
        }
        buf_free(&raw);
        if (*title) {
            return;
        }
    }
}

static int is_kw(const unsigned char *p, const unsigned char *end, const char *kw) {
    size_t n = strlen(kw);
    if ((size_t)(end - p) < n || memcmp(p, kw, n) != 0) {
        return 0;
    }
    if ((size_t)(end - p) == n) {
        return 1;
    }
    unsigned char c = p[n];
    return !isalnum(c);
}

/* One content stream's font resource names, each pointing at that font's map. */
typedef struct FontUse {
    unsigned content;
    unsigned uni_obj;
    char name[32];
} FontUse;

typedef struct UniSlot {
    unsigned uni_obj;
    Cmap cmap;
    int ready;
} UniSlot;

typedef struct FontCtx {
    FontUse *uses;
    int nuses;
    UniSlot *slots;
    int nslots;
    unsigned content;
} FontCtx;

static Cmap *font_cmap(const FontCtx *fonts, const char *name) {
    if (!fonts || !name || !name[0]) {
        return NULL;
    }
    for (int i = 0; i < fonts->nuses; i++) {
        if (fonts->uses[i].content != fonts->content || strcmp(fonts->uses[i].name, name) != 0) {
            continue;
        }
        unsigned uni = fonts->uses[i].uni_obj;
        for (int s = 0; s < fonts->nslots; s++) {
            if (fonts->slots[s].uni_obj == uni) {
                return &fonts->slots[s].cmap;
            }
        }
        return NULL;
    }
    return NULL;
}

static void content_to_text(const unsigned char *s, size_t n, const FontCtx *fonts, Buf *text) {
    const unsigned char *p = s;
    const unsigned char *end = s + n;
    Buf raw;
    buf_init(&raw);
    Buf pending;
    buf_init(&pending);
    int pend_space = 0;
    int depth = 0;
    int have_pending = 0;
    Cmap *cur = NULL;
    if (fonts && fonts->nuses == 0 && fonts->nslots == 1) {
        cur = &fonts->slots[0].cmap;
    }
    Cmap *stack[32];
    int sp = 0;
    char fontname[32];
    int have_font = 0;
    while (p < end) {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') {
            p++;
            continue;
        }
        if (c == '%') {
            while (p < end && *p != '\n' && *p != '\r') {
                p++;
            }
            continue;
        }
        if (c == '(') {
            if (read_lit(p, end, &raw, &p) != 0) {
                break;
            }
            Buf dec;
            buf_init(&dec);
            decode_pdf_string(cur, (unsigned char *)raw.data, raw.len, &dec);
            if (pend_space && pending.len) {
                buf_putc(&pending, ' ');
            }
            buf_append(&pending, dec.data, dec.len);
            buf_free(&dec);
            pend_space = 0;
            have_pending = 1;
            continue;
        }
        if (c == '<' && p + 1 < end && p[1] == '<') {
            p += 2;
            continue;
        }
        if (c == '<' ) {
            if (read_hex_string(p, end, &raw, &p) != 0) {
                p++;
                continue;
            }
            Buf dec;
            buf_init(&dec);
            decode_pdf_string(cur, (unsigned char *)raw.data, raw.len, &dec);
            if (pend_space && pending.len) {
                buf_putc(&pending, ' ');
            }
            buf_append(&pending, dec.data, dec.len);
            buf_free(&dec);
            pend_space = 0;
            have_pending = 1;
            continue;
        }
        if (c == '[') {
            depth++;
            p++;
            continue;
        }
        if (c == ']') {
            if (depth) {
                depth--;
            }
            p++;
            continue;
        }
        if (c == '/') {
            p++;
            const unsigned char *ns = p;
            while (p < end && !isspace(*p) && *p != '[' && *p != ']' && *p != '(' && *p != '<' && *p != '>' &&
                   *p != '/') {
                p++;
            }
            size_t nlen = (size_t)(p - ns);
            have_font = 0;
            if (nlen > 0 && nlen < sizeof fontname) {
                memcpy(fontname, ns, nlen);
                fontname[nlen] = 0;
                have_font = 1;
            }
            continue;
        }
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            const unsigned char *ns = p;
            if (*p == '+' || *p == '-') {
                p++;
            }
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.')) {
                p++;
            }
            if (depth) {
                char tmp[64];
                size_t ln = (size_t)(p - ns);
                /* A positioning number that does not fit is not a kern. */
                if (ln > 0 && ln < sizeof tmp) {
                    memcpy(tmp, ns, ln);
                    tmp[ln] = 0;
                    errno = 0;
                    char *conv = NULL;
                    double v = strtod(tmp, &conv);
                    if (conv && conv != tmp && *conv == 0 && errno == 0 && v <= -120.0) {
                        pend_space = 1;
                    }
                }
            }
            continue;
        }
        if (is_kw(p, end, "Tj") || is_kw(p, end, "TJ") || is_kw(p, end, "'") || is_kw(p, end, "\"")) {
            int nl = (p[0] == '\'');
            if (have_pending && pending.len) {
                if (text->len && text->data[text->len - 1] != '\n') {
                    if (nl) {
                        buf_putc(text, '\n');
                    } else if (!isspace((unsigned char)text->data[text->len - 1])) {
                        buf_putc(text, ' ');
                    }
                }
                buf_append(text, pending.data, pending.len);
            }
            buf_clear(&pending);
            have_pending = 0;
            pend_space = 0;
            p += (p[0] == '"' || p[0] == '\'') ? 1 : (p[1] == 'J' || p[1] == 'j' ? 2 : 2);
            if (nl) {
                buf_putc(text, '\n');
            }
            continue;
        }
        if (is_kw(p, end, "T*") || is_kw(p, end, "Td") || is_kw(p, end, "TD")) {
            if (text->len && text->data[text->len - 1] != '\n') {
                buf_putc(text, '\n');
            }
            p += (p[0] == 'T' && p[1] == '*') ? 2 : 2;
            continue;
        }
        if (is_kw(p, end, "Tf")) {
            cur = have_font ? font_cmap(fonts, fontname) : NULL;
            have_font = 0;
            p += 2;
            continue;
        }
        if (is_kw(p, end, "q")) {
            if (sp < 32) {
                stack[sp++] = cur;
            }
            p++;
            continue;
        }
        if (is_kw(p, end, "Q")) {
            if (sp > 0) {
                cur = stack[--sp];
            }
            have_font = 0;
            p++;
            continue;
        }
        p++;
    }
    if (have_pending && pending.len) {
        if (text->len && !isspace((unsigned char)text->data[text->len - 1])) {
            buf_putc(text, ' ');
        }
        buf_append(text, pending.data, pending.len);
    }
    buf_free(&raw);
    buf_free(&pending);
}

static unsigned obj_num_before(const unsigned char *b, const unsigned char *at);
static int collect_font_uses(const unsigned char *b, size_t n, FontUse **uses, int *nuses, UniSlot **slots,
                             int *nslots);

static int window_has(const unsigned char *s, size_t n, size_t at, const char *lit) {
    size_t from = at > 400 ? at - 400 : 0;
    size_t ln = strlen(lit);
    for (size_t i = from; i + ln <= at && i + ln <= n; i++) {
        if (memcmp(s + i, lit, ln) == 0) {
            return 1;
        }
    }
    return 0;
}

int pdf_extract(const unsigned char *bytes, size_t n, Buf *text, char **title, Err *e) {
    (void)e;
    *title = NULL;
    buf_clear(text);
    if (n < 5 || memcmp(bytes, "%PDF-", 5) != 0) {
        err_set(e, "not a PDF");
        return -1;
    }
    FontUse *uses = NULL;
    UniSlot *slots = NULL;
    int nuses = 0;
    int nslots = 0;
    if (collect_font_uses(bytes, n, &uses, &nuses, &slots, &nslots) != 0) {
        nuses = 0;
        nslots = 0;
    }
    /* No font dictionary: one map for the whole file, as a bare ToUnicode stream. */
    UniSlot orphan;
    memset(&orphan, 0, sizeof orphan);
    take_title(bytes, n, title);
    /* Two passes: first learn each font's ToUnicode map, then pull text. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n; i++) {
            size_t word = 0;
            if (size_add(i, 6, &word) != 0 || word > n) {
                break;
            }
            if (memcmp(bytes + i, "stream", 6) != 0) {
                continue;
            }
            /* InDesign writes >>stream with no space. The keyword still has to
             * end at CR, LF, or the buffer, so streaming is not a stream. */
            int boundary = (i == 0) || isspace(bytes[i - 1]);
            if (!boundary && i >= 2 && bytes[i - 2] == '>' && bytes[i - 1] == '>') {
                boundary = 1;
            }
            if (!boundary) {
                continue;
            }
            if (word < n && bytes[word] != '\r' && bytes[word] != '\n') {
                continue;
            }
            size_t data = word;
            if (data < n && bytes[data] == '\r') {
                data++;
            }
            if (data < n && bytes[data] == '\n') {
                data++;
            }
            size_t length = 0;
            int have_len = 0;
            size_t from = i > 80 ? i - 80 : 0;
            for (size_t k = from; k + 7 < i; k++) {
                if (memcmp(bytes + k, "/Length", 7) == 0) {
                    size_t j = k + 7;
                    while (j < i && isspace(bytes[j])) {
                        j++;
                    }
                    if (j < i && bytes[j] >= '0' && bytes[j] <= '9') {
                        size_t acc = 0;
                        int over = 0;
                        while (j < i && bytes[j] >= '0' && bytes[j] <= '9') {
                            size_t next = 0;
                            if (size_mul(acc, 10, &next) != 0 ||
                                size_add(next, (size_t)(bytes[j] - '0'), &next) != 0) {
                                over = 1;
                                break;
                            }
                            acc = next;
                            j++;
                        }
                        if (!over) {
                            length = acc;
                            have_len = 1;
                        } else {
                            have_len = 0;
                        }
                    }
                }
            }
            const unsigned char *stream = bytes + data;
            size_t slen = 0;
            if (have_len && size_fits(n, data, length)) {
                slen = length;
            } else {
                const unsigned char *ed = (const unsigned char *)memmem(stream, n - data, "endstream", 9);
                if (!ed) {
                    continue;
                }
                slen = (size_t)(ed - stream);
                while (slen && isspace(stream[slen - 1])) {
                    slen--;
                }
            }
            int flate = window_has(bytes, n, i, "/FlateDecode");
            int skip = window_has(bytes, n, i, "/ASCII85Decode") || window_has(bytes, n, i, "/LZWDecode") ||
                       window_has(bytes, n, i, "/Crypt");
            if (skip) {
                i = data + (slen ? slen - 1 : 0);
                continue;
            }
            unsigned char *dec = NULL;
            size_t dn = 0;
            const unsigned char *use = stream;
            size_t un = slen;
            if (flate) {
                if (inflate_zlib(stream, slen, &dec, &dn) != 0) {
                    continue;
                }
                use = dec;
                un = dn;
            }
            unsigned obj = obj_num_before(bytes, bytes + i);
            if (pass == 0) {
                if (memmem(use, un, "beginbfchar", 11) || memmem(use, un, "beginbfrange", 12)) {
                    int matched = 0;
                    for (int s = 0; s < nslots; s++) {
                        if (slots[s].uni_obj == obj && !slots[s].ready) {
                            parse_cmap(use, un, &slots[s].cmap);
                            slots[s].ready = 1;
                            matched = 1;
                        }
                    }
                    if (!matched && nslots == 0) {
                        parse_cmap(use, un, &orphan.cmap);
                    }
                }
                if (!*title) {
                    take_title(use, un, title);
                }
            } else {
                int is_content = 0;
                if (nuses) {
                    for (int u = 0; u < nuses; u++) {
                        if (uses[u].content == obj) {
                            is_content = 1;
                            break;
                        }
                    }
                }
                /* Page contents only, once a page catalog exists. An image
                 * stream can contain the letters Tj and is not the book. */
                int textual = memmem(use, un, " Tj", 3) || memmem(use, un, " TJ", 3) || memmem(use, un, "Tj", 2) ||
                              memmem(use, un, "TJ", 2);
                if ((!nuses || is_content) && textual) {
                    /* One form feed per text stream after the first. A PDF that
                     * stores one stream per page then has a real page number. */
                    size_t before = text->len;
                    if (text->len) {
                        buf_putc(text, '\f');
                    }
                    size_t marked = text->len;
                    FontCtx ctx;
                    ctx.uses = uses;
                    ctx.nuses = nuses;
                    ctx.slots = slots;
                    ctx.nslots = nslots;
                    ctx.content = obj;
                    const FontCtx *use_fonts = NULL;
                    if (nuses || nslots) {
                        use_fonts = &ctx;
                    } else if (orphan.cmap.n) {
                        ctx.slots = &orphan;
                        ctx.nslots = 1;
                        ctx.nuses = 0;
                        use_fonts = &ctx;
                    }
                    content_to_text(use, un, use_fonts, text);
                    if (text->len == marked && text->data) {
                        text->len = before;
                        text->data[before] = 0;
                    }
                }
            }
            free(dec);
            if (slen) {
                i = data + slen - 1;
            }
        }
    }
    for (int s = 0; s < nslots; s++) {
        cmap_free(&slots[s].cmap);
    }
    cmap_free(&orphan.cmap);
    free(slots);
    free(uses);
    return 0;
}

void pdf_sections_free(PdfSection *s, int n) {
    if (!s) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(s[i].title);
    }
    free(s);
}

static const unsigned char *pdf_obj(const unsigned char *b, size_t n, unsigned num, size_t *len) {
    char pat[32];
    int pn = snprintf(pat, sizeof pat, "%u 0 obj", num);
    if (pn <= 0 || (size_t)pn >= sizeof pat) {
        return NULL;
    }
    const unsigned char *p = b;
    size_t left = n;
    while (left >= (size_t)pn) {
        const unsigned char *f = memmem(p, left, pat, (size_t)pn);
        if (!f) {
            return NULL;
        }
        if (f == b || isspace(f[-1])) {
            const unsigned char *s = f + pn;
            while (s < b + n && isspace(*s)) {
                s++;
            }
            const unsigned char *end = memmem(s, (size_t)(b + n - s), "endobj", 6);
            if (!end) {
                return NULL;
            }
            *len = (size_t)(end - s);
            return s;
        }
        size_t step = (size_t)(f - p) + 1;
        p += step;
        left -= step;
    }
    return NULL;
}

static unsigned ref_after(const unsigned char *s, size_t n, const char *key) {
    size_t kn = strlen(key);
    const unsigned char *p = memmem(s, n, key, kn);
    if (!p) {
        return 0;
    }
    p += kn;
    const unsigned char *end = s + n;
    while (p < end && isspace(*p)) {
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') {
        return 0;
    }
    unsigned v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (u_accum(&v, (unsigned)(*p - '0')) != 0) {
            return 0;
        }
        p++;
    }
    while (p < end && isspace(*p)) {
        p++;
    }
    if (p >= end || *p != '0') {
        return 0;
    }
    return v;
}

static unsigned obj_num_before(const unsigned char *b, const unsigned char *at) {
    size_t span = (size_t)(at - b);
    size_t from = span > 2000 ? span - 2000 : 0;
    unsigned found = 0;
    for (size_t i = from; i + 6 < span; i++) {
        if (i > 0 && !isspace(b[i - 1])) {
            continue;
        }
        if (b[i] < '0' || b[i] > '9') {
            continue;
        }
        unsigned v = 0;
        size_t j = i;
        int over = 0;
        while (j < span && b[j] >= '0' && b[j] <= '9') {
            if (u_accum(&v, (unsigned)(b[j] - '0')) != 0) {
                over = 1;
                break;
            }
            j++;
        }
        if (over || !size_fits(span, j, 6) || b[j] != ' ' || memcmp(b + j, " 0 obj", 6) != 0) {
            continue;
        }
        found = v;
        i = j;
    }
    return found;
}

static int collect_page_ids(const unsigned char *b, size_t n, unsigned **ids, int *nids) {
    *ids = NULL;
    *nids = 0;
    int cap = 0;
    for (size_t i = 0; i + 11 < n; i++) {
        if (memcmp(b + i, "/Type", 5) != 0) {
            continue;
        }
        size_t j = i + 5;
        while (j < n && isspace(b[j])) {
            j++;
        }
        if (j + 5 >= n || memcmp(b + j, "/Page", 5) != 0) {
            continue;
        }
        if (j + 5 < n && b[j + 5] == 's') {
            continue;
        }
        unsigned num = obj_num_before(b, b + i);
        if (!num) {
            continue;
        }
        if (*nids == cap) {
            int nc;
            if (cap_grow(cap, 16, &nc) != 0 || grow_alloc((void **)ids, (size_t)nc, sizeof(unsigned)) != 0) {
                free(*ids);
                *ids = NULL;
                return -1;
            }
            cap = nc;
        }
        (*ids)[(*nids)++] = num;
    }
    return 0;
}

static int pdf_delim(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '(' || c == ')' || c == '<' ||
           c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static unsigned read_ref(const unsigned char **pp, const unsigned char *end) {
    const unsigned char *p = *pp;
    while (p < end && isspace(*p)) {
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') {
        return 0;
    }
    unsigned v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (u_accum(&v, (unsigned)(*p - '0')) != 0) {
            return 0;
        }
        p++;
    }
    while (p < end && isspace(*p)) {
        p++;
    }
    if (p >= end || *p != '0') {
        return 0;
    }
    p++;
    while (p < end && isspace(*p)) {
        p++;
    }
    if (p >= end || (*p != 'R' && *p != 'r')) {
        return 0;
    }
    *pp = p + 1;
    return v;
}

static size_t dict_span(const unsigned char *body, size_t len) {
    for (size_t i = 0; i + 6 <= len; i++) {
        if (memcmp(body + i, "stream", 6) != 0) {
            continue;
        }
        int boundary = (i == 0) || isspace(body[i - 1]) || (i >= 2 && body[i - 2] == '>' && body[i - 1] == '>');
        if (!boundary) {
            continue;
        }
        if (i + 6 < len && body[i + 6] != '\r' && body[i + 6] != '\n') {
            continue;
        }
        return i;
    }
    return len;
}

static int ensure_slot(UniSlot **slots, int *nslots, int *cap, unsigned uni) {
    for (int i = 0; i < *nslots; i++) {
        if ((*slots)[i].uni_obj == uni) {
            return 0;
        }
    }
    if (*nslots == *cap) {
        int nc;
        if (cap_grow(*cap, 8, &nc) != 0 || grow_alloc((void **)slots, (size_t)nc, sizeof(UniSlot)) != 0) {
            return -1;
        }
        *cap = nc;
    }
    memset(&(*slots)[*nslots], 0, sizeof(UniSlot));
    (*slots)[*nslots].uni_obj = uni;
    (*nslots)++;
    return 0;
}

static int push_font_use(FontUse **uses, int *nuses, int *cap, unsigned content, unsigned uni, const char *name) {
    if (!content || !name || !name[0] || strlen(name) >= 32) {
        return 0;
    }
    for (int i = 0; i < *nuses; i++) {
        if ((*uses)[i].content == content && (*uses)[i].uni_obj == uni && strcmp((*uses)[i].name, name) == 0) {
            return 0;
        }
    }
    if (*nuses == *cap) {
        int nc;
        if (cap_grow(*cap, 16, &nc) != 0 || grow_alloc((void **)uses, (size_t)nc, sizeof(FontUse)) != 0) {
            return -1;
        }
        *cap = nc;
    }
    FontUse *row = &(*uses)[*nuses];
    memset(row, 0, sizeof *row);
    row->content = content;
    row->uni_obj = uni;
    memcpy(row->name, name, strlen(name) + 1);
    (*nuses)++;
    return 0;
}

static void clear_font_uses(FontUse **uses, int *nuses, UniSlot **slots, int *nslots) {
    if (slots && *slots) {
        for (int i = 0; i < *nslots; i++) {
            cmap_free(&(*slots)[i].cmap);
        }
    }
    free(*slots);
    free(*uses);
    *slots = NULL;
    *uses = NULL;
    *nslots = 0;
    *nuses = 0;
}

/* A ToUnicode map belongs to one font. One shared map spells a header
 * with another font's letters. */
static int collect_font_uses(const unsigned char *b, size_t n, FontUse **uses, int *nuses, UniSlot **slots,
                             int *nslots) {
    *uses = NULL;
    *nuses = 0;
    *slots = NULL;
    *nslots = 0;
    typedef struct FontLink {
        unsigned font_obj;
        unsigned uni_obj;
    } FontLink;
    FontLink *links = NULL;
    int nlinks = 0;
    int lcap = 0;
    int ucap = 0;
    int ucap_uses = 0;
    for (size_t i = 0; i + 10 < n; i++) {
        if (memcmp(b + i, "/ToUnicode", 10) != 0 || (i + 10 < n && !pdf_delim(b[i + 10]))) {
            continue;
        }
        unsigned font = obj_num_before(b, b + i);
        if (!font) {
            continue;
        }
        size_t len = 0;
        const unsigned char *body = pdf_obj(b, n, font, &len);
        if (!body) {
            continue;
        }
        size_t dictn = dict_span(body, len);
        unsigned uni = ref_after(body, dictn, "/ToUnicode");
        if (!uni) {
            continue;
        }
        int known = 0;
        for (int k = 0; k < nlinks; k++) {
            if (links[k].font_obj == font) {
                known = 1;
                break;
            }
        }
        if (known) {
            continue;
        }
        if (nlinks == lcap) {
            int nc;
            if (cap_grow(lcap, 8, &nc) != 0 || grow_alloc((void **)&links, (size_t)nc, sizeof(FontLink)) != 0) {
                free(links);
                clear_font_uses(uses, nuses, slots, nslots);
                return -1;
            }
            lcap = nc;
        }
        links[nlinks].font_obj = font;
        links[nlinks].uni_obj = uni;
        nlinks++;
        if (ensure_slot(slots, nslots, &ucap, uni) != 0) {
            free(links);
            clear_font_uses(uses, nuses, slots, nslots);
            return -1;
        }
    }
    unsigned *ids = NULL;
    int nids = 0;
    if (collect_page_ids(b, n, &ids, &nids) != 0) {
        free(links);
        clear_font_uses(uses, nuses, slots, nslots);
        return -1;
    }
    for (int p = 0; p < nids; p++) {
        size_t len = 0;
        const unsigned char *body = pdf_obj(b, n, ids[p], &len);
        if (!body) {
            continue;
        }
        size_t dictn = dict_span(body, len);
        unsigned contents[8];
        int nc = 0;
        for (size_t i = 0; i + 10 <= dictn; i++) {
            if (memcmp(body + i, "/Contents", 9) != 0 || !pdf_delim(body[i + 9])) {
                continue;
            }
            const unsigned char *q = body + i + 9;
            const unsigned char *end = body + dictn;
            while (q < end && isspace(*q)) {
                q++;
            }
            int array = q < end && *q == '[';
            if (array) {
                q++;
            }
            while (nc < 8) {
                unsigned v = read_ref(&q, end);
                if (!v) {
                    break;
                }
                contents[nc++] = v;
                if (!array) {
                    break;
                }
            }
            break;
        }
        if (!nc) {
            continue;
        }
        int pushed = 0;
        for (size_t i = 0; i + 6 <= dictn; i++) {
            if (memcmp(body + i, "/Font", 5) != 0 || !pdf_delim(body[i + 5])) {
                continue;
            }
            const unsigned char *q = body + i + 5;
            const unsigned char *end = body + dictn;
            while (q < end && isspace(*q)) {
                q++;
            }
            if (q + 1 >= end || q[0] != '<' || q[1] != '<') {
                continue;
            }
            q += 2;
            while (q < end) {
                while (q < end && isspace(*q)) {
                    q++;
                }
                if (q + 1 < end && q[0] == '>' && q[1] == '>') {
                    break;
                }
                if (q >= end || *q != '/') {
                    break;
                }
                q++;
                const unsigned char *ns = q;
                while (q < end && !pdf_delim(*q)) {
                    q++;
                }
                size_t nlen = (size_t)(q - ns);
                char name[32];
                if (nlen == 0 || nlen >= sizeof name) {
                    break;
                }
                memcpy(name, ns, nlen);
                name[nlen] = 0;
                unsigned font_obj = read_ref(&q, end);
                if (!font_obj) {
                    break;
                }
                unsigned uni = 0;
                for (int k = 0; k < nlinks; k++) {
                    if (links[k].font_obj == font_obj) {
                        uni = links[k].uni_obj;
                        break;
                    }
                }
                if (!uni) {
                    continue;
                }
                for (int c = 0; c < nc; c++) {
                    if (push_font_use(uses, nuses, &ucap_uses, contents[c], uni, name) != 0) {
                        free(ids);
                        free(links);
                        clear_font_uses(uses, nuses, slots, nslots);
                        return -1;
                    }
                    pushed = 1;
                }
            }
            break;
        }
        /* A page with no ToUnicode still has content streams. Naming them
         * keeps an image stream that merely contains Tj out of the text. */
        if (!pushed) {
            for (int c = 0; c < nc; c++) {
                if (push_font_use(uses, nuses, &ucap_uses, contents[c], 0, "*") != 0) {
                    free(ids);
                    free(links);
                    clear_font_uses(uses, nuses, slots, nslots);
                    return -1;
                }
            }
        }
    }
    free(ids);
    free(links);
    return 0;
}

static char *outline_title(const unsigned char *s, size_t n) {
    const unsigned char *p = memmem(s, n, "/Title", 6);
    if (!p) {
        return NULL;
    }
    p += 6;
    const unsigned char *end = s + n;
    while (p < end && isspace(*p)) {
        p++;
    }
    Buf raw;
    buf_init(&raw);
    const unsigned char *np = NULL;
    int rc = -1;
    if (p < end && *p == '(') {
        rc = read_lit(p, end, &raw, &np);
    } else if (p < end && *p == '<' && p + 1 < end && p[1] != '<') {
        rc = read_hex_string(p, end, &raw, &np);
    }
    if (rc != 0) {
        buf_free(&raw);
        return NULL;
    }
    Buf u;
    buf_init(&u);
    decode_pdf_string(NULL, (unsigned char *)raw.data, raw.len, &u);
    buf_free(&raw);
    if (!u.len) {
        buf_free(&u);
        return NULL;
    }
    char *t = xstrndup(u.data, u.len);
    buf_free(&u);
    return t;
}

static unsigned outline_dest_page(const unsigned char *s, size_t n) {
    const unsigned char *p = memmem(s, n, "/Dest", 5);
    if (!p) {
        p = memmem(s, n, "/D", 2);
        if (p && p > s && !isspace(p[-1]) && p[-1] != '/') {
            p = NULL;
        }
    }
    if (!p) {
        return 0;
    }
    const unsigned char *end = s + n;
    const unsigned char *br = memmem(p, (size_t)(end - p), "[", 1);
    if (!br || br > p + 32) {
        return 0;
    }
    br++;
    while (br < end && isspace(*br)) {
        br++;
    }
    if (br >= end || *br < '0' || *br > '9') {
        return 0;
    }
    unsigned v = 0;
    while (br < end && *br >= '0' && *br <= '9') {
        if (u_accum(&v, (unsigned)(*br - '0')) != 0) {
            return 0;
        }
        br++;
    }
    return v;
}

static int page_index(const unsigned *ids, int nids, unsigned obj) {
    for (int i = 0; i < nids; i++) {
        if (ids[i] == obj) {
            return i + 1;
        }
    }
    return 0;
}

static int walk_outline(const unsigned char *b, size_t n, unsigned obj, int level, const unsigned *pages, int npages,
                        unsigned *seen, int *nseen, PdfSection **out, int *nout, int *cap) {
    if (!obj || level > 8 || *nseen >= OUTLINE_SEEN_MAX || *nout >= OUTLINE_CAP) {
        return 0;
    }
    for (int i = 0; i < *nseen; i++) {
        if (seen[i] == obj) {
            return 0;
        }
    }
    seen[(*nseen)++] = obj;
    size_t len = 0;
    const unsigned char *body = pdf_obj(b, n, obj, &len);
    if (!body) {
        return 0;
    }
    char *title = outline_title(body, len);
    int pg = page_index(pages, npages, outline_dest_page(body, len));
    if (title && title[0] && pg > 0) {
        if (*nout == *cap) {
            int nc;
            if (cap_grow(*cap, 8, &nc) != 0 || grow_alloc((void **)out, (size_t)nc, sizeof(PdfSection)) != 0) {
                free(title);
                return -1;
            }
            *cap = nc;
        }
        (*out)[*nout].page = (uint32_t)pg;
        (*out)[*nout].level = (uint8_t)(level > 255 ? 255 : level);
        (*out)[*nout].title = title;
        (*nout)++;
    } else {
        free(title);
    }
    unsigned first = ref_after(body, len, "/First");
    unsigned next = ref_after(body, len, "/Next");
    if (walk_outline(b, n, first, level + 1, pages, npages, seen, nseen, out, nout, cap) != 0) {
        return -1;
    }
    return walk_outline(b, n, next, level, pages, npages, seen, nseen, out, nout, cap);
}

int pdf_sections(const unsigned char *bytes, size_t n, PdfSection **out, int *nout) {
    *out = NULL;
    *nout = 0;
    /* The outlines dictionary is also "/Type /Outlines". Take the reference,
     * which is a number after the name, and keep looking until one appears. */
    unsigned root = 0;
    const unsigned char *p = bytes;
    size_t left = n;
    while (left >= 9) {
        const unsigned char *f = memmem(p, left, "/Outlines", 9);
        if (!f) {
            break;
        }
        const unsigned char *q = f + 9;
        const unsigned char *end = bytes + n;
        while (q < end && isspace(*q)) {
            q++;
        }
        if (q < end && *q >= '0' && *q <= '9') {
            unsigned v = 0;
            int over = 0;
            while (q < end && *q >= '0' && *q <= '9') {
                if (u_accum(&v, (unsigned)(*q - '0')) != 0) {
                    over = 1;
                    break;
                }
                q++;
            }
            if (!over && v) {
                root = v;
                break;
            }
        }
        size_t step = (size_t)(f - p) + 9;
        if (step > left) {
            break;
        }
        p += step;
        left -= step;
    }
    if (!root) {
        return 0;
    }
    size_t len = 0;
    const unsigned char *body = pdf_obj(bytes, n, root, &len);
    if (!body) {
        return 0;
    }
    unsigned *pages = NULL;
    int npages = 0;
    if (collect_page_ids(bytes, n, &pages, &npages) != 0) {
        return -1;
    }
    unsigned seen[OUTLINE_SEEN_MAX];
    int nseen = 0;
    int cap = 0;
    unsigned first = ref_after(body, len, "/First");
    int rc = walk_outline(bytes, n, first, 1, pages, npages, seen, &nseen, out, nout, &cap);
    free(pages);
    if (rc != 0) {
        pdf_sections_free(*out, *nout);
        *out = NULL;
        *nout = 0;
    }
    return rc;
}
