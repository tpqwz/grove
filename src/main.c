#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
    fputs(
        "grove 0.1.0 — local full-text search\n"
        "\n"
        "usage: grove [--index DIR] [--quiet] [--help] [--version] COMMAND ...\n"
        "\n"
        "  index [--force] [--rebind OLD NEW] [PATH...]\n"
        "      Index each PATH. With no PATH, refresh every fully indexed root.\n"
        "      --force re-reads files even when size and mtime are unchanged.\n"
        "      --rebind OLD NEW retargets a root after the tree moves.\n"
        "  search [--limit N] [--json] [--at SPEC] QUERY...\n"
        "      QUERY words are ANDed. OR and NOT are uppercase. -term excludes.\n"
        "      \"quoted phrases\", trailing *, and ext: path: title: filters.\n"
        "      --at OLD=NEW rewrites a stored root in the printed paths.\n"
        "      With one root, --at NEW replaces it. Scores are a percent of the best hit.\n"
        "      Each hit prints up to 8 passages: a byte offset, a PDF page, and the extract.\n"
        "      --json adds matchinfo and token offsets, plus type and folder\n"
        "      file counts for every match, past --limit too.\n"
        "  purge\n"
        "      Drop documents whose files are gone. Missing roots are kept.\n"
        "  compact\n"
        "      Rebuild the full-text index and vacuum the database.\n"
        "  status\n"
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

static char *join_query(int argc, char **argv) {
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        n += strlen(argv[i]) + 1;
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
        if (strcmp(argv[i], "--limit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove: --limit needs a number\n");
                return 1;
            }
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (!end || *end || v < 1 || v > 1000) {
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
    int rc = index_search(ix, query, limit, at, &hits, &n, &facets, &e);
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
        fputs("]}}\n", stdout);
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

int main(int argc, char **argv) {
    const char *index_arg = NULL;
    int quiet = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("grove 0.1.0\n");
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
    int writable = strcmp(cmd, "search") != 0 && strcmp(cmd, "status") != 0;
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
    } else {
        fprintf(stderr, "grove: unknown command %s\n", cmd);
        rc = 1;
    }
    index_close(ix);
    free(owned);
    return rc;
}
