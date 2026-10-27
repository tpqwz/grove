#include "extract.h"

#include "pdf.h"
#include "text.h"
#include "zip.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { MAX_INPUT = 32u * 1024u * 1024u, MAX_TEXT = 8u * 1024u * 1024u };

void extract_free(Extract *x) {
    if (!x) {
        return;
    }
    free(x->text);
    free(x->title);
    free(x->detail);
    marks_free(x->marks, x->nmarks);
    memset(x, 0, sizeof *x);
}

static const char *ext_of(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || dot[1] == 0) {
        return "";
    }
    return dot + 1;
}

static int ext_is(const char *ext, const char *want) {
    return ascii_ieq(ext, want);
}

static int is_text_ext(const char *ext) {
    static const char *names[] = {
        "adoc", "asciidoc", "c",  "cc",  "conf", "cpp", "css", "csv", "go",  "h",   "hh",
        "hpp",  "htm",      "html", "ini", "java", "js",  "json", "log", "markdown", "md",
        "org",  "py",       "rs",  "rst", "sh",  "tex", "text", "toml", "tsv", "txt", "xml",
        "yaml", "yml"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (ext_is(ext, names[i])) {
            return 1;
        }
    }
    return 0;
}

static int has_nul(const unsigned char *s, size_t n) {
    size_t m = n > 8192 ? 8192 : n;
    for (size_t i = 0; i < m; i++) {
        if (s[i] == 0) {
            return 1;
        }
    }
    return 0;
}

/* map[i] is the output offset of input byte i, or UINT32_MAX if that byte was
 * dropped. map[len] is the output length. */
static int collapse_newlines(Buf *b, uint32_t **map_out) {
    uint32_t *map = malloc((b->len + 1) * sizeof(uint32_t));
    Buf o;
    buf_init(&o);
    if (!map) {
        buf_free(&o);
        return -1;
    }
    int nl = 0;
    uint32_t out = 0;
    for (size_t i = 0; i < b->len; i++) {
        char c = b->data[i];
        if (c == '\r') {
            map[i] = UINT32_MAX;
            continue;
        }
        if (c == '\n') {
            nl++;
            if (nl <= 2) {
                map[i] = out++;
                buf_putc(&o, '\n');
            } else {
                map[i] = UINT32_MAX;
            }
            continue;
        }
        nl = 0;
        map[i] = out++;
        buf_putc(&o, c);
    }
    map[b->len] = out;
    free(b->data);
    b->data = o.data;
    b->len = o.len;
    b->cap = o.cap;
    *map_out = map;
    return 0;
}

static int is_trim_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v';
}

static void trim_buf(Buf *b) {
    if (!b->data || b->len == 0) {
        b->len = 0;
        return;
    }
    size_t a = 0;
    while (a < b->len && is_trim_space((unsigned char)b->data[a])) {
        a++;
    }
    size_t z = b->len;
    while (z > a && is_trim_space((unsigned char)b->data[z - 1])) {
        z--;
    }
    size_t n = z - a;
    memmove(b->data, b->data + a, n);
    b->len = n;
    if (b->data) {
        b->data[n] = 0;
    }
}

static int decode_entity(const char *s, size_t n, size_t *used, uint32_t *cp) {
    if (n < 3 || s[0] != '&') {
        return 0;
    }
    size_t semi = 0;
    while (semi < n && semi < 16 && s[semi] != ';') {
        semi++;
    }
    if (semi >= n || s[semi] != ';') {
        return 0;
    }
    *used = semi + 1;
    if (s[1] == '#') {
        unsigned long v = 0;
        if (s[2] == 'x' || s[2] == 'X') {
            v = strtoul(s + 3, NULL, 16);
        } else {
            v = strtoul(s + 2, NULL, 10);
        }
        *cp = (uint32_t)v;
        return 1;
    }
    if (semi == 3 && memcmp(s, "&lt", 3) == 0) {
        *cp = '<';
        return 1;
    }
    if (semi == 3 && memcmp(s, "&gt", 3) == 0) {
        *cp = '>';
        return 1;
    }
    if (semi == 4 && memcmp(s, "&amp", 4) == 0) {
        *cp = '&';
        return 1;
    }
    if (semi == 5 && memcmp(s, "&quot", 5) == 0) {
        *cp = '"';
        return 1;
    }
    if (semi == 5 && memcmp(s, "&apos", 5) == 0) {
        *cp = '\'';
        return 1;
    }
    if (semi == 5 && memcmp(s, "&nbsp", 5) == 0) {
        *cp = ' ';
        return 1;
    }
    *cp = ' ';
    return 1;
}

static int tag_is(const char *name, size_t n, const char *want) {
    const char *local = name;
    size_t ln = n;
    for (size_t i = 0; i < n; i++) {
        if (name[i] == ':') {
            local = name + i + 1;
            ln = n - i - 1;
            break;
        }
    }
    if (ln != strlen(want)) {
        return 0;
    }
    for (size_t i = 0; i < ln; i++) {
        char c = local[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != want[i]) {
            return 0;
        }
    }
    return 1;
}

static int head_level_name(const char *nm, size_t nn) {
    if (nn == 2 && (nm[0] == 'h' || nm[0] == 'H') && nm[1] >= '1' && nm[1] <= '6') {
        return nm[1] - '0';
    }
    return 0;
}

static int tag_has(const char *tag, size_t n, const char *lit) {
    size_t ln = strlen(lit);
    for (size_t i = 0; i + ln <= n; i++) {
        size_t k = 0;
        while (k < ln) {
            char c = tag[i + k];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c + 32);
            }
            if (c != lit[k]) {
                break;
            }
            k++;
        }
        if (k == ln) {
            return 1;
        }
    }
    return 0;
}

static int tag_page_num(const char *tag, size_t n, uint32_t *page) {
    const char *keys[] = {"title=\"", "title='", "id=\"page", "id='page", "id=\"pg", "id='pg"};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        size_t kn = strlen(keys[k]);
        for (size_t i = 0; i + kn < n; i++) {
            size_t j = 0;
            while (j < kn) {
                char c = tag[i + j];
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c + 32);
                }
                if (c != keys[k][j]) {
                    break;
                }
                j++;
            }
            if (j != kn) {
                continue;
            }
            size_t p = i + kn;
            if (p >= n || !isdigit((unsigned char)tag[p])) {
                continue;
            }
            uint32_t v = 0;
            while (p < n && isdigit((unsigned char)tag[p])) {
                v = v * 10u + (uint32_t)(tag[p] - '0');
                p++;
            }
            if (v > 0) {
                *page = v;
                return 1;
            }
        }
    }
    return 0;
}

typedef struct MarkBuf {
    Mark *a;
    int n;
    int cap;
} MarkBuf;

static void mbuf_free(MarkBuf *m) {
    if (!m) {
        return;
    }
    marks_free(m->a, m->n);
    m->a = NULL;
    m->n = m->cap = 0;
}

static int mark_add(MarkBuf *m, uint32_t off, uint8_t kind, uint8_t level, uint32_t page, const char *text,
                    size_t tn) {
    if (m->n >= 10000) {
        return 0;
    }
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 16;
        Mark *p = realloc(m->a, (size_t)nc * sizeof(Mark));
        if (!p) {
            return -1;
        }
        m->a = p;
        m->cap = nc;
    }
    Mark *mk = &m->a[m->n];
    memset(mk, 0, sizeof *mk);
    mk->offset = off;
    mk->kind = kind;
    mk->level = level;
    mk->page = page;
    if (text && tn) {
        if (tn > 160) {
            tn = utf8_prefix_bytes(text, tn, 160);
        }
        mk->text = xstrndup(text, tn);
        if (!mk->text) {
            return -1;
        }
    }
    m->n++;
    return 0;
}

static char *squash_text(const char *s, size_t n) {
    Buf b;
    buf_init(&b);
    int sp = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            if (!sp && buf_putc(&b, ' ') != 0) {
                buf_free(&b);
                return NULL;
            }
            sp = 1;
            continue;
        }
        sp = 0;
        if (buf_putc(&b, (char)c) != 0) {
            buf_free(&b);
            return NULL;
        }
    }
    trim_buf(&b);
    if (!b.len) {
        buf_free(&b);
        return NULL;
    }
    return b.data;
}

static int mark_cmp(const void *A, const void *B) {
    const Mark *a = A;
    const Mark *b = B;
    if (a->offset != b->offset) {
        return a->offset < b->offset ? -1 : 1;
    }
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    return (int)a->level - (int)b->level;
}

static uint32_t snap_off(const uint32_t *map, size_t old_len, uint32_t off) {
    if (off > old_len) {
        off = (uint32_t)old_len;
    }
    while (off < old_len && map[off] == UINT32_MAX) {
        off++;
    }
    return map[off];
}

static int add_ff_pages(MarkBuf *m, const char *text, size_t len, int always) {
    int has = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\f') {
            has = 1;
            break;
        }
    }
    if (!has && !always) {
        return 0;
    }
    if (mark_add(m, 0, MARK_PAGE, 0, 1, NULL, 0) != 0) {
        return -1;
    }
    uint32_t page = 1;
    for (size_t i = 0; i < len; i++) {
        if (text[i] != '\f') {
            continue;
        }
        page++;
        uint32_t at = i + 1 < len ? (uint32_t)(i + 1) : (uint32_t)i;
        if (mark_add(m, at, MARK_PAGE, 0, page, NULL, 0) != 0) {
            return -1;
        }
    }
    return 0;
}


static int html_to_buf(const char *s, size_t n, Buf *text, char **title, MarkBuf *marks) {
    int skip = 0;
    int skip_kind = 0; /* 1 script, 2 style, 3 head-until-title-done? just skip script/style */
    Buf titleb;
    buf_init(&titleb);
    Buf headb;
    buf_init(&headb);
    int in_title = 0;
    int in_head = 0;
    uint32_t head_at = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '<') {
            if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                i += 4;
                while (i + 2 < n && !(s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>')) {
                    i++;
                }
                i += (i + 2 < n) ? 3 : (n - i);
                continue;
            }
            int closing = (i + 1 < n && s[i + 1] == '/');
            size_t j = i + 1 + closing;
            while (j < n && isspace((unsigned char)s[j])) {
                j++;
            }
            size_t nb = j;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == ':' || s[j] == '-')) {
                j++;
            }
            size_t ne = j;
            int self = 0;
            while (j < n && s[j] != '>') {
                if (s[j] == '/' && j + 1 < n && s[j + 1] == '>') {
                    self = 1;
                }
                j++;
            }
            if (j < n && s[j] == '>') {
                j++;
            }
            const char *nm = s + nb;
            size_t nn = ne - nb;
            if (!closing && (tag_is(nm, nn, "script") || tag_is(nm, nn, "style"))) {
                skip = 1;
                skip_kind = tag_is(nm, nn, "script") ? 1 : 2;
            } else if (closing && ((skip_kind == 1 && tag_is(nm, nn, "script")) ||
                                   (skip_kind == 2 && tag_is(nm, nn, "style")))) {
                skip = 0;
                skip_kind = 0;
            } else if (!skip && tag_is(nm, nn, "title")) {
                in_title = !closing && !self;
                if (closing || self) {
                    in_title = 0;
                }
            } else if (!skip && (closing || self) &&
                       (tag_is(nm, nn, "p") || tag_is(nm, nn, "div") || tag_is(nm, nn, "br") ||
                        tag_is(nm, nn, "li") || tag_is(nm, nn, "tr") || tag_is(nm, nn, "h1") ||
                        tag_is(nm, nn, "h2") || tag_is(nm, nn, "h3") || tag_is(nm, nn, "h4") ||
                        tag_is(nm, nn, "h5") || tag_is(nm, nn, "h6") || tag_is(nm, nn, "blockquote") ||
                        tag_is(nm, nn, "section") || tag_is(nm, nn, "article"))) {
                buf_putc(text, '\n');
            } else if (!skip && !closing &&
                       (tag_is(nm, nn, "br") || tag_is(nm, nn, "p") || tag_is(nm, nn, "div") ||
                        tag_is(nm, nn, "li") || tag_is(nm, nn, "h1") || tag_is(nm, nn, "h2") ||
                        tag_is(nm, nn, "h3") || tag_is(nm, nn, "h4") || tag_is(nm, nn, "h5") ||
                        tag_is(nm, nn, "h6"))) {
                buf_putc(text, '\n');
            }
            if (!skip && marks) {
                int hl = head_level_name(nm, nn);
                if (!closing && hl) {
                    in_head = hl;
                    head_at = (uint32_t)text->len;
                    headb.len = 0;
                    if (headb.data) {
                        headb.data[0] = 0;
                    }
                } else if (closing && in_head && hl == in_head) {
                    char *sq = squash_text(headb.data ? headb.data : "", headb.len);
                    if (sq && mark_add(marks, head_at, MARK_HEAD, (uint8_t)in_head, 0, sq, strlen(sq)) != 0) {
                        free(sq);
                        buf_free(&titleb);
                        buf_free(&headb);
                        return -1;
                    }
                    free(sq);
                    in_head = 0;
                }
                uint32_t pg = 0;
                if (!closing && tag_has(s + i, j - i, "pagebreak") && tag_page_num(s + i, j - i, &pg)) {
                    if (mark_add(marks, (uint32_t)text->len, MARK_PAGE, 0, pg, NULL, 0) != 0) {
                        buf_free(&titleb);
                        buf_free(&headb);
                        return -1;
                    }
                }
            }
            i = j;
            continue;
        }
        if (skip) {
            i++;
            continue;
        }
        uint32_t cp = 0;
        size_t used = 0;
        if (s[i] == '&' && decode_entity(s + i, n - i, &used, &cp)) {
            char enc[4];
            int k = utf8_encode(cp ? cp : ' ', enc);
            if (in_title) {
                buf_append(&titleb, enc, (size_t)k);
            } else {
                buf_append(text, enc, (size_t)k);
                if (in_head) {
                    buf_append(&headb, enc, (size_t)k);
                }
            }
            i += used;
            continue;
        }
        if (in_title) {
            buf_putc(&titleb, s[i]);
        } else {
            buf_putc(text, s[i]);
            if (in_head) {
                buf_putc(&headb, s[i]);
            }
        }
        i++;
    }
    if (title && !*title && titleb.len) {
        trim_buf(&titleb);
        *title = titleb.data;
        titleb.data = NULL;
    }
    buf_free(&titleb);
    buf_free(&headb);
    return 0;
}

static void xml_texts(const char *s, size_t n, Buf *text, const char *const *blocks, int nblocks) {
    /* Append character data. Newline after the named end tags. */
    for (size_t i = 0; i < n;) {
        if (s[i] == '<') {
            int closing = (i + 1 < n && s[i + 1] == '/');
            size_t j = i + 1 + closing;
            size_t nb = j;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == ':' || s[j] == '-' || s[j] == '_')) {
                j++;
            }
            int is_block = 0;
            int is_tab = tag_is(s + nb, j - nb, "tab");
            int is_br = tag_is(s + nb, j - nb, "br");
            if (closing) {
                for (int b = 0; b < nblocks; b++) {
                    if (tag_is(s + nb, j - nb, blocks[b])) {
                        is_block = 1;
                        break;
                    }
                }
            }
            while (j < n && s[j] != '>') {
                j++;
            }
            if (j < n) {
                j++;
            }
            if (is_tab) {
                buf_putc(text, ' ');
            } else if (is_br || is_block) {
                buf_putc(text, '\n');
            }
            i = j;
            continue;
        }
        uint32_t cp = 0;
        size_t used = 0;
        if (s[i] == '&' && decode_entity(s + i, n - i, &used, &cp)) {
            char enc[4];
            int k = utf8_encode(cp ? cp : ' ', enc);
            buf_append(text, enc, (size_t)k);
            i += used;
            continue;
        }
        buf_putc(text, s[i]);
        i++;
    }
}

static char *first_tag_text(const char *s, size_t n, const char *local) {
    size_t ln = strlen(local);
    for (size_t i = 0; i + ln + 1 < n; i++) {
        if (s[i] != '<') {
            continue;
        }
        size_t j = i + 1;
        int ok = 0;
        if (j + ln <= n && tag_is(s + j, ln, local) && (j + ln == n || !isalnum((unsigned char)s[j + ln]))) {
            ok = 1;
            j += ln;
        } else {
            while (j < n && s[j] != ':' && s[j] != '>' && s[j] != ' ') {
                j++;
            }
            if (j < n && s[j] == ':' && tag_is(s + j + 1, ln, local)) {
                ok = 1;
                j += 1 + ln;
            }
        }
        if (!ok) {
            continue;
        }
        while (j < n && s[j] != '>') {
            j++;
        }
        if (j >= n) {
            return NULL;
        }
        j++;
        size_t b = j;
        while (j < n && s[j] != '<') {
            j++;
        }
        Buf t;
        buf_init(&t);
        xml_texts(s + b, j - b, &t, NULL, 0);
        trim_buf(&t);
        if (t.len == 0) {
            buf_free(&t);
            continue;
        }
        return t.data;
    }
    return NULL;
}

static char *guess_title_line(const char *text) {
    const char *p = text;
    while (*p) {
        while (*p == '\n' || *p == '\f' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#') {
            while (*p == '#') {
                p++;
            }
            while (*p == ' ') {
                p++;
            }
        }
        const char *b = p;
        while (*p && *p != '\n' && *p != '\f') {
            p++;
        }
        size_t n = (size_t)(p - b);
        while (n && isspace((unsigned char)b[n - 1])) {
            n--;
        }
        if (n) {
            n = utf8_prefix_bytes(b, n, 180);
            return xstrndup(b, n);
        }
    }
    return NULL;
}

/* page_mode 0: leave marks alone. 1: a form feed starts a page. 2: always page 1. */
static ExStatus finish(Buf *text, char *title, const char *detail, Extract *out, MarkBuf *mb, int page_mode) {
    uint32_t *map = NULL;
    size_t old_len = text->len;
    if (collapse_newlines(text, &map) != 0) {
        buf_free(text);
        free(title);
        mbuf_free(mb);
        return EX_ERR;
    }
    size_t lead = 0;
    while (lead < text->len && is_trim_space((unsigned char)text->data[lead])) {
        lead++;
    }
    trim_buf(text);
    if (text->len > MAX_TEXT) {
        text->len = utf8_prefix_bytes(text->data, text->len, MAX_TEXT);
        text->data[text->len] = 0;
    }
    if (mb && mb->n) {
        int w = 0;
        for (int i = 0; i < mb->n; i++) {
            uint32_t at = snap_off(map, old_len, mb->a[i].offset);
            /* A mark on the trimmed opening whitespace still covers the text. */
            if (at < lead) {
                at = 0;
            } else {
                at -= (uint32_t)lead;
            }
            if (at > text->len) {
                free(mb->a[i].text);
                continue;
            }
            mb->a[i].offset = at;
            mb->a[w++] = mb->a[i];
        }
        mb->n = w;
    }
    free(map);
    if (text->len == 0) {
        buf_free(text);
        free(title);
        mbuf_free(mb);
        out->text = NULL;
        out->title = NULL;
        out->detail = detail ? xstrdup(detail) : xstrdup("no text extracted");
        return EX_SKIP;
    }
    MarkBuf local = {0};
    MarkBuf *use = mb ? mb : &local;
    if (page_mode && add_ff_pages(use, text->data, text->len, page_mode == 2) != 0) {
        buf_free(text);
        free(title);
        mbuf_free(use);
        return EX_ERR;
    }
    if (use->n > 1) {
        qsort(use->a, (size_t)use->n, sizeof(Mark), mark_cmp);
    }
    out->text = text->data ? text->data : xstrdup("");
    text->data = NULL;
    out->title = title ? title : guess_title_line(out->text);
    out->detail = NULL;
    out->marks = use->a;
    out->nmarks = use->n;
    use->a = NULL;
    use->n = use->cap = 0;
    return out->text ? EX_OK : EX_ERR;
}

static int attr_quoted(const char *tag, size_t n, const char *key, const char **vb, size_t *vn) {
    size_t kn = strlen(key);
    for (size_t i = 0; i + kn + 2 < n; i++) {
        if (memcmp(tag + i, key, kn) != 0) {
            continue;
        }
        if (i > 0) {
            unsigned char prev = (unsigned char)tag[i - 1];
            if (isalnum(prev) || prev == '-' || prev == '_') {
                continue;
            }
        }
        size_t p = i + kn;
        while (p < n && isspace((unsigned char)tag[p])) {
            p++;
        }
        if (p >= n || tag[p] != '=') {
            continue;
        }
        p++;
        while (p < n && isspace((unsigned char)tag[p])) {
            p++;
        }
        if (p >= n || (tag[p] != '"' && tag[p] != '\'')) {
            continue;
        }
        char q = tag[p++];
        size_t b = p;
        while (p < n && tag[p] != q) {
            p++;
        }
        *vb = tag + b;
        *vn = p - b;
        return 1;
    }
    return 0;
}

static int heading_style_level(const char *v, size_t n) {
    char buf[32];
    if (n == 0 || n >= sizeof buf) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        buf[i] = c;
    }
    buf[n] = 0;
    if (strncmp(buf, "heading", 7) != 0) {
        return 0;
    }
    const char *p = buf + 7;
    if (*p == ' ') {
        p++;
    }
    if (p[0] >= '1' && p[0] <= '6' && p[1] == 0) {
        return p[0] - '0';
    }
    return 0;
}

/* A break at offset 0 starts page 1. A later break starts the next page,
 * and the text before the first break is page 1. */
static int note_page_break(MarkBuf *marks, size_t len, int *page) {
    if (len == 0 && *page == 0) {
        if (mark_add(marks, 0, MARK_PAGE, 0, 1, NULL, 0) != 0) {
            return -1;
        }
        *page = 1;
        return 0;
    }
    if (*page == 0) {
        if (mark_add(marks, 0, MARK_PAGE, 0, 1, NULL, 0) != 0) {
            return -1;
        }
        *page = 1;
    }
    uint32_t next = (uint32_t)*page + 1;
    if (mark_add(marks, (uint32_t)len, MARK_PAGE, 0, next, NULL, 0) != 0) {
        return -1;
    }
    *page = (int)next;
    return 0;
}

static int emit_head(MarkBuf *marks, uint32_t at, int level, Buf *headb) {
    if (level <= 0) {
        return 0;
    }
    char *sq = squash_text(headb->data ? headb->data : "", headb->len);
    if (!sq) {
        return 0;
    }
    int rc = mark_add(marks, at, MARK_HEAD, (uint8_t)level, 0, sq, strlen(sq));
    free(sq);
    return rc;
}

static void take_xml_char(const char *s, size_t n, size_t *i, Buf *text, Buf *also) {
    uint32_t cp = 0;
    size_t used = 0;
    if (s[*i] == '&' && decode_entity(s + *i, n - *i, &used, &cp)) {
        char enc[4];
        int k = utf8_encode(cp ? cp : ' ', enc);
        buf_append(text, enc, (size_t)k);
        if (also) {
            buf_append(also, enc, (size_t)k);
        }
        *i += used;
        return;
    }
    buf_putc(text, s[*i]);
    if (also) {
        buf_putc(also, s[*i]);
    }
    (*i)++;
}

static int is_docx_page_break(const char *nm, size_t nn, const char *raw, size_t rn, int closing) {
    if (closing) {
        return 0;
    }
    if (tag_is(nm, nn, "lastrenderedpagebreak")) {
        return 1;
    }
    if (!tag_is(nm, nn, "br")) {
        return 0;
    }
    return tag_has(raw, rn, "type=\"page\"") || tag_has(raw, rn, "type='page'");
}

static int docx_to_buf(const char *s, size_t n, Buf *text, MarkBuf *marks) {
    Buf headb;
    buf_init(&headb);
    int in_p = 0;
    int heading = 0;
    uint32_t head_at = 0;
    int page = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] != '<') {
            take_xml_char(s, n, &i, text, in_p ? &headb : NULL);
            continue;
        }
        int closing = (i + 1 < n && s[i + 1] == '/');
        size_t j = i + 1 + closing;
        size_t nb = j;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == ':' || s[j] == '-' || s[j] == '_')) {
            j++;
        }
        size_t ne = j;
        size_t raw_end = j;
        while (raw_end < n && s[raw_end] != '>') {
            raw_end++;
        }
        j = raw_end < n ? raw_end + 1 : n;
        const char *nm = s + nb;
        size_t nn = ne > nb ? ne - nb : 0;
        const char *raw = s + i;
        size_t rn = raw_end > i ? raw_end - i : 0;
        if (!closing && tag_is(nm, nn, "p")) {
            if (in_p && emit_head(marks, head_at, heading, &headb) != 0) {
                buf_free(&headb);
                return -1;
            }
            if (in_p) {
                buf_putc(text, '\n');
            }
            in_p = 1;
            heading = 0;
            head_at = (uint32_t)text->len;
            headb.len = 0;
            if (headb.data) {
                headb.data[0] = 0;
            }
        } else if (closing && tag_is(nm, nn, "p")) {
            if (in_p && emit_head(marks, head_at, heading, &headb) != 0) {
                buf_free(&headb);
                return -1;
            }
            in_p = 0;
            heading = 0;
            buf_putc(text, '\n');
        } else if (!closing && tag_is(nm, nn, "pstyle")) {
            const char *vb = NULL;
            size_t vn = 0;
            if (attr_quoted(raw, rn, "val", &vb, &vn)) {
                int lv = heading_style_level(vb, vn);
                if (lv) {
                    heading = lv;
                }
            }
        } else if (is_docx_page_break(nm, nn, raw, rn, closing)) {
            if (note_page_break(marks, text->len, &page) != 0) {
                buf_free(&headb);
                return -1;
            }
            if (text->len > 0) {
                buf_putc(text, '\n');
            }
        } else if (tag_is(nm, nn, "tab")) {
            buf_putc(text, ' ');
            if (in_p) {
                buf_putc(&headb, ' ');
            }
        } else if (tag_is(nm, nn, "br")) {
            buf_putc(text, '\n');
            if (in_p) {
                buf_putc(&headb, '\n');
            }
        }
        i = j;
    }
    if (in_p && emit_head(marks, head_at, heading, &headb) != 0) {
        buf_free(&headb);
        return -1;
    }
    if (in_p) {
        buf_putc(text, '\n');
    }
    buf_free(&headb);
    return 0;
}

static int outline_level_of(const char *raw, size_t rn) {
    const char *vb = NULL;
    size_t vn = 0;
    if (!attr_quoted(raw, rn, "outline-level", &vb, &vn) || vn != 1 || vb[0] < '1' || vb[0] > '6') {
        return 1;
    }
    return vb[0] - '0';
}

static int is_odt_page_break(const char *nm, size_t nn, const char *raw, size_t rn, int closing) {
    if (closing) {
        return 0;
    }
    if (tag_is(nm, nn, "soft-page-break")) {
        return 1;
    }
    return tag_has(raw, rn, "break-before=\"page\"") || tag_has(raw, rn, "break-before='page'");
}

static int odt_to_buf(const char *s, size_t n, Buf *text, MarkBuf *marks) {
    Buf headb;
    buf_init(&headb);
    int in_h = 0;
    uint32_t head_at = 0;
    int page = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] != '<') {
            take_xml_char(s, n, &i, text, in_h ? &headb : NULL);
            continue;
        }
        int closing = (i + 1 < n && s[i + 1] == '/');
        size_t j = i + 1 + closing;
        size_t nb = j;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == ':' || s[j] == '-' || s[j] == '_')) {
            j++;
        }
        size_t ne = j;
        size_t raw_end = j;
        while (raw_end < n && s[raw_end] != '>') {
            raw_end++;
        }
        j = raw_end < n ? raw_end + 1 : n;
        const char *nm = s + nb;
        size_t nn = ne > nb ? ne - nb : 0;
        const char *raw = s + i;
        size_t rn = raw_end > i ? raw_end - i : 0;
        if (!closing && tag_is(nm, nn, "h")) {
            if (in_h && emit_head(marks, head_at, in_h, &headb) != 0) {
                buf_free(&headb);
                return -1;
            }
            if (in_h) {
                buf_putc(text, '\n');
            }
            in_h = outline_level_of(raw, rn);
            head_at = (uint32_t)text->len;
            headb.len = 0;
            if (headb.data) {
                headb.data[0] = 0;
            }
        } else if (closing && tag_is(nm, nn, "h")) {
            if (emit_head(marks, head_at, in_h, &headb) != 0) {
                buf_free(&headb);
                return -1;
            }
            in_h = 0;
            buf_putc(text, '\n');
        } else if (closing && tag_is(nm, nn, "p")) {
            buf_putc(text, '\n');
        } else if (is_odt_page_break(nm, nn, raw, rn, closing)) {
            if (note_page_break(marks, text->len, &page) != 0) {
                buf_free(&headb);
                return -1;
            }
            if (text->len > 0) {
                buf_putc(text, '\n');
            }
        }
        i = j;
    }
    if (in_h && emit_head(marks, head_at, in_h, &headb) != 0) {
        buf_free(&headb);
        return -1;
    }
    buf_free(&headb);
    return 0;
}

static char *stem_label(const char *name) {
    const char *base = name ? name : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        n = (size_t)(dot - base);
    }
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < n; i++) {
        char c = base[i];
        if (c == '_' || c == '-') {
            c = ' ';
        }
        if (buf_putc(&b, c) != 0) {
            buf_free(&b);
            return NULL;
        }
    }
    char *sq = squash_text(b.data ? b.data : "", b.len);
    buf_free(&b);
    return sq;
}

static int count_heads(const MarkBuf *m, int from) {
    int c = 0;
    for (int i = from; i < m->n; i++) {
        if (m->a[i].kind == MARK_HEAD) {
            c++;
        }
    }
    return c;
}

/* ATX (#) or AsciiDoc (=) headings on the finished text. The mark sits on the
 * marker so the heading line belongs to the section. */
static int add_line_heads(Extract *out, char marker) {
    const char *s = out->text ? out->text : "";
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        size_t line = i;
        int count = 0;
        while (i < n && s[i] == marker) {
            count++;
            i++;
        }
        int ok = count >= 1 && count <= 6 && i < n && s[i] == ' ';
        if (!ok) {
            i = line;
            while (i < n && s[i] != '\n') {
                i++;
            }
            if (i < n) {
                i++;
            }
            continue;
        }
        i++;
        size_t b = i;
        while (i < n && s[i] != '\n' && s[i] != '\f') {
            i++;
        }
        size_t e = i;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
            e--;
        }
        if (marker == '#') {
            size_t t = e;
            while (t > b && s[t - 1] == '#') {
                t--;
            }
            if (t < e && t > b && s[t - 1] == ' ') {
                e = t - 1;
                while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
                    e--;
                }
            }
        }
        char *sq = squash_text(s + b, e > b ? e - b : 0);
        if (sq) {
            MarkBuf mb = {out->marks, out->nmarks, out->nmarks};
            int rc = mark_add(&mb, (uint32_t)line, MARK_HEAD, (uint8_t)count, 0, sq, strlen(sq));
            out->marks = mb.a;
            out->nmarks = mb.n;
            free(sq);
            if (rc != 0) {
                return -1;
            }
        }
        if (i < n && s[i] == '\n') {
            i++;
        }
    }
    if (out->nmarks > 1) {
        qsort(out->marks, (size_t)out->nmarks, sizeof(Mark), mark_cmp);
    }
    return 0;
}

static uint32_t offset_of_page(const char *text, uint32_t page) {
    if (page <= 1) {
        return 0;
    }
    const char *s = text ? text : "";
    size_t n = strlen(s);
    uint32_t seen = 1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\f') {
            continue;
        }
        seen++;
        if (seen == page) {
            return i + 1 < n ? (uint32_t)(i + 1) : (uint32_t)i;
        }
    }
    return UINT32_MAX;
}

static int attach_pdf_sections(Extract *out, const unsigned char *bytes, size_t n) {
    PdfSection *secs = NULL;
    int ns = 0;
    if (pdf_sections(bytes, n, &secs, &ns) != 0) {
        return -1;
    }
    for (int i = 0; i < ns; i++) {
        if (!secs[i].title || !secs[i].title[0]) {
            continue;
        }
        uint32_t at = offset_of_page(out->text, secs[i].page);
        if (at == UINT32_MAX) {
            continue;
        }
        uint8_t level = secs[i].level ? secs[i].level : 1;
        MarkBuf mb = {out->marks, out->nmarks, out->nmarks};
        int rc = mark_add(&mb, at, MARK_HEAD, level, 0, secs[i].title, strlen(secs[i].title));
        out->marks = mb.a;
        out->nmarks = mb.n;
        if (rc != 0) {
            pdf_sections_free(secs, ns);
            return -1;
        }
    }
    pdf_sections_free(secs, ns);
    if (out->nmarks > 1) {
        qsort(out->marks, (size_t)out->nmarks, sizeof(Mark), mark_cmp);
    }
    return 0;
}

static ExStatus extract_html_bytes(const char *s, size_t n, Extract *out) {
    Buf text;
    buf_init(&text);
    char *title = NULL;
    MarkBuf mb = {0};
    if (html_to_buf(s, n, &text, &title, &mb) != 0) {
        buf_free(&text);
        free(title);
        mbuf_free(&mb);
        return EX_ERR;
    }
    return finish(&text, title, "html had no text", out, &mb, 0);
}

static ExStatus extract_docx(Zip *z, Extract *out, Err *e) {
    ZipFile *doc = zip_find(z, "word/document.xml");
    if (!doc) {
        err_set(e, "docx is missing word/document.xml");
        return EX_ERR;
    }
    char *title = NULL;
    ZipFile *core = zip_find(z, "docProps/core.xml");
    if (core) {
        title = first_tag_text((char *)core->data, core->len, "title");
    }
    Buf text;
    buf_init(&text);
    MarkBuf mb = {0};
    if (docx_to_buf((char *)doc->data, doc->len, &text, &mb) != 0) {
        buf_free(&text);
        free(title);
        mbuf_free(&mb);
        err_set(e, "out of memory");
        return EX_ERR;
    }
    return finish(&text, title, "docx had no text", out, &mb, 0);
}

static ExStatus extract_odt(Zip *z, Extract *out, Err *e) {
    ZipFile *doc = zip_find(z, "content.xml");
    if (!doc) {
        err_set(e, "odt is missing content.xml");
        return EX_ERR;
    }
    char *title = NULL;
    ZipFile *meta = zip_find(z, "meta.xml");
    if (meta) {
        title = first_tag_text((char *)meta->data, meta->len, "title");
    }
    Buf text;
    buf_init(&text);
    MarkBuf mb = {0};
    if (odt_to_buf((char *)doc->data, doc->len, &text, &mb) != 0) {
        buf_free(&text);
        free(title);
        mbuf_free(&mb);
        err_set(e, "out of memory");
        return EX_ERR;
    }
    return finish(&text, title, "odt had no text", out, &mb, 0);
}

static int zip_name_cmp(const void *a, const void *b) {
    const ZipFile *const *x = a;
    const ZipFile *const *y = b;
    return strcmp((*x)->name, (*y)->name);
}

static ExStatus extract_epub(Zip *z, Extract *out) {
    char *title = NULL;
    for (size_t i = 0; i < z->n; i++) {
        if (zip_name_ends(&z->files[i], ".opf")) {
            title = first_tag_text((char *)z->files[i].data, z->files[i].len, "title");
            if (title) {
                break;
            }
        }
    }
    ZipFile **htmls = calloc(z->n ? z->n : 1, sizeof(ZipFile *));
    size_t nh = 0;
    if (!htmls) {
        free(title);
        return EX_ERR;
    }
    for (size_t i = 0; i < z->n; i++) {
        if (zip_name_ends(&z->files[i], ".xhtml") || zip_name_ends(&z->files[i], ".html") ||
            zip_name_ends(&z->files[i], ".htm")) {
            htmls[nh++] = &z->files[i];
        }
    }
    qsort(htmls, nh, sizeof *htmls, zip_name_cmp);
    Buf text;
    buf_init(&text);
    MarkBuf mb = {0};
    for (size_t i = 0; i < nh; i++) {
        uint32_t at = (uint32_t)text.len;
        int before = mb.n;
        if (html_to_buf((char *)htmls[i]->data, htmls[i]->len, &text, title ? NULL : &title, &mb) != 0) {
            buf_free(&text);
            free(title);
            mbuf_free(&mb);
            free(htmls);
            return EX_ERR;
        }
        if (count_heads(&mb, before) == 0) {
            char *label = first_tag_text((char *)htmls[i]->data, htmls[i]->len, "title");
            if (!label) {
                label = stem_label(htmls[i]->name);
            }
            if (label && label[0] && mark_add(&mb, at, MARK_HEAD, 1, 0, label, strlen(label)) != 0) {
                free(label);
                buf_free(&text);
                free(title);
                mbuf_free(&mb);
                free(htmls);
                return EX_ERR;
            }
            free(label);
        }
        buf_putc(&text, '\n');
    }
    free(htmls);
    return finish(&text, title, "epub had no text", out, &mb, 0);
}

/* heads: 0 plain text, 1 markdown, 2 asciidoc. */
static ExStatus extract_plain(const unsigned char *bytes, size_t n, int heads, Extract *out, Err *e) {
    if (has_nul(bytes, n)) {
        out->detail = xstrdup("skipped binary-looking file");
        return EX_SKIP;
    }
    (void)e;
    Buf text;
    buf_init(&text);
    if (buf_append(&text, bytes, n) != 0) {
        err_set(e, "out of memory");
        return EX_ERR;
    }
    ExStatus rc = finish(&text, NULL, "empty file", out, NULL, heads ? 0 : 1);
    if (rc == EX_OK && heads) {
        if (add_line_heads(out, heads == 2 ? '=' : '#') != 0) {
            extract_free(out);
            err_set(e, "out of memory");
            return EX_ERR;
        }
    }
    return rc;
}

ExStatus extract_path(const char *path, Extract *out, Err *e) {
    memset(out, 0, sizeof *out);
    err_clear(e);
    struct stat st;
    if (stat(path, &st) != 0) {
        err_set(e, "cannot stat %s", path);
        return EX_ERR;
    }
    if (!S_ISREG(st.st_mode)) {
        err_set(e, "%s is not a regular file", path);
        return EX_ERR;
    }
    if ((uint64_t)st.st_size > MAX_INPUT) {
        out->detail = xstrdup("file is larger than 32MB");
        return EX_SKIP;
    }
    const char *ext = ext_of(path);
    int text = is_text_ext(ext);
    int html = ext_is(ext, "html") || ext_is(ext, "htm");
    int xml = ext_is(ext, "xml");
    int zippy = ext_is(ext, "docx") || ext_is(ext, "odt") || ext_is(ext, "epub");
    int pdf = ext_is(ext, "pdf");
    int md = ext_is(ext, "md") || ext_is(ext, "markdown");
    int adoc = ext_is(ext, "adoc") || ext_is(ext, "asciidoc");
    if (!text && !zippy && !pdf) {
        out->detail = xstrdup("unsupported type");
        return EX_SKIP;
    }
    unsigned char *bytes = NULL;
    size_t n = 0;
    if (read_entire_file(path, &bytes, &n, MAX_INPUT, e) != 0) {
        return EX_ERR;
    }
    ExStatus rc = EX_OK;
    if (pdf) {
        Buf t;
        buf_init(&t);
        char *title = NULL;
        if (pdf_extract(bytes, n, &t, &title, e) != 0) {
            buf_free(&t);
            free(title);
            free(bytes);
            return EX_ERR;
        }
        rc = finish(&t, title, "pdf had no digital text", out, NULL, 2);
        if (rc == EX_OK && attach_pdf_sections(out, bytes, n) != 0) {
            extract_free(out);
            err_set(e, "out of memory");
            rc = EX_ERR;
        }
    } else if (zippy) {
        Zip z;
        if (zip_read(bytes, n, &z, e) != 0) {
            free(bytes);
            return EX_ERR;
        }
        if (ext_is(ext, "docx")) {
            rc = extract_docx(&z, out, e);
        } else if (ext_is(ext, "odt")) {
            rc = extract_odt(&z, out, e);
        } else {
            rc = extract_epub(&z, out);
        }
        zip_free(&z);
    } else if (html) {
        rc = extract_html_bytes((char *)bytes, n, out);
    } else if (xml) {
        Buf t;
        buf_init(&t);
        const char *blocks[] = {"p", "div", "title", "h1", "h2"};
        xml_texts((char *)bytes, n, &t, blocks, 5);
        rc = finish(&t, NULL, "xml had no text", out, NULL, 0);
    } else if (md) {
        rc = extract_plain(bytes, n, 1, out, e);
    } else if (adoc) {
        rc = extract_plain(bytes, n, 2, out, e);
    } else {
        rc = extract_plain(bytes, n, 0, out, e);
    }
    free(bytes);
    if (rc == EX_ERR && e->msg[0] == 0) {
        err_set(e, "extract failed");
    }
    return rc;
}
