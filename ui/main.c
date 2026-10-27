#include "index.h"
#include "webui.h"

#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "page.h"

extern char **environ;

enum { SEARCH_LIMIT = 40, ALLOW_MAX = 40, QUERY_MAX = 8192 };

static Index *ix;
static char *index_dir;
static char open_err[768];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char *allow[ALLOW_MAX];
static int nallow;

static char *default_index_dir(void) {
    const char *env = getenv("GROVE_INDEX");
    if (env && *env) {
        return xstrdup(env);
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return path_join(xdg, "grove");
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    char *base = path_join(home, ".local/share");
    char *full = base ? path_join(base, "grove") : NULL;
    free(base);
    return full;
}

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
    if (!path || nallow >= ALLOW_MAX || allow_has(path)) {
        return;
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

static char *search_json(const char *query) {
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
    if (index_search(ix, query, SEARCH_LIMIT, NULL, &hits, &n, &facets, &e) != 0) {
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
    if (buf_puts(&b, "]}}") != 0) {
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

static char *more_json(const char *query, const char *path, uint32_t after) {
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
    if (index_more(ix, query, path, after, &ps, &n, &more, &e) != 0) {
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

static void on_search(webui_event_t *e) {
    const char *in = webui_get_string(e);
    char *query = xstrdup(in ? in : "");
    pthread_mutex_lock(&mu);
    char *json = query ? search_json(query) : NULL;
    pthread_mutex_unlock(&mu);
    free(query);
    reply_owned(e, json);
}

static void on_more(webui_event_t *e) {
    const char *path_in = webui_get_string_at(e, 0);
    long long after_in = webui_get_int_at(e, 1);
    const char *query_in = webui_get_string_at(e, 2);
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
        json = more_json(query, path, after);
    }
    pthread_mutex_unlock(&mu);
    free(path);
    free(query);
    reply_owned(e, json);
}

static int spawn_open(const char *path) {
    pid_t pid = 0;
    char *argv[] = {"xdg-open", (char *)path, NULL};
    int rc = posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ);
    if (rc != 0) {
        argv[0] = "/usr/bin/xdg-open";
        rc = posix_spawn(&pid, "/usr/bin/xdg-open", NULL, NULL, argv, environ);
    }
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

/* The reader stays up. Detach so this process does not wait, and so the
 * child is not a zombie. */
static int spawn_detached(char *const argv[]) {
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
        const char *colon = strchr(p, ':');
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
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 32L * 1024L * 1024L) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    unsigned char *b = malloc((size_t)sz + 1);
    if (!b) {
        fclose(f);
        return 0;
    }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return 0;
    }
    fclose(f);
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

/* One line, capped, with the highlight marks removed. Too short a string
 * would land on the first copy in the book, so it is refused. */
static char *clean_needle(const char *in) {
    if (!in || !in[0] || in[0] == '-') {
        return NULL;
    }
    size_t n = strlen(in);
    char *s = malloc(n + 1);
    if (!s) {
        return NULL;
    }
    size_t w = 0;
    int sp = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '<' && i + 1 < n && in[i + 1] == '<') {
            i++;
            continue;
        }
        if (c == '>' && i + 1 < n && in[i + 1] == '>') {
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
    if (w > 180) {
        w = utf8_prefix_bytes(s, w, 180);
        s[w] = 0;
    }
    if (w < 12) {
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
        char *argv[8];
        int n = 0;
        argv[n++] = "okular";
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
        char *argv[] = {"okular", "--find", (char *)needle, (char *)path, NULL};
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
    int allowed = 0;
    if (path) {
        pthread_mutex_lock(&mu);
        allowed = allow_has(path);
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
        char *end = NULL;
        unsigned long long v = strtoull(at_s, &end, 10);
        if (end && *end == 0 && v <= UINT32_MAX) {
            at = (uint32_t)v;
            has_at = 1;
        }
    }
    if (page_s && page_s[0]) {
        char *end = NULL;
        unsigned long v = strtoul(page_s, &end, 10);
        if (end && *end == 0 && v > 0 && v < 1000000UL) {
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
        } else if ((placed ? open_place(path, has_at, at, page, needle) : spawn_open(path)) != 0) {
            json = json_error("could not open the file");
        } else {
            json = xstrdup("{\"ok\":true}");
        }
    }
    free(needle);
    free(path);
    reply_owned(e, json);
}

static void on_info(webui_event_t *e) {
    pthread_mutex_lock(&mu);
    char *json = info_json();
    pthread_mutex_unlock(&mu);
    reply_owned(e, json);
}

static void usage(FILE *out) {
    fputs(
        "grove-ui — search window for a grove index\n"
        "\n"
        "usage: grove-ui [--index DIR] [--port N]\n"
        "\n"
        "Opens a browser window on the grove index. The query language matches\n"
        "grove search. A click on the file name opens it with the system.\n"
        "A click on a passage opens that place when the reader can take a\n"
        "line, a PDF page, or a found line. Otherwise it opens the file.\n"
        "\n"
        "The index directory is $GROVE_INDEX, or $XDG_DATA_HOME/grove,\n"
        "or ~/.local/share/grove.\n",
        out);
}

int main(int argc, char **argv) {
    const char *index_arg = NULL;
    size_t port = 0;
    const char *smoke = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--index") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove-ui: --index needs a directory\n");
                return 1;
            }
            index_arg = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove-ui: --port needs a number\n");
                return 1;
            }
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (!end || *end || v < 1 || v > 65535) {
                fprintf(stderr, "grove-ui: --port must be from 1 to 65535\n");
                return 1;
            }
            port = (size_t)v;
            continue;
        }
        if (strcmp(argv[i], "--smoke") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove-ui: --smoke needs a query\n");
                return 1;
            }
            smoke = argv[++i];
            continue;
        }
        fprintf(stderr, "grove-ui: unknown option %s\n", argv[i]);
        return 1;
    }

    index_dir = index_arg ? xstrdup(index_arg) : default_index_dir();
    if (!index_dir) {
        fprintf(stderr, "grove-ui: out of memory\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    ix = index_open(index_dir, 0, &e);
    if (!ix) {
        snprintf(open_err, sizeof open_err, "%s", e.msg[0] ? e.msg : "could not open the index");
        fprintf(stderr, "grove-ui: %s\n", open_err);
    }

    size_t win = webui_new_window();
    webui_set_size(win, 1040, 840);
    webui_set_center(win);
    webui_bind(win, "grove_search", on_search);
    webui_bind(win, "grove_more", on_more);
    webui_bind(win, "grove_open", on_open);
    webui_bind(win, "grove_info", on_info);
    if (port && !webui_set_port(win, port)) {
        fprintf(stderr, "grove-ui: port %zu is not available\n", port);
        return 1;
    }
    if (!webui_show(win, page_html)) {
        fprintf(stderr, "grove-ui: could not open a browser window\n");
        return 1;
    }
    const char *url = webui_get_url(win);
    fprintf(stderr, "grove-ui: %s\n", url ? url : "");
    fflush(stderr);

    if (smoke) {
        char kicked[64];
        char buf[1200];
        memset(kicked, 0, sizeof kicked);
        memset(buf, 0, sizeof buf);
        Buf js;
        buf_init(&js);
        if (buf_puts(&js, "grove_search(") != 0 || jesc(&js, smoke) != 0 ||
            buf_puts(&js, ").then(function(r){ window.__groveSmoke = r; }); return 'kicked';") != 0) {
            fprintf(stderr, "grove-ui: smoke setup failed\n");
            webui_exit();
            webui_wait();
            return 1;
        }
        int ok = webui_script(win, js.data, 15, kicked, sizeof kicked) ? 1 : 0;
        buf_free(&js);
        if (!ok) {
            fprintf(stderr, "grove-ui: smoke script failed\n");
            webui_exit();
            webui_wait();
            return 1;
        }
        for (int i = 0; i < 50; i++) {
            memset(buf, 0, sizeof buf);
            if (!webui_script(win, "return window.__groveSmoke ? String(window.__groveSmoke).slice(0, 1000) : '';", 10,
                              buf, sizeof buf)) {
                fprintf(stderr, "grove-ui: smoke poll failed\n");
                webui_exit();
                webui_wait();
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

    webui_wait();
    webui_clean();
    if (ix) {
        index_close(ix);
    }
    allow_clear();
    free(index_dir);
    return 0;
}
