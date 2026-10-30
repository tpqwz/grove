#include "cli.h"
#include "fetch.h"
#include "index.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GROVE_VERSION "0.1.0-alpha"

static void usage(FILE *out) {
    fputs(
        "grove " GROVE_VERSION " — local full-text search\n"
        "\n"
        "usage: grove [--index DIR] [--quiet] [--help] [--version] COMMAND ...\n"
        "\n"
        "  index [--force] [--rebind OLD NEW] [PATH...]\n"
        "      Index each PATH. With no PATH, refresh every fully indexed root.\n"
        "      --force re-reads files even when size and mtime are unchanged.\n"
        "      --rebind OLD NEW retargets a root after the tree moves.\n"
        "  fetch URL...\n"
        "      Save each web page into this index and index its text.\n"
        "      The same address again replaces that page. http and https.\n"
        "  search [--limit N] [--json] [--at SPEC] [--stem] [--fuzzy] [--concepts] QUERY...\n"
        "      QUERY words are ANDed. OR and NOT are uppercase. -term excludes.\n"
        "      \"quoted phrases\", trailing *, and ext: path: title: mtime: filters.\n"
        "      --stem matches inflections. --fuzzy allows one edit on longer words.\n"
        "      --concepts adds related words. A trailing ~ is one edit on its own.\n"
        "      language: author: form: period: genre: narrow by the record.\n"
        "      Several of one are OR. Those five combine. Author is the creator,\n"
        "      form the type, period the coverage, and genre the subject.\n"
        "      --at OLD=NEW rewrites a stored root in the printed paths.\n"
        "      With one root, --at NEW replaces it. Scores are a percent of the best hit.\n"
        "      Each hit prints up to 8 passages: a byte offset, a PDF page, and the extract.\n"
        "      --json adds matchinfo and token offsets, plus type, folder,\n"
        "      month, and record counts for every match, past --limit too.\n"
        "      mtime:YYYY-MM keeps files touched in that month.\n"
        "  purge\n"
        "      Drop documents whose files are gone. Missing roots are kept.\n"
        "  compact\n"
        "      Rebuild the full-text index and vacuum the database.\n"
        "  saved\n"
        "      List remembered queries, newest first.\n"
        "  save QUERY...\n"
        "      Remember a query on this index. Saving it again moves it first.\n"
        "  forget QUERY...\n"
        "      Drop a remembered query.\n"
        "  notes\n"
        "      List notes on passages. Each line is offset, path, and text.\n"
        "  note PATH OFFSET TEXT...\n"
        "      Keep text on that place. The same place again replaces it.\n"
        "  forget-note PATH OFFSET\n"
        "      Drop the note on that place.\n"
        "  watch QUERY...\n"
        "      Remember a query and the files that match it now.\n"
        "  unwatch QUERY...\n"
        "      Stop watching that query. The remembered query stays.\n"
        "  alerts\n"
        "      List watched queries that have new or changed files.\n"
        "  status\n"
        "  ui [--port N]\n"
        "      Open the window on this index. grove-ui with a command is the\n"
        "      same tool, and it can run while the window is open.\n"
        "  help\n"
        "\n"
        "The index is grove.db in $GROVE_INDEX, or $XDG_DATA_HOME/grove,\n"
        "or ~/.local/share/grove. Copy the binary and that directory together.\n"
        "An older index has to be removed and indexed again.\n",
        out);
}

static void json_matchinfo(FILE *out, const MatchInfo *m) {
    int nph = m && m->phrase ? m->nphrase : 0;
    fprintf(out, ",\"matchinfo\":{\"p\":%u,\"c\":%u,\"n\":%u,\"avg\":%u,\"len\":%u,\"phrases\":[",
            m ? m->phrases : 0, m ? m->columns : 0, m ? m->rows : 0, m ? m->avg_tokens : 0, m ? m->tokens : 0);
    for (int i = 0; i < nph; i++) {
        if (i) {
            fputc(',', out);
        }
        fprintf(out, "{\"hits\":%u,\"total\":%u,\"docs\":%u}", m->phrase[i].hits, m->phrase[i].total, m->phrase[i].docs);
    }
    fputs("]}", out);
}

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

static void json_escape(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            fprintf(out, "\\%c", c);
        } else if (c == '\n') {
            fputs("\\n", out);
        } else if (c == '\r') {
            fputs("\\r", out);
        } else if (c == '\t') {
            fputs("\\t", out);
        } else if (c < 0x20) {
            fprintf(out, "\\u%04x", c);
        } else {
            fputc(c, out);
        }
    }
    fputc('"', out);
}

static void print_stats(const IndexStats *st) {
    printf("indexed %u, unchanged %u, removed %u, skipped %u\n", st->indexed, st->unchanged, st->removed,
           st->skipped);
}

static void print_facets(const Facet *a, int n) {
    for (int i = 0; i < n; i++) {
        if (i) {
            fputc(',', stdout);
        }
        fputs("{\"value\":", stdout);
        json_escape(stdout, a[i].value);
        fprintf(stdout, ",\"count\":%d}", a[i].count);
    }
}

static int fail(const Err *e) {
    fprintf(stderr, "grove: %s\n", e->msg[0] ? e->msg : "failed");
    return 1;
}

static int cmd_index(Index *ix, int quiet, int argc, char **argv) {
    int force = 0;
    const char *from = NULL;
    const char *to = NULL;
    int i = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
            continue;
        }
        if (strcmp(argv[i], "--rebind") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "grove: --rebind needs OLD and NEW\n");
                return 1;
            }
            from = argv[++i];
            to = argv[++i];
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "grove: unknown option %s\n", argv[i]);
            return 1;
        }
        break;
    }
    Err e;
    err_clear(&e);
    if (from) {
        if (index_rebind(ix, from, to, &e) != 0) {
            return fail(&e);
        }
    }
    index_set_force(ix, force);
    IndexStats st;
    memset(&st, 0, sizeof st);
    int did = 0;
    if (i >= argc) {
        if (!from) {
            did = 1;
            if (index_reindex(ix, &st, &e) != 0) {
                return fail(&e);
            }
        }
    } else {
        did = 1;
        for (; i < argc; i++) {
            if (index_add(ix, argv[i], &st, &e) != 0) {
                return fail(&e);
            }
        }
    }
    if (did && !quiet) {
        print_stats(&st);
    }
    return 0;
}

static int cmd_fetch(Index *ix, const char *dir, int quiet, int argc, char **argv) {
    int i = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "grove: unknown option %s\n", argv[i]);
            return 1;
        }
        break;
    }
    if (i >= argc) {
        fprintf(stderr, "grove: fetch needs an address\n");
        return 1;
    }
    IndexStats st;
    memset(&st, 0, sizeof st);
    for (; i < argc; i++) {
        Err e;
        err_clear(&e);
        if (grove_fetch_into(ix, dir, argv[i], &st, &e) != 0) {
            return fail(&e);
        }
    }
    if (!quiet) {
        print_stats(&st);
    }
    return 0;
}

static char *join_query(int argc, char **argv) {
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i] || size_add(n, strlen(argv[i]), &n) != 0 || size_add(n, 1, &n) != 0) {
            return NULL;
        }
    }
    char *q = malloc(n ? n : 1);
    if (!q) {
        return NULL;
    }
    size_t at = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (i && at < n) {
            q[at++] = ' ';
        }
        memcpy(q + at, argv[i], len);
        at += len;
    }
    q[at] = 0;
    return q;
}

static int cmd_search(Index *ix, int argc, char **argv) {
    int limit = 20;
    int json = 0;
    const char *at = NULL;
    SearchModes modes;
    memset(&modes, 0, sizeof modes);
    int i = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "--json") == 0) {
            json = 1;
            continue;
        }
        if (strcmp(argv[i], "--stem") == 0) {
            modes.stem = 1;
            continue;
        }
        if (strcmp(argv[i], "--fuzzy") == 0) {
            modes.fuzzy = 1;
            continue;
        }
        if (strcmp(argv[i], "--concepts") == 0) {
            modes.concept = 1;
            continue;
        }
        if (strcmp(argv[i], "--limit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove: --limit needs a number\n");
                return 1;
            }
            errno = 0;
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (!end || end == argv[i] || *end || errno != 0 || v < 1 || v > 1000) {
                fprintf(stderr, "grove: --limit must be from 1 to 1000\n");
                return 1;
            }
            limit = (int)v;
            continue;
        }
        if (strcmp(argv[i], "--at") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove: --at needs OLD=NEW or a path\n");
                return 1;
            }
            at = argv[++i];
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "grove: unknown option %s\n", argv[i]);
            return 1;
        }
        break;
    }
    if (i >= argc) {
        fprintf(stderr, "grove: search needs a query\n");
        return 1;
    }
    char *query = join_query(argc - i, argv + i);
    if (!query) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    Hit *hits = NULL;
    int n = 0;
    Facets facets;
    memset(&facets, 0, sizeof facets);
    int rc = index_search(ix, query, limit, at, &hits, &n, &facets, &modes, &e);
    free(query);
    if (rc != 0) {
        facets_free(&facets);
        return fail(&e);
    }
    if (json) {
        fputs("{\"hits\":[", stdout);
        for (int h = 0; h < n; h++) {
            if (h) {
                fputc(',', stdout);
            }
            fputs("{\"path\":", stdout);
            json_escape(stdout, hits[h].path);
            fputs(",\"title\":", stdout);
            json_escape(stdout, hits[h].title);
            fputs(",\"ext\":", stdout);
            json_escape(stdout, hits[h].ext);
            fprintf(stdout, ",\"score\":%d,\"nmatches\":%d,\"snippet\":", hits[h].score, hits[h].nmatches);
            json_escape(stdout, hits[h].snippet);
            fputs(",\"offsets\":[", stdout);
            for (int k = 0; k < hits[h].noffsets; k++) {
                if (k) {
                    fputc(',', stdout);
                }
                fprintf(stdout, "{\"start\":%u,\"length\":%u,\"term\":%d,\"column\":%d}",
                        (unsigned)hits[h].offsets[k].start, (unsigned)hits[h].offsets[k].len, hits[h].offsets[k].term,
                        hits[h].offsets[k].column);
            }
            fputc(']', stdout);
            json_matchinfo(stdout, &hits[h].matchinfo);
            fputs(",\"passages\":[", stdout);
            for (int p = 0; p < hits[h].npassages; p++) {
                if (p) {
                    fputc(',', stdout);
                }
                fputs("{\"text\":", stdout);
                json_escape(stdout, hits[h].passages[p].text);
                fprintf(stdout, ",\"offset\":%u,\"page\":%d,\"section\":", (unsigned)hits[h].passages[p].offset,
                        hits[h].passages[p].page);
                json_escape(stdout, hits[h].passages[p].section);
                fputc('}', stdout);
            }
            fprintf(stdout, "],\"more\":%s}", hits[h].more ? "true" : "false");
        }
        fprintf(stdout, "],\"nfiles\":%d,\"facets\":{\"types\":[", facets.nfiles);
        print_facets(facets.types, facets.ntypes);
        fputs("],\"folders\":[", stdout);
        print_facets(facets.folders, facets.nfolders);
        fputs("],\"months\":[", stdout);
        print_facets(facets.months, facets.nmonths);
        fputs("],\"narrow\":{\"language\":[", stdout);
        print_facets(facets.narrow.language, facets.narrow.nlanguage);
        fputs("],\"author\":[", stdout);
        print_facets(facets.narrow.author, facets.narrow.nauthor);
        fputs("],\"form\":[", stdout);
        print_facets(facets.narrow.form, facets.narrow.nform);
        fputs("],\"period\":[", stdout);
        print_facets(facets.narrow.period, facets.narrow.nperiod);
        fputs("],\"genre\":[", stdout);
        print_facets(facets.narrow.genre, facets.narrow.ngenre);
        fputs("]}}}\n", stdout);
    } else {
        for (int h = 0; h < n; h++) {
            printf("%d %d%% %s\n", h + 1, hits[h].score, hits[h].path ? hits[h].path : "");
            if (hits[h].title && hits[h].title[0]) {
                printf("%s\n", hits[h].title);
            }
            for (int p = 0; p < hits[h].npassages; p++) {
                Passage *ps = &hits[h].passages[p];
                int has_sec = ps->section && ps->section[0];
                if (has_sec && ps->page > 0) {
                    printf("%s · p.%d +%u\n", ps->section, ps->page, (unsigned)ps->offset);
                } else if (has_sec) {
                    printf("%s +%u\n", ps->section, (unsigned)ps->offset);
                } else if (ps->page > 0) {
                    printf("p.%d +%u\n", ps->page, (unsigned)ps->offset);
                } else {
                    printf("+%u\n", (unsigned)ps->offset);
                }
                if (ps->text && ps->text[0]) {
                    printf("%s\n", ps->text);
                }
            }
            fputc('\n', stdout);
        }
    }
    hits_free(hits, n);
    facets_free(&facets);
    return 0;
}

static int cmd_saved_list(Index *ix) {
    char **queries = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (index_saved_list(ix, &queries, &n, &e) != 0) {
        return fail(&e);
    }
    for (int i = 0; i < n; i++) {
        printf("%s\n", queries[i]);
    }
    index_saved_free(queries, n);
    return 0;
}

static int cmd_alerts(Index *ix) {
    Alert *alerts = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (index_alerts_list(ix, &alerts, &n, &e) != 0) {
        return fail(&e);
    }
    for (int i = 0; i < n; i++) {
        if (alerts[i].nfiles <= 0) {
            continue;
        }
        printf("%d %s\n", alerts[i].nfiles, alerts[i].query ? alerts[i].query : "");
        for (int j = 0; j < alerts[i].nfiles; j++) {
            printf("%s\n", alerts[i].files[j].path ? alerts[i].files[j].path : "");
        }
    }
    index_alerts_free(alerts, n);
    return 0;
}

static int cmd_watch_change(Index *ix, int drop, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "grove: %s needs a query\n", drop ? "unwatch" : "watch");
        return 1;
    }
    char *query = join_query(argc, argv);
    if (!query) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    int rc = drop ? index_alert_unwatch(ix, query, &e) : index_alert_watch(ix, query, &e);
    free(query);
    return rc == 0 ? 0 : fail(&e);
}

static int cmd_saved_change(Index *ix, int drop, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "grove: %s needs a query\n", drop ? "forget" : "save");
        return 1;
    }
    char *query = join_query(argc, argv);
    if (!query) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    int rc = drop ? index_saved_drop(ix, query, &e) : index_saved_put(ix, query, &e);
    free(query);
    return rc == 0 ? 0 : fail(&e);
}

static int parse_offset(const char *s, uint32_t *out) {
    if (!s || !*s) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end || errno || v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int cmd_notes(Index *ix) {
    Note *notes = NULL;
    int n = 0;
    Err e;
    err_clear(&e);
    if (index_notes_list(ix, &notes, &n, &e) != 0) {
        return fail(&e);
    }
    for (int i = 0; i < n; i++) {
        printf("%u\t%s\t%s\n", notes[i].offset, notes[i].path, notes[i].body);
    }
    index_notes_free(notes, n);
    return 0;
}

static int cmd_note_put(Index *ix, int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "grove: note needs a path, an offset, and text\n");
        return 1;
    }
    uint32_t off = 0;
    if (parse_offset(argv[1], &off) != 0) {
        fprintf(stderr, "grove: note offset must be a byte count\n");
        return 1;
    }
    char *body = join_query(argc - 2, argv + 2);
    if (!body) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    int rc = index_note_put(ix, argv[0], off, body, &e);
    free(body);
    return rc == 0 ? 0 : fail(&e);
}

static int cmd_note_drop(Index *ix, int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "grove: forget-note needs a path and an offset\n");
        return 1;
    }
    uint32_t off = 0;
    if (parse_offset(argv[1], &off) != 0) {
        fprintf(stderr, "grove: note offset must be a byte count\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    return index_note_drop(ix, argv[0], off, &e) == 0 ? 0 : fail(&e);
}

int grove_is_command(const char *s) {
    static const char *names[] = {"index", "fetch", "search",  "purge",      "compact", "status", "saved", "save",
                                  "forget", "notes",  "note",       "forget-note", "watch", "unwatch", "alerts",
                                  "help",   NULL};
    if (!s) {
        return 0;
    }
    for (int i = 0; names[i]; i++) {
        if (strcmp(s, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int launch_ui(const char *index_dir, int argc, char **argv) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || (size_t)n >= sizeof exe) {
        fprintf(stderr, "grove: cannot find this program\n");
        return 1;
    }
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) {
        fprintf(stderr, "grove: cannot find this program\n");
        return 1;
    }
    const char *leaf = "grove-ui";
    size_t room = sizeof exe - (size_t)(slash + 1 - exe);
    if (strlen(leaf) + 1 > room) {
        fprintf(stderr, "grove: cannot open the window\n");
        return 1;
    }
    memcpy(slash + 1, leaf, strlen(leaf) + 1);
    if (access(exe, X_OK) != 0) {
        fprintf(stderr, "grove: the window program is not beside this one (%s)\n", exe);
        return 1;
    }
    if (argc < 0) {
        return 1;
    }
    size_t narg = 0;
    if (size_add((size_t)argc, 4, &narg) != 0) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    char **av = calloc(narg, sizeof *av);
    if (!av) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    av[0] = exe;
    av[1] = "--index";
    av[2] = (char *)index_dir;
    for (int i = 0; i < argc; i++) {
        av[3 + i] = argv[i];
    }
    execv(exe, av);
    fprintf(stderr, "grove: cannot open the window: %s\n", strerror(errno));
    free(av);
    return 1;
}

static int cmd_status(Index *ix) {
    Err e;
    err_clear(&e);
    IndexInfo info;
    if (index_info(ix, &info, &e) != 0) {
        return fail(&e);
    }
    printf("index: %s\n", info.dir);
    printf("documents: %u\n", info.n_docs);
    printf("segments: %u\n", info.n_segments);
    printf("text: %llu bytes\n", (unsigned long long)info.text_bytes);
    for (uint32_t i = 0; i < info.n_roots; i++) {
        const char *kind = (info.root_full && info.root_full[i]) ? "full" : "partial";
        printf("root: %s [%s]\n", info.roots[i], kind);
    }
    return 0;
}

int grove_cli(int argc, char **argv) {
    const char *index_arg = NULL;
    int quiet = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("grove %s\n", GROVE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
            continue;
        }
        if (strcmp(argv[i], "--index") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove: --index needs a directory\n");
                return 1;
            }
            index_arg = argv[++i];
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "grove: unknown option %s\n", argv[i]);
            return 1;
        }
        break;
    }
    if (i >= argc) {
        usage(stderr);
        return 1;
    }
    const char *cmd = argv[i++];
    if (strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    char *owned = NULL;
    const char *dir = index_arg;
    if (!dir) {
        owned = default_index_dir();
        dir = owned;
    }
    if (!dir) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    if (strcmp(cmd, "ui") == 0) {
        int rc = launch_ui(dir, argc - i, argv + i);
        free(owned);
        return rc;
    }
    int writable = strcmp(cmd, "search") != 0 && strcmp(cmd, "status") != 0 && strcmp(cmd, "saved") != 0 &&
                   strcmp(cmd, "notes") != 0 && strcmp(cmd, "alerts") != 0;
    Err e;
    err_clear(&e);
    Index *ix = index_open(dir, writable, &e);
    if (!ix) {
        fprintf(stderr, "grove: %s\n", e.msg[0] ? e.msg : "cannot open index");
        free(owned);
        return 1;
    }
    index_set_quiet(ix, quiet);
    int rc = 0;
    if (strcmp(cmd, "index") == 0) {
        rc = cmd_index(ix, quiet, argc - i, argv + i);
    } else if (strcmp(cmd, "fetch") == 0) {
        rc = cmd_fetch(ix, dir, quiet, argc - i, argv + i);
    } else if (strcmp(cmd, "search") == 0) {
        rc = cmd_search(ix, argc - i, argv + i);
    } else if (strcmp(cmd, "purge") == 0) {
        IndexStats st;
        memset(&st, 0, sizeof st);
        err_clear(&e);
        if (index_purge(ix, &st, &e) != 0) {
            rc = fail(&e);
        } else if (!quiet) {
            print_stats(&st);
        }
    } else if (strcmp(cmd, "compact") == 0) {
        err_clear(&e);
        if (index_compact(ix, &e) != 0) {
            rc = fail(&e);
        } else if (!quiet) {
            printf("compacted\n");
        }
    } else if (strcmp(cmd, "status") == 0) {
        rc = cmd_status(ix);
    } else if (strcmp(cmd, "saved") == 0) {
        rc = cmd_saved_list(ix);
    } else if (strcmp(cmd, "save") == 0) {
        rc = cmd_saved_change(ix, 0, argc - i, argv + i);
    } else if (strcmp(cmd, "forget") == 0) {
        rc = cmd_saved_change(ix, 1, argc - i, argv + i);
    } else if (strcmp(cmd, "notes") == 0) {
        rc = cmd_notes(ix);
    } else if (strcmp(cmd, "note") == 0) {
        rc = cmd_note_put(ix, argc - i, argv + i);
    } else if (strcmp(cmd, "forget-note") == 0) {
        rc = cmd_note_drop(ix, argc - i, argv + i);
    } else if (strcmp(cmd, "alerts") == 0) {
        rc = cmd_alerts(ix);
    } else if (strcmp(cmd, "watch") == 0) {
        rc = cmd_watch_change(ix, 0, argc - i, argv + i);
    } else if (strcmp(cmd, "unwatch") == 0) {
        rc = cmd_watch_change(ix, 1, argc - i, argv + i);
    } else {
        fprintf(stderr, "grove: unknown command %s\n", cmd);
        rc = 1;
    }
    index_close(ix);
    free(owned);
    return rc;
}

#ifndef GROVE_EMBED
int main(int argc, char **argv) {
    return grove_cli(argc, argv);
}
#endif
