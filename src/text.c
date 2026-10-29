#include "text.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int utf8_next(const char *s, size_t n, size_t *i, uint32_t *cp) {
    if (*i >= n) {
        return 0;
    }
    const unsigned char *u = (const unsigned char *)s;
    size_t k = *i;
    unsigned char c = u[k];
    if (c < 0x80) {
        *cp = c;
        *i = k + 1;
        return 1;
    }
    if ((c & 0xe0) == 0xc0 && k + 1 < n && (u[k + 1] & 0xc0) == 0x80 && c >= 0xc2) {
        *cp = ((uint32_t)(c & 0x1f) << 6) | (u[k + 1] & 0x3f);
        *i = k + 2;
        return 1;
    }
    if ((c & 0xf0) == 0xe0 && k + 2 < n && (u[k + 1] & 0xc0) == 0x80 && (u[k + 2] & 0xc0) == 0x80) {
        uint32_t v = ((uint32_t)(c & 0x0f) << 12) | ((uint32_t)(u[k + 1] & 0x3f) << 6) | (u[k + 2] & 0x3f);
        if (v >= 0x800 && !(v >= 0xd800 && v <= 0xdfff)) {
            *cp = v;
            *i = k + 3;
            return 1;
        }
    }
    if ((c & 0xf8) == 0xf0 && k + 3 < n && (u[k + 1] & 0xc0) == 0x80 && (u[k + 2] & 0xc0) == 0x80 &&
        (u[k + 3] & 0xc0) == 0x80) {
        uint32_t v = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(u[k + 1] & 0x3f) << 12) |
                     ((uint32_t)(u[k + 2] & 0x3f) << 6) | (u[k + 3] & 0x3f);
        if (v >= 0x10000 && v <= 0x10ffff) {
            *cp = v;
            *i = k + 4;
            return 1;
        }
    }
    *cp = c;
    *i = k + 1;
    return 1;
}

int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static uint32_t fold_greek_ext(uint32_t cp);

uint32_t casefold_cp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32;
    }
    if (cp >= 0xc0 && cp <= 0xde && cp != 0xd7) {
        return cp + 32;
    }
    if (cp >= 0x100 && cp <= 0x17e && cp != 0x130) {
        if ((cp & 1) == 0) {
            return cp + 1;
        }
        return cp;
    }
    if (cp >= 0x391 && cp <= 0x3a9 && cp != 0x3a2) {
        return cp + 0x20;
    }
    if (cp >= 0x410 && cp <= 0x42f) {
        return cp + 0x20;
    }
    if (cp >= 0xff21 && cp <= 0xff3a) {
        return cp - 0xff21 + 'a';
    }
    if (cp >= 0xff41 && cp <= 0xff5a) {
        return cp - 0xff41 + 'a';
    }
    if (cp >= 0xff10 && cp <= 0xff19) {
        return cp - 0xff10 + '0';
    }
    return fold_greek_ext(cp);
}

/* Case only. A capital with prosgegrammeni folds to the precomposed small
 * letter, and that letter is not split into a following iota. */
static uint32_t fold_greek_ext(uint32_t cp) {
    if (cp < 0x1f00 || cp > 0x1fff) {
        return cp;
    }
    if ((cp >= 0x1f08 && cp <= 0x1f0f) || (cp >= 0x1f18 && cp <= 0x1f1d) ||
        (cp >= 0x1f28 && cp <= 0x1f2f) || (cp >= 0x1f38 && cp <= 0x1f3f) ||
        (cp >= 0x1f48 && cp <= 0x1f4d) || (cp >= 0x1f68 && cp <= 0x1f6f) ||
        (cp >= 0x1f88 && cp <= 0x1f8f) || (cp >= 0x1f98 && cp <= 0x1f9f) ||
        (cp >= 0x1fa8 && cp <= 0x1faf) || cp == 0x1f59 || cp == 0x1f5b || cp == 0x1f5d ||
        cp == 0x1f5f || cp == 0x1fb8 || cp == 0x1fb9 || cp == 0x1fd8 || cp == 0x1fd9 ||
        cp == 0x1fe8 || cp == 0x1fe9) {
        return cp - 8;
    }
    switch (cp) {
    case 0x1fba:
        return 0x1f70;
    case 0x1fbb:
        return 0x1f71;
    case 0x1fbc:
        return 0x1fb3;
    case 0x1fc8:
        return 0x1f72;
    case 0x1fc9:
        return 0x1f73;
    case 0x1fca:
        return 0x1f74;
    case 0x1fcb:
        return 0x1f75;
    case 0x1fcc:
        return 0x1fc3;
    case 0x1fda:
        return 0x1f76;
    case 0x1fdb:
        return 0x1f77;
    case 0x1fea:
        return 0x1f7a;
    case 0x1feb:
        return 0x1f7b;
    case 0x1fec:
        return 0x1fe5;
    case 0x1ff8:
        return 0x1f78;
    case 0x1ff9:
        return 0x1f79;
    case 0x1ffa:
        return 0x1f7c;
    case 0x1ffb:
        return 0x1f7d;
    case 0x1ffc:
        return 0x1ff3;
    default:
        return cp;
    }
}

int cp_is_cjk(uint32_t cp) {
    if (cp >= 0x3040 && cp <= 0x30ff) {
        return 1;
    }
    if (cp >= 0x31f0 && cp <= 0x31ff) {
        return 1;
    }
    if (cp >= 0x3400 && cp <= 0x9fff) {
        return 1;
    }
    if (cp >= 0xf900 && cp <= 0xfaff) {
        return 1;
    }
    if (cp >= 0xac00 && cp <= 0xd7af) {
        return 1;
    }
    if (cp >= 0x20000 && cp <= 0x2ceaf) {
        return 1;
    }
    return 0;
}

int cp_is_letter(uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
        return 1;
    }
    if (cp >= 0xc0 && cp <= 0x24f && cp != 0xd7 && cp != 0xf7) {
        return 1;
    }
    if (cp >= 0x370 && cp <= 0x3ff) {
        return 1;
    }
    if (cp >= 0x400 && cp <= 0x52f) {
        return 1;
    }
    /* Greek Extended, except the spacing marks (koronis, psili, dasia, oxia). */
    if (cp >= 0x1f00 && cp <= 0x1fff) {
        if (cp == 0x1fbd || cp == 0x1fbf || cp == 0x1fc0 || cp == 0x1fc1 ||
            (cp >= 0x1fcd && cp <= 0x1fcf) || (cp >= 0x1fdd && cp <= 0x1fdf) ||
            (cp >= 0x1fed && cp <= 0x1fef) || cp == 0x1ffd || cp == 0x1ffe) {
            return 0;
        }
        return 1;
    }
    return 0;
}

int cp_is_digit(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 0xff10 && cp <= 0xff19);
}

void tokens_init(Tokens *t) {
    buf_init(&t->arena);
    t->toks = NULL;
    t->n = 0;
    t->cap = 0;
}

void tokens_free(Tokens *t) {
    buf_free(&t->arena);
    free(t->toks);
    t->toks = NULL;
    t->n = 0;
    t->cap = 0;
}

void tokens_clear(Tokens *t) {
    t->arena.len = 0;
    if (t->arena.data) {
        t->arena.data[0] = 0;
    }
    t->n = 0;
}

static int tokens_push(Tokens *t, uint32_t off, uint32_t len, uint32_t pos, uint32_t start, uint32_t end) {
    if (len == 0) {
        return 0;
    }
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 64;
        Token *p = realloc(t->toks, cap * sizeof(Token));
        if (!p) {
            return -1;
        }
        t->toks = p;
        t->cap = cap;
    }
    t->toks[t->n].off = off;
    t->toks[t->n].len = len;
    t->toks[t->n].pos = pos;
    t->toks[t->n].start = start;
    t->toks[t->n].end = end;
    t->n++;
    return 0;
}

static int emit_bytes(Tokens *t, const char *s, size_t n, uint32_t *pos, uint32_t start, uint32_t end) {
    if (n > 64) {
        n = 64;
    }
    uint32_t off = (uint32_t)t->arena.len;
    if (buf_append(&t->arena, s, n) != 0) {
        return -1;
    }
    if (tokens_push(t, off, (uint32_t)n, *pos, start, end) != 0) {
        return -1;
    }
    (*pos)++;
    return 0;
}

static int emit_cps(Tokens *t, const uint32_t *cps, size_t n, uint32_t *pos, uint32_t start, uint32_t end) {
    char tmp[256];
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        char enc[4];
        int k = utf8_encode(cps[i], enc);
        if (len + (size_t)k > sizeof tmp) {
            break;
        }
        memcpy(tmp + len, enc, (size_t)k);
        len += (size_t)k;
    }
    if (len == 0) {
        return 0;
    }
    return emit_bytes(t, tmp, len, pos, start, end);
}

int tokenize(Tokens *t, const char *s, size_t n) {
    uint32_t word[64];
    uint32_t word_s[64];
    uint32_t word_e[64];
    size_t nw = 0;
    uint32_t cjk[64];
    uint32_t cjk_s[64];
    uint32_t cjk_e[64];
    size_t nc = 0;
    char digits[64];
    uint32_t dig_s = 0;
    uint32_t dig_e = 0;
    size_t nd = 0;
    uint32_t pos = 0;
    size_t i = 0;

    if (!s) {
        s = "";
        n = 0;
    }
    while (i < n || nw || nc || nd) {
        uint32_t raw = 0;
        uint32_t cp = 0;
        int have = 0;
        size_t cp_start = i;
        size_t cp_end = i;
        if (i < n) {
            if (!utf8_next(s, n, &i, &raw)) {
                break;
            }
            cp_end = i;
            cp = casefold_cp(raw);
            have = 1;
        }
        int kind = 0; /* 1 word, 2 cjk, 3 digit */
        if (have) {
            if (cp_is_digit(cp)) {
                kind = 3;
            } else if (cp_is_cjk(cp)) {
                kind = 2;
            } else if (cp_is_letter(cp) || (cp == '\'' && nw)) {
                kind = 1;
            }
        }
        if (kind != 1 && nw) {
            if (emit_cps(t, word, nw, &pos, word_s[0], word_e[nw - 1]) != 0) {
                return -1;
            }
            nw = 0;
        }
        if (kind != 2 && nc) {
            if (nc == 1) {
                if (emit_cps(t, cjk, 1, &pos, cjk_s[0], cjk_e[0]) != 0) {
                    return -1;
                }
            } else {
                for (size_t k = 0; k + 1 < nc; k++) {
                    uint32_t bg[2] = {cjk[k], cjk[k + 1]};
                    if (emit_cps(t, bg, 2, &pos, cjk_s[k], cjk_e[k + 1]) != 0) {
                        return -1;
                    }
                }
            }
            nc = 0;
        }
        if (kind != 3 && nd) {
            if (emit_bytes(t, digits, nd, &pos, dig_s, dig_e) != 0) {
                return -1;
            }
            nd = 0;
        }
        if (!have) {
            break;
        }
        if (kind == 1 && cp != '\'') {
            if (nw < 64) {
                word[nw] = cp;
                word_s[nw] = (uint32_t)cp_start;
                word_e[nw] = (uint32_t)cp_end;
                nw++;
            }
        } else if (kind == 2) {
            if (nc < 64) {
                cjk[nc] = cp;
                cjk_s[nc] = (uint32_t)cp_start;
                cjk_e[nc] = (uint32_t)cp_end;
                nc++;
            } else {
                /* flush a full CJK window and keep the last code point for overlap */
                for (size_t k = 0; k + 1 < nc; k++) {
                    uint32_t bg[2] = {cjk[k], cjk[k + 1]};
                    if (emit_cps(t, bg, 2, &pos, cjk_s[k], cjk_e[k + 1]) != 0) {
                        return -1;
                    }
                }
                cjk[0] = cjk[nc - 1];
                cjk_s[0] = cjk_s[nc - 1];
                cjk_e[0] = cjk_e[nc - 1];
                cjk[1] = cp;
                cjk_s[1] = (uint32_t)cp_start;
                cjk_e[1] = (uint32_t)cp_end;
                nc = 2;
            }
        } else if (kind == 3) {
            if (nd < sizeof digits) {
                if (nd == 0) {
                    dig_s = (uint32_t)cp_start;
                }
                digits[nd++] = (char)cp;
                dig_e = (uint32_t)cp_end;
            }
        }
    }
    return 0;
}

static Query *q_new(QKind kind) {
    Query *q = calloc(1, sizeof(Query));
    if (!q) {
        return NULL;
    }
    q->kind = kind;
    return q;
}

static void query_free(Query *q) {
    if (!q) {
        return;
    }
    free(q->text);
    query_free(q->a);
    query_free(q->b);
    for (int i = 0; i < q->nkids; i++) {
        query_free(q->kids[i]);
    }
    free(q->kids);
    free(q);
}

void parsed_free(ParsedQuery *p) {
    if (!p) {
        return;
    }
    query_free(p->q);
    free(p->ext);
    free(p->path_sub);
    free(p->title_sub);
    memset(p, 0, sizeof *p);
}

static Query *q_term_owned(char *text, size_t len, int prefix) {
    Query *q = q_new(Q_TERM);
    if (!q) {
        free(text);
        return NULL;
    }
    q->text = text;
    q->len = len;
    q->prefix = prefix;
    return q;
}

static Query *tokens_to_query(Tokens *t, int prefix) {
    if (t->n == 0) {
        return NULL;
    }
    if (t->n == 1 && !prefix) {
        char *s = xstrndup(t->arena.data + t->toks[0].off, t->toks[0].len);
        if (!s) {
            return NULL;
        }
        return q_term_owned(s, t->toks[0].len, 0);
    }
    if (t->n == 1 && prefix) {
        char *s = xstrndup(t->arena.data + t->toks[0].off, t->toks[0].len);
        if (!s) {
            return NULL;
        }
        return q_term_owned(s, t->toks[0].len, 1);
    }
    Query *q = q_new(Q_PHRASE);
    if (!q) {
        return NULL;
    }
    q->kids = calloc((size_t)t->n, sizeof(Query *));
    if (!q->kids) {
        free(q);
        return NULL;
    }
    q->nkids = (int)t->n;
    for (size_t i = 0; i < t->n; i++) {
        char *s = xstrndup(t->arena.data + t->toks[i].off, t->toks[i].len);
        if (!s) {
            query_free(q);
            return NULL;
        }
        int pref = (prefix && i + 1 == t->n) ? 1 : 0;
        q->kids[i] = q_term_owned(s, t->toks[i].len, pref);
        if (!q->kids[i]) {
            query_free(q);
            return NULL;
        }
    }
    return q;
}

static Query *q_and(Query *a, Query *b) {
    if (!a) {
        return b;
    }
    if (!b) {
        return a;
    }
    Query *q = q_new(Q_AND);
    if (!q) {
        query_free(a);
        query_free(b);
        return NULL;
    }
    q->a = a;
    q->b = b;
    return q;
}

static Query *q_or(Query *a, Query *b) {
    if (!a) {
        return b;
    }
    if (!b) {
        return a;
    }
    Query *q = q_new(Q_OR);
    if (!q) {
        query_free(a);
        query_free(b);
        return NULL;
    }
    q->a = a;
    q->b = b;
    return q;
}

typedef struct Cur {
    const char *s;
    size_t n;
    size_t i;
    Err *e;
} Cur;

static void skip_ws(Cur *c) {
    while (c->i < c->n) {
        char ch = c->s[c->i];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            break;
        }
        c->i++;
    }
}

static int keyword(Cur *c, const char *kw) {
    size_t n = strlen(kw);
    if (c->i + n > c->n || memcmp(c->s + c->i, kw, n) != 0) {
        return 0;
    }
    if (c->i + n < c->n) {
        char ch = c->s[c->i + n];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            return 0;
        }
    }
    c->i += n;
    return 1;
}

static Query *parse_or(Cur *c, ParsedQuery *pq);

static int parse_quoted(Cur *c, char **out, size_t *len) {
    Buf b;
    buf_init(&b);
    c->i++; /* opening quote already detected by caller? we are on the quote */
    if (c->i == 0 || c->s[c->i - 1] != '"') {
        buf_free(&b);
        return -1;
    }
    while (c->i < c->n) {
        char ch = c->s[c->i++];
        if (ch == '"') {
            *out = b.data ? b.data : xstrdup("");
            *len = b.len;
            if (!*out) {
                buf_free(&b);
                return -1;
            }
            if (!b.data) {
                /* xstrdup used */
            }
            return 0;
        }
        if (ch == '\\' && c->i < c->n) {
            ch = c->s[c->i++];
        }
        if (buf_putc(&b, ch) != 0) {
            buf_free(&b);
            return -1;
        }
    }
    buf_free(&b);
    err_set(c->e, "unterminated quote");
    return -1;
}

static Query *atom_from_raw(const char *s, size_t n, int prefix) {
    Tokens t;
    tokens_init(&t);
    if (tokenize(&t, s, n) != 0) {
        tokens_free(&t);
        return NULL;
    }
    Query *q = tokens_to_query(&t, prefix);
    tokens_free(&t);
    return q;
}

static int take_filter(Cur *c, ParsedQuery *out) {
    skip_ws(c);
    size_t save = c->i;
    const char *name = NULL;
    size_t namelen = 0;
    if (c->i + 4 <= c->n && memcmp(c->s + c->i, "ext:", 4) == 0) {
        name = "ext";
        namelen = 4;
    } else if (c->i + 5 <= c->n && memcmp(c->s + c->i, "path:", 5) == 0) {
        name = "path";
        namelen = 5;
    } else if (c->i + 6 <= c->n && memcmp(c->s + c->i, "title:", 6) == 0) {
        name = "title";
        namelen = 6;
    } else if (c->i + 6 <= c->n && memcmp(c->s + c->i, "mtime:", 6) == 0) {
        name = "mtime";
        namelen = 6;
    } else {
        return 0;
    }
    c->i += namelen;
    char *val = NULL;
    size_t vn = 0;
    if (c->i < c->n && c->s[c->i] == '"') {
        if (parse_quoted(c, &val, &vn) != 0) {
            return -1;
        }
    } else {
        size_t b = c->i;
        while (c->i < c->n) {
            char ch = c->s[c->i];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ')') {
                break;
            }
            c->i++;
        }
        if (c->i == b) {
            c->i = save;
            return 0;
        }
        val = xstrndup(c->s + b, c->i - b);
        vn = c->i - b;
        if (!val) {
            err_set(c->e, "out of memory");
            return -1;
        }
    }
    if (strcmp(name, "ext") == 0) {
        ascii_lower_inplace(val);
        if (val[0] == '.') {
            memmove(val, val + 1, strlen(val));
        }
        free(out->ext);
        out->ext = val;
    } else if (strcmp(name, "path") == 0) {
        free(out->path_sub);
        out->path_sub = val;
    } else if (strcmp(name, "title") == 0) {
        free(out->title_sub);
        out->title_sub = val;
    } else {
        int year = 0;
        int mon = 0;
        int ok = 0;
        if (vn == 7 && val[4] == '-') {
            ok = 1;
            for (int i = 0; i < 7; i++) {
                if (i == 4) {
                    continue;
                }
                if (val[i] < '0' || val[i] > '9') {
                    ok = 0;
                    break;
                }
                if (i < 4) {
                    year = year * 10 + (val[i] - '0');
                } else {
                    mon = mon * 10 + (val[i] - '0');
                }
            }
        }
        free(val);
        if (!ok || year < 1 || mon < 1 || mon > 12) {
            err_set(c->e, "mtime: wants a month, like 2026-10");
            return -1;
        }
        out->mtime_year = year;
        out->mtime_mon = mon;
    }
    (void)vn;
    return 1;
}

static Query *parse_primary(Cur *c, ParsedQuery *pq) {
    skip_ws(c);
    if (c->i >= c->n) {
        err_set(c->e, "unexpected end of query");
        return NULL;
    }
    if (c->s[c->i] == '(') {
        c->i++;
        Query *q = parse_or(c, pq);
        if (!q && c->e->msg[0]) {
            return NULL;
        }
        skip_ws(c);
        if (c->i >= c->n || c->s[c->i] != ')') {
            query_free(q);
            err_set(c->e, "missing closing parenthesis");
            return NULL;
        }
        c->i++;
        return q;
    }
    if (c->s[c->i] == '"') {
        char *raw = NULL;
        size_t rn = 0;
        if (parse_quoted(c, &raw, &rn) != 0) {
            return NULL;
        }
        Query *q = atom_from_raw(raw, rn, 0);
        free(raw);
        return q;
    }
    int fr = take_filter(c, pq);
    if (fr < 0) {
        return NULL;
    }
    if (fr > 0) {
        return NULL; /* not a text clause; caller treats NULL without error as empty */
    }
    size_t b = c->i;
    while (c->i < c->n) {
        char ch = c->s[c->i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ')' || ch == '(') {
            break;
        }
        c->i++;
    }
    size_t e = c->i;
    int prefix = 0;
    if (e > b && c->s[e - 1] == '*') {
        prefix = 1;
        e--;
    }
    if (e == b) {
        err_set(c->e, "empty search term");
        return NULL;
    }
    return atom_from_raw(c->s + b, e - b, prefix);
}

static Query *parse_unary(Cur *c, ParsedQuery *pq) {
    skip_ws(c);
    int neg = 0;
    if (keyword(c, "NOT")) {
        neg = 1;
        skip_ws(c);
    } else if (c->i < c->n && c->s[c->i] == '-' && (c->i + 1 >= c->n || c->s[c->i + 1] != ' ')) {
        /* minus attached to a term, or a bare minus */
        if (c->i + 1 < c->n && c->s[c->i + 1] != ' ' && c->s[c->i + 1] != '\t') {
            neg = 1;
            c->i++;
        }
    }
    size_t mark = c->i;
    Err saved = *c->e;
    err_clear(c->e);
    Query *q = parse_primary(c, pq);
    if (!q && c->e->msg[0] == 0) {
        /* filter only */
        *c->e = saved;
        if (neg) {
            err_set(c->e, "NOT needs a term");
            return NULL;
        }
        return NULL;
    }
    if (!q) {
        return NULL;
    }
    if (!neg) {
        return q;
    }
    Query *n = q_new(Q_NOT);
    if (!n) {
        query_free(q);
        err_set(c->e, "out of memory");
        return NULL;
    }
    n->a = q;
    (void)mark;
    return n;
}

static int at_or(Cur *c) {
    size_t save = c->i;
    skip_ws(c);
    int yes = keyword(c, "OR");
    c->i = save;
    return yes;
}

static Query *parse_and(Cur *c, ParsedQuery *pq) {
    Query *acc = NULL;
    for (;;) {
        skip_ws(c);
        if (c->i >= c->n || c->s[c->i] == ')' || at_or(c)) {
            break;
        }
        if (keyword(c, "AND")) {
            continue;
        }
        size_t before = c->i;
        Query *u = parse_unary(c, pq);
        if (!u && c->e->msg[0]) {
            query_free(acc);
            return NULL;
        }
        if (!u) {
            if (c->i == before) {
                break;
            }
            continue;
        }
        acc = q_and(acc, u);
        if (!acc) {
            err_set(c->e, "out of memory");
            return NULL;
        }
    }
    return acc;
}

static Query *parse_or(Cur *c, ParsedQuery *pq) {
    Query *acc = parse_and(c, pq);
    if (!acc && c->e->msg[0]) {
        return NULL;
    }
    for (;;) {
        skip_ws(c);
        if (!keyword(c, "OR")) {
            break;
        }
        Query *rhs = parse_and(c, pq);
        if (!rhs && c->e->msg[0]) {
            query_free(acc);
            return NULL;
        }
        if (!rhs) {
            query_free(acc);
            err_set(c->e, "OR is missing a term");
            return NULL;
        }
        acc = q_or(acc, rhs);
        if (!acc) {
            err_set(c->e, "out of memory");
            return NULL;
        }
    }
    return acc;
}

int query_parse(const char *s, ParsedQuery *out, Err *e) {
    memset(out, 0, sizeof *out);
    err_clear(e);
    if (!s) {
        s = "";
    }
    Cur c = {s, strlen(s), 0, e};
    Query *q = parse_or(&c, out);
    if (e->msg[0]) {
        query_free(q);
        parsed_free(out);
        return -1;
    }
    skip_ws(&c);
    if (c.i < c.n) {
        query_free(q);
        parsed_free(out);
        err_set(e, "unexpected text at the end of the query");
        return -1;
    }
    if (!q && !out->ext && !out->path_sub && !out->title_sub && !out->mtime_year) {
        err_set(e, "empty query");
        return -1;
    }
    out->q = q;
    return 0;
}

static int qterms_push(QTerm **arr, int *n, int *cap, const char *s, size_t len, int prefix, int phrase) {
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 8;
        QTerm *p = realloc(*arr, (size_t)c * sizeof(QTerm));
        if (!p) {
            return -1;
        }
        *arr = p;
        *cap = c;
    }
    char *copy = xstrndup(s, len);
    if (!copy) {
        return -1;
    }
    (*arr)[*n].s = copy;
    (*arr)[*n].len = len;
    (*arr)[*n].prefix = prefix;
    (*arr)[*n].phrase = phrase;
    (*n)++;
    return 0;
}

static int qterms_collect(const Query *q, QTerm **arr, int *n, int *cap, int phrase, int *phrase_seq) {
    if (!q || q->kind == Q_NOT) {
        return 0;
    }
    if (q->kind == Q_TERM) {
        return qterms_push(arr, n, cap, q->text, q->len, q->prefix, phrase);
    }
    if (q->kind == Q_PHRASE) {
        int id = ++(*phrase_seq);
        for (int i = 0; i < q->nkids; i++) {
            if (qterms_collect(q->kids[i], arr, n, cap, id, phrase_seq) != 0) {
                return -1;
            }
        }
        return 0;
    }
    if (qterms_collect(q->a, arr, n, cap, 0, phrase_seq) != 0) {
        return -1;
    }
    return qterms_collect(q->b, arr, n, cap, 0, phrase_seq);
}

int query_positive_terms(const Query *q, QTerm **out, int *n) {
    int cap = 0;
    int phrase_seq = 0;
    *out = NULL;
    *n = 0;
    if (qterms_collect(q, out, n, &cap, 0, &phrase_seq) != 0) {
        qterms_free(*out, *n);
        *out = NULL;
        *n = 0;
        return -1;
    }
    return 0;
}

void qterms_free(QTerm *t, int n) {
    for (int i = 0; i < n; i++) {
        free(t[i].s);
    }
    free(t);
}

static int fold_text(const char *s, size_t n, Buf *cf, uint32_t **src_at) {
    buf_clear(cf);
    uint32_t *map = NULL;
    size_t mapcap = 0;
    size_t i = 0;
    int prev_letter = 0;
    while (i < n) {
        size_t src = i;
        uint32_t cp = 0;
        if (!utf8_next(s, n, &i, &cp)) {
            break;
        }
        cp = casefold_cp(cp);
        /* An apostrophe between letters is inside the token. Drop it here so
         * the term "dont" covers the source word "Don't", mark included. */
        if (cp == '\'' && prev_letter) {
            size_t j = i;
            int drop = 0;
            while (j < n) {
                uint32_t next = 0;
                if (!utf8_next(s, n, &j, &next)) {
                    break;
                }
                next = casefold_cp(next);
                if (next == '\'') {
                    continue;
                }
                if (cp_is_letter(next)) {
                    drop = 1;
                }
                break;
            }
            if (drop) {
                continue;
            }
        }
        char enc[4];
        int k = utf8_encode(cp, enc);
        if (buf_append(cf, enc, (size_t)k) != 0) {
            free(map);
            return -1;
        }
        if (cf->len > mapcap) {
            size_t nc = mapcap ? mapcap * 2 : 64;
            while (nc < cf->len) {
                nc *= 2;
            }
            uint32_t *nm = realloc(map, nc * sizeof(uint32_t));
            if (!nm) {
                free(map);
                return -1;
            }
            map = nm;
            mapcap = nc;
        }
        for (int b = 0; b < k; b++) {
            map[cf->len - (size_t)k + (size_t)b] = (uint32_t)src;
        }
        prev_letter = cp_is_letter(cp);
    }
    *src_at = map;
    return 0;
}

int fold_contains(const char *hay, size_t hn, const char *needle, size_t nn) {
    if (!needle || nn == 0) {
        return 1;
    }
    Buf h, nd;
    buf_init(&h);
    buf_init(&nd);
    uint32_t *ignore = NULL;
    int ok = 0;
    if (fold_text(hay ? hay : "", hay ? hn : 0, &h, &ignore) != 0) {
        goto done;
    }
    free(ignore);
    ignore = NULL;
    if (fold_text(needle, nn, &nd, &ignore) != 0) {
        goto done;
    }
    if (nd.len == 0 || nd.len > h.len) {
        goto done;
    }
    for (size_t i = 0; i + nd.len <= h.len; i++) {
        if (memcmp(h.data + i, nd.data, nd.len) == 0) {
            ok = 1;
            break;
        }
    }
done:
    free(ignore);
    buf_free(&h);
    buf_free(&nd);
    return ok;
}

static int term_is_cjk(const char *s, size_t n) {
    size_t i = 0;
    uint32_t cp = 0;
    while (i < n && utf8_next(s, n, &i, &cp)) {
        if (cp_is_cjk(cp)) {
            return 1;
        }
    }
    return 0;
}

static int cp_is_word(uint32_t cp) {
    cp = casefold_cp(cp);
    return cp_is_letter(cp) || cp_is_digit(cp);
}

typedef struct Range {
    size_t a, b;
    int term;
} Range;

static int range_cmp(const void *x, const void *y) {
    const Range *a = x;
    const Range *b = y;
    if (a->a < b->a) {
        return -1;
    }
    if (a->a > b->a) {
        return 1;
    }
    return 0;
}

static const char *find_bytes(const char *h, size_t hn, const char *n, size_t nn) {
    if (nn == 0 || nn > hn) {
        return NULL;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        if (memcmp(h + i, n, nn) == 0) {
            return h + i;
        }
    }
    return NULL;
}

enum { PASSAGE_CAP = 960, PASSAGE_AIM = 640 };

static int page_at(const char *text, size_t offset, int paged) {
    int page = 1;
    if (!paged) {
        return 0;
    }
    for (size_t i = 0; i < offset; i++) {
        if (text[i] == '\f') {
            page++;
        }
    }
    return page;
}

static int is_space_byte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* A passage is one paragraph. Blank lines and form feeds bound it. */
static void para_bounds(const char *text, size_t n, size_t pos, size_t *a, size_t *b) {
    if (pos > n) {
        pos = n;
    }
    size_t i = pos;
    while (i > 0) {
        if (text[i - 1] == '\f') {
            break;
        }
        if (i >= 2 && text[i - 1] == '\n' && text[i - 2] == '\n') {
            break;
        }
        i--;
    }
    *a = i;
    i = pos;
    while (i < n) {
        if (text[i] == '\f') {
            break;
        }
        if (i + 1 < n && text[i] == '\n' && text[i + 1] == '\n') {
            break;
        }
        i++;
    }
    *b = i;
}

static int abbrev_word(const char *s, size_t n) {
    if (n == 1) {
        unsigned char c = (unsigned char)s[0];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    if (n < 2 || n > 4) {
        return 0;
    }
    char buf[8];
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        } else if (c < 'a' || c > 'z') {
            return 0;
        }
        buf[i] = (char)c;
    }
    buf[n] = 0;
    char key[8];
    key[0] = ' ';
    memcpy(key + 1, buf, n);
    key[n + 1] = ' ';
    key[n + 2] = 0;
    return strstr(" mr mrs ms dr st jr sr rev esq etc prof ", key) != NULL;
}

static size_t skip_closers(const char *text, size_t n, size_t j) {
    while (j < n) {
        unsigned char c = (unsigned char)text[j];
        if (c == '"' || c == '\'' || c == ')' || c == ']' || c == '>') {
            j++;
            continue;
        }
        if (j + 2 < n && c == 0xe2 && (unsigned char)text[j + 1] == 0x80) {
            unsigned char t = (unsigned char)text[j + 2];
            if (t == 0x9d || t == 0x99 || t == 0xa6) {
                j += 3;
                continue;
            }
        }
        break;
    }
    return j;
}

/* If a sentence ends at i, return the index after its punctuation and closers. */
static size_t boundary_after(const char *text, size_t n, size_t i) {
    if (i >= n) {
        return 0;
    }
    unsigned char c = (unsigned char)text[i];
    size_t punct = 0;
    if (c == '.' || c == '?' || c == '!') {
        punct = 1;
    } else if (i + 2 < n && c == 0xe3 && (unsigned char)text[i + 1] == 0x80 &&
               (unsigned char)text[i + 2] == 0x82) {
        punct = 3;
    } else if (i + 2 < n && c == 0xef && (unsigned char)text[i + 1] == 0xbc &&
               ((unsigned char)text[i + 2] == 0x9f || (unsigned char)text[i + 2] == 0x81)) {
        punct = 3;
    } else if (i + 2 < n && c == 0xe2 && (unsigned char)text[i + 1] == 0x80 &&
               (unsigned char)text[i + 2] == 0xa6) {
        punct = 3;
    } else {
        return 0;
    }
    if (c == '.') {
        if (i + 1 < n && text[i + 1] >= '0' && text[i + 1] <= '9') {
            return 0;
        }
        size_t w = i;
        while (w > 0) {
            unsigned char p = (unsigned char)text[w - 1];
            if ((p >= 'A' && p <= 'Z') || (p >= 'a' && p <= 'z')) {
                w--;
                continue;
            }
            break;
        }
        if (abbrev_word(text + w, i - w)) {
            return 0;
        }
    }
    size_t j = skip_closers(text, n, i + punct);
    if (j < n && !is_space_byte((unsigned char)text[j])) {
        return 0;
    }
    return j;
}

static size_t snap_word_forward(const char *text, size_t from, size_t limit) {
    size_t s = from;
    if (s > 0 && s < limit && !is_space_byte((unsigned char)text[s]) && !is_space_byte((unsigned char)text[s - 1])) {
        while (s < limit && !is_space_byte((unsigned char)text[s])) {
            s++;
        }
        while (s < limit && is_space_byte((unsigned char)text[s])) {
            s++;
        }
        if (s >= limit) {
            s = from;
        }
    }
    while (s < limit && ((unsigned char)text[s] & 0xc0) == 0x80) {
        s++;
    }
    return s;
}

static size_t snap_word_back(const char *text, size_t from, size_t limit) {
    size_t c = limit;
    while (c > from && !is_space_byte((unsigned char)text[c - 1])) {
        c--;
    }
    if (c <= from) {
        c = limit;
        while (c > from && ((unsigned char)text[c] & 0xc0) == 0x80) {
            c--;
        }
    }
    return c;
}

/* Start of the sentence that contains pos. limit is the leftmost byte. */
static size_t sentence_start_containing(const char *text, size_t n, size_t pos, size_t limit) {
    if (pos > n) {
        pos = n;
    }
    if (limit > pos) {
        limit = pos;
    }
    size_t i = pos;
    while (i > limit) {
        i--;
        size_t bend = boundary_after(text, n, i);
        if (!bend || bend > pos) {
            continue;
        }
        size_t s = bend;
        while (s < pos && is_space_byte((unsigned char)text[s])) {
            s++;
        }
        if (s > pos) {
            continue;
        }
        return s < limit ? limit : s;
    }
    return snap_word_forward(text, limit, pos);
}

static size_t sentence_end_after(const char *text, size_t n, size_t from, size_t limit) {
    if (limit > n) {
        limit = n;
    }
    if (from > limit) {
        return limit;
    }
    for (size_t i = from; i < limit; i++) {
        size_t bend = boundary_after(text, n, i);
        if (!bend) {
            continue;
        }
        if (bend > limit) {
            bend = limit;
        }
        return bend;
    }
    return snap_word_back(text, from, limit);
}

/* Whole paragraph when it fits. Otherwise the sentences around the match. */
static void passage_span(const char *text, size_t n, size_t hit_a, size_t hit_b, size_t *wa, size_t *wb, int *cut_l,
                         int *cut_r) {
    size_t pa = 0;
    size_t pb = n;
    para_bounds(text, n, hit_a, &pa, &pb);
    if (hit_b > pb) {
        pb = hit_b > n ? n : hit_b;
    }
    size_t ta = pa;
    size_t tb = pb;
    while (ta < hit_a && ta < tb && is_space_byte((unsigned char)text[ta])) {
        ta++;
    }
    while (tb > hit_b && tb > ta && is_space_byte((unsigned char)text[tb - 1])) {
        tb--;
    }
    if (tb >= ta && tb - ta <= PASSAGE_CAP) {
        *wa = ta;
        *wb = tb;
        *cut_l = 0;
        *cut_r = 0;
        return;
    }
    size_t left = sentence_start_containing(text, n, hit_a, pa);
    size_t right = sentence_end_after(text, n, hit_b, pb);
    if (left > hit_a) {
        left = hit_a;
    }
    if (right < hit_b) {
        right = hit_b;
    }
    while (right < pb && right > left && right - left < PASSAGE_AIM) {
        size_t nr = sentence_end_after(text, n, right, pb);
        if (nr <= right) {
            break;
        }
        if (nr - left > PASSAGE_CAP) {
            break;
        }
        right = nr;
    }
    while (left > pa && right > left && right - left < PASSAGE_AIM) {
        size_t probe = left;
        while (probe > pa && is_space_byte((unsigned char)text[probe - 1])) {
            probe--;
        }
        if (probe <= pa) {
            break;
        }
        size_t nl = sentence_start_containing(text, n, probe - 1, pa);
        if (nl >= left) {
            break;
        }
        if (right - nl > PASSAGE_CAP) {
            break;
        }
        left = nl;
    }
    if (right > left && right - left > PASSAGE_CAP) {
        size_t back = PASSAGE_CAP / 3;
        left = hit_a > pa + back ? hit_a - back : pa;
        left = snap_word_forward(text, left, hit_a);
        if (left > hit_a) {
            left = hit_a;
        }
        right = left + PASSAGE_CAP;
        if (right > pb) {
            right = pb;
        }
        if (right < hit_b) {
            right = hit_b;
        }
        if (right < pb) {
            size_t snapped = snap_word_back(text, hit_b, right);
            if (snapped >= hit_b) {
                right = snapped;
            }
        }
    }
    while (left < hit_a && ((unsigned char)text[left] & 0xc0) == 0x80) {
        left++;
    }
    *cut_l = left > ta;
    *cut_r = right < tb;
    while (left < hit_a && is_space_byte((unsigned char)text[left])) {
        left++;
    }
    while (right > hit_b && is_space_byte((unsigned char)text[right - 1])) {
        right--;
    }
    *wa = left;
    *wb = right;
}

/* Copy one window. Newlines stay, so a verse paragraph keeps its lines. */
static int render_window(const char *text, size_t n, size_t a, size_t b, const Range *hits, int nh, int ellip_l,
                         int ellip_r, char **out) {
    (void)n;
    Buf sn;
    buf_init(&sn);
    if (ellip_l && buf_puts(&sn, "... ") != 0) {
        buf_free(&sn);
        return -1;
    }
    int open = 0;
    int sp = 1;
    for (size_t i = a; i < b && sn.len < 8192; i++) {
        unsigned char ch = (unsigned char)text[i];
        int hl = 0;
        for (int h = 0; h < nh; h++) {
            if (i >= hits[h].a && i < hits[h].b) {
                hl = 1;
                break;
            }
        }
        if (hl && !open) {
            if (buf_puts(&sn, "<<") != 0) {
                buf_free(&sn);
                return -1;
            }
            open = 1;
            sp = 0;
        } else if (!hl && open) {
            if (buf_puts(&sn, ">>") != 0) {
                buf_free(&sn);
                return -1;
            }
            open = 0;
        }
        if (ch == '\n') {
            if (sp == 2) {
                continue;
            }
            if (buf_putc(&sn, '\n') != 0) {
                buf_free(&sn);
                return -1;
            }
            sp = 2;
            continue;
        }
        if (ch == '\r' || ch == '\t' || ch == '\f') {
            ch = ' ';
        }
        if (ch == ' ') {
            if (sp) {
                continue;
            }
            sp = 1;
        } else {
            sp = 0;
        }
        if (buf_putc(&sn, (char)ch) != 0) {
            buf_free(&sn);
            return -1;
        }
    }
    if (open && buf_puts(&sn, ">>") != 0) {
        buf_free(&sn);
        return -1;
    }
    if (ellip_r && buf_puts(&sn, " ...") != 0) {
        buf_free(&sn);
        return -1;
    }
    if (!sn.data && buf_puts(&sn, "") != 0) {
        return -1;
    }
    *out = sn.data ? sn.data : xstrdup("");
    sn.data = NULL;
    sn.len = 0;
    sn.cap = 0;
    return *out ? 0 : -1;
}

void marks_free(Mark *m, int n) {
    if (!m) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(m[i].text);
    }
    free(m);
}

void passages_free(Passage *p, int n) {
    if (!p) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(p[i].text);
        free(p[i].section);
    }
    free(p);
}

/* Heading stack open at offset, and the page mark in effect there. */
static int place_at(const Mark *marks, int nmarks, uint32_t offset, int *page, char **section) {
    typedef struct Frame {
        uint8_t level;
        const char *text;
    } Frame;
    Frame stack[8];
    int sp = 0;
    int pg = 0;
    for (int i = 0; i < nmarks; i++) {
        if (marks[i].offset > offset) {
            break;
        }
        if (marks[i].kind == MARK_PAGE) {
            if (marks[i].page > 0) {
                pg = (int)marks[i].page;
            }
            continue;
        }
        if (marks[i].kind != MARK_HEAD || !marks[i].text || !marks[i].text[0]) {
            continue;
        }
        uint8_t lv = marks[i].level ? marks[i].level : 1;
        while (sp > 0 && stack[sp - 1].level >= lv) {
            sp--;
        }
        if (sp == 8) {
            memmove(stack, stack + 1, 7 * sizeof(Frame));
            sp = 7;
        }
        stack[sp].level = lv;
        stack[sp].text = marks[i].text;
        sp++;
    }
    *page = pg;
    *section = NULL;
    if (sp == 0) {
        return 0;
    }
    Buf b;
    buf_init(&b);
    for (int i = 0; i < sp; i++) {
        if (i && buf_puts(&b, " · ") != 0) {
            buf_free(&b);
            return -1;
        }
        if (buf_puts(&b, stack[i].text) != 0) {
            buf_free(&b);
            return -1;
        }
    }
    if (b.len > 180) {
        size_t keep = utf8_prefix_bytes(b.data, b.len, 180);
        b.len = keep;
        b.data[keep] = 0;
    }
    *section = b.data;
    return *section ? 0 : -1;
}

/* A term match at a folded offset. *end is the folded end, extended for a prefix. */
static int term_at(const char *cf, size_t cfn, const uint32_t *src_at, const char *text, size_t n, size_t at,
                   const QTerm *term, size_t *end) {
    if (!term->s || term->len == 0 || at + term->len > cfn) {
        return 0;
    }
    if (memcmp(cf + at, term->s, term->len) != 0) {
        return 0;
    }
    size_t e = at + term->len;
    int cjk = term_is_cjk(term->s, term->len);
    if (!cjk && at > 0 && src_at) {
        size_t pi = src_at[at - 1];
        uint32_t cp = 0;
        if (utf8_next(text, n, &pi, &cp) && cp_is_word(cp)) {
            return 0;
        }
    }
    if (term->prefix) {
        while (e < cfn) {
            size_t j = e;
            uint32_t cp = 0;
            if (!utf8_next(cf, cfn, &j, &cp)) {
                break;
            }
            if (!cp_is_word(cp) && !cp_is_cjk(cp)) {
                break;
            }
            e = j;
        }
    } else if (!cjk && e < cfn && src_at) {
        size_t pi = src_at[e];
        uint32_t cp = 0;
        if (utf8_next(text, n, &pi, &cp) && (cp_is_word(cp) || cp_is_cjk(cp))) {
            return 0;
        }
    }
    *end = e;
    return 1;
}

static size_t skip_gap(const char *cf, size_t cfn, size_t i) {
    while (i < cfn) {
        size_t j = i;
        uint32_t cp = 0;
        if (!utf8_next(cf, cfn, &j, &cp)) {
            break;
        }
        if (cp_is_letter(cp) || cp_is_digit(cp) || cp_is_cjk(cp)) {
            break;
        }
        i = j;
    }
    return i;
}

static size_t next_cp(const char *cf, size_t cfn, size_t i) {
    size_t j = i;
    uint32_t cp = 0;
    if (!utf8_next(cf, cfn, &j, &cp)) {
        return i;
    }
    return j;
}

static int farthest_hit(const Range *hits, int nh) {
    int m = 0;
    for (int i = 1; i < nh; i++) {
        if (hits[i].a >= hits[m].a) {
            m = i;
        }
    }
    return m;
}

/* Count every place. Keep the earliest GROVE_OFFSET_MAX spans. */
static void add_hit(Range *hits, int *nh, int *far, int *nmatches, const uint32_t *src_at, size_t cfn, size_t n,
                    size_t start, size_t end, int term) {
    if (!src_at || end <= start) {
        return;
    }
    size_t sa = src_at[start];
    size_t sb = end < cfn ? src_at[end] : n;
    if (sb <= sa || sa > UINT32_MAX || sb - sa > UINT32_MAX) {
        return;
    }
    (*nmatches)++;
    Range r;
    r.a = sa;
    r.b = sb;
    r.term = term;
    if (*nh < GROVE_OFFSET_MAX) {
        hits[*nh] = r;
        if (*nh == 0 || r.a >= hits[*far].a) {
            *far = *nh;
        }
        (*nh)++;
        return;
    }
    if (r.a >= hits[*far].a) {
        return;
    }
    hits[*far] = r;
    *far = farthest_hit(hits, *nh);
}

static void add_term_hits(Range *hits, int *nh, int *far, int *nmatches, const char *cf, size_t cfn,
                          const uint32_t *src_at, const char *text, size_t n, const QTerm *term, int term_i) {
    const char *from = cf;
    size_t left = cfn;
    while (left > 0 && term->s && term->len) {
        const char *p = find_bytes(from, left, term->s, term->len);
        if (!p) {
            break;
        }
        size_t start = (size_t)(p - cf);
        size_t end = 0;
        if (term_at(cf, cfn, src_at, text, n, start, term, &end)) {
            add_hit(hits, nh, far, nmatches, src_at, cfn, n, start, end, term_i);
        }
        size_t step = (size_t)(p - from) + 1;
        if (step > left) {
            break;
        }
        from += step;
        left -= step;
    }
}

/* Words of one quoted phrase, in order, with only punctuation between them. */
static void add_phrase_hits(Range *hits, int *nh, int *far, int *nmatches, const char *cf, size_t cfn,
                            const uint32_t *src_at, const char *text, size_t n, const QTerm *terms, int lo, int hi) {
    if (hi <= lo || !terms[lo].s) {
        return;
    }
    const char *from = cf;
    size_t left = cfn;
    while (left > 0) {
        const char *p = find_bytes(from, left, terms[lo].s, terms[lo].len);
        if (!p) {
            break;
        }
        size_t start = (size_t)(p - cf);
        size_t end = 0;
        size_t prev = start;
        int ok = term_at(cf, cfn, src_at, text, n, start, &terms[lo], &end);
        for (int k = lo + 1; ok && k < hi; k++) {
            size_t at;
            if (term_is_cjk(terms[k - 1].s, terms[k - 1].len) && term_is_cjk(terms[k].s, terms[k].len)) {
                at = next_cp(cf, cfn, prev);
            } else {
                at = skip_gap(cf, cfn, end);
            }
            size_t e2 = 0;
            if (!term_at(cf, cfn, src_at, text, n, at, &terms[k], &e2)) {
                ok = 0;
                break;
            }
            prev = at;
            end = e2;
        }
        if (ok) {
            add_hit(hits, nh, far, nmatches, src_at, cfn, n, start, end, lo);
        }
        size_t step = (size_t)(p - from) + 1;
        if (step > left) {
            break;
        }
        from += step;
        left -= step;
    }
}

int make_passages(const char *text, size_t n, const QTerm *terms, int nterms, const Mark *marks, int nmarks,
                  int paged, Passage **out, int *nout, MatchOff **offsets, int *noffsets, int *nmatches,
                  uint32_t after, int resume, int *more_out) {
    *out = NULL;
    *nout = 0;
    *offsets = NULL;
    *noffsets = 0;
    *nmatches = 0;
    if (more_out) {
        *more_out = 0;
    }
    if (!text) {
        text = "";
        n = 0;
    }
    Buf cf;
    buf_init(&cf);
    uint32_t *src_at = NULL;
    if (fold_text(text, n, &cf, &src_at) != 0) {
        buf_free(&cf);
        free(src_at);
        return -1;
    }
    Range *hits = calloc((size_t)GROVE_OFFSET_MAX, sizeof(Range));
    if (!hits) {
        buf_free(&cf);
        free(src_at);
        return -1;
    }
    int nh = 0;
    int far = 0;
    int nfound = 0;
    for (int t = 0; t < nterms;) {
        if (!terms) {
            break;
        }
        if (terms[t].phrase == 0) {
            add_term_hits(hits, &nh, &far, &nfound, cf.data, cf.len, src_at, text, n, &terms[t], t);
            t++;
            continue;
        }
        int id = terms[t].phrase;
        int u = t + 1;
        while (u < nterms && terms[u].phrase == id) {
            u++;
        }
        add_phrase_hits(hits, &nh, &far, &nfound, cf.data, cf.len, src_at, text, n, terms, t, u);
        t = u;
    }

    typedef struct Win {
        size_t a, b, at;
        int cut_l, cut_r;
    } Win;
    Win wins[GROVE_PASSAGE_PAGE];
    int nw = 0;
    int more_flag = 0;
    if (nh == 0) {
        if (n && !resume) {
            passage_span(text, n, 0, 0, &wins[0].a, &wins[0].b, &wins[0].cut_l, &wins[0].cut_r);
            wins[0].at = 0;
            if (wins[0].b > wins[0].a) {
                nw = 1;
            }
        }
    } else {
        qsort(hits, (size_t)nh, sizeof(Range), range_cmp);
        typedef struct Cluster {
            size_t a, b, at;
        } Cluster;
        Cluster cs[GROVE_PASSAGE_PAGE];
        int nc = 0;
        int skipping = 0;
        size_t skip_a = 0;
        for (int i = 0; i < nh; i++) {
            if (skipping) {
                size_t pa = 0;
                size_t pb = 0;
                para_bounds(text, n, skip_a, &pa, &pb);
                if (hits[i].a >= pa && hits[i].a < pb && hits[i].a >= skip_a && hits[i].a - skip_a <= PASSAGE_CAP) {
                    continue;
                }
                skipping = 0;
            }
            if (nc) {
                size_t pa = 0;
                size_t pb = 0;
                para_bounds(text, n, cs[nc - 1].at, &pa, &pb);
                if (hits[i].a >= pa && hits[i].a < pb && hits[i].a >= cs[nc - 1].a &&
                    hits[i].a - cs[nc - 1].a <= PASSAGE_CAP) {
                    if (hits[i].b > cs[nc - 1].b) {
                        cs[nc - 1].b = hits[i].b;
                    }
                    continue;
                }
            }
            if (resume && hits[i].a <= after) {
                skipping = 1;
                skip_a = hits[i].a;
                continue;
            }
            if (nc >= GROVE_PASSAGE_PAGE) {
                more_flag = 1;
                break;
            }
            cs[nc].a = hits[i].a;
            cs[nc].b = hits[i].b;
            cs[nc].at = hits[i].a;
            nc++;
        }
        for (int i = 0; i < nc; i++) {
            passage_span(text, n, cs[i].a, cs[i].b, &wins[i].a, &wins[i].b, &wins[i].cut_l, &wins[i].cut_r);
            wins[i].at = cs[i].at;
        }
        nw = nc;
    }

    buf_free(&cf);
    free(src_at);
    src_at = NULL;

    size_t count = 1;
    if (nw > 0 && nw <= GROVE_PASSAGE_PAGE) {
        count = (size_t)nw;
    }
    Passage *ps = calloc(count, sizeof(Passage));
    if (!ps) {
        free(hits);
        return -1;
    }
    for (int w = 0; w < nw; w++) {
        if (render_window(text, n, wins[w].a, wins[w].b, hits, nh, wins[w].cut_l, wins[w].cut_r, &ps[w].text) !=
            0) {
            passages_free(ps, w + 1);
            free(hits);
            return -1;
        }
        ps[w].offset = (uint32_t)wins[w].at;
        if (nmarks > 0) {
            if (place_at(marks, nmarks, ps[w].offset, &ps[w].page, &ps[w].section) != 0) {
                passages_free(ps, w + 1);
                free(hits);
                return -1;
            }
        } else {
            ps[w].page = page_at(text, wins[w].at, paged);
        }
    }
    MatchOff *mo = NULL;
    if (nh > 0) {
        mo = calloc((size_t)nh, sizeof(MatchOff));
        if (!mo) {
            passages_free(ps, nw);
            free(hits);
            return -1;
        }
        for (int i = 0; i < nh; i++) {
            mo[i].start = (uint32_t)hits[i].a;
            mo[i].len = (uint32_t)(hits[i].b - hits[i].a);
            mo[i].term = hits[i].term;
        }
    }
    free(hits);
    if (more_out) {
        *more_out = more_flag;
    }
    *offsets = mo;
    *noffsets = nh;
    *nmatches = nfound;
    *out = nw ? ps : NULL;
    if (!nw) {
        free(ps);
    }
    *nout = nw;
    return 0;
}
