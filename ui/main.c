#include "index.h"
#include "mod.h"
#include "os.h"
#include "persona.h"
#include "webui.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#if !defined(_WIN32)
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <dlfcn.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <shobjidl.h>
#endif

#include "page.h"

#if !defined(_WIN32)
extern char **environ;
#endif

enum { SEARCH_LIMIT = 40, ALLOW_MAX = 40, QUERY_MAX = 8192 };

static Index *ix;
static const char *index_dir;
static char open_err[768];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t call_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t call_cv = PTHREAD_COND_INITIALIZER;
static int in_call;
static int stopping;
static char *allow[ALLOW_MAX];
static int nallow;

static int ui_enter(void) {
    pthread_mutex_lock(&call_mu);
    if (stopping) {
        pthread_mutex_unlock(&call_mu);
        return -1;
    }
    in_call++;
    pthread_mutex_unlock(&call_mu);
    return 0;
}

static void ui_leave(void) {
    pthread_mutex_lock(&call_mu);
    if (in_call > 0) {
        in_call--;
    }
    if (in_call == 0) {
        pthread_cond_signal(&call_cv);
    }
    pthread_mutex_unlock(&call_mu);
}

static int write_facet_list(Buf *b, const Facet *a, int n);
static int write_narrow(Buf *b, const Narrow *n);

static int jesc(Buf *b, const char *s) {
    if (buf_putc(b, '"') != 0) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char tmp[8];
        const char *esc = NULL;
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            tmp[2] = 0;
            esc = tmp;
        } else if (*p == '\n') {
            esc = "\\n";
        } else if (*p == '\r') {
            esc = "\\r";
        } else if (*p == '\t') {
            esc = "\\t";
        } else if (*p < 0x20) {
            snprintf(tmp, sizeof tmp, "\\u%04x", *p);
            esc = tmp;
        }
        if (esc) {
            if (buf_puts(b, esc) != 0) {
                return -1;
            }
        } else if (buf_putc(b, *p) != 0) {
            return -1;
        }
    }
    return buf_putc(b, '"');
}

static char *json_error(const char *msg) {
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"error\":") != 0 || jesc(&b, msg) != 0 || buf_puts(&b, "}") != 0) {
        buf_free(&b);
        return NULL;
    }
    return b.data;
}

static void reply(webui_event_t *e, const char *s) {
    webui_return_string(e, s ? s : "{\"error\":\"out of memory\"}");
}

static void reply_owned(webui_event_t *e, char *s) {
    reply(e, s);
    free(s);
}

static void allow_clear(void) {
    for (int i = 0; i < nallow; i++) {
        free(allow[i]);
        allow[i] = NULL;
    }
    nallow = 0;
}

static int allow_has(const char *path) {
    for (int i = 0; i < nallow; i++) {
        if (strcmp(allow[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void allow_add(const char *path) {
    if (!path || !path[0] || allow_has(path)) {
        return;
    }
    if (nallow >= ALLOW_MAX) {
        free(allow[0]);
        memmove(allow, allow + 1, (size_t)(nallow - 1) * sizeof *allow);
        nallow--;
    }
    char *copy = xstrdup(path);
    if (copy) {
        allow[nallow++] = copy;
    }
}

static int blank_query(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            return 0;
        }
    }
    return 1;
}

static int json_matchinfo(Buf *b, const MatchInfo *m) {
    int nph = m && m->phrase ? m->nphrase : 0;
    if (buf_printf(b, ",\"matchinfo\":{\"p\":%u,\"c\":%u,\"n\":%u,\"avg\":%u,\"len\":%u,\"phrases\":[",
                   m ? m->phrases : 0, m ? m->columns : 0, m ? m->rows : 0, m ? m->avg_tokens : 0,
                   m ? m->tokens : 0) != 0) {
        return -1;
    }
    for (int i = 0; i < nph; i++) {
        if (i && buf_putc(b, ',') != 0) {
            return -1;
        }
        if (buf_printf(b, "{\"hits\":%u,\"total\":%u,\"docs\":%u}", m->phrase[i].hits, m->phrase[i].total,
                       m->phrase[i].docs) != 0) {
            return -1;
        }
    }
    return buf_puts(b, "]}");
}

static int flag_on(const char *s) {
    return s && s[0] == '1' && s[1] == 0;
}

static char *search_json(const char *query, const SearchModes *modes) {
    if (!query || blank_query(query)) {
        return xstrdup("[]");
    }
    if (strlen(query) > QUERY_MAX) {
        return json_error("query is too long");
    }
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    Err e;
    err_clear(&e);
    Hit *hits = NULL;
    int n = 0;
    Facets facets;
    memset(&facets, 0, sizeof facets);
    if (index_search(ix, query, SEARCH_LIMIT, NULL, &hits, &n, &facets, modes, &e) != 0) {
        facets_free(&facets);
        return json_error(e.msg[0] ? e.msg : "search failed");
    }
    allow_clear();
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"hits\":[") != 0) {
        hits_free(hits, n);
        facets_free(&facets);
        buf_free(&b);
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        allow_add(hits[i].path);
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"path\":") != 0 || jesc(&b, hits[i].path) != 0 || buf_puts(&b, ",\"title\":") != 0 ||
            jesc(&b, hits[i].title) != 0 || buf_puts(&b, ",\"ext\":") != 0 || jesc(&b, hits[i].ext) != 0) {
            goto fail;
        }
        if (buf_printf(&b, ",\"score\":%d,\"nmatches\":%d,\"snippet\":", hits[i].score, hits[i].nmatches) != 0 ||
            jesc(&b, hits[i].snippet) != 0 || buf_puts(&b, ",\"offsets\":[") != 0) {
            goto fail;
        }
        for (int k = 0; k < hits[i].noffsets; k++) {
            if (k && buf_putc(&b, ',') != 0) {
                goto fail;
            }
            if (buf_printf(&b, "{\"start\":%u,\"length\":%u,\"term\":%d,\"column\":%d}",
                           (unsigned)hits[i].offsets[k].start, (unsigned)hits[i].offsets[k].len, hits[i].offsets[k].term,
                           hits[i].offsets[k].column) != 0) {
                goto fail;
            }
        }
        if (buf_putc(&b, ']') != 0 || json_matchinfo(&b, &hits[i].matchinfo) != 0 ||
            buf_puts(&b, ",\"passages\":[") != 0) {
            goto fail;
        }
        for (int p = 0; p < hits[i].npassages; p++) {
            Passage *ps = &hits[i].passages[p];
            if (p && buf_putc(&b, ',') != 0) {
                goto fail;
            }
            if (buf_puts(&b, "{\"text\":") != 0 || jesc(&b, ps->text) != 0) {
                goto fail;
            }
            if (buf_printf(&b, ",\"offset\":%u,\"page\":%d,\"section\":", (unsigned)ps->offset, ps->page) != 0 ||
                jesc(&b, ps->section) != 0 || buf_putc(&b, '}') != 0) {
                goto fail;
            }
        }
        if (buf_printf(&b, "],\"more\":%s}", hits[i].more ? "true" : "false") != 0) {
            goto fail;
        }
    }
    if (buf_printf(&b, "],\"nfiles\":%d,\"facets\":{\"types\":[", facets.nfiles) != 0) {
        goto fail;
    }
    for (int t = 0; t < facets.ntypes; t++) {
        if (t && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"value\":") != 0 || jesc(&b, facets.types[t].value) != 0 ||
            buf_printf(&b, ",\"count\":%d}", facets.types[t].count) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "],\"folders\":[") != 0) {
        goto fail;
    }
    for (int t = 0; t < facets.nfolders; t++) {
        if (t && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"value\":") != 0 || jesc(&b, facets.folders[t].value) != 0 ||
            buf_printf(&b, ",\"count\":%d}", facets.folders[t].count) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "],\"months\":[") != 0 || write_facet_list(&b, facets.months, facets.nmonths) != 0 ||
        buf_puts(&b, "],\"narrow\":") != 0 || write_narrow(&b, &facets.narrow) != 0 || buf_puts(&b, "}}") != 0) {
        goto fail;
    }
    hits_free(hits, n);
    facets_free(&facets);
    return b.data;
fail:
    hits_free(hits, n);
    facets_free(&facets);
    buf_free(&b);
    return NULL;
}

static char *more_json(const char *query, const char *path, uint32_t after, const SearchModes *modes) {
    if (!query || blank_query(query)) {
        return xstrdup("{\"passages\":[],\"more\":false}");
    }
    if (strlen(query) > QUERY_MAX) {
        return json_error("query is too long");
    }
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    Err e;
    err_clear(&e);
    Passage *ps = NULL;
    int n = 0;
    int more = 0;
    if (index_more(ix, query, path, after, &ps, &n, &more, modes, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not show more");
    }
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"passages\":[") != 0) {
        passages_free(ps, n);
        buf_free(&b);
        return NULL;
    }
    for (int p = 0; p < n; p++) {
        if (p && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"text\":") != 0 || jesc(&b, ps[p].text) != 0) {
            goto fail;
        }
        if (buf_printf(&b, ",\"offset\":%u,\"page\":%d,\"section\":", (unsigned)ps[p].offset, ps[p].page) != 0 ||
            jesc(&b, ps[p].section) != 0 || buf_putc(&b, '}') != 0) {
            goto fail;
        }
    }
    if (buf_printf(&b, "],\"more\":%s}", more ? "true" : "false") != 0) {
        goto fail;
    }
    passages_free(ps, n);
    return b.data;
fail:
    passages_free(ps, n);
    buf_free(&b);
    return NULL;
}

static char *info_json(void) {
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    Err e;
    err_clear(&e);
    IndexInfo info;
    memset(&info, 0, sizeof info);
    if (index_info(ix, &info, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not read the index");
    }
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"index\":") != 0 || jesc(&b, index_dir) != 0 ||
        buf_printf(&b, ",\"documents\":%u}", info.n_docs) != 0) {
        buf_free(&b);
        return NULL;
    }
    return b.data;
}

static void on_search(webui_event_t *ev) {
    const char *in = webui_get_string_at(ev, 0);
    SearchModes modes;
    modes.stem = flag_on(webui_get_string_at(ev, 1));
    modes.fuzzy = flag_on(webui_get_string_at(ev, 2));
    modes.concept = flag_on(webui_get_string_at(ev, 3));
    char *query = xstrdup(in ? in : "");
    pthread_mutex_lock(&mu);
    char *json = query ? search_json(query, &modes) : NULL;
    pthread_mutex_unlock(&mu);
    free(query);
    reply_owned(ev, json);
}

static void on_more(webui_event_t *ev) {
    const char *path_in = webui_get_string_at(ev, 0);
    long long after_in = webui_get_int_at(ev, 1);
    const char *query_in = webui_get_string_at(ev, 2);
    SearchModes modes;
    modes.stem = flag_on(webui_get_string_at(ev, 3));
    modes.fuzzy = flag_on(webui_get_string_at(ev, 4));
    modes.concept = flag_on(webui_get_string_at(ev, 5));
    char *path = xstrdup(path_in ? path_in : "");
    char *query = xstrdup(query_in ? query_in : "");
    int bad_after = after_in < 0 || (unsigned long long)after_in > UINT32_MAX;
    uint32_t after = bad_after ? 0 : (uint32_t)after_in;
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!path || !query) {
        json = NULL;
    } else if (bad_after) {
        json = json_error("bad offset");
    } else if (!allow_has(path)) {
        json = json_error("that file is not in these results");
    } else {
        json = more_json(query, path, after, &modes);
    }
    pthread_mutex_unlock(&mu);
    free(path);
    free(query);
    reply_owned(ev, json);
}

static void on_modes(webui_event_t *ev) {
    reply(ev, "{\"ok\":true}");
}

static int http_address(const char *s) {
    const char *rest = NULL;
    if (!s || !s[0]) {
        return 0;
    }
    if (starts_with(s, "https://")) {
        rest = s + 8;
    } else if (starts_with(s, "http://")) {
        rest = s + 7;
    } else {
        return 0;
    }
    if (!rest[0] || strlen(s) > 2048) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f || *p == '\\') {
            return 0;
        }
    }
    return 1;
}

static int spawn_open(const char *path) {
    pid_t pid = 0;
    int rc;
#if defined(__APPLE__)
    char *argv[] = {"open", (char *)path, NULL};
    rc = posix_spawnp(&pid, "open", NULL, NULL, argv, environ);
#elif defined(_WIN32)
    char *argv[] = {"cmd.exe", "/c", "start", "", (char *)path, NULL};
    rc = posix_spawnp(&pid, "cmd.exe", NULL, NULL, argv, environ);
#else
    char *argv[] = {"xdg-open", (char *)path, NULL};
    rc = posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ);
    if (rc != 0) {
        argv[0] = "/usr/bin/xdg-open";
        rc = posix_spawn(&pid, "/usr/bin/xdg-open", NULL, NULL, argv, environ);
    }
#endif
    if (rc != 0) {
        return -1;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return -1;
    }
    return 0;
}

static int open_saved(const char *path, const char *page_url) {
    if (http_address(page_url)) {
        return spawn_open(page_url);
    }
    return spawn_open(path);
}

/* The reader stays up. Detach so this process does not wait, and so the
 * child is not a zombie. */
static int spawn_detached(char *const argv[]) {
#if defined(_WIN32)
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0) {
        return -1;
    }
    /* The reader stays open. Drop our handle so the table does not fill. */
    return grove_disown(pid);
#else
    pid_t mid = fork();
    if (mid < 0) {
        return -1;
    }
    if (mid == 0) {
        if (setsid() < 0) {
            _exit(127);
        }
        pid_t pid = fork();
        if (pid < 0) {
            _exit(127);
        }
        if (pid == 0) {
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    int st = 0;
    if (waitpid(mid, &st, 0) < 0) {
        return -1;
    }
    return 0;
#endif
}

static int on_path(const char *name) {
    if (!name || !name[0]) {
        return 0;
    }
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0;
    }
    const char *env = getenv("PATH");
    if (!env) {
        return 0;
    }
    size_t namelen = strlen(name);
    const char *p = env;
    while (*p) {
    #if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        const char *colon = strchr(p, sep);
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n > 0 && n + 1 + namelen + 1 < 4096) {
            char buf[4096];
            memcpy(buf, p, n);
            buf[n] = '/';
            memcpy(buf + n + 1, name, namelen + 1);
            if (access(buf, X_OK) == 0) {
                return 1;
            }
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
    return 0;
}

static int capture_out(char *const argv[], char *out, size_t n) {
    int fds[2];
    if (n == 0 || pipe(fds) != 0) {
        return -1;
    }
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return -1;
    }
    size_t got = 0;
    while (got + 1 < n) {
        ssize_t r = read(fds[0], out + got, n - 1 - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    out[got] = 0;
    close(fds[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return -1;
    }
    while (got && (out[got - 1] == '\n' || out[got - 1] == '\r' || out[got - 1] == ' ')) {
        out[--got] = 0;
    }
    return got ? 0 : -1;
}

static const char *ext_of(const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    const char *dot = NULL;
    for (const char *p = base; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot || dot == base || !dot[1]) {
        return "";
    }
    return dot + 1;
}

static int ext_is(const char *ext, const char *want) {
    return ascii_ieq(ext, want);
}

/* Plain files whose extracted offset maps back onto the file bytes.
 * HTML and XML are extracted, so a line number there would be a lie. */
static int plain_source(const char *ext) {
    static const char *names[] = {
        "adoc", "asciidoc", "c",   "cc",   "conf", "cpp", "css", "csv", "go",  "h",   "hh",  "hpp",
        "ini",  "java",     "js",  "json", "log",  "markdown", "md", "org", "py", "rs", "rst", "sh",
        "tex",  "text",     "toml", "tsv", "txt",  "yaml", "yml"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (ext_is(ext, names[i])) {
            return 1;
        }
    }
    return 0;
}

static int is_trim_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v';
}

/* 1-based line in the file for an offset into Grove's extracted text.
 * Extraction drops CR, keeps at most two newlines in a row, and trims
 * the start. Returns 0 when the file cannot be read. */
static int plain_line(const char *path, uint32_t at) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > 32L * 1024L * 1024L) {
        close(fd);
        return 0;
    }
    size_t sz = (size_t)st.st_size;
    unsigned char *b = malloc(sz + 1);
    if (!b) {
        close(fd);
        return 0;
    }
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, b + got, sz - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(b);
            close(fd);
            return 0;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    if (close(fd) != 0 || got != sz) {
        free(b);
        return 0;
    }
    int nl = 0;
    int in_lead = 1;
    int found = 0;
    uint32_t out = 0;
    size_t src = 0;
    for (size_t i = 0; i < (size_t)sz; i++) {
        unsigned char c = b[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            nl++;
            if (nl > 2) {
                continue;
            }
        } else {
            nl = 0;
        }
        if (in_lead && is_trim_space(c)) {
            continue;
        }
        in_lead = 0;
        if (out == at) {
            src = i;
            found = 1;
            break;
        }
        out++;
    }
    if (!found) {
        src = sz ? (size_t)sz - 1 : 0;
    }
    int line = 1;
    for (size_t i = 0; i < src && i < (size_t)sz; i++) {
        if (b[i] == '\n') {
            line++;
        }
    }
    free(b);
    return line;
}

static int desk_has(const char *desk, const char *frag) {
    return desk && frag && strstr(desk, frag) != NULL;
}

static int query_mime(const char *path, char *mime, size_t n) {
    char *argv[] = {"xdg-mime", "query", "filetype", (char *)path, NULL};
    if (capture_out(argv, mime, n) != 0) {
        return -1;
    }
    char *semi = strchr(mime, ';');
    if (semi) {
        *semi = 0;
    }
    return mime[0] ? 0 : -1;
}

static int query_desktop(const char *mime, char *desk, size_t n) {
    char *argv[] = {"xdg-mime", "query", "default", (char *)mime, NULL};
    return capture_out(argv, desk, n);
}

/* The passage, as one line the reader can search. Grove's own "... " and
 * " ..." are not in the file. Okular keeps a line break as a newline, and
 * its find cannot cross that, so a long passage never matches. A run of
 * about four words does. Under 12 characters the file just opens. */
enum { NEEDLE_MIN = 12, NEEDLE_MAX = 22 };

static char *clean_needle(const char *in) {
    if (!in || !in[0]) {
        return NULL;
    }
    size_t n = strlen(in);
    size_t b = 0;
    size_t e = n;
    if (e >= b + 4 && memcmp(in + b, "... ", 4) == 0) {
        b += 4;
    }
    if (e >= b + 4 && memcmp(in + e - 4, " ...", 4) == 0) {
        e -= 4;
    }
    char *s = malloc(e - b + 1);
    if (!s) {
        return NULL;
    }
    size_t w = 0;
    int sp = 1;
    for (size_t i = b; i < e; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '<' && i + 1 < e && in[i + 1] == '<') {
            i++;
            continue;
        }
        if (c == '>' && i + 1 < e && in[i + 1] == '>') {
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            c = ' ';
        }
        if (c == ' ') {
            if (sp) {
                continue;
            }
            sp = 1;
        } else {
            sp = 0;
        }
        s[w++] = (char)c;
    }
    while (w && s[w - 1] == ' ') {
        w--;
    }
    s[w] = 0;
    if (w > NEEDLE_MAX) {
        w = utf8_prefix_bytes(s, w, NEEDLE_MAX);
        while (w > NEEDLE_MIN && s[w - 1] != ' ') {
            w--;
        }
        while (w && s[w - 1] == ' ') {
            w--;
        }
        s[w] = 0;
    }
    if (!w || s[0] == '-' || w < NEEDLE_MIN) {
        free(s);
        return NULL;
    }
    return s;
}

static int open_reader(char *const argv[]) {
    if (!on_path(argv[0])) {
        return -1;
    }
    return spawn_detached(argv);
}

/* The desktop id says Okular. The package may be absent while the Flatpak
 * is what actually opens the book. xdg-open then shows page 1, with no
 * --find. */
static const char *okular_bin(void) {
    if (on_path("okular")) {
        return "okular";
    }
    if (on_path("org.kde.okular")) {
        return "org.kde.okular";
    }
    const char *home = getenv("HOME");
    static char user_bin[4096];
    if (home && home[0]) {
        int n = snprintf(user_bin, sizeof user_bin, "%s/.local/share/flatpak/exports/bin/org.kde.okular", home);
        if (n > 0 && (size_t)n < sizeof user_bin && access(user_bin, X_OK) == 0) {
            return user_bin;
        }
    }
    if (access("/var/lib/flatpak/exports/bin/org.kde.okular", X_OK) == 0) {
        return "/var/lib/flatpak/exports/bin/org.kde.okular";
    }
    return NULL;
}

static int open_pdf(const char *path, const char *desk, int page, const char *needle) {
    char pg[32];
    snprintf(pg, sizeof pg, "%d", page);
    int have_page = page > 0;
    int have_find = needle != NULL;
    if (desk_has(desk, "xreader") || desk_has(desk, "evince") || desk_has(desk, "Evince")) {
        const char *bin = desk_has(desk, "xreader") ? "xreader" : "evince";
        char *argv[8];
        int n = 0;
        argv[n++] = (char *)bin;
        if (have_page) {
            argv[n++] = "-i";
            argv[n++] = pg;
        }
        if (have_find) {
            argv[n++] = "-l";
            argv[n++] = (char *)needle;
        }
        argv[n++] = (char *)path;
        argv[n] = NULL;
        if (n > 2 && open_reader(argv) == 0) {
            return 0;
        }
        return -1;
    }
    if (desk_has(desk, "okular") || desk_has(desk, "Okular")) {
        const char *bin = okular_bin();
        if (!bin) {
            return -1;
        }
        char *argv[8];
        int n = 0;
        argv[n++] = (char *)bin;
        if (have_page) {
            argv[n++] = "--page";
            argv[n++] = pg;
        }
        if (have_find) {
            argv[n++] = "--find";
            argv[n++] = (char *)needle;
        }
        argv[n++] = (char *)path;
        argv[n] = NULL;
        if (n > 2 && open_reader(argv) == 0) {
            return 0;
        }
        return -1;
    }
    return -1;
}

static int open_epub(const char *path, const char *desk, const char *needle) {
    /* Okular's EPUB sheets are its own layout. Grove's page is not one of
     * them. --find is the location that reader accepts. */
    if (!needle) {
        return -1;
    }
    if (desk_has(desk, "okular") || desk_has(desk, "Okular")) {
        const char *bin = okular_bin();
        if (!bin) {
            return -1;
        }
        char *argv[] = {(char *)bin, "--find", (char *)needle, (char *)path, NULL};
        return open_reader(argv);
    }
    return -1;
}

static int open_plain(const char *path, const char *desk, int has_at, uint32_t at) {
    if (!has_at) {
        return -1;
    }
    int line = plain_line(path, at);
    if (line <= 0) {
        return -1;
    }
    char num[32];
    snprintf(num, sizeof num, "%d", line);
    if (desk_has(desk, "org.x.editor") || desk_has(desk, "xed.desktop")) {
        char plus[32];
        snprintf(plus, sizeof plus, "+%d", line);
        char *argv[] = {"xed", (char *)path, plus, NULL};
        return open_reader(argv);
    }
    if (desk_has(desk, "mousepad")) {
        char *argv[] = {"mousepad", "-l", num, (char *)path, NULL};
        return open_reader(argv);
    }
    return -1;
}

/* has_at is the extracted-text offset of the clicked place. page is Grove's
 * 1-based page, or 0 when the file has none. needle is one line of that
 * passage. Anything the reader cannot take falls through to xdg-open. */
static int open_place(const char *path, int has_at, uint32_t at, int page, const char *needle) {
    const char *ext = ext_of(path);
    char mime[160];
    char desk[256];
    if (query_mime(path, mime, sizeof mime) != 0 || query_desktop(mime, desk, sizeof desk) != 0) {
        return spawn_open(path);
    }
    int jumped = -1;
    if (plain_source(ext)) {
        jumped = open_plain(path, desk, has_at, at);
    } else if (ext_is(ext, "pdf")) {
        jumped = open_pdf(path, desk, page, needle);
    } else if (ext_is(ext, "epub")) {
        jumped = open_epub(path, desk, needle);
    }
    if (jumped == 0) {
        return 0;
    }
    return spawn_open(path);
}

static void on_open(webui_event_t *e) {
    size_t nargs = webui_get_count(e);
    const char *in = webui_get_string_at(e, 0);
    char *path = xstrdup(in ? in : "");
    char *page_url = NULL;
    int allowed = 0;
    if (path) {
        pthread_mutex_lock(&mu);
        allowed = allow_has(path);
        if (allowed && ix) {
            Err ignore;
            err_clear(&ignore);
            if (index_page_url(ix, path, &page_url, &ignore) != 0) {
                free(page_url);
                page_url = NULL;
            }
        }
        pthread_mutex_unlock(&mu);
    }
    int placed = nargs >= 2;
    int has_at = 0;
    uint32_t at = 0;
    int page = 0;
    const char *at_s = placed ? webui_get_string_at(e, 1) : "";
    const char *page_s = nargs >= 3 ? webui_get_string_at(e, 2) : "";
    const char *needle_s = nargs >= 4 ? webui_get_string_at(e, 3) : "";
    if (at_s && at_s[0]) {
        errno = 0;
        char *end = NULL;
        unsigned long long v = strtoull(at_s, &end, 10);
        if (end && end != at_s && *end == 0 && errno == 0 && v <= UINT32_MAX) {
            at = (uint32_t)v;
            has_at = 1;
        }
    }
    if (page_s && page_s[0]) {
        errno = 0;
        char *end = NULL;
        unsigned long v = strtoul(page_s, &end, 10);
        if (end && end != page_s && *end == 0 && errno == 0 && v > 0 && v < 1000000UL) {
            page = (int)v;
        }
    }
    char *needle = placed ? clean_needle(needle_s) : NULL;
    char *json = NULL;
    if (!path) {
        json = NULL;
    } else if (!allowed) {
        json = json_error("that file is not in these results");
    } else {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            json = json_error("that file is not there");
        } else if ((placed ? open_place(path, has_at, at, page, needle) : open_saved(path, page_url)) != 0) {
            json = json_error("could not open the file");
        } else {
            json = xstrdup("{\"ok\":true}");
        }
    }
    free(needle);
    free(page_url);
    free(path);
    reply_owned(e, json);
}

static void on_info(webui_event_t *e) {
    pthread_mutex_lock(&mu);
    char *json = info_json();
    pthread_mutex_unlock(&mu);
    reply_owned(e, json);
}

static char *saved_json(void) {
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    char **queries = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (index_saved_list(ix, &queries, &n, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not read saved searches");
    }
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"saved\":[") != 0) {
        goto fail;
    }
    for (int i = 0; i < n; i++) {
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (jesc(&b, queries[i]) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "]}") != 0) {
        goto fail;
    }
    index_saved_free(queries, n);
    return b.data;
fail:
    index_saved_free(queries, n);
    buf_free(&b);
    return NULL;
}

static void on_saved(webui_event_t *e) {
    pthread_mutex_lock(&mu);
    char *json = saved_json();
    pthread_mutex_unlock(&mu);
    reply_owned(e, json);
}

static void on_save_change(webui_event_t *e, int drop) {
    const char *in = webui_get_string(e);
    char *query = xstrdup(in ? in : "");
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!query) {
        json = NULL;
    } else if (!ix) {
        json = json_error(open_err[0] ? open_err : "no index");
    } else {
        Err err;
        err_clear(&err);
        int rc = drop ? index_saved_drop(ix, query, &err) : index_saved_put(ix, query, &err);
        json = rc == 0 ? saved_json() : json_error(err.msg[0] ? err.msg : "could not save");
    }
    pthread_mutex_unlock(&mu);
    free(query);
    reply_owned(e, json);
}

static void on_save(webui_event_t *e) {
    on_save_change(e, 0);
}

static void on_forget(webui_event_t *e) {
    on_save_change(e, 1);
}

static char *notes_json(void) {
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    Note *notes = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (index_notes_list(ix, &notes, &n, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not read notes");
    }
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"notes\":[") != 0) {
        goto fail;
    }
    for (int i = 0; i < n; i++) {
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"path\":") != 0 || jesc(&b, notes[i].path) != 0 ||
            buf_printf(&b, ",\"offset\":%u,\"body\":", notes[i].offset) != 0 || jesc(&b, notes[i].body) != 0 ||
            buf_putc(&b, '}') != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "]}") != 0) {
        goto fail;
    }
    index_notes_free(notes, n);
    return b.data;
fail:
    index_notes_free(notes, n);
    buf_free(&b);
    return NULL;
}

static void on_notes(webui_event_t *e) {
    pthread_mutex_lock(&mu);
    char *json = notes_json();
    pthread_mutex_unlock(&mu);
    reply_owned(e, json);
}

static int take_offset(webui_event_t *e, int at, uint32_t *out) {
    long long v = webui_get_int_at(e, at);
    if (v < 0 || (unsigned long long)v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static void on_note_put(webui_event_t *e) {
    const char *path_in = webui_get_string_at(e, 0);
    const char *body_in = webui_get_string_at(e, 2);
    uint32_t offset = 0;
    int bad = take_offset(e, 1, &offset);
    char *path = xstrdup(path_in ? path_in : "");
    char *body = xstrdup(body_in ? body_in : "");
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!path || !body) {
        json = NULL;
    } else if (!ix) {
        json = json_error(open_err[0] ? open_err : "no index");
    } else if (bad) {
        json = json_error("bad offset");
    } else {
        Err err;
        err_clear(&err);
        json = index_note_put(ix, path, offset, body, &err) == 0
                   ? notes_json()
                   : json_error(err.msg[0] ? err.msg : "could not keep the note");
    }
    pthread_mutex_unlock(&mu);
    free(path);
    free(body);
    reply_owned(e, json);
}

static char *alerts_json(void) {
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    Alert *alerts = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (index_alerts_list(ix, &alerts, &n, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not read alerts");
    }
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"alerts\":[") != 0) {
        goto fail;
    }
    for (int i = 0; i < n; i++) {
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"query\":") != 0 || jesc(&b, alerts[i].query) != 0 ||
            buf_printf(&b, ",\"count\":%d}", alerts[i].nfiles) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "]}") != 0) {
        goto fail;
    }
    index_alerts_free(alerts, n);
    return b.data;
fail:
    index_alerts_free(alerts, n);
    buf_free(&b);
    return NULL;
}

static void on_alerts(webui_event_t *e) {
    pthread_mutex_lock(&mu);
    char *json = alerts_json();
    pthread_mutex_unlock(&mu);
    reply_owned(e, json);
}

static void on_watch_change(webui_event_t *e, int drop) {
    const char *in = webui_get_string(e);
    char *query = xstrdup(in ? in : "");
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!query) {
        json = NULL;
    } else if (!ix) {
        json = json_error(open_err[0] ? open_err : "no index");
    } else {
        Err err;
        err_clear(&err);
        int rc = drop ? index_alert_unwatch(ix, query, &err) : index_alert_watch(ix, query, &err);
        json = rc == 0 ? alerts_json() : json_error(err.msg[0] ? err.msg : "could not watch");
    }
    pthread_mutex_unlock(&mu);
    free(query);
    reply_owned(e, json);
}

static void on_watch(webui_event_t *e) {
    on_watch_change(e, 0);
}

static void on_unwatch(webui_event_t *e) {
    on_watch_change(e, 1);
}

static void on_note_drop(webui_event_t *e) {
    const char *path_in = webui_get_string_at(e, 0);
    uint32_t offset = 0;
    int bad = take_offset(e, 1, &offset);
    char *path = xstrdup(path_in ? path_in : "");
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!path) {
        json = NULL;
    } else if (!ix) {
        json = json_error(open_err[0] ? open_err : "no index");
    } else if (bad) {
        json = json_error("bad offset");
    } else {
        Err err;
        err_clear(&err);
        json = index_note_drop(ix, path, offset, &err) == 0 ? notes_json()
                                                            : json_error(err.msg[0] ? err.msg : "could not remove the note");
    }
    pthread_mutex_unlock(&mu);
    free(path);
    reply_owned(e, json);
}

static char *excerpts_json(void) {
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    Excerpt *rows = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (index_excerpts_list(ix, &rows, &n, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not read excerpts");
    }
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"excerpts\":[") != 0) {
        goto fail;
    }
    for (int i = 0; i < n; i++) {
        Excerpt *ex = &rows[i];
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"id\":") != 0 || jesc(&b, ex->id) != 0 || buf_puts(&b, ",\"path\":") != 0 ||
            jesc(&b, ex->path) != 0 || buf_puts(&b, ",\"title\":") != 0 || jesc(&b, ex->title) != 0 ||
            buf_puts(&b, ",\"creator\":") != 0 || jesc(&b, ex->creator) != 0 || buf_puts(&b, ",\"heading\":") != 0 ||
            jesc(&b, ex->heading) != 0 || buf_printf(&b, ",\"page\":%d,\"text\":", ex->page) != 0 ||
            jesc(&b, ex->text) != 0 ||
            buf_printf(&b, ",\"byte\":%" PRIu32 ",\"endByte\":%" PRIu32 ",\"tags\":[", ex->byte_at, ex->end_byte) != 0) {
            goto fail;
        }
        for (int t = 0; t < ex->ntags; t++) {
            if ((t && buf_putc(&b, ',') != 0) || jesc(&b, ex->tags[t]) != 0) {
                goto fail;
            }
        }
        if (buf_printf(&b, "],\"created\":%" PRId64 "}", ex->created) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "]}") != 0) {
        goto fail;
    }
    index_excerpts_free(rows, n);
    return b.data;
fail:
    index_excerpts_free(rows, n);
    buf_free(&b);
    return NULL;
}

static int excerpt_tag_ptrs(char *s, const char **out, int *n, Err *err) {
    *n = 0;
    if (!s || !s[0]) {
        return 0;
    }
    char *p = s;
    while (*p) {
        if (*n >= EXCERPT_TAGS_MAX) {
            err_set(err, "that passage has enough tags");
            return -1;
        }
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = 0;
        }
        out[(*n)++] = p;
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return 0;
}

static void on_excerpts(webui_event_t *e) {
    pthread_mutex_lock(&mu);
    char *json = excerpts_json();
    pthread_mutex_unlock(&mu);
    reply_owned(e, json);
}

static void on_excerpt_save(webui_event_t *e) {
    char *id = xstrdup(webui_get_string_at(e, 0) ? webui_get_string_at(e, 0) : "");
    char *path = xstrdup(webui_get_string_at(e, 1) ? webui_get_string_at(e, 1) : "");
    char *title = xstrdup(webui_get_string_at(e, 2) ? webui_get_string_at(e, 2) : "");
    char *creator = xstrdup(webui_get_string_at(e, 3) ? webui_get_string_at(e, 3) : "");
    char *heading = xstrdup(webui_get_string_at(e, 4) ? webui_get_string_at(e, 4) : "");
    char *text = xstrdup(webui_get_string_at(e, 6) ? webui_get_string_at(e, 6) : "");
    char *tags = xstrdup(webui_get_string_at(e, 10) ? webui_get_string_at(e, 10) : "");
    long long page = webui_get_int_at(e, 5);
    long long byte_at = webui_get_int_at(e, 7);
    long long end_byte = webui_get_int_at(e, 8);
    long long created = webui_get_int_at(e, 9);
    const char *tagv[EXCERPT_TAGS_MAX];
    int ntags = 0;
    Err terr;
    err_clear(&terr);
    int tags_bad = !tags || excerpt_tag_ptrs(tags, tagv, &ntags, &terr) != 0;
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!id || !path || !title || !creator || !heading || !text || !tags) {
        json = NULL;
    } else if (!ix) {
        json = json_error(open_err[0] ? open_err : "no index");
    } else if (page < 0 || page > INT_MAX) {
        json = json_error("bad page");
    } else if (byte_at < 0 || byte_at > UINT32_MAX || end_byte < 0 || end_byte > UINT32_MAX) {
        json = json_error("bad place");
    } else if (created < 0) {
        json = json_error("bad time");
    } else if (tags_bad) {
        json = json_error(terr.msg[0] ? terr.msg : "empty tag");
    } else {
        ExcerptSpec spec;
        memset(&spec, 0, sizeof spec);
        spec.id = id;
        spec.path = path;
        spec.title = title;
        spec.creator = creator;
        spec.heading = heading;
        spec.page = (int)page;
        spec.text = text;
        spec.byte_at = (uint32_t)byte_at;
        spec.end_byte = (uint32_t)end_byte;
        spec.tags = tagv;
        spec.ntags = ntags;
        spec.created = (int64_t)created;
        Err err;
        err_clear(&err);
        json = index_excerpt_put(ix, &spec, &err) == 0
                   ? excerpts_json()
                   : json_error(err.msg[0] ? err.msg : "could not keep the excerpt");
    }
    pthread_mutex_unlock(&mu);
    free(id);
    free(path);
    free(title);
    free(creator);
    free(heading);
    free(text);
    free(tags);
    reply_owned(e, json);
}

static void on_excerpt_tags(webui_event_t *e) {
    char *id = xstrdup(webui_get_string_at(e, 0) ? webui_get_string_at(e, 0) : "");
    char *tags = xstrdup(webui_get_string_at(e, 1) ? webui_get_string_at(e, 1) : "");
    const char *tagv[EXCERPT_TAGS_MAX];
    int ntags = 0;
    Err terr;
    err_clear(&terr);
    int tags_bad = !tags || excerpt_tag_ptrs(tags, tagv, &ntags, &terr) != 0;
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!id || !tags) {
        json = NULL;
    } else if (!ix) {
        json = json_error(open_err[0] ? open_err : "no index");
    } else if (tags_bad) {
        json = json_error(terr.msg[0] ? terr.msg : "empty tag");
    } else {
        Err err;
        err_clear(&err);
        json = index_excerpt_tags(ix, id, tagv, ntags, &err) == 0
                   ? excerpts_json()
                   : json_error(err.msg[0] ? err.msg : "could not keep the excerpt");
    }
    pthread_mutex_unlock(&mu);
    free(id);
    free(tags);
    reply_owned(e, json);
}

static void on_excerpt_drop(webui_event_t *e) {
    char *id = xstrdup(webui_get_string(e) ? webui_get_string(e) : "");
    pthread_mutex_lock(&mu);
    char *json = NULL;
    if (!id) {
        json = NULL;
    } else if (!ix) {
        json = json_error(open_err[0] ? open_err : "no index");
    } else {
        Err err;
        err_clear(&err);
        json = index_excerpt_drop(ix, id, &err) == 0 ? excerpts_json()
                                                      : json_error(err.msg[0] ? err.msg : "could not keep the excerpt");
    }
    pthread_mutex_unlock(&mu);
    free(id);
    reply_owned(e, json);
}

/* The host stays up. This module is the program: a save of page.html reloads
 * the page, and a new build of this file replaces the module. */
static const char *live_path;
static const char *live_root;
static const char *live_name;
static struct timespec live_mtim;
static volatile int live_stop;
static size_t live_win;
static pthread_t watcher;
static int watching;

static int mtim_eq(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static void *live_watch(void *arg) {
    (void)arg;
    struct timespec pending;
    int have_pending = 0;
    memset(&pending, 0, sizeof pending);
    while (!live_stop) {
        struct stat st;
        if (stat(live_path, &st) == 0 && st.st_size > 0) {
            struct timespec now = grove_mtim(&st);
            if (mtim_eq(&now, &live_mtim)) {
                have_pending = 0;
            } else if (have_pending && mtim_eq(&now, &pending)) {
                live_mtim = now;
                have_pending = 0;
                /* Keep the query the way CToy keeps a registered block. */
                webui_run(live_win,
                          "(function(){try{var q=document.getElementById('q');"
                          "if(q)sessionStorage.setItem('grove.q',q.value);"
                          "}catch(e){}location.reload();})()");
                fprintf(stderr, "grove: reloaded %s\n", live_path);
                fflush(stderr);
            } else {
                pending = now;
                have_pending = 1;
            }
        } else {
            have_pending = 0;
        }
        usleep(100000);
    }
    return NULL;
}

#define GUARD(name)                                                                 \
    static void name##_guard(webui_event_t *e) {                                    \
        if (ui_enter() != 0) {                                                      \
            reply(e, "{\"error\":\"updating\"}");                                   \
            return;                                                                 \
        }                                                                           \
        name(e);                                                                    \
        ui_leave();                                                                 \
    }

static char *read_json(const char *path, uint32_t at, int dir) {
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    Err e;
    err_clear(&e);
    DocRead *rd = NULL;
    if (index_read(ix, path, at, dir, &rd, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not open the text");
    }
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"format\":\"grove\",\"title\":") != 0 || jesc(&b, rd->title) != 0 ||
        buf_puts(&b, ",\"section\":") != 0 || jesc(&b, rd->section) != 0 || buf_puts(&b, ",\"path\":") != 0 ||
        jesc(&b, path) != 0) {
        goto fail;
    }
    if (buf_printf(&b, ",\"from\":%u,\"to\":%u,\"at\":%u,\"length\":%u,\"page\":%d,\"text\":", rd->from, rd->to, rd->at,
                   rd->length, rd->page) != 0 ||
        jesc(&b, rd->text) != 0 || buf_puts(&b, ",\"heads\":[") != 0) {
        goto fail;
    }
    for (int i = 0; i < rd->nheads; i++) {
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_printf(&b, "{\"at\":%u,\"level\":%d,\"text\":", rd->heads[i].at, rd->heads[i].level) != 0 ||
            jesc(&b, rd->heads[i].text) != 0 || buf_putc(&b, '}') != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "],\"pages\":[") != 0) {
        goto fail;
    }
    for (int i = 0; i < rd->npages; i++) {
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_printf(&b, "{\"at\":%u,\"page\":%d}", rd->pages[i].at, rd->pages[i].page) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "]}") != 0) {
        goto fail;
    }
    docread_free(rd);
    return b.data;
fail:
    docread_free(rd);
    buf_free(&b);
    return NULL;
}

static void on_read(webui_event_t *e) {
    const char *in = webui_get_string_at(e, 0);
    char *path = xstrdup(in ? in : "");
    const char *at_s = webui_get_string_at(e, 1);
    const char *dir_s = webui_get_string_at(e, 2);
    uint32_t at = 0;
    int dir = 0;
    if (at_s && at_s[0]) {
        errno = 0;
        char *end = NULL;
        unsigned long long v = strtoull(at_s, &end, 10);
        if (end && end != at_s && *end == 0 && errno == 0 && v <= UINT32_MAX) {
            at = (uint32_t)v;
        }
    }
    if (dir_s && dir_s[0] == '-') {
        dir = -1;
    } else if (dir_s && dir_s[0] == '1') {
        dir = 1;
    }
    char *json = NULL;
    if (!path) {
        json = NULL;
    } else {
        pthread_mutex_lock(&mu);
        json = read_json(path, at, dir);
        if (json && strncmp(json, "{\"error\":", 9) != 0) {
            allow_add(path);
        }
        pthread_mutex_unlock(&mu);
    }
    free(path);
    reply_owned(e, json);
}

/* A file input in the page does not yield a path, so the chooser is a
 * desktop dialog. 0 is a path, 1 is cancel, -1 is no chooser.
 * Windows uses the system file dialog instead of a helper program. */
#if !defined(_WIN32)
static int dialog_path(char *const argv[], char *out, size_t n, int *cancel) {
    *cancel = 0;
    if (!out || n < 2) {
        return -1;
    }
    out[0] = 0;
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return -1;
    }
    size_t got = 0;
    int extra = 0;
    int failed = 0;
    while (got + 1 < n) {
        ssize_t r = read(fds[0], out + got, n - 1 - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = 1;
            break;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    if (!failed && got + 1 >= n) {
        char dump[256];
        for (;;) {
            ssize_t r = read(fds[0], dump, sizeof dump);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (r == 0) {
                break;
            }
            extra = 1;
        }
    }
    out[got] = 0;
    close(fds[0]);
    int st = 0;
    for (;;) {
        pid_t w = waitpid(pid, &st, 0);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0) {
            return -1;
        }
        break;
    }
    while (got && (out[got - 1] == '\n' || out[got - 1] == '\r')) {
        out[--got] = 0;
    }
    if (failed || extra || !WIFEXITED(st)) {
        return -1;
    }
    if (WEXITSTATUS(st) == 0 && out[0]) {
        return 0;
    }
    if (WEXITSTATUS(st) == 1) {
        *cancel = 1;
        out[0] = 0;
        return 1;
    }
    return -1;
}
#endif

#if defined(_WIN32)
/* FileOpenDialog. Defined here so the program does not depend on an import library for the GUID. */
static const CLSID grove_clsid_file_open = {0xDC1C5A9C, 0xE88A, 0x4dde, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const IID grove_iid_file_open = {0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};

struct WinPick {
    int directory;
    HWND owner;
    char *out;
    size_t n;
    int cancel;
    int rc;
};

static int win_pick_show(struct WinPick *job) {
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR name = NULL;
    HRESULT hr;
    DWORD opts = 0;
    HWND owner;
    DWORD tid;
    int chars;
    int rc = -1;
    job->cancel = 0;
    if (!job->out || job->n < 2 || job->n > 2147483647u) {
        return -1;
    }
    job->out[0] = 0;
    owner = job->owner;
    if (owner) {
        tid = GetWindowThreadProcessId(owner, NULL);
        if (tid != GetCurrentThreadId()) {
            owner = NULL;
        }
    }
    hr = CoCreateInstance(&grove_clsid_file_open, NULL, CLSCTX_INPROC_SERVER, &grove_iid_file_open, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        return -1;
    }
    if (FAILED(dlg->lpVtbl->GetOptions(dlg, &opts))) {
        opts = 0;
    }
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (job->directory) {
        opts |= FOS_PICKFOLDERS;
    } else {
        opts |= FOS_FILEMUSTEXIST;
    }
    dlg->lpVtbl->SetOptions(dlg, opts);
    dlg->lpVtbl->SetTitle(dlg, job->directory ? L"Add a folder" : L"Add a file");
    hr = dlg->lpVtbl->Show(dlg, owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        job->cancel = 1;
        rc = 1;
        goto done;
    }
    if (FAILED(hr)) {
        goto done;
    }
    hr = dlg->lpVtbl->GetResult(dlg, &item);
    if (FAILED(hr) || !item) {
        goto done;
    }
    hr = item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &name);
    if (FAILED(hr) || !name || !name[0]) {
        goto done;
    }
    chars = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, job->out, (int)job->n, NULL, NULL);
    if (chars <= 1) {
        job->out[0] = 0;
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            rc = 2;
        }
        goto done;
    }
    rc = 0;
done:
    if (name) {
        CoTaskMemFree(name);
    }
    if (item) {
        item->lpVtbl->Release(item);
    }
    if (dlg) {
        dlg->lpVtbl->Release(dlg);
    }
    return rc;
}

static DWORD WINAPI win_pick_thread(void *arg) {
    struct WinPick *job = arg;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        job->rc = -1;
        return 0;
    }
    job->rc = win_pick_show(job);
    CoUninitialize();
    return 0;
}

static int win_pick(size_t window, int directory, char *out, size_t n, int *cancel) {
    struct WinPick job;
    HRESULT hr;
    int own_init = 0;
    memset(&job, 0, sizeof job);
    job.directory = directory;
    job.out = out;
    job.n = n;
    job.owner = (HWND)webui_get_hwnd(window);
    job.rc = -1;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == S_OK) {
        own_init = 1;
    } else if (hr == RPC_E_CHANGED_MODE) {
        HANDLE thread = CreateThread(NULL, 0, win_pick_thread, &job, 0, NULL);
        if (!thread) {
            return -1;
        }
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        *cancel = job.cancel;
        return job.rc;
    } else if (FAILED(hr)) {
        return -1;
    }
    job.rc = win_pick_show(&job);
    if (own_init) {
        CoUninitialize();
    }
    *cancel = job.cancel;
    return job.rc;
}
#endif

#if defined(__APPLE__)
static int mac_pick(int directory, char *out, size_t n, int *cancel) {
    char *script = directory ? "POSIX path of (choose folder with prompt \"Add a folder\")"
                             : "POSIX path of (choose file with prompt \"Add a file\")";
    char *argv[] = {"osascript", "-e", script, NULL};
    return dialog_path(argv, out, n, cancel);
}
#endif

static void strip_dir_slash(char *path) {
    size_t n;
    if (!path) {
        return;
    }
    n = strlen(path);
    if (n <= 1) {
        return;
    }
    if (path[n - 1] != '/' && path[n - 1] != '\\') {
        return;
    }
    if (n == 3 && path[1] == ':') {
        return;
    }
    path[n - 1] = 0;
}

#if !defined(_WIN32) && !defined(__APPLE__)
/* The WebKit window is ours. Zenity stays above it when it knows that id.
 * libX11 is opened only for this lookup, so a command does not need it. */
static unsigned long chooser_xid(void) {
    void *lib;
    Display *(*open_dpy)(const char *);
    int (*close_dpy)(Display *);
    Atom (*intern)(Display *, const char *, Bool);
    int (*get_prop)(Display *, Window, Atom, long, long, Bool, Atom, Atom *, int *, unsigned long *, unsigned long *,
                    unsigned char **);
    int (*xfree)(void *);
    int (*get_attr)(Display *, Window, XWindowAttributes *);
    Display *dpy;
    Atom clients;
    Atom pid_atom;
    Atom actual = None;
    int format = 0;
    unsigned long n = 0;
    unsigned long left = 0;
    unsigned char *raw = NULL;
    unsigned long xid = 0;
    unsigned long best = 0;
    pid_t self;
    lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!lib) {
        return 0;
    }
    open_dpy = (Display * (*)(const char *))dlsym(lib, "XOpenDisplay");
    close_dpy = (int (*)(Display *))dlsym(lib, "XCloseDisplay");
    intern = (Atom(*)(Display *, const char *, Bool))dlsym(lib, "XInternAtom");
    get_prop = (int (*)(Display *, Window, Atom, long, long, Bool, Atom, Atom *, int *, unsigned long *, unsigned long *,
                        unsigned char **))dlsym(lib, "XGetWindowProperty");
    xfree = (int (*)(void *))dlsym(lib, "XFree");
    get_attr = (int (*)(Display *, Window, XWindowAttributes *))dlsym(lib, "XGetWindowAttributes");
    if (!open_dpy || !close_dpy || !intern || !get_prop || !xfree) {
        dlclose(lib);
        return 0;
    }
    dpy = open_dpy(NULL);
    if (!dpy) {
        dlclose(lib);
        return 0;
    }
    self = getpid();
    clients = intern(dpy, "_NET_CLIENT_LIST", False);
    pid_atom = intern(dpy, "_NET_WM_PID", False);
    if (clients != None && pid_atom != None &&
        get_prop(dpy, DefaultRootWindow(dpy), clients, 0, 1024, False, XA_WINDOW, &actual, &format, &n, &left, &raw) ==
            Success &&
        raw && format == 32) {
        const long *ids = (const long *)raw;
        unsigned long i;
        if (n > 1024) {
            n = 1024;
        }
        for (i = 0; i < n; i++) {
            Window w = (Window)ids[i];
            unsigned char *pdata = NULL;
            Atom pactual = None;
            int pformat = 0;
            unsigned long pn = 0;
            unsigned long pleft = 0;
            if (get_prop(dpy, w, pid_atom, 0, 1, False, XA_CARDINAL, &pactual, &pformat, &pn, &pleft, &pdata) == Success &&
                pdata && pformat == 32 && pn >= 1) {
                long v = *(long *)pdata;
                if (v == (long)self && v > 0) {
                    XWindowAttributes attr;
                    unsigned long area;
                    memset(&attr, 0, sizeof attr);
                    if (get_attr && (get_attr(dpy, w, &attr) == 0 || attr.width < 64 || attr.height < 64)) {
                        xfree(pdata);
                        continue;
                    }
                    area = get_attr ? (unsigned long)attr.width * (unsigned long)attr.height : 1;
                    if (area >= best) {
                        best = area;
                        xid = (unsigned long)w;
                    }
                }
            }
            if (pdata) {
                xfree(pdata);
            }
        }
    }
    if (raw) {
        xfree(raw);
    }
    close_dpy(dpy);
    dlclose(lib);
    return xid;
}
#endif

static char *path_json(const char *path) {
    Buf b;
    buf_init(&b);
    if (buf_puts(&b, "{\"path\":") != 0 || jesc(&b, path ? path : "") != 0 || buf_putc(&b, '}') != 0) {
        buf_free(&b);
        return NULL;
    }
    return b.data;
}

static char *pick_json(size_t window, const char *kind) {
    int directory = 0;
    if (kind && strcmp(kind, "dir") == 0) {
        directory = 1;
    } else if (!kind || strcmp(kind, "file") != 0) {
        return json_error("choose a folder or a file");
    }
    char out[8192];
    int cancel = 0;
    int ran = -1;
#if defined(_WIN32)
    ran = win_pick(window, directory, out, sizeof out, &cancel);
#elif defined(__APPLE__)
    ran = mac_pick(directory, out, sizeof out, &cancel);
    (void)window;
#else
    if (on_path("zenity") || on_path("qarma") || on_path("yad")) {
        char *bin = on_path("zenity") ? "zenity" : (on_path("qarma") ? "qarma" : "yad");
        char *title = directory ? "Add a folder" : "Add a file";
        char attach[64];
        unsigned long xid = 0;
        int pass;
        int parented = strcmp(bin, "yad") != 0;
        if (parented) {
            xid = chooser_xid();
        }
        for (pass = 0; pass < 2 && ran < 0; pass++) {
            char *argv[8];
            int argc = 0;
            if (pass == 1 && !parented) {
                break;
            }
            argv[argc++] = bin;
            argv[argc++] = "--file-selection";
            if (directory) {
                argv[argc++] = "--directory";
            }
            argv[argc++] = "--title";
            argv[argc++] = title;
            if (pass == 0 && parented) {
                int nw;
                argv[argc++] = "--modal";
                if (xid != 0) {
                    nw = snprintf(attach, sizeof attach, "--attach=%lu", xid);
                    if (nw > 0 && (size_t)nw < sizeof attach) {
                        argv[argc++] = attach;
                    }
                }
            }
            argv[argc] = NULL;
            cancel = 0;
            ran = dialog_path(argv, out, sizeof out, &cancel);
        }
    }
    if (ran < 0 && on_path("kdialog")) {
        const char *home = getenv("HOME");
        if (!home || !home[0]) {
            home = ".";
        }
        char *argv[] = {"kdialog", directory ? "--getexistingdirectory" : "--getopenfilename", (char *)home, NULL};
        cancel = 0;
        ran = dialog_path(argv, out, sizeof out, &cancel);
    }
    (void)window;
#endif
    if (ran == 2) {
        return json_error("that path is not usable");
    }
    if (ran < 0) {
        return json_error("Could not open the folder chooser.");
    }
    if (cancel || !out[0]) {
        return xstrdup("{\"path\":\"\"}");
    }
    if (directory) {
        strip_dir_slash(out);
    }
    if (strchr(out, '\n') || strchr(out, '\r') || strlen(out) > 4096) {
        return json_error("that path is not usable");
    }
    return path_json(out);
}

static int write_facet_list(Buf *b, const Facet *a, int n) {
    for (int t = 0; t < n; t++) {
        if (t && buf_putc(b, ',') != 0) {
            return -1;
        }
        if (buf_puts(b, "{\"value\":") != 0 || jesc(b, a[t].value) != 0 ||
            buf_printf(b, ",\"count\":%d}", a[t].count) != 0) {
            return -1;
        }
    }
    return 0;
}

static int write_narrow(Buf *b, const Narrow *n) {
    const Facet *cols[5];
    int ns[5];
    const char *names[5] = {"language", "author", "form", "period", "genre"};
    cols[0] = n->language;
    ns[0] = n->nlanguage;
    cols[1] = n->author;
    ns[1] = n->nauthor;
    cols[2] = n->form;
    ns[2] = n->nform;
    cols[3] = n->period;
    ns[3] = n->nperiod;
    cols[4] = n->genre;
    ns[4] = n->ngenre;
    if (buf_putc(b, '{') != 0) {
        return -1;
    }
    for (int i = 0; i < 5; i++) {
        if (i && buf_putc(b, ',') != 0) {
            return -1;
        }
        if (buf_putc(b, '"') != 0 || buf_puts(b, names[i]) != 0 || buf_puts(b, "\":[") != 0 ||
            write_facet_list(b, cols[i], ns[i]) != 0 || buf_putc(b, ']') != 0) {
            return -1;
        }
    }
    return buf_putc(b, '}');
}

static int jstr(Buf *b, const char *key, const char *val) {
    if (buf_putc(b, ',') != 0 || buf_putc(b, '"') != 0 || buf_puts(b, key) != 0 || buf_puts(b, "\":") != 0) {
        return -1;
    }
    return jesc(b, val);
}

static int library_sort_col(const char *name) {
    static const char *names[] = {"title",  "creator", "date",     "language",
                                  "type",   "subject", "coverage", "source"};
    if (!name || !name[0]) {
        return LIB_SORT_TITLE;
    }
    for (int i = 0; i < LIB_SORT_SOURCE + 1; i++) {
        if (strcmp(name, names[i]) == 0) {
            return i;
        }
    }
    return LIB_SORT_TITLE;
}

static const char *library_sort_name(int col) {
    static const char *names[] = {"title",  "creator", "date",     "language",
                                  "type",   "subject", "coverage", "source"};
    if (col < LIB_SORT_TITLE || col > LIB_SORT_SOURCE) {
        return names[LIB_SORT_TITLE];
    }
    return names[col];
}

static char *library_json(const char *filter, const char *language, const char *author, const char *form,
                           const char *period, const char *genre, const char *sort_name, const char *sort_dir) {
    int sort_col = library_sort_col(sort_name);
    int sort_desc = sort_dir && strcmp(sort_dir, "desc") == 0;
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    if (!index_shelf_ready(ix)) {
        return json_error("Preparing the shelf…");
    }
    Library lib;
    memset(&lib, 0, sizeof lib);
    Err e;
    err_clear(&e);
    if (index_library_sorted(ix, filter && filter[0] ? filter : NULL, language, author, form, period, genre, sort_col,
                             sort_desc, &lib, &e) != 0) {
        return json_error(e.msg[0] ? e.msg : "could not read the library");
    }
    Buf b;
    buf_init(&b);
    if (buf_printf(&b, "{\"total\":%d,\"shown\":%d,\"sort\":", lib.total, lib.n) != 0 ||
        jesc(&b, library_sort_name(sort_col)) != 0 || buf_printf(&b, ",\"desc\":%d,\"roots\":[", sort_desc) != 0) {
        goto fail;
    }
    for (int i = 0; i < lib.nroots; i++) {
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"path\":") != 0 || jesc(&b, lib.roots[i]) != 0 ||
            buf_printf(&b, ",\"full\":%d}", lib.root_full[i] ? 1 : 0) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "],\"volumes\":[") != 0) {
        goto fail;
    }
    for (int i = 0; i < lib.n; i++) {
        const Volume *v = &lib.volumes[i];
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        char *page_url = NULL;
        if (index_page_url(ix, v->path, &page_url, &e) != 0) {
            free(page_url);
            library_free(&lib);
            buf_free(&b);
            return json_error(e.msg[0] ? e.msg : "could not read the library");
        }
        if (buf_puts(&b, "{\"path\":") != 0 || jesc(&b, v->path) != 0 || jstr(&b, "title", v->title) != 0 ||
            jstr(&b, "ext", v->ext) != 0 ||
            buf_printf(&b, ",\"mtime\":%" PRId64 ",\"size\":%" PRId64, v->mtime, v->size) != 0 ||
            jstr(&b, "creator", v->creator) != 0 || jstr(&b, "date", v->date) != 0 ||
            jstr(&b, "language", v->language) != 0 || jstr(&b, "type", v->type) != 0 ||
            jstr(&b, "subject", v->subject) != 0 || jstr(&b, "coverage", v->coverage) != 0 ||
            jstr(&b, "source", v->source) != 0 || jstr(&b, "description", v->description) != 0 ||
            jstr(&b, "publisher", v->publisher) != 0 || jstr(&b, "contributor", v->contributor) != 0 ||
            jstr(&b, "format", v->format) != 0 || jstr(&b, "identifier", v->identifier) != 0 ||
            jstr(&b, "url", page_url) != 0 ||
            jstr(&b, "relation", v->relation) != 0 || jstr(&b, "rights", v->rights) != 0 || buf_putc(&b, '}') != 0) {
            free(page_url);
            goto fail;
        }
        free(page_url);
    }
    if (buf_puts(&b, "],\"narrow\":") != 0 || write_narrow(&b, &lib.narrow) != 0 || buf_putc(&b, '}') != 0) {
        goto fail;
    }
    library_free(&lib);
    return b.data;
fail:
    library_free(&lib);
    buf_free(&b);
    return NULL;
}

static char *personas_json(void) {
    Personas list;
    Buf b;
    int i;
    if (!ix) {
        return json_error(open_err[0] ? open_err : "no index");
    }
    memset(&list, 0, sizeof list);
    Err e;
    err_clear(&e);
    if (index_personas(ix, &list, &e) != 0) {
        personas_free(&list);
        return json_error(e.msg[0] ? e.msg : "could not read personas");
    }
    buf_init(&b);
    if (buf_printf(&b, "{\"truncated\":%d,\"links\":[", list.truncated ? 1 : 0) != 0) {
        goto fail;
    }
    for (i = 0; i < list.n; i++) {
        const PersonaLink *link = &list.links[i];
        if (i && buf_putc(&b, ',') != 0) {
            goto fail;
        }
        if (buf_puts(&b, "{\"name\":") != 0 || jesc(&b, link->name) != 0 || jstr(&b, "role", link->role) != 0 ||
            jstr(&b, "title", link->title) != 0 || jstr(&b, "path", link->path) != 0 ||
            jstr(&b, "date", link->date) != 0 || buf_putc(&b, '}') != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "]}") != 0) {
        goto fail;
    }
    personas_free(&list);
    return b.data;
fail:
    personas_free(&list);
    buf_free(&b);
    return NULL;
}

static int b64_append(Buf *b, const unsigned char *p, size_t n) {
    static const char enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    while (i + 3 <= n) {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8) | (unsigned)p[i + 2];
        char out[4];
        out[0] = enc[(v >> 18) & 63];
        out[1] = enc[(v >> 12) & 63];
        out[2] = enc[(v >> 6) & 63];
        out[3] = enc[v & 63];
        if (buf_append(b, out, 4) != 0) {
            return -1;
        }
        i += 3;
    }
    if (i < n) {
        unsigned v = (unsigned)p[i] << 16;
        char out[4];
        int two = i + 1 < n;
        if (two) {
            v |= (unsigned)p[i + 1] << 8;
        }
        out[0] = enc[(v >> 18) & 63];
        out[1] = enc[(v >> 12) & 63];
        out[2] = two ? enc[(v >> 6) & 63] : '=';
        out[3] = '=';
        if (buf_append(b, out, 4) != 0) {
            return -1;
        }
    }
    return 0;
}

static int photo_mime(const char *type) {
    return type && (strcmp(type, "image/jpeg") == 0 || strcmp(type, "image/png") == 0 || strcmp(type, "image/gif") == 0 ||
                    strcmp(type, "image/webp") == 0);
}

static char *profile_json(const PersonaProfile *p) {
    Buf b;
    int i;
    const char *state = "none";
    buf_init(&b);
    if (p->state == PERSONA_READY) {
        state = "ready";
    } else if (p->state == PERSONA_UNAVAILABLE) {
        state = "unavailable";
    }
    if (buf_puts(&b, "{\"state\":") != 0 || jesc(&b, state) != 0 || jstr(&b, "qid", p->qid) != 0 ||
        jstr(&b, "description", p->description) != 0 || jstr(&b, "summary", p->summary) != 0 ||
        jstr(&b, "born", p->born) != 0 || jstr(&b, "died", p->died) != 0 ||
        jstr(&b, "occupation", p->occupation) != 0 || jstr(&b, "wikipedia", p->wikipedia) != 0 ||
        buf_puts(&b, ",\"aliases\":[") != 0) {
        goto fail;
    }
    for (i = 0; i < p->alias_n; i++) {
        if ((i && buf_putc(&b, ',') != 0) || jesc(&b, p->aliases[i]) != 0) {
            goto fail;
        }
    }
    if (buf_puts(&b, "],\"works\":[") != 0) {
        goto fail;
    }
    for (i = 0; i < p->work_n; i++) {
        const PersonaPubWork *w = &p->works[i];
        if ((i && buf_putc(&b, ',') != 0) || buf_puts(&b, "{\"title\":") != 0 || jesc(&b, w->title) != 0 ||
            jstr(&b, "year", w->year) != 0 || jstr(&b, "url", w->url) != 0 || buf_putc(&b, '}') != 0) {
            goto fail;
        }
    }
    if (buf_putc(&b, ']') != 0) {
        goto fail;
    }
    if (p->photo && p->photo_n > 0 && photo_mime(p->photo_type)) {
        if (buf_puts(&b, ",\"photo\":\"data:") != 0 || buf_puts(&b, p->photo_type) != 0 ||
            buf_puts(&b, ";base64,") != 0 || b64_append(&b, p->photo, p->photo_n) != 0 || buf_putc(&b, '"') != 0) {
            goto fail;
        }
    }
    if (buf_putc(&b, '}') != 0) {
        goto fail;
    }
    return b.data;
fail:
    buf_free(&b);
    return NULL;
}

static void on_personas(webui_event_t *e) {
    char *json = NULL;
    pthread_mutex_lock(&mu);
    json = personas_json();
    pthread_mutex_unlock(&mu);
    reply_owned(e, json);
}

static char *job_json(void) {
    GroveJob job;
    grove_host_job(&job);
    Buf b;
    buf_init(&b);
    if (buf_printf(&b,
                   "{\"running\":%d,\"kind\":%d,\"ok\":%d,\"indexed\":%" PRIu32 ",\"unchanged\":%" PRIu32
                   ",\"removed\":%" PRIu32 ",\"skipped\":%" PRIu32 ",\"error\":",
                   job.running ? 1 : 0, job.kind, job.ok ? 1 : 0, job.indexed, job.unchanged, job.removed,
                   job.skipped) != 0 ||
        jesc(&b, job.error) != 0 || buf_putc(&b, '}') != 0) {
        buf_free(&b);
        return NULL;
    }
    return b.data;
}

static const char *arg_at(webui_event_t *e, size_t i) {
    const char *s = webui_get_string_at(e, i);
    return s ? s : "";
}

static void on_persona_profile(webui_event_t *e) {
    char *name = xstrdup(arg_at(e, 0));
    char *dir = xstrdup(index_dir ? index_dir : "");
    PersonaProfile profile;
    Err err;
    char *json = NULL;
    if (!name || !dir) {
        free(name);
        free(dir);
        reply(e, "{\"error\":\"out of memory\"}");
        return;
    }
    memset(&profile, 0, sizeof profile);
    err_clear(&err);
    if (persona_profile(dir, name, &profile, &err) != 0) {
        json = json_error(err.msg[0] ? err.msg : "could not read that profile");
    } else {
        json = profile_json(&profile);
    }
    persona_profile_free(&profile);
    free(name);
    free(dir);
    reply_owned(e, json);
}

static void on_library(webui_event_t *e) {
    enum { LIB_ARGS = 8 };
    char *field[LIB_ARGS];
    int n = 0;
    char *json = NULL;
    for (size_t i = 0; i < LIB_ARGS; i++) {
        field[i] = xstrdup(arg_at(e, i));
        if (!field[i]) {
            for (int j = 0; j < n; j++) {
                free(field[j]);
            }
            reply(e, "{\"error\":\"out of memory\"}");
            return;
        }
        n++;
    }
    pthread_mutex_lock(&mu);
    json = library_json(field[0], field[1], field[2], field[3], field[4], field[5], field[6], field[7]);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < LIB_ARGS; i++) {
        free(field[i]);
    }
    reply_owned(e, json);
}

static void on_catalog(webui_event_t *e) {
    enum { CAT_N = 16 };
    char *field[CAT_N];
    int n = 0;
    for (size_t i = 0; i < CAT_N; i++) {
        field[i] = xstrdup(arg_at(e, i));
        if (!field[i]) {
            for (int j = 0; j < n; j++) {
                free(field[j]);
            }
            reply(e, "{\"error\":\"out of memory\"}");
            return;
        }
        n++;
    }
    Volume rec;
    memset(&rec, 0, sizeof rec);
    rec.path = field[0];
    rec.title = field[1];
    rec.creator = field[2];
    rec.date = field[3];
    rec.language = field[4];
    rec.type = field[5];
    rec.subject = field[6];
    rec.coverage = field[7];
    rec.source = field[8];
    rec.description = field[9];
    rec.publisher = field[10];
    rec.contributor = field[11];
    rec.format = field[12];
    rec.identifier = field[13];
    rec.relation = field[14];
    rec.rights = field[15];
    Err err;
    err_clear(&err);
    pthread_mutex_lock(&mu);
    int rc = ix ? index_catalog_put(ix, &rec, &err) : -1;
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < CAT_N; i++) {
        free(field[i]);
    }
    if (!ix) {
        reply_owned(e, json_error(open_err[0] ? open_err : "no index"));
        return;
    }
    reply_owned(e, rc == 0 ? xstrdup("{\"ok\":true}") : json_error(err.msg[0] ? err.msg : "could not save the record"));
}

static void on_drop(webui_event_t *e) {
    const char *in = webui_get_string_at(e, 0);
    char *path = xstrdup(in ? in : "");
    if (!path) {
        reply(e, "{\"error\":\"out of memory\"}");
        return;
    }
    Err err;
    err_clear(&err);
    int rc = -1;
    pthread_mutex_lock(&mu);
    if (ix) {
        rc = index_library_drop(ix, path, &err);
    }
    pthread_mutex_unlock(&mu);
    free(path);
    if (!ix) {
        reply_owned(e, json_error(open_err[0] ? open_err : "no index"));
        return;
    }
    reply_owned(e, rc == 0 ? xstrdup("{\"ok\":true}") : json_error(err.msg[0] ? err.msg : "could not remove the volume"));
}

static void on_add(webui_event_t *e) {
    const char *in = webui_get_string_at(e, 0);
    if (!in || !in[0]) {
        reply(e, "{\"error\":\"type a folder or a file\"}");
        return;
    }
    if (strlen(in) > 4096 || strchr(in, '\n') || strchr(in, '\r')) {
        reply(e, "{\"error\":\"that path is not usable\"}");
        return;
    }
    if (grove_host_import(index_dir, in) != 0) {
        GroveJob job;
        grove_host_job(&job);
        reply(e, job.running ? "{\"error\":\"an import is already running\"}" : "{\"error\":\"could not start\"}");
        return;
    }
    reply(e, "{\"ok\":true}");
}

static int address_blank(const char *s) {
    if (!s) {
        return 1;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return *s == 0;
}

static void on_fetch(webui_event_t *e) {
    const char *in = webui_get_string_at(e, 0);
    if (address_blank(in)) {
        reply(e, "{\"error\":\"type a page address\"}");
        return;
    }
    if (strlen(in) > 4096) {
        reply(e, "{\"error\":\"that address is too long\"}");
        return;
    }
    if (strchr(in, '\n') || strchr(in, '\r')) {
        reply(e, "{\"error\":\"that address is not a web page\"}");
        return;
    }
    if (grove_host_fetch(index_dir, in) != 0) {
        GroveJob job;
        grove_host_job(&job);
        reply(e, job.running ? "{\"error\":\"an import is already running\"}" : "{\"error\":\"could not start\"}");
        return;
    }
    reply(e, "{\"ok\":true}");
}

static void on_job(webui_event_t *e) {
    reply_owned(e, job_json());
}

static void on_pick(webui_event_t *e) {
    const char *kind = webui_get_string_at(e, 0);
    reply_owned(e, pick_json(e->window, kind));
}

static void on_shelf(webui_event_t *e) {
    int ready = 0;
    pthread_mutex_lock(&mu);
    if (ix) {
        ready = index_shelf_ready(ix);
    }
    pthread_mutex_unlock(&mu);
    if (!ix) {
        reply_owned(e, json_error(open_err[0] ? open_err : "no index"));
        return;
    }
    if (ready) {
        reply(e, "{\"ready\":true}");
        return;
    }
    if (grove_host_prepare(index_dir) != 0) {
        GroveJob job;
        grove_host_job(&job);
        if (job.running) {
            reply(e, "{\"ready\":false,\"preparing\":true}");
            return;
        }
        pthread_mutex_lock(&mu);
        ready = ix && index_shelf_ready(ix);
        pthread_mutex_unlock(&mu);
        if (ready) {
            reply(e, "{\"ready\":true}");
            return;
        }
        reply_owned(e, json_error(job.error[0] ? job.error : "could not prepare the shelf"));
        return;
    }
    reply(e, "{\"ready\":false,\"preparing\":true}");
}

GUARD(on_search)
GUARD(on_more)
GUARD(on_modes)
GUARD(on_open)
GUARD(on_read)
GUARD(on_info)
GUARD(on_saved)
GUARD(on_save)
GUARD(on_forget)
GUARD(on_notes)
GUARD(on_note_put)
GUARD(on_note_drop)
GUARD(on_excerpts)
GUARD(on_excerpt_save)
GUARD(on_excerpt_tags)
GUARD(on_excerpt_drop)
GUARD(on_alerts)
GUARD(on_watch)
GUARD(on_unwatch)
GUARD(on_library)
GUARD(on_personas)
GUARD(on_persona_profile)
GUARD(on_catalog)
GUARD(on_drop)
GUARD(on_add)
GUARD(on_fetch)
GUARD(on_job)
GUARD(on_pick)
GUARD(on_shelf)

void grove_ui_detach(void) {
    pthread_mutex_lock(&call_mu);
    stopping = 1;
    while (in_call > 0) {
        pthread_cond_wait(&call_cv, &call_mu);
    }
    pthread_mutex_unlock(&call_mu);
    if (watching) {
        live_stop = 1;
        pthread_join(watcher, NULL);
        watching = 0;
    }
}

int grove_ui_attach(const GroveUi *ui) {
    if (!ui) {
        return 1;
    }
    pthread_mutex_lock(&call_mu);
    stopping = 0;
    pthread_mutex_unlock(&call_mu);
    ix = ui->ix;
    index_dir = ui->index_dir ? ui->index_dir : "";
    open_err[0] = 0;
    if (ui->open_err && ui->open_err[0]) {
        snprintf(open_err, sizeof open_err, "%s", ui->open_err);
    }
    live_path = ui->page_path;
    live_root = ui->page_root;
    live_name = ui->page_name;
    live_win = ui->window;

    size_t win = ui->window;
    webui_bind(win, "grove_search", on_search_guard);
    webui_bind(win, "grove_more", on_more_guard);
    webui_bind(win, "grove_modes", on_modes_guard);
    webui_bind(win, "grove_open", on_open_guard);
    webui_bind(win, "grove_read", on_read_guard);
    webui_bind(win, "grove_info", on_info_guard);
    webui_bind(win, "grove_saved", on_saved_guard);
    webui_bind(win, "grove_save", on_save_guard);
    webui_bind(win, "grove_forget", on_forget_guard);
    webui_bind(win, "grove_notes", on_notes_guard);
    webui_bind(win, "grove_note", on_note_put_guard);
    webui_bind(win, "grove_forget_note", on_note_drop_guard);
    webui_bind(win, "grove_excerpts", on_excerpts_guard);
    webui_bind(win, "grove_excerpt_save", on_excerpt_save_guard);
    webui_bind(win, "grove_excerpt_tags", on_excerpt_tags_guard);
    webui_bind(win, "grove_excerpt_drop", on_excerpt_drop_guard);
    webui_bind(win, "grove_alerts", on_alerts_guard);
    webui_bind(win, "grove_watch", on_watch_guard);
    webui_bind(win, "grove_unwatch", on_unwatch_guard);
    webui_bind(win, "grove_library", on_library_guard);
    webui_bind(win, "grove_personas", on_personas_guard);
    webui_bind(win, "grove_persona_profile", on_persona_profile_guard);
    webui_bind(win, "grove_catalog", on_catalog_guard);
    webui_bind(win, "grove_drop", on_drop_guard);
    webui_bind(win, "grove_add", on_add_guard);
    webui_bind(win, "grove_fetch", on_fetch_guard);
    webui_bind(win, "grove_job", on_job_guard);
    webui_bind(win, "grove_pick", on_pick_guard);
    webui_bind(win, "grove_shelf", on_shelf_guard);
    const char *page = live_name ? live_name : page_html;
    /* WebUI's own window (WebView2, WebKit, or WKWebView). A browser
     * is only the fallback, and it is what opens a tab when one is
     * already running. */
    if (!webui_show_wv(win, page) && !webui_show(win, page)) {
        fprintf(stderr, "grove: could not open a window\n");
        return 1;
    }
    const char *log = getenv("GROVE_CONSOLE");
    if (log && log[0] == '1' && log[1] == 0) {
        const char *url = webui_get_url(win);
        fprintf(stderr, "grove: %s\n", url ? url : "");
        if (live_path) {
            fprintf(stderr, "grove: watching %s\n", live_path);
        }
        fflush(stderr);
    }

    struct stat st;
    if (!ui->smoke && live_path && stat(live_path, &st) == 0) {
        live_mtim = grove_mtim(&st);
        live_stop = 0;
        watching = pthread_create(&watcher, NULL, live_watch, NULL) == 0;
    }

    if (ui->smoke) {
        char kicked[64];
        char buf[1200];
        memset(kicked, 0, sizeof kicked);
        memset(buf, 0, sizeof buf);
        Buf js;
        buf_init(&js);
        if (buf_puts(&js, "grove_search(") != 0 || jesc(&js, ui->smoke) != 0 ||
            buf_puts(&js, ").then(function(r){ window.__groveSmoke = r; }); return 'kicked';") != 0) {
            fprintf(stderr, "grove: smoke setup failed\n");
            buf_free(&js);
            webui_exit();
            return 1;
        }
        int ok = webui_script(win, js.data, 15, kicked, sizeof kicked) ? 1 : 0;
        buf_free(&js);
        if (!ok) {
            fprintf(stderr, "grove: smoke script failed\n");
            webui_exit();
            return 1;
        }
        for (int i = 0; i < 50; i++) {
            memset(buf, 0, sizeof buf);
            if (!webui_script(win, "return window.__groveSmoke ? String(window.__groveSmoke).slice(0, 1000) : '';", 10,
                              buf, sizeof buf)) {
                fprintf(stderr, "grove: smoke poll failed\n");
                webui_exit();
                return 1;
            }
            if (buf[0]) {
                break;
            }
            usleep(200000);
        }
        printf("%s\n", buf);
        fflush(stdout);
        webui_exit();
    }
    return 0;
}


