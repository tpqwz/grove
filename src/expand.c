#include "expand.h"

#include "os.h"
#include "util.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Query-only expansion. The index stores surface words, so a stem is an extra
 * term beside the original, not a replacement, and a missing thesaurus is not
 * an error. At most FORM_CAP spellings are kept, STEM_CAP of them inflections. */

enum { FORM_CAP = 64, STEM_CAP = 24, REL_CAP = 12, WORD_MAX = 48 };

static int az_word(const char *s, size_t n) {
    if (n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 'a' || s[i] > 'z') {
            return 0;
        }
    }
    return 1;
}

static int ends_with(const char *s, size_t n, const char *suf, size_t m) {
    return n >= m && memcmp(s + (n - m), suf, m) == 0;
}

/* 1 when s[i] is a Porter consonant. y is a consonant at 0 and after a vowel. */
static int cons(const char *s, size_t i) {
    char c = s[i];
    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') {
        return 0;
    }
    if (c != 'y') {
        return 1;
    }
    if (i == 0) {
        return 1;
    }
    return !cons(s, i - 1);
}

static int measure(const char *s, size_t n) {
    size_t i = 0;
    int m = 0;
    while (i < n && cons(s, i)) {
        i++;
    }
    while (i < n) {
        while (i < n && !cons(s, i)) {
            i++;
        }
        if (i >= n) {
            break;
        }
        m++;
        while (i < n && cons(s, i)) {
            i++;
        }
    }
    return m;
}

static int has_vowel(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!cons(s, i)) {
            return 1;
        }
    }
    return 0;
}

static int doublec(const char *s, size_t n) {
    return n >= 2 && s[n - 1] == s[n - 2] && cons(s, n - 1);
}

static int cvc(const char *s, size_t n) {
    if (n < 3 || !cons(s, n - 1) || cons(s, n - 2) || !cons(s, n - 3)) {
        return 0;
    }
    char c = s[n - 1];
    return c != 'w' && c != 'x' && c != 'y';
}

static void step1a(char *s, size_t *n) {
    if (ends_with(s, *n, "sses", 4) || ends_with(s, *n, "ies", 3)) {
        *n -= 2;
        s[*n] = 0;
        return;
    }
    if (ends_with(s, *n, "ss", 2)) {
        return;
    }
    if (ends_with(s, *n, "s", 1)) {
        *n -= 1;
        s[*n] = 0;
    }
}

static void step1b(char *s, size_t *n) {
    if (ends_with(s, *n, "eed", 3)) {
        if (measure(s, *n - 3) > 0) {
            *n -= 1;
            s[*n] = 0;
        }
        return;
    }
    size_t cut = 0;
    if (ends_with(s, *n, "ed", 2)) {
        cut = 2;
    } else if (ends_with(s, *n, "ing", 3)) {
        cut = 3;
    } else {
        return;
    }
    size_t stem = *n - cut;
    if (!has_vowel(s, stem)) {
        return;
    }
    *n = stem;
    s[*n] = 0;
    if (ends_with(s, *n, "at", 2) || ends_with(s, *n, "bl", 2) || ends_with(s, *n, "iz", 2)) {
        s[(*n)++] = 'e';
        s[*n] = 0;
        return;
    }
    if (doublec(s, *n)) {
        char last = s[*n - 1];
        if (last != 'l' && last != 's' && last != 'z') {
            *n -= 1;
            s[*n] = 0;
            return;
        }
    }
    if (measure(s, *n) == 1 && cvc(s, *n)) {
        s[(*n)++] = 'e';
        s[*n] = 0;
    }
}

static void step1c(char *s, size_t *n) {
    if (*n >= 2 && ends_with(s, *n, "y", 1) && has_vowel(s, *n - 1)) {
        s[*n - 1] = 'i';
    }
}

/* First suffix whose stem measures more than mmin is replaced. */
static int swap_suf(char *s, size_t *n, const char *suf, const char *rep, int mmin) {
    size_t sn = strlen(suf);
    size_t rn = strlen(rep);
    if (*n < sn || memcmp(s + *n - sn, suf, sn) != 0) {
        return 0;
    }
    size_t stem = *n - sn;
    if (measure(s, stem) <= mmin) {
        return 0;
    }
    if (stem + rn >= 128) {
        return 0;
    }
    memcpy(s + stem, rep, rn);
    *n = stem + rn;
    s[*n] = 0;
    return 1;
}

static void step2(char *s, size_t *n) {
    static const char *suf[] = {"ational", "tional", "enci",  "anci",    "izer", "abli", "alli",    "entli", "eli",
                                "ousli",   "ization", "ation", "ator",    "alism", "iveness", "fulness", "ousness",
                                "aliti",   "iviti",   "biliti"};
    static const char *rep[] = {"ate", "tion", "ence", "ance", "ize", "able", "al",  "ent", "e",   "ous",
                                "ize", "ate",  "ate",  "al",   "ive", "ful",  "ous", "al",  "ive", "ble"};
    for (int i = 0; i < 20; i++) {
        if (swap_suf(s, n, suf[i], rep[i], 0)) {
            return;
        }
    }
}

static void step3(char *s, size_t *n) {
    static const char *suf[] = {"icate", "ative", "alize", "iciti", "ical", "ful", "ness"};
    static const char *rep[] = {"ic", "", "al", "ic", "ic", "", ""};
    for (int i = 0; i < 7; i++) {
        if (swap_suf(s, n, suf[i], rep[i], 0)) {
            return;
        }
    }
}

static void step4(char *s, size_t *n) {
    static const char *suf[] = {"al",  "ance", "ence", "er",  "ic",  "able", "ible", "ant", "ement", "ment",
                                "ent", "ou",   "ism",  "ate", "iti", "ous",  "ive",  "ize"};
    for (int i = 0; i < 18; i++) {
        size_t m = strlen(suf[i]);
        if (!ends_with(s, *n, suf[i], m)) {
            continue;
        }
        if (measure(s, *n - m) > 1) {
            *n -= m;
            s[*n] = 0;
            return;
        }
    }
    if (ends_with(s, *n, "ion", 3) && *n > 3) {
        char prev = s[*n - 4];
        if ((prev == 's' || prev == 't') && measure(s, *n - 3) > 1) {
            *n -= 3;
            s[*n] = 0;
        }
    }
}

static void step5(char *s, size_t *n) {
    if (ends_with(s, *n, "e", 1)) {
        size_t stem = *n - 1;
        int m = measure(s, stem);
        if (m > 1 || (m == 1 && !cvc(s, stem))) {
            *n = stem;
            s[*n] = 0;
        }
    }
    if (measure(s, *n) > 1 && doublec(s, *n) && ends_with(s, *n, "l", 1)) {
        *n -= 1;
        s[*n] = 0;
    }
}

size_t porter_stem(const char *word, size_t len, char *out) {
    if (!word || !out || len < 3 || len >= 128 || !az_word(word, len)) {
        return 0;
    }
    char s[128];
    memcpy(s, word, len);
    s[len] = 0;
    size_t n = len;
    step1a(s, &n);
    step1b(s, &n);
    step1c(s, &n);
    step2(s, &n);
    step3(s, &n);
    step4(s, &n);
    step5(s, &n);
    if (n >= 128) {
        return 0;
    }
    memcpy(out, s, n);
    out[n] = 0;
    return n;
}

typedef struct FormSet {
    WordForm *v;
    int n;
    int stem_n;
} FormSet;

static int form_has(const FormSet *fs, const char *s, size_t n) {
    for (int i = 0; i < fs->n; i++) {
        if (fs->v[i].len == n && memcmp(fs->v[i].s, s, n) == 0) {
            return i;
        }
    }
    return -1;
}

/* 1 added, 0 skipped, -1 out of memory. An inflection clears kin on a duplicate. */
static int form_add(FormSet *fs, const char *s, size_t n, int kin, int stem_slot) {
    if (!s || n == 0 || n > WORD_MAX) {
        return 0;
    }
    int at = form_has(fs, s, n);
    if (at >= 0) {
        if (!kin) {
            fs->v[at].kin = 0;
        }
        return 0;
    }
    if (fs->n >= FORM_CAP) {
        return 0;
    }
    if (stem_slot && fs->stem_n >= STEM_CAP) {
        return 0;
    }
    char *copy = xstrndup(s, n);
    if (!copy) {
        return -1;
    }
    fs->v[fs->n].s = copy;
    fs->v[fs->n].len = n;
    fs->v[fs->n].kin = kin ? 1 : 0;
    fs->n++;
    if (stem_slot) {
        fs->stem_n++;
    }
    return 1;
}

static int push_cat(FormSet *fs, const char *a, size_t na, const char *b, size_t nb) {
    size_t n = 0;
    if (size_add(na, nb, &n) != 0 || n > WORD_MAX) {
        return 0;
    }
    char buf[WORD_MAX + 1];
    memcpy(buf, a, na);
    memcpy(buf + na, b, nb);
    return form_add(fs, buf, n, 0, 1);
}

static int emit_base(FormSet *fs, const char *b, size_t n) {
    if (n < 2 || n > WORD_MAX) {
        return 0;
    }
    int rc = form_add(fs, b, n, 0, 1);
    if (rc < 0) {
        return -1;
    }
    if (n >= 2 && b[n - 1] == 'y' && cons(b, n - 2)) {
        rc = push_cat(fs, b, n - 1, "ies", 3);
    } else if (ends_with(b, n, "ch", 2) || ends_with(b, n, "sh", 2) || ends_with(b, n, "s", 1) ||
               ends_with(b, n, "x", 1) || ends_with(b, n, "z", 1)) {
        rc = push_cat(fs, b, n, "es", 2);
    } else {
        rc = push_cat(fs, b, n, "s", 1);
    }
    if (rc < 0) {
        return -1;
    }
    if (b[n - 1] == 'e') {
        rc = push_cat(fs, b, n - 1, "ed", 2);
        if (rc < 0) {
            return -1;
        }
        rc = push_cat(fs, b, n - 1, "ing", 3);
        return rc < 0 ? -1 : 0;
    }
    rc = push_cat(fs, b, n, "ed", 2);
    if (rc < 0) {
        return -1;
    }
    rc = push_cat(fs, b, n, "ing", 3);
    if (rc < 0) {
        return -1;
    }
    if (!cvc(b, n) || n + 1 > WORD_MAX) {
        return 0;
    }
    char d[WORD_MAX + 1];
    memcpy(d, b, n);
    d[n] = b[n - 1];
    rc = push_cat(fs, d, n + 1, "ed", 2);
    if (rc < 0) {
        return -1;
    }
    rc = push_cat(fs, d, n + 1, "ing", 3);
    return rc < 0 ? -1 : 0;
}

static void add_base(char bases[][WORD_MAX + 1], size_t *bl, int *nb, const char *s, size_t n) {
    if (n < 2 || n > WORD_MAX || *nb >= 8) {
        return;
    }
    for (int i = 0; i < *nb; i++) {
        if (bl[i] == n && memcmp(bases[i], s, n) == 0) {
            return;
        }
    }
    memcpy(bases[*nb], s, n);
    bases[*nb][n] = 0;
    bl[*nb] = n;
    (*nb)++;
}

static int stem_forms(FormSet *fs, const char *w, size_t n) {
    char bases[8][WORD_MAX + 1];
    size_t bl[8];
    int nb = 0;
    if (n >= 5 && ends_with(w, n, "ing", 3)) {
        size_t m = n - 3;
        if (m >= 2 && w[m - 1] == w[m - 2] && cons(w, m - 1)) {
            add_base(bases, bl, &nb, w, m - 1);
        } else if (m >= 1 && m < WORD_MAX && cons(w, m - 1)) {
            char ebuf[WORD_MAX + 1];
            memcpy(ebuf, w, m);
            ebuf[m] = 'e';
            add_base(bases, bl, &nb, ebuf, m + 1);
        }
        add_base(bases, bl, &nb, w, m);
    }
    if (n >= 4 && ends_with(w, n, "ed", 2)) {
        size_t m = n - 2;
        if (m >= 2 && w[m - 1] == w[m - 2] && cons(w, m - 1)) {
            add_base(bases, bl, &nb, w, m - 1);
        } else if (m >= 1 && m < WORD_MAX && cons(w, m - 1)) {
            char ebuf[WORD_MAX + 1];
            memcpy(ebuf, w, m);
            ebuf[m] = 'e';
            add_base(bases, bl, &nb, ebuf, m + 1);
        }
        add_base(bases, bl, &nb, w, m);
    }
    if (n >= 4 && ends_with(w, n, "ies", 3)) {
        char yb[WORD_MAX + 1];
        size_t m = n - 3;
        memcpy(yb, w, m);
        yb[m] = 'y';
        add_base(bases, bl, &nb, yb, m + 1);
    }
    if (n >= 4 && ends_with(w, n, "es", 2)) {
        add_base(bases, bl, &nb, w, n - 2);
    }
    if (n >= 4 && ends_with(w, n, "s", 1) && !ends_with(w, n, "ss", 2)) {
        add_base(bases, bl, &nb, w, n - 1);
    }
    add_base(bases, bl, &nb, w, n);
    for (int i = 0; i < nb; i++) {
        if (emit_base(fs, bases[i], bl[i]) != 0) {
            return -1;
        }
    }
    char stemmed[128];
    size_t sn = porter_stem(w, n, stemmed);
    if (sn > 0 && (sn != n || memcmp(stemmed, w, n) != 0)) {
        if (form_add(fs, stemmed, sn, 0, 1) < 0) {
            return -1;
        }
    }
    return 0;
}

static int edit_forms(FormSet *fs, const char *w, size_t n) {
    char buf[WORD_MAX + 1];
    if (n < 2 || n > 24) {
        return 0;
    }
    for (size_t i = 0; i < n && fs->n < FORM_CAP; i++) {
        memcpy(buf, w, i);
        memcpy(buf + i, w + i + 1, n - i - 1);
        int rc = form_add(fs, buf, n - 1, 0, 0);
        if (rc < 0) {
            return -1;
        }
    }
    for (size_t i = 0; i + 1 < n && fs->n < FORM_CAP; i++) {
        memcpy(buf, w, n);
        char t = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = t;
        int rc = form_add(fs, buf, n, 0, 0);
        if (rc < 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < n && fs->n < FORM_CAP; i++) {
        memcpy(buf, w, n);
        for (char c = 'a'; c <= 'z' && fs->n < FORM_CAP; c++) {
            if (c == w[i]) {
                continue;
            }
            buf[i] = c;
            int rc = form_add(fs, buf, n, 0, 0);
            if (rc < 0) {
                return -1;
            }
        }
    }
    for (size_t i = 0; i <= n && fs->n < FORM_CAP; i++) {
        for (char c = 'a'; c <= 'z' && fs->n < FORM_CAP; c++) {
            memcpy(buf, w, i);
            buf[i] = c;
            memcpy(buf + i + 1, w + i, n - i);
            int rc = form_add(fs, buf, n + 1, 0, 0);
            if (rc < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static const char *const fam_virtue[] = {"virtue", "excellence", "arete", "noble", NULL};
static const char *const fam_soul[] = {"soul", "mind", "psyche", "spirit", NULL};
static const char *const fam_god[] = {"god", "gods", "divine", "deity", "zeus", NULL};
static const char *const fam_death[] = {"death", "die", "dying", "mortal", "mortality", NULL};
static const char *const fam_war[] = {"war", "battle", "fight", "fighting", "strife", NULL};
static const char *const fam_love[] = {"love", "beloved", "eros", "desire", NULL};
static const char *const fam_wisdom[] = {"wisdom", "wise", "philosophy", "philosopher", NULL};
static const char *const fam_city[] = {"city", "polis", "state", "citizen", "citizens", NULL};
static const char *const fam_justice[] = {"justice", "just", "injustice", "righteous", NULL};
static const char *const fam_fate[] = {"fate", "destiny", "doom", NULL};
static const char *const fam_sea[] = {"sea", "ocean", "wave", "waves", "sail", "sailing", NULL};
static const char *const fam_anger[] = {"anger", "wrath", "rage", NULL};
static const char *const fam_friend[] = {"friend", "friendship", "companion", "companions", NULL};
static const char *const fam_nature[] = {"nature", "natural", "physis", NULL};
static const char *const fam_reason[] = {"reason", "rational", "logos", NULL};
static const char *const fam_happy[] = {"happiness", "flourishing", "eudaimonia", "happy", NULL};
static const char *const fam_courage[] = {"courage", "brave", "bravery", "coward", "cowardice", NULL};
static const char *const fam_pleasure[] = {"pleasure", "pain", NULL};
static const char *const fam_law[] = {"law", "custom", "nomos", NULL};
static const char *const fam_fear[] = {"fear", "afraid", "terror", NULL};
static const char *const fam_honor[] = {"honor", "honour", "glory", "fame", NULL};
static const char *const fam_home[] = {"home", "household", "return", "nostos", NULL};

static const char *const *const families[] = {
    fam_virtue, fam_soul,   fam_god,      fam_death,   fam_war,    fam_love,    fam_wisdom, fam_city,
    fam_justice, fam_fate,  fam_sea,      fam_anger,   fam_friend, fam_nature,  fam_reason, fam_happy,
    fam_courage, fam_pleasure, fam_law,   fam_fear,    fam_honor,  fam_home};

typedef struct FamKey {
    char stem[32];
    size_t n;
    int fam;
} FamKey;

static FamKey famkeys[160];
static int nfam;

static int fam_cmp(const void *a, const void *b) {
    const FamKey *x = a;
    const FamKey *y = b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->stem, y->stem, n);
    if (c != 0) {
        return c;
    }
    if (x->n < y->n) {
        return -1;
    }
    if (x->n > y->n) {
        return 1;
    }
    return 0;
}

static void fam_init(void) {
    int nfam_local = 0;
    int nfamilies = (int)(sizeof families / sizeof families[0]);
    for (int f = 0; f < nfamilies; f++) {
        for (int i = 0; families[f][i]; i++) {
            const char *w = families[f][i];
            size_t wn = strlen(w);
            char stem[32];
            size_t sn = porter_stem(w, wn, stem);
            const char *key = sn ? stem : w;
            size_t kn = sn ? sn : wn;
            if (kn == 0 || kn >= sizeof stem) {
                continue;
            }
            int seen = 0;
            for (int k = 0; k < nfam_local; k++) {
                if (famkeys[k].n == kn && memcmp(famkeys[k].stem, key, kn) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (seen || nfam_local >= (int)(sizeof famkeys / sizeof famkeys[0])) {
                continue;
            }
            memcpy(famkeys[nfam_local].stem, key, kn);
            famkeys[nfam_local].stem[kn] = 0;
            famkeys[nfam_local].n = kn;
            famkeys[nfam_local].fam = f;
            nfam_local++;
        }
    }
    nfam = nfam_local;
    if (nfam > 1) {
        qsort(famkeys, (size_t)nfam, sizeof(FamKey), fam_cmp);
    }
}

static int family_of(const char *s, size_t n) {
    if (!s || n == 0 || n >= 32 || nfam <= 0) {
        return -1;
    }
    FamKey key;
    memset(&key, 0, sizeof key);
    memcpy(key.stem, s, n);
    key.n = n;
    const FamKey *found = bsearch(&key, famkeys, (size_t)nfam, sizeof(FamKey), fam_cmp);
    return found ? found->fam : -1;
}

typedef struct WnEnt {
    const char *key;
    size_t klen;
    const char *syn;
    size_t slen;
} WnEnt;

static char *wn_buf;
static WnEnt *wn_ents;
static size_t wn_n;
static pthread_once_t wn_once = PTHREAD_ONCE_INIT;

static int wn_cmp(const void *a, const void *b) {
    const WnEnt *x = a;
    const WnEnt *y = b;
    size_t n = x->klen < y->klen ? x->klen : y->klen;
    int c = memcmp(x->key, y->key, n);
    if (c != 0) {
        return c;
    }
    if (x->klen < y->klen) {
        return -1;
    }
    if (x->klen > y->klen) {
        return 1;
    }
    return 0;
}

enum { WN_CAP = 2 * 1024 * 1024 };

static int wn_load_path(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }
    Err e;
    err_clear(&e);
    unsigned char *buf = NULL;
    size_t n = 0;
    if (read_entire_file(path, &buf, &n, WN_CAP, &e) != 0) {
        return -1;
    }
    size_t lines = 1;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            lines++;
        }
    }
    WnEnt *ents = calloc(lines, sizeof(WnEnt));
    if (!ents) {
        free(buf);
        return -1;
    }
    size_t i = 0;
    size_t ne = 0;
    while (i < n && ne < lines) {
        size_t start = i;
        while (i < n && buf[i] != '\n') {
            i++;
        }
        size_t len = i - start;
        if (i < n && buf[i] == '\n') {
            buf[i] = 0;
            i++;
        }
        if (len > 0 && buf[start + len - 1] == '\r') {
            buf[start + len - 1] = 0;
            len--;
        }
        if (len == 0 || buf[start] == '#') {
            continue;
        }
        void *tab = memchr(buf + start, '\t', len);
        if (!tab || tab == buf + start) {
            continue;
        }
        char *split = tab;
        size_t klen = (size_t)(split - (char *)(buf + start));
        *split = 0;
        const char *syn = split + 1;
        size_t slen = len - klen - 1;
        if (klen == 0 || slen == 0) {
            continue;
        }
        ents[ne].key = (char *)buf + start;
        ents[ne].klen = klen;
        ents[ne].syn = syn;
        ents[ne].slen = slen;
        ne++;
    }
    if (ne > 1) {
        qsort(ents, ne, sizeof(WnEnt), wn_cmp);
    }
    wn_buf = (char *)buf;
    wn_ents = ents;
    wn_n = ne;
    return 0;
}

static int wn_from_exe(void) {
    char exe[4096];
    if (grove_self_path(exe, sizeof exe) != 0) {
        return -1;
    }
    char *slash = strrchr(exe, '/');
    char *bslash = strrchr(exe, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
    if (!slash || slash == exe) {
        return -1;
    }
    *slash = 0;
    char *p = path_join(exe, "../share/wordnet-syn.txt");
    if (p && wn_load_path(p) == 0) {
        free(p);
        return 0;
    }
    free(p);
    p = path_join(exe, "share/wordnet-syn.txt");
    int rc = p ? wn_load_path(p) : -1;
    free(p);
    return rc;
}

static void wn_init(void) {
    fam_init();
    const char *env = getenv("GROVE_WORDNET");
    if (env && env[0]) {
        (void)wn_load_path(env);
        return;
    }
    if (wn_from_exe() == 0) {
        return;
    }
    (void)wn_load_path("share/wordnet-syn.txt");
}

static void wn_ensure(void) {
    pthread_once(&wn_once, wn_init);
}

static const WnEnt *wn_find(const char *s, size_t n) {
    if (!wn_buf || !wn_ents || wn_n == 0 || !s || n == 0) {
        return NULL;
    }
    WnEnt key;
    key.key = s;
    key.klen = n;
    key.syn = NULL;
    key.slen = 0;
    return bsearch(&key, wn_ents, wn_n, sizeof(WnEnt), wn_cmp);
}

static int add_related(FormSet *fs, const char *s, size_t n, int *nrel) {
    if (*nrel >= REL_CAP) {
        return 0;
    }
    int rc = form_add(fs, s, n, 1, 0);
    if (rc < 0) {
        return -1;
    }
    if (rc == 1) {
        (*nrel)++;
    }
    return 0;
}

static int add_syn_list(FormSet *fs, const char *syn, size_t n, int *nrel) {
    size_t i = 0;
    while (i < n && *nrel < REL_CAP) {
        while (i < n && (syn[i] == ',' || syn[i] == ' ')) {
            i++;
        }
        size_t b = i;
        while (i < n && syn[i] != ',') {
            i++;
        }
        size_t e = i;
        while (e > b && syn[e - 1] == ' ') {
            e--;
        }
        size_t len = e - b;
        if (len < 2 || len > 32) {
            continue;
        }
        char tmp[33];
        int ok = 1;
        for (size_t k = 0; k < len; k++) {
            unsigned char c = (unsigned char)syn[b + k];
            if (c >= 'A' && c <= 'Z') {
                c = (unsigned char)(c - 'A' + 'a');
            }
            if (c < 'a' || c > 'z') {
                ok = 0;
                break;
            }
            tmp[k] = (char)c;
        }
        if (ok && add_related(fs, tmp, len, nrel) != 0) {
            return -1;
        }
    }
    return 0;
}

static int concept_forms(FormSet *fs, const char *w, size_t n) {
    wn_ensure();
    int nrel = 0;
    char stem[128];
    size_t sn = porter_stem(w, n, stem);
    const char *key = sn ? stem : w;
    size_t kn = sn ? sn : n;
    int fam = family_of(key, kn);
    if (fam >= 0) {
        for (int i = 0; families[fam][i] && nrel < REL_CAP; i++) {
            const char *m = families[fam][i];
            if (add_related(fs, m, strlen(m), &nrel) != 0) {
                return -1;
            }
        }
    }
    const WnEnt *ent = wn_find(w, n);
    if (!ent && sn && (sn != n || memcmp(stem, w, n) != 0)) {
        ent = wn_find(stem, sn);
    }
    if (ent && add_syn_list(fs, ent->syn, ent->slen, &nrel) != 0) {
        return -1;
    }
    return 0;
}

void wordforms_free(WordForm *f, int n) {
    if (!f) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(f[i].s);
    }
    free(f);
}

int expand_word(const char *word, size_t len, int tilde, const SearchModes *modes, WordForm **out, int *n) {
    if (out) {
        *out = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!word || !out || !n || len == 0 || len > WORD_MAX) {
        return 0;
    }
    int az = az_word(word, len);
    int stem = modes && modes->stem && az && len >= 3;
    int fuzzy = az && len <= 24 && ((modes && modes->fuzzy && len >= 5) || (tilde && len >= 3));
    int concept = modes && modes->concept;
    if (!stem && !fuzzy && !concept) {
        return 0;
    }
    WordForm *v = calloc(FORM_CAP, sizeof(WordForm));
    if (!v) {
        return -1;
    }
    FormSet fs;
    fs.v = v;
    fs.n = 0;
    fs.stem_n = 0;
    if (form_add(&fs, word, len, 0, 1) < 0) {
        goto oom;
    }
    if (stem && stem_forms(&fs, word, len) != 0) {
        goto oom;
    }
    if (concept && concept_forms(&fs, word, len) != 0) {
        goto oom;
    }
    if (fuzzy && edit_forms(&fs, word, len) != 0) {
        goto oom;
    }
    if (fs.n <= 1) {
        wordforms_free(v, fs.n);
        return 0;
    }
    *out = v;
    *n = fs.n;
    return 0;
oom:
    wordforms_free(v, fs.n);
    return -1;
}
