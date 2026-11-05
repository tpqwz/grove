#include "persona.h"

#include "fetch.h"
#include "os.h"
#include "text.h"

#include "sqlite3.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* shortcut: a summary stops at 1200 bytes, aliases at 8, attributed works at 24.
 * Raise the caps if a real page is cut off. */
enum {
    PERSONA_NAME_MAX = 200,
    PERSONA_BODY_MAX = 1 << 20,
    PERSONA_PHOTO_MAX = 400 * 1024,
    PERSONA_SUMMARY_MAX = 1200,
    PERSONA_ALIAS_MAX = 8,
    PERSONA_WORK_MAX = 24,
    PERSONA_OCC_MAX = 4
};

static const char *HOST_WIKIDATA = "www.wikidata.org";
static const char *HOST_QUERY = "query.wikidata.org";
static const char *HOST_WIKI = "en.wikipedia.org";
static const char *HOST_UPLOAD = "upload.wikimedia.org";
static const char HOST_THUMB[] = "thumb.wikimedia.org";

void persona_profile_free(PersonaProfile *p) {
    int i;
    if (!p) {
        return;
    }
    free(p->qid);
    free(p->description);
    free(p->summary);
    free(p->born);
    free(p->died);
    free(p->occupation);
    free(p->wikipedia);
    free(p->photo_url);
    free(p->photo);
    free(p->photo_type);
    for (i = 0; i < p->alias_n; i++) {
        free(p->aliases ? p->aliases[i] : NULL);
    }
    free(p->aliases);
    for (i = 0; i < p->work_n; i++) {
        if (p->works) {
            free(p->works[i].title);
            free(p->works[i].year);
            free(p->works[i].url);
        }
    }
    free(p->works);
    memset(p, 0, sizeof *p);
}

static void trim_span(const char *s, const char **out, size_t *n) {
    if (!s) {
        s = "";
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    *n = strlen(s);
    while (*n && (s[*n - 1] == ' ' || s[*n - 1] == '\t')) {
        (*n)--;
    }
    *out = s;
}

static int name_copy(const char *name, char *dst, size_t cap) {
    const char *s = NULL;
    size_t n = 0;
    trim_span(name, &s, &n);
    if (n == 0 || n > PERSONA_NAME_MAX || n >= cap) {
        return -1;
    }
    memcpy(dst, s, n);
    dst[n] = 0;
    return 0;
}

static int fold_into(const char *s, char *dst, size_t cap) {
    size_t n = s ? strlen(s) : 0;
    size_t i = 0;
    size_t w = 0;
    while (i < n) {
        uint32_t cp = 0;
        char enc[4];
        int k;
        if (!utf8_next(s, n, &i, &cp)) {
            break;
        }
        cp = casefold_cp(cp);
        k = utf8_encode(cp, enc);
        if (k < 1 || w + (size_t)k >= cap) {
            return -1;
        }
        memcpy(dst + w, enc, (size_t)k);
        w += (size_t)k;
    }
    dst[w] = 0;
    return 0;
}

static int same_fold(const char *a, const char *b) {
    char fa[1024];
    char fb[1024];
    if (!a || !b || fold_into(a, fa, sizeof fa) != 0 || fold_into(b, fb, sizeof fb) != 0) {
        return 0;
    }
    return strcmp(fa, fb) == 0;
}

static char *clean_text(const char *s, size_t max_bytes) {
    size_t n;
    size_t w = 0;
    size_t b = 0;
    char *d;
    if (!s || !s[0]) {
        return NULL;
    }
    n = strlen(s);
    if (n > max_bytes) {
        n = utf8_prefix_bytes(s, n, max_bytes);
    }
    d = malloc(n + 1);
    if (!d) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        } else if (c < 0x20 || c == 0x7f) {
            continue;
        }
        d[w++] = (char)c;
    }
    while (w && d[w - 1] == ' ') {
        w--;
    }
    while (b < w && d[b] == ' ') {
        b++;
    }
    if (b) {
        memmove(d, d + b, w - b);
        w -= b;
    }
    d[w] = 0;
    if (!w) {
        free(d);
        return NULL;
    }
    return d;
}

static int qid_ok(const char *s) {
    size_t i;
    if (!s || s[0] != 'Q' || s[1] < '0' || s[1] > '9') {
        return 0;
    }
    for (i = 1; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9' || i > 16) {
            return 0;
        }
    }
    return 1;
}

static int year_of(const char *s, char *out, size_t cap) {
    int neg;
    unsigned long y = 0;
    const char *p;
    if (!s || (s[0] != '+' && s[0] != '-') || cap < 16) {
        return -1;
    }
    neg = s[0] == '-';
    p = s + 1;
    while (*p == '0') {
        p++;
    }
    if (*p < '1' || *p > '9') {
        return -1;
    }
    while (*p >= '0' && *p <= '9') {
        if (y > 999999) {
            return -1;
        }
        y = y * 10 + (unsigned long)(*p - '0');
        p++;
    }
    if (y == 0 || *p != '-' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9') {
        return -1;
    }
    if (snprintf(out, cap, neg ? "%lu BCE" : "%lu", y) < 0) {
        return -1;
    }
    return 0;
}

static char *year_dup(const char *s) {
    char buf[32];
    if (year_of(s, buf, sizeof buf) != 0) {
        return NULL;
    }
    return xstrdup(buf);
}

static int desc_score(const char *lower) {
    int score = 0;
    if (!lower) {
        return 0;
    }
    if (strstr(lower, "poet") || strstr(lower, "philosoph") || strstr(lower, "histor") || strstr(lower, "writer") ||
        strstr(lower, "author") || strstr(lower, "dramat") || strstr(lower, "emperor") || strstr(lower, "stoic") ||
        strstr(lower, "orator")) {
        score += 3;
    }
    if (strstr(lower, "disambiguation") || strstr(lower, "crater") || strstr(lower, "surname") ||
        strstr(lower, "given name")) {
        score -= 6;
    }
    return score;
}

static char *ascii_lower_dup(const char *s) {
    size_t n = s ? strlen(s) : 0;
    size_t i;
    char *d;
    if (n > 400) {
        n = 400;
    }
    d = malloc(n + 1);
    if (!d) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        d[i] = (char)c;
    }
    d[n] = 0;
    return d;
}

static char *keep_https(const char *url, const char *host) {
    const char *s = NULL;
    size_t n = 0;
    char *t;
    const char *hosts[1];
    trim_span(url, &s, &n);
    if (n == 0 || n > (size_t)FETCH_URL_MAX) {
        return NULL;
    }
    t = xstrndup(s, n);
    hosts[0] = host;
    if (!t || !fetch_https_allowed(t, hosts, 1)) {
        free(t);
        return NULL;
    }
    return t;
}

/* Wikipedia's summary API serves the portrait from thumb.wikimedia.org.
 * The same /wikipedia/ path on upload.wikimedia.org is the file Grove fetches.
 * A lookalike host, another scheme, or a path outside /wikipedia/ is dropped. */
static int thumb_path(const char *s, size_t n, size_t *path_at) {
    size_t hlen = sizeof HOST_THUMB - 1;
    size_t i;
    if (n < 8 + hlen || memcmp(s, "https://", 8) != 0) {
        return 0;
    }
    for (i = 0; i < hlen; i++) {
        unsigned char c = (unsigned char)s[8 + i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        if (c != (unsigned char)HOST_THUMB[i]) {
            return 0;
        }
    }
    i = 8 + hlen;
    if (i + 4 <= n && memcmp(s + i, ":443", 4) == 0) {
        size_t j = i + 4;
        if (j < n && s[j] != '/' && s[j] != '?' && s[j] != '#') {
            return 0;
        }
        i = j;
    }
    if (i >= n || s[i] != '/' || n - i < 11 || memcmp(s + i, "/wikipedia/", 11) != 0) {
        return 0;
    }
    *path_at = i;
    return 1;
}

static char *photo_on_upload(const char *url) {
    const char *s = NULL;
    size_t n = 0;
    char *copy;
    char *kept;
    size_t path = 0;
    size_t path_n;
    size_t i;
    static const char prefix[] = "https://upload.wikimedia.org";
    char *built;
    trim_span(url, &s, &n);
    if (n == 0 || n > (size_t)FETCH_URL_MAX) {
        return NULL;
    }
    copy = xstrndup(s, n);
    if (!copy) {
        return NULL;
    }
    kept = keep_https(copy, HOST_UPLOAD);
    free(copy);
    if (kept) {
        return kept;
    }
    if (!thumb_path(s, n, &path)) {
        return NULL;
    }
    path_n = n - path;
    for (i = 0; i < path_n; i++) {
        if (s[path + i] == '?' || s[path + i] == '#') {
            path_n = i;
            break;
        }
    }
    if (path_n == 0 || path_n > (size_t)FETCH_URL_MAX - (sizeof prefix - 1)) {
        return NULL;
    }
    built = malloc((sizeof prefix - 1) + path_n + 1);
    if (!built) {
        return NULL;
    }
    memcpy(built, prefix, sizeof prefix - 1);
    memcpy(built + (sizeof prefix - 1), s + path, path_n);
    built[(sizeof prefix - 1) + path_n] = 0;
    kept = keep_https(built, HOST_UPLOAD);
    free(built);
    return kept;
}

typedef struct Jdoc {
    sqlite3 *db;
    sqlite3_stmt *one;
    sqlite3_stmt *len;
} Jdoc;

static void jdoc_close(Jdoc *j) {
    if (!j) {
        return;
    }
    sqlite3_finalize(j->one);
    sqlite3_finalize(j->len);
    sqlite3_close(j->db);
    memset(j, 0, sizeof *j);
}

static int jdoc_open(Jdoc *j) {
    memset(j, 0, sizeof *j);
    if (sqlite3_open(":memory:", &j->db) != SQLITE_OK) {
        sqlite3_close(j->db);
        j->db = NULL;
        return -1;
    }
    if (sqlite3_prepare_v2(j->db, "SELECT json_extract(?1, ?2)", -1, &j->one, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(j->db, "SELECT json_array_length(?1, ?2)", -1, &j->len, NULL) != SQLITE_OK) {
        jdoc_close(j);
        return -1;
    }
    return 0;
}

static int jbind(sqlite3_stmt *st, const char *doc, int docn, const char *path) {
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (sqlite3_bind_text(st, 1, doc, docn, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC) != SQLITE_OK) {
        return -1;
    }
    return 0;
}

static char *jtext(Jdoc *j, const char *doc, int docn, const char *path) {
    const unsigned char *t;
    char *out = NULL;
    if (!doc || docn <= 0 || jbind(j->one, doc, docn, path) != 0) {
        return NULL;
    }
    if (sqlite3_step(j->one) == SQLITE_ROW && sqlite3_column_type(j->one, 0) == SQLITE_TEXT) {
        t = sqlite3_column_text(j->one, 0);
        if (t && t[0]) {
            out = xstrdup((const char *)t);
        }
    }
    sqlite3_reset(j->one);
    return out;
}

static int jcount(Jdoc *j, const char *doc, int docn, const char *path) {
    int n = 0;
    if (!doc || docn <= 0 || jbind(j->len, doc, docn, path) != 0) {
        return 0;
    }
    if (sqlite3_step(j->len) == SQLITE_ROW && sqlite3_column_type(j->len, 0) == SQLITE_INTEGER) {
        n = sqlite3_column_int(j->len, 0);
    }
    sqlite3_reset(j->len);
    return n < 0 ? 0 : n;
}

static int jnumber(Jdoc *j, const char *doc, int docn, const char *path, int *out) {
    int rc = -1;
    if (!doc || docn <= 0 || jbind(j->one, doc, docn, path) != 0) {
        return -1;
    }
    if (sqlite3_step(j->one) == SQLITE_ROW && sqlite3_column_type(j->one, 0) == SQLITE_INTEGER) {
        *out = sqlite3_column_int(j->one, 0);
        rc = 0;
    }
    sqlite3_reset(j->one);
    return rc;
}

static int path_at(char *dst, size_t cap, const char *fmt, int i) {
    int n = snprintf(dst, cap, fmt, i);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int best_hit(Jdoc *j, const char *doc, int docn, const char *name, char **qid, char **label, char **desc) {
    int n = jcount(j, doc, docn, "$.search");
    int best = -1000;
    int found = 0;
    int i;
    char folded_name[1024];
    if (n > 8) {
        n = 8;
    }
    if (fold_into(name, folded_name, sizeof folded_name) != 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        char path[80];
        char *id = NULL;
        char *lab = NULL;
        char *ds = NULL;
        char *low = NULL;
        char *cut = NULL;
        char folded_lab[1024];
        int score = 0;
        size_t ln;
        if (path_at(path, sizeof path, "$.search[%d].id", i) != 0) {
            return -1;
        }
        id = jtext(j, doc, docn, path);
        if (path_at(path, sizeof path, "$.search[%d].label", i) != 0) {
            free(id);
            return -1;
        }
        lab = jtext(j, doc, docn, path);
        if (path_at(path, sizeof path, "$.search[%d].description", i) != 0) {
            free(id);
            free(lab);
            return -1;
        }
        ds = jtext(j, doc, docn, path);
        if (!id || !qid_ok(id) || !lab) {
            free(id);
            free(lab);
            free(ds);
            continue;
        }
        ln = strlen(lab);
        if (ln > 400) {
            ln = utf8_prefix_bytes(lab, ln, 400);
        }
        cut = xstrndup(lab, ln);
        if (!cut || fold_into(cut, folded_lab, sizeof folded_lab) != 0) {
            free(id);
            free(lab);
            free(ds);
            free(cut);
            return -1;
        }
        free(cut);
        if (strcmp(folded_lab, folded_name) == 0) {
            score += 6;
        } else if (folded_name[0] && strstr(folded_lab, folded_name)) {
            score += 2;
        }
        low = ascii_lower_dup(ds ? ds : "");
        if (!low) {
            free(id);
            free(lab);
            free(ds);
            return -1;
        }
        score += desc_score(low);
        free(low);
        if (!found || score > best) {
            found = 1;
            best = score;
            free(*qid);
            free(*label);
            free(*desc);
            *qid = id;
            *label = lab;
            *desc = ds;
            id = NULL;
            lab = NULL;
            ds = NULL;
        }
        free(id);
        free(lab);
        free(ds);
    }
    if (!found || best < 2) {
        free(*qid);
        free(*label);
        free(*desc);
        *qid = NULL;
        *label = NULL;
        *desc = NULL;
    }
    return 0;
}

static int add_alias(PersonaProfile *p, char *alias) {
    int i;
    if (!alias) {
        return 0;
    }
    if (p->alias_n >= PERSONA_ALIAS_MAX) {
        free(alias);
        return 0;
    }
    for (i = 0; i < p->alias_n; i++) {
        if (same_fold(p->aliases[i], alias)) {
            free(alias);
            return 0;
        }
    }
    if (grow_alloc((void **)&p->aliases, (size_t)p->alias_n + 1, sizeof(char *)) != 0) {
        free(alias);
        return -1;
    }
    p->aliases[p->alias_n++] = alias;
    return 0;
}

static int add_work(PersonaProfile *p, char *title, char *year, char *url) {
    int i;
    if (!title) {
        free(year);
        free(url);
        return 0;
    }
    if (qid_ok(title) || p->work_n >= PERSONA_WORK_MAX) {
        free(title);
        free(year);
        free(url);
        return 0;
    }
    for (i = 0; i < p->work_n; i++) {
        if (same_fold(p->works[i].title, title)) {
            free(title);
            free(year);
            free(url);
            return 0;
        }
    }
    if (grow_alloc((void **)&p->works, (size_t)p->work_n + 1, sizeof(PersonaPubWork)) != 0) {
        free(title);
        free(year);
        free(url);
        return -1;
    }
    p->works[p->work_n].title = title;
    p->works[p->work_n].year = year;
    p->works[p->work_n].url = url;
    p->work_n++;
    return 0;
}

static int fill_entity(Jdoc *j, const char *doc, int docn, const char *name, PersonaProfile *p) {
    char path[96];
    int n;
    int i;
    int nw;
    if (!doc || !p->qid) {
        return 0;
    }
    nw = snprintf(path, sizeof path, "$.entities.%s.aliases.en", p->qid);
    if (nw < 0 || (size_t)nw >= sizeof path) {
        return -1;
    }
    n = jcount(j, doc, docn, path);
    if (n > 16) {
        n = 16;
    }
    for (i = 0; i < n; i++) {
        char item[112];
        char *value;
        char *clean;
        int iw = snprintf(item, sizeof item, "$.entities.%s.aliases.en[%d].value", p->qid, i);
        if (iw < 0 || (size_t)iw >= sizeof item) {
            return -1;
        }
        value = jtext(j, doc, docn, item);
        clean = clean_text(value, PERSONA_NAME_MAX);
        free(value);
        if (clean && same_fold(clean, name)) {
            free(clean);
            continue;
        }
        if (add_alias(p, clean) != 0) {
            return -1;
        }
    }
    return 0;
}

static int fill_summary(Jdoc *j, const char *doc, int docn, const char *search_desc, PersonaProfile *p) {
    char *extract = NULL;
    char *description = NULL;
    char *thumb = NULL;
    char *page = NULL;
    if (doc) {
        extract = jtext(j, doc, docn, "$.extract");
        description = jtext(j, doc, docn, "$.description");
        thumb = jtext(j, doc, docn, "$.thumbnail.source");
        page = jtext(j, doc, docn, "$.content_urls.desktop.page");
    }
    p->summary = clean_text(extract, PERSONA_SUMMARY_MAX);
    p->description = clean_text(description, 300);
    if (!p->description) {
        p->description = clean_text(search_desc, 300);
    }
    p->wikipedia = keep_https(page, HOST_WIKI);
    p->photo_url = photo_on_upload(thumb);
    free(extract);
    free(description);
    free(thumb);
    free(page);
    return 0;
}

static int fill_sparql(Jdoc *j, const char *doc, int docn, PersonaProfile *p) {
    int n;
    int i;
    char *occs[PERSONA_OCC_MAX];
    int occ_n = 0;
    char joined[400];
    size_t w = 0;
    if (!doc) {
        return 0;
    }
    memset(occs, 0, sizeof occs);
    n = jcount(j, doc, docn, "$.results.bindings");
    if (n > 40) {
        n = 40;
    }
    for (i = 0; i < n; i++) {
        char path[80];
        char *birth = NULL;
        char *death = NULL;
        char *occ = NULL;
        char *title = NULL;
        char *date = NULL;
        char *article = NULL;
        char *clean_occ;
        char *clean_title;
        int k;
        if (!p->born && path_at(path, sizeof path, "$.results.bindings[%d].birth.value", i) == 0) {
            birth = jtext(j, doc, docn, path);
            p->born = year_dup(birth);
            free(birth);
        }
        if (!p->died && path_at(path, sizeof path, "$.results.bindings[%d].death.value", i) == 0) {
            death = jtext(j, doc, docn, path);
            p->died = year_dup(death);
            free(death);
        }
        if (path_at(path, sizeof path, "$.results.bindings[%d].occLabel.value", i) == 0) {
            occ = jtext(j, doc, docn, path);
        }
        clean_occ = clean_text(occ, 80);
        free(occ);
        if (clean_occ && !qid_ok(clean_occ) && occ_n < PERSONA_OCC_MAX) {
            int seen = 0;
            for (k = 0; k < occ_n; k++) {
                if (same_fold(occs[k], clean_occ)) {
                    seen = 1;
                }
            }
            if (seen) {
                free(clean_occ);
            } else {
                occs[occ_n++] = clean_occ;
            }
        } else {
            free(clean_occ);
        }
        if (path_at(path, sizeof path, "$.results.bindings[%d].workLabel.value", i) == 0) {
            title = jtext(j, doc, docn, path);
        }
        if (path_at(path, sizeof path, "$.results.bindings[%d].date.value", i) == 0) {
            date = jtext(j, doc, docn, path);
        }
        if (path_at(path, sizeof path, "$.results.bindings[%d].article.value", i) == 0) {
            article = jtext(j, doc, docn, path);
        }
        clean_title = clean_text(title, PERSONA_NAME_MAX);
        free(title);
        if (add_work(p, clean_title, year_dup(date), keep_https(article, HOST_WIKI)) != 0) {
            free(date);
            free(article);
            for (k = 0; k < occ_n; k++) {
                free(occs[k]);
            }
            return -1;
        }
        free(date);
        free(article);
    }
    for (i = 0; i < occ_n; i++) {
        size_t ln = strlen(occs[i]);
        if (w && w + 2 < sizeof joined) {
            memcpy(joined + w, ", ", 2);
            w += 2;
        }
        if (w + ln >= sizeof joined) {
            free(occs[i]);
            continue;
        }
        memcpy(joined + w, occs[i], ln);
        w += ln;
        free(occs[i]);
    }
    joined[w] = 0;
    if (w) {
        p->occupation = xstrdup(joined);
        if (!p->occupation) {
            return -1;
        }
    }
    return 0;
}

static int take_body(const char *s, size_t n, const char **doc, int *docn) {
    if (!s || n == 0 || n > (size_t)PERSONA_BODY_MAX) {
        *doc = NULL;
        *docn = 0;
        return 0;
    }
    *doc = s;
    *docn = (int)n;
    return 0;
}

int persona_profile_parse(const char *name, const char *search, size_t search_n, const char *entity, size_t entity_n,
                          const char *summary, size_t summary_n, const char *sparql, size_t sparql_n,
                          PersonaProfile *out, Err *e) {
    char who[PERSONA_NAME_MAX + 1];
    Jdoc j;
    const char *search_doc = NULL;
    const char *entity_doc = NULL;
    const char *summary_doc = NULL;
    const char *sparql_doc = NULL;
    int search_dn = 0;
    int entity_dn = 0;
    int summary_dn = 0;
    int sparql_dn = 0;
    char *qid = NULL;
    char *label = NULL;
    char *search_desc = NULL;
    if (!out) {
        err_set(e, "out of memory");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (name_copy(name, who, sizeof who) != 0) {
        return 0;
    }
    take_body(search, search_n, &search_doc, &search_dn);
    take_body(entity, entity_n, &entity_doc, &entity_dn);
    take_body(summary, summary_n, &summary_doc, &summary_dn);
    take_body(sparql, sparql_n, &sparql_doc, &sparql_dn);
    if (jdoc_open(&j) != 0) {
        err_set(e, "could not read that profile");
        return -1;
    }
    if (search_doc && best_hit(&j, search_doc, search_dn, who, &qid, &label, &search_desc) != 0) {
        jdoc_close(&j);
        free(qid);
        free(label);
        free(search_desc);
        err_set(e, "out of memory");
        return -1;
    }
    if (!qid) {
        jdoc_close(&j);
        free(label);
        free(search_desc);
        return 0;
    }
    out->qid = qid;
    out->state = PERSONA_READY;
    if (fill_entity(&j, entity_doc, entity_dn, who, out) != 0 ||
        fill_summary(&j, summary_doc, summary_dn, search_desc, out) != 0 ||
        fill_sparql(&j, sparql_doc, sparql_dn, out) != 0) {
        persona_profile_free(out);
        jdoc_close(&j);
        free(label);
        free(search_desc);
        err_set(e, "out of memory");
        return -1;
    }
    jdoc_close(&j);
    free(label);
    free(search_desc);
    return 0;
}

static int pct(Buf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            if (buf_putc(b, c) != 0) {
                return -1;
            }
        } else {
            char tmp[4];
            tmp[0] = '%';
            tmp[1] = hex[c >> 4];
            tmp[2] = hex[c & 15];
            tmp[3] = 0;
            if (buf_puts(b, tmp) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int finish_url(Buf *b, char **url) {
    if (b->len == 0 || b->len > (size_t)FETCH_URL_MAX || buf_putc(b, 0) != 0) {
        buf_free(b);
        return -1;
    }
    *url = b->data;
    return 0;
}

int persona_lookup_url(int kind, const char *arg, char **url, Err *e) {
    Buf b;
    char who[PERSONA_NAME_MAX + 1];
    const char *s = NULL;
    size_t n = 0;
    if (url) {
        *url = NULL;
    }
    if (!url) {
        err_set(e, "out of memory");
        return -1;
    }
    buf_init(&b);
    if (kind == PERSONA_FIND) {
        if (name_copy(arg, who, sizeof who) != 0 ||
            buf_puts(&b, "https://www.wikidata.org/w/api.php?action=wbsearchentities&search=") != 0 ||
            pct(&b, who, strlen(who)) != 0 || buf_puts(&b, "&language=en&type=item&limit=8&format=json") != 0 ||
            finish_url(&b, url) != 0) {
            err_set(e, "that name has no public lookup");
            return -1;
        }
        return 0;
    }
    if (kind == PERSONA_ENTITY || kind == PERSONA_SPARQL) {
        trim_span(arg, &s, &n);
        if (n == 0 || n >= sizeof who) {
            err_set(e, "that name has no public lookup");
            return -1;
        }
        memcpy(who, s, n);
        who[n] = 0;
        if (!qid_ok(who)) {
            err_set(e, "that name has no public lookup");
            return -1;
        }
        if (kind == PERSONA_ENTITY) {
            if (buf_puts(&b, "https://www.wikidata.org/w/api.php?action=wbgetentities&ids=") != 0 ||
                buf_puts(&b, who) != 0 ||
                buf_puts(&b, "&props=sitelinks%7Caliases&sitefilter=enwiki&languages=en&format=json") != 0 ||
                finish_url(&b, url) != 0) {
                err_set(e, "that name has no public lookup");
                return -1;
            }
            return 0;
        }
        {
            char query[1200];
            int nw = snprintf(query, sizeof query,
                              "SELECT ?birth ?death ?occLabel ?work ?workLabel ?date ?article WHERE {"
                              " OPTIONAL { wd:%s wdt:P569 ?birth }"
                              " OPTIONAL { wd:%s wdt:P570 ?death }"
                              " OPTIONAL { wd:%s wdt:P106 ?occ }"
                              " OPTIONAL { ?work wdt:P50 wd:%s . OPTIONAL { ?work wdt:P577 ?date }"
                              " OPTIONAL { ?article schema:about ?work ; schema:isPartOf <https://en.wikipedia.org/> } }"
                              " SERVICE wikibase:label { bd:serviceParam wikibase:language \"en\". } } LIMIT 40",
                              who, who, who, who);
            if (nw < 0 || (size_t)nw >= sizeof query ||
                buf_puts(&b, "https://query.wikidata.org/sparql?format=json&query=") != 0 ||
                pct(&b, query, (size_t)nw) != 0 || finish_url(&b, url) != 0) {
                err_set(e, "that name has no public lookup");
                return -1;
            }
        }
        return 0;
    }
    if (kind == PERSONA_SUMMARY) {
        trim_span(arg, &s, &n);
        if (n > 400) {
            n = utf8_prefix_bytes(s, n, 400);
        }
        if (n == 0 || buf_puts(&b, "https://en.wikipedia.org/api/rest_v1/page/summary/") != 0 || pct(&b, s, n) != 0 ||
            finish_url(&b, url) != 0) {
            err_set(e, "that name has no public lookup");
            return -1;
        }
        return 0;
    }
    err_set(e, "that name has no public lookup");
    return -1;
}

static const char *photo_kind(const unsigned char *p, size_t n) {
    if (!p) {
        return NULL;
    }
    if (n >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) {
        return "image/jpeg";
    }
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return "image/png";
    }
    if (n >= 6 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) {
        return "image/gif";
    }
    if (n >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }
    return NULL;
}

static int cache_names(const char *index_dir, const char *name, char **json_path, char **img_path, char **dir_path,
                       char hex_out[65]) {
    char who[PERSONA_NAME_MAX + 1];
    char folded[1024];
    char hex[65];
    char base[80];
    char *dir;
    if (json_path) {
        *json_path = NULL;
    }
    if (img_path) {
        *img_path = NULL;
    }
    if (dir_path) {
        *dir_path = NULL;
    }
    if (hex_out) {
        hex_out[0] = 0;
    }
    if (!index_dir || !index_dir[0] || name_copy(name, who, sizeof who) != 0 ||
        fold_into(who, folded, sizeof folded) != 0) {
        return -1;
    }
    fetch_sha256_hex(folded, strlen(folded), hex);
    if (hex_out) {
        memcpy(hex_out, hex, 65);
    }
    dir = path_join(index_dir, "personas");
    if (!dir) {
        return -1;
    }
    if (snprintf(base, sizeof base, "%s.json", hex) < 0) {
        free(dir);
        return -1;
    }
    if (json_path) {
        *json_path = path_join(dir, base);
    }
    if (snprintf(base, sizeof base, "%s.img", hex) < 0) {
        free(dir);
        return -1;
    }
    if (img_path) {
        *img_path = path_join(dir, base);
    }
    if (dir_path) {
        *dir_path = dir;
    } else {
        free(dir);
    }
    if ((json_path && !*json_path) || (img_path && !*img_path)) {
        return -1;
    }
    return 0;
}

static const char PERSONA_SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS persona("
    " id TEXT PRIMARY KEY,"
    " body TEXT NOT NULL,"
    " photo BLOB,"
    " mime TEXT NOT NULL)";

static int hex64(const char *s) {
    size_t i;
    if (!s) {
        return 0;
    }
    for (i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return s[64] == 0;
}

static unsigned char *copy_bytes(const void *p, size_t n) {
    size_t need = 0;
    unsigned char *d;
    if (!p || n == 0 || size_add(n, 1, &need) != 0) {
        return NULL;
    }
    d = malloc(need);
    if (!d) {
        return NULL;
    }
    memcpy(d, p, n);
    d[n] = 0;
    return d;
}

static char *persona_db(const char *index_dir) {
    if (!index_dir || !index_dir[0]) {
        return NULL;
    }
    return path_join(index_dir, "grove.db");
}

static void persona_abort(sqlite3 *db) {
    if (!db) {
        return;
    }
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
}

static int persona_exec(sqlite3 *db, const char *sql, Err *e) {
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

/* 0 open, 1 no database file, -1 error. create also makes the file. */
static int persona_open(const char *index_dir, int create, sqlite3 **db, Err *e) {
    char *path = persona_db(index_dir);
    int flags = SQLITE_OPEN_READWRITE;
    int rc;
    *db = NULL;
    if (!path) {
        err_set(e, "cannot save that profile");
        return -1;
    }
    if (create) {
        flags |= SQLITE_OPEN_CREATE;
    } else if (access(path, F_OK) != 0) {
        free(path);
        return 1;
    }
    rc = sqlite3_open_v2(path, db, flags, NULL);
    free(path);
    if (rc != SQLITE_OK || !*db) {
        err_set(e, "cannot save that profile");
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

static int persona_begin(const char *index_dir, sqlite3 **db, Err *e) {
    if (persona_open(index_dir, 1, db, e) != 0) {
        return -1;
    }
    if (persona_exec(*db, "BEGIN IMMEDIATE", e) != 0) {
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    if (persona_exec(*db, PERSONA_SCHEMA, e) != 0) {
        persona_abort(*db);
        *db = NULL;
        return -1;
    }
    return 0;
}

static int persona_write(const char *index_dir, const char *hex, const void *body, size_t body_n, const void *photo,
                         size_t photo_n, const char *mime, Err *e) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    const char *use_mime = mime ? mime : "";
    if (!hex64(hex) || !body || body_n == 0 || body_n > (size_t)PERSONA_BODY_MAX) {
        err_set(e, "cannot save that profile");
        return -1;
    }
    if (!photo || photo_n == 0 || photo_n > (size_t)PERSONA_PHOTO_MAX) {
        photo = NULL;
        photo_n = 0;
        use_mime = "";
    }
    if (persona_begin(index_dir, &db, e) != 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO persona(id, body, photo, mime) VALUES(?,?,?,?)"
                           " ON CONFLICT(id) DO UPDATE SET"
                           " body=excluded.body, photo=excluded.photo, mime=excluded.mime",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, hex, 64, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, (const char *)body, (int)body_n, SQLITE_TRANSIENT) != SQLITE_OK) {
        err_set(e, "cannot save that profile");
        goto fail;
    }
    if (photo) {
        if (sqlite3_bind_blob(st, 3, photo, (int)photo_n, SQLITE_TRANSIENT) != SQLITE_OK) {
            err_set(e, "cannot save that profile");
            goto fail;
        }
    } else if (sqlite3_bind_null(st, 3) != SQLITE_OK) {
        err_set(e, "cannot save that profile");
        goto fail;
    }
    if (sqlite3_bind_text(st, 4, use_mime, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE) {
        err_set(e, "cannot save that profile");
        goto fail;
    }
    sqlite3_finalize(st);
    if (persona_exec(db, "COMMIT", e) != 0) {
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
    persona_abort(db);
    return -1;
}

/* 1 row, 0 miss, 2 unusable row, -1 out of memory or the index is busy. */
static int persona_read(const char *index_dir, const char *hex, unsigned char **body, size_t *body_n,
                        unsigned char **photo, size_t *photo_n, Err *e) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int opened;
    int step;
    int nb;
    const void *src;
    *body = NULL;
    *body_n = 0;
    *photo = NULL;
    *photo_n = 0;
    if (!hex64(hex)) {
        return 0;
    }
    opened = persona_open(index_dir, 0, &db, e);
    if (opened != 0) {
        err_clear(e);
        return 0;
    }
    if (sqlite3_prepare_v2(db, "SELECT body, photo FROM persona WHERE id=?", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        err_clear(e);
        return 0;
    }
    if (sqlite3_bind_text(st, 1, hex, 64, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        err_set(e, "out of memory");
        return -1;
    }
    step = sqlite3_step(st);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return 0;
    }
    if (step != SQLITE_ROW) {
        int primary = sqlite3_extended_errcode(db) & 0xff;
        sqlite3_finalize(st);
        sqlite3_close(db);
        if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
            err_set(e, "index is in use");
            return -1;
        }
        err_clear(e);
        return 0;
    }
    nb = sqlite3_column_bytes(st, 0);
    if (nb <= 0 || (size_t)nb > (size_t)PERSONA_BODY_MAX) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return 2;
    }
    src = sqlite3_column_blob(st, 0);
    *body = src ? copy_bytes(src, (size_t)nb) : NULL;
    if (!*body) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        err_set(e, "out of memory");
        return -1;
    }
    *body_n = (size_t)nb;
    if (sqlite3_column_type(st, 1) == SQLITE_BLOB) {
        int np = sqlite3_column_bytes(st, 1);
        const void *blob;
        if (np > 0 && (size_t)np <= (size_t)PERSONA_PHOTO_MAX) {
            blob = sqlite3_column_blob(st, 1);
            if (!blob) {
                free(*body);
                *body = NULL;
                *body_n = 0;
                sqlite3_finalize(st);
                sqlite3_close(db);
                err_set(e, "out of memory");
                return -1;
            }
            *photo = copy_bytes(blob, (size_t)np);
            if (!*photo) {
                free(*body);
                *body = NULL;
                *body_n = 0;
                sqlite3_finalize(st);
                sqlite3_close(db);
                err_set(e, "out of memory");
                return -1;
            }
            *photo_n = (size_t)np;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 1;
}

static int persona_saved(const char *index_dir, const char *hex, const void *body, size_t body_n, const void *photo,
                         size_t photo_n) {
    unsigned char *got_body = NULL;
    unsigned char *got_photo = NULL;
    size_t got_body_n = 0;
    size_t got_photo_n = 0;
    Err ignore;
    int got;
    int ok = 0;
    err_clear(&ignore);
    got = persona_read(index_dir, hex, &got_body, &got_body_n, &got_photo, &got_photo_n, &ignore);
    if (got == 1 && got_body && body && got_body_n == body_n && memcmp(got_body, body, body_n) == 0) {
        if (photo_n == 0) {
            ok = got_photo_n == 0;
        } else if (photo && got_photo && got_photo_n == photo_n && memcmp(got_photo, photo, photo_n) == 0) {
            ok = 1;
        }
    }
    free(got_body);
    free(got_photo);
    return ok;
}

static int profile_drop(PersonaProfile *out, char *d, char *s, char *born, char *died, char *occ, char *wiki,
                        char *photo, Jdoc *j, Err *e) {
    free(d);
    free(s);
    free(born);
    free(died);
    free(occ);
    free(wiki);
    free(photo);
    persona_profile_free(out);
    jdoc_close(j);
    err_set(e, "out of memory");
    return -1;
}

static int profile_from_cache(const unsigned char *raw, size_t raw_n, const unsigned char *img, size_t img_n,
                              PersonaProfile *out, Err *e) {
    Jdoc j;
    int version = 0;
    int n;
    int i;
    int docn;
    char *qid = NULL;
    if (!out) {
        err_set(e, "out of memory");
        return -1;
    }
    if (!raw || raw_n == 0 || raw_n > (size_t)PERSONA_BODY_MAX) {
        return 0;
    }
    docn = (int)raw_n;
    memset(&j, 0, sizeof j);
    if (jdoc_open(&j) != 0 || jnumber(&j, (const char *)raw, docn, "$.v", &version) != 0 || version != 1) {
        jdoc_close(&j);
        return 0;
    }
    qid = jtext(&j, (const char *)raw, docn, "$.qid");
    if (!qid_ok(qid)) {
        free(qid);
        jdoc_close(&j);
        return 0;
    }
    out->state = PERSONA_READY;
    out->qid = qid;
    {
        char *d = jtext(&j, (const char *)raw, docn, "$.description");
        char *s = jtext(&j, (const char *)raw, docn, "$.summary");
        char *born = jtext(&j, (const char *)raw, docn, "$.born");
        char *died = jtext(&j, (const char *)raw, docn, "$.died");
        char *occ = jtext(&j, (const char *)raw, docn, "$.occupation");
        char *wiki = jtext(&j, (const char *)raw, docn, "$.wikipedia");
        char *photo = jtext(&j, (const char *)raw, docn, "$.photo");
        free(out->description);
        out->description = clean_text(d, 300);
        out->summary = clean_text(s, PERSONA_SUMMARY_MAX);
        out->born = clean_text(born, 32);
        out->died = clean_text(died, 32);
        out->occupation = clean_text(occ, 400);
        out->wikipedia = keep_https(wiki, HOST_WIKI);
        n = jcount(&j, (const char *)raw, docn, "$.aliases");
        if (n > PERSONA_ALIAS_MAX) {
            n = PERSONA_ALIAS_MAX;
        }
        for (i = 0; i < n; i++) {
            char path[40];
            char *alias;
            char *clean;
            if (path_at(path, sizeof path, "$.aliases[%d]", i) != 0) {
                break;
            }
            alias = jtext(&j, (const char *)raw, docn, path);
            clean = clean_text(alias, PERSONA_NAME_MAX);
            free(alias);
            if (add_alias(out, clean) != 0) {
                return profile_drop(out, d, s, born, died, occ, wiki, photo, &j, e);
            }
        }
        n = jcount(&j, (const char *)raw, docn, "$.works");
        if (n > PERSONA_WORK_MAX) {
            n = PERSONA_WORK_MAX;
        }
        for (i = 0; i < n; i++) {
            char path[48];
            char *title = NULL;
            char *year = NULL;
            char *url = NULL;
            char *clean_title;
            if (path_at(path, sizeof path, "$.works[%d].title", i) != 0) {
                break;
            }
            title = jtext(&j, (const char *)raw, docn, path);
            if (path_at(path, sizeof path, "$.works[%d].year", i) == 0) {
                year = jtext(&j, (const char *)raw, docn, path);
            }
            if (path_at(path, sizeof path, "$.works[%d].url", i) == 0) {
                url = jtext(&j, (const char *)raw, docn, path);
            }
            clean_title = clean_text(title, PERSONA_NAME_MAX);
            if (add_work(out, clean_title, clean_text(year, 32), keep_https(url, HOST_WIKI)) != 0) {
                free(title);
                free(year);
                free(url);
                return profile_drop(out, d, s, born, died, occ, wiki, photo, &j, e);
            }
            free(title);
            free(year);
            free(url);
        }
        if (photo && img && img_n > 0 && img_n <= (size_t)PERSONA_PHOTO_MAX &&
            (strcmp(photo, "image/jpeg") == 0 || strcmp(photo, "image/png") == 0 || strcmp(photo, "image/gif") == 0 ||
             strcmp(photo, "image/webp") == 0)) {
            const char *got = photo_kind(img, img_n);
            if (got && strcmp(got, photo) == 0) {
                out->photo = copy_bytes(img, img_n);
                if (!out->photo || !(out->photo_type = xstrdup(got))) {
                    free(out->photo);
                    out->photo = NULL;
                    return profile_drop(out, d, s, born, died, occ, wiki, photo, &j, e);
                }
                out->photo_n = img_n;
            }
        }
        free(d);
        free(s);
        free(born);
        free(died);
        free(occ);
        free(wiki);
        free(photo);
    }
    jdoc_close(&j);
    return 0;
}

static int legacy_hex(const char *filename, char hex[65]) {
    size_t i;
    size_t n;
    if (!filename) {
        return -1;
    }
    n = strlen(filename);
    if (n != 69) {
        return -1;
    }
    for (i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return -1;
        }
        hex[i] = (char)c;
    }
    hex[64] = 0;
    if (strcmp(filename + 64, ".json") != 0) {
        return -1;
    }
    return 0;
}

static void import_legacy(const char *index_dir, const char *hex, const char *json_path, const char *img_path) {
    unsigned char *raw = NULL;
    unsigned char *img = NULL;
    unsigned char *have = NULL;
    unsigned char *have_photo = NULL;
    size_t raw_n = 0;
    size_t img_n = 0;
    size_t have_n = 0;
    size_t have_photo_n = 0;
    PersonaProfile tmp;
    Err ignore;
    int got;
    err_clear(&ignore);
    memset(&tmp, 0, sizeof tmp);
    if (!hex64(hex) || !json_path) {
        return;
    }
    got = persona_read(index_dir, hex, &have, &have_n, &have_photo, &have_photo_n, &ignore);
    free(have);
    free(have_photo);
    if (got < 0 || got == 2) {
        return;
    }
    if (read_entire_file(json_path, &raw, &raw_n, PERSONA_BODY_MAX, &ignore) != 0) {
        err_clear(&ignore);
        free(raw);
        return;
    }
    err_clear(&ignore);
    if (img_path && read_entire_file(img_path, &img, &img_n, PERSONA_PHOTO_MAX, &ignore) != 0) {
        free(img);
        img = NULL;
        img_n = 0;
        err_clear(&ignore);
    }
    if (profile_from_cache(raw, raw_n, img, img_n, &tmp, &ignore) != 0 || tmp.state != PERSONA_READY) {
        persona_profile_free(&tmp);
        free(raw);
        free(img);
        return;
    }
    /* A row already in the index stays. The file is removed only when that row is this cache. */
    if (got == 1) {
        if (persona_saved(index_dir, hex, raw, raw_n, tmp.photo, tmp.photo_n)) {
            unlink(json_path);
            if (tmp.photo_n > 0 && img_path) {
                unlink(img_path);
            }
        }
        persona_profile_free(&tmp);
        free(raw);
        free(img);
        return;
    }
    err_clear(&ignore);
    if (persona_write(index_dir, hex, raw, raw_n, tmp.photo, tmp.photo_n, tmp.photo_type ? tmp.photo_type : "",
                      &ignore) == 0 &&
        persona_saved(index_dir, hex, raw, raw_n, tmp.photo, tmp.photo_n)) {
        unlink(json_path);
        if (tmp.photo_n > 0 && img_path) {
            unlink(img_path);
        }
    }
    persona_profile_free(&tmp);
    free(raw);
    free(img);
}

void persona_cache_adopt(const char *index_dir) {
    char *dir;
    DIR *d;
    struct dirent *de;
    if (!index_dir || !index_dir[0]) {
        return;
    }
    dir = path_join(index_dir, "personas");
    if (!dir) {
        return;
    }
    d = opendir(dir);
    if (!d) {
        free(dir);
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char hex[65];
        char img_base[80];
        char *json_path;
        char *img_path;
        if (legacy_hex(de->d_name, hex) != 0) {
            continue;
        }
        json_path = path_join(dir, de->d_name);
        img_path = NULL;
        {
            int nw = snprintf(img_base, sizeof img_base, "%s.img", hex);
            if (nw > 0 && (size_t)nw < sizeof img_base) {
                img_path = path_join(dir, img_base);
            }
        }
        if (json_path) {
            import_legacy(index_dir, hex, json_path, img_path);
        }
        free(json_path);
        free(img_path);
    }
    closedir(d);
    free(dir);
}

static int jesc_buf(Buf *b, const char *s) {
    const unsigned char *p;
    if (buf_putc(b, '"') != 0) {
        return -1;
    }
    for (p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char tmp[8];
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            tmp[2] = 0;
            if (buf_puts(b, tmp) != 0) {
                return -1;
            }
        } else if (*p < 0x20 || *p == 0x7f) {
            if (snprintf(tmp, sizeof tmp, "\\u%04x", *p) < 0 || buf_puts(b, tmp) != 0) {
                return -1;
            }
        } else if (buf_putc(b, *p) != 0) {
            return -1;
        }
    }
    return buf_putc(b, '"');
}

static int jfield(Buf *b, const char *key, const char *val, int comma) {
    if ((comma && buf_putc(b, ',') != 0) || buf_putc(b, '"') != 0 || buf_puts(b, key) != 0 || buf_puts(b, "\":") != 0 ||
        jesc_buf(b, val) != 0) {
        return -1;
    }
    return 0;
}

int persona_profile_store(const char *index_dir, const char *name, const PersonaProfile *in, Err *e) {
    char hex[65];
    const char *kind = NULL;
    Buf b;
    int i;
    buf_init(&b);
    hex[0] = 0;
    if (!in || in->state != PERSONA_READY || !qid_ok(in->qid)) {
        err_set(e, "that profile cannot be kept");
        return -1;
    }
    if (cache_names(index_dir, name, NULL, NULL, NULL, hex) != 0 || !hex64(hex)) {
        err_set(e, "cannot save that profile");
        return -1;
    }
    if (in->photo && in->photo_n > 0 && in->photo_n <= (size_t)PERSONA_PHOTO_MAX) {
        kind = photo_kind(in->photo, in->photo_n);
    }
    if (!(kind && in->photo_type && strcmp(kind, in->photo_type) == 0)) {
        kind = NULL;
    }
    if (buf_puts(&b, "{\"v\":1") != 0 || jfield(&b, "qid", in->qid, 1) != 0 ||
        jfield(&b, "description", in->description, 1) != 0 || jfield(&b, "summary", in->summary, 1) != 0 ||
        jfield(&b, "born", in->born, 1) != 0 || jfield(&b, "died", in->died, 1) != 0 ||
        jfield(&b, "occupation", in->occupation, 1) != 0 || jfield(&b, "wikipedia", in->wikipedia, 1) != 0 ||
        buf_puts(&b, ",\"aliases\":[") != 0) {
        goto fail;
    }
    for (i = 0; i < in->alias_n; i++) {
        if ((i && buf_putc(&b, ',') != 0) || jesc_buf(&b, in->aliases[i]) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "],\"works\":[") != 0) {
        goto fail;
    }
    for (i = 0; i < in->work_n; i++) {
        const PersonaPubWork *w = &in->works[i];
        char *url = keep_https(w->url, HOST_WIKI);
        if ((i && buf_putc(&b, ',') != 0) || buf_puts(&b, "{\"title\":") != 0 || jesc_buf(&b, w->title) != 0 ||
            jfield(&b, "year", w->year, 1) != 0 || jfield(&b, "url", url, 1) != 0 || buf_putc(&b, '}') != 0) {
            free(url);
            goto fail;
        }
        free(url);
    }
    if (buf_puts(&b, "],") != 0 || jfield(&b, "photo", kind, 0) != 0 || buf_putc(&b, '}') != 0 || !b.data || b.len == 0 ||
        b.len > (size_t)PERSONA_BODY_MAX) {
        goto fail;
    }
    if (persona_write(index_dir, hex, b.data, b.len, kind ? in->photo : NULL, kind ? in->photo_n : 0, kind ? kind : "",
                      e) != 0) {
        goto fail;
    }
    buf_free(&b);
    return 0;
fail:
    buf_free(&b);
    if (e && !e->msg[0]) {
        err_set(e, "cannot save that profile");
    }
    return -1;
}

int persona_profile_load(const char *index_dir, const char *name, PersonaProfile *out, Err *e) {
    char *json_path = NULL;
    char *img_path = NULL;
    char *dir = NULL;
    char hex[65];
    unsigned char *body = NULL;
    unsigned char *photo = NULL;
    size_t body_n = 0;
    size_t photo_n = 0;
    int got;
    int rc;
    if (!out) {
        err_set(e, "out of memory");
        return -1;
    }
    memset(out, 0, sizeof *out);
    hex[0] = 0;
    if (cache_names(index_dir, name, &json_path, &img_path, &dir, hex) != 0 || !json_path || !hex64(hex)) {
        free(json_path);
        free(img_path);
        free(dir);
        return 0;
    }
    free(dir);
    got = persona_read(index_dir, hex, &body, &body_n, &photo, &photo_n, e);
    if (got < 0) {
        free(json_path);
        free(img_path);
        free(body);
        free(photo);
        return -1;
    }
    if (got == 2) {
        free(json_path);
        free(img_path);
        free(body);
        free(photo);
        return 0;
    }
    if (got != 1) {
        free(body);
        free(photo);
        body = NULL;
        photo = NULL;
        import_legacy(index_dir, hex, json_path, img_path);
        got = persona_read(index_dir, hex, &body, &body_n, &photo, &photo_n, e);
        if (got < 0) {
            free(json_path);
            free(img_path);
            free(body);
            free(photo);
            return -1;
        }
    }
    free(json_path);
    free(img_path);
    if (got != 1) {
        free(body);
        free(photo);
        return 0;
    }
    rc = profile_from_cache(body, body_n, photo, photo_n, out, e);
    free(body);
    free(photo);
    return rc;
}

static char *wiki_title(const char *entity, size_t entity_n, const char *qid, const char *label) {
    Jdoc j;
    char path[96];
    char *title = NULL;
    int n;
    if (entity && entity_n > 0 && entity_n <= (size_t)PERSONA_BODY_MAX && qid_ok(qid) && jdoc_open(&j) == 0) {
        n = snprintf(path, sizeof path, "$.entities.%s.sitelinks.enwiki.title", qid);
        if (n > 0 && (size_t)n < sizeof path) {
            title = jtext(&j, entity, (int)entity_n, path);
        }
        jdoc_close(&j);
    }
    if (!title && label && label[0]) {
        title = xstrdup(label);
    }
    return title;
}

static int fetch_json(const char *url, const char *host, unsigned char **body, size_t *n, Err *e) {
    const char *hosts[1];
    hosts[0] = host;
    return fetch_https_get(url, hosts, 1, "application/json", PERSONA_BODY_MAX, body, n, e);
}

int persona_profile(const char *index_dir, const char *name, PersonaProfile *out, Err *e) {
    char *find_url = NULL;
    char *entity_url = NULL;
    char *summary_url = NULL;
    char *sparql_url = NULL;
    unsigned char *search = NULL;
    unsigned char *entity = NULL;
    unsigned char *summary = NULL;
    unsigned char *sparql = NULL;
    unsigned char *image = NULL;
    size_t search_n = 0;
    size_t entity_n = 0;
    size_t summary_n = 0;
    size_t sparql_n = 0;
    size_t image_n = 0;
    char *qid = NULL;
    char *label = NULL;
    char *desc = NULL;
    char *title = NULL;
    Jdoc j;
    const char *upload[1];
    if (!out) {
        err_set(e, "out of memory");
        return -1;
    }
    memset(out, 0, sizeof *out);
    memset(&j, 0, sizeof j);
    if (persona_profile_load(index_dir, name, out, e) != 0) {
        return -1;
    }
    if (out->state == PERSONA_READY) {
        return 0;
    }
    if (persona_lookup_url(PERSONA_FIND, name, &find_url, e) != 0) {
        err_clear(e);
        return 0;
    }
    if (fetch_json(find_url, HOST_WIKIDATA, &search, &search_n, e) != 0) {
        out->state = PERSONA_UNAVAILABLE;
        goto done;
    }
    {
        char who[PERSONA_NAME_MAX + 1];
        const char *scored = name;
        if (name_copy(name, who, sizeof who) == 0) {
            scored = who;
        }
        if (jdoc_open(&j) != 0 ||
            best_hit(&j, (const char *)search, (int)search_n, scored, &qid, &label, &desc) != 0) {
            err_set(e, "could not read that profile");
            persona_profile_free(out);
            jdoc_close(&j);
            free(find_url);
            free(search);
            free(qid);
            free(label);
            free(desc);
            return -1;
        }
    }
    jdoc_close(&j);
    if (!qid) {
        goto done;
    }
    if (persona_lookup_url(PERSONA_ENTITY, qid, &entity_url, e) == 0) {
        fetch_json(entity_url, HOST_WIKIDATA, &entity, &entity_n, e);
        err_clear(e);
    }
    title = wiki_title((const char *)entity, entity_n, qid, label);
    if (title && persona_lookup_url(PERSONA_SUMMARY, title, &summary_url, e) == 0) {
        fetch_json(summary_url, HOST_WIKI, &summary, &summary_n, e);
        err_clear(e);
    }
    if (persona_lookup_url(PERSONA_SPARQL, qid, &sparql_url, e) == 0) {
        fetch_json(sparql_url, HOST_QUERY, &sparql, &sparql_n, e);
        err_clear(e);
    }
    if (persona_profile_parse(name, (const char *)search, search_n, (const char *)entity, entity_n,
                              (const char *)summary, summary_n, (const char *)sparql, sparql_n, out, e) != 0) {
        free(find_url);
        free(entity_url);
        free(summary_url);
        free(sparql_url);
        free(search);
        free(entity);
        free(summary);
        free(sparql);
        free(qid);
        free(label);
        free(desc);
        free(title);
        return -1;
    }
    if (out->state == PERSONA_READY && out->photo_url) {
        upload[0] = HOST_UPLOAD;
        if (fetch_https_get(out->photo_url, upload, 1, NULL, PERSONA_PHOTO_MAX, &image, &image_n, e) == 0) {
            const char *kind = photo_kind(image, image_n);
            if (kind) {
                out->photo = image;
                out->photo_n = image_n;
                out->photo_type = xstrdup(kind);
                image = NULL;
            }
        }
        err_clear(e);
        free(image);
    }
    if (out->state == PERSONA_READY) {
        Err ignore;
        err_clear(&ignore);
        persona_profile_store(index_dir, name, out, &ignore);
    }
done:
    free(find_url);
    free(entity_url);
    free(summary_url);
    free(sparql_url);
    free(search);
    free(entity);
    free(summary);
    free(sparql);
    free(qid);
    free(label);
    free(desc);
    free(title);
    return 0;
}
