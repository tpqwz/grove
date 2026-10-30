#include "index.h"
#include "mod.h"
#include "webui.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
        "Save remembers the current query on this index.\n"
        "Watch remembers that query and the files that match it.\n"
        "Note keeps text on one passage.\n"
        "\n"
        "The index directory is $GROVE_INDEX, or $XDG_DATA_HOME/grove,\n"
        "or ~/.local/share/grove.\n"
        "\n"
        "Saving ui/page.html reloads the open window. Saving ui/main.c\n"
        "rebuilds that program and swaps it in. The process and the index\n"
        "stay up, and the query stays too. A compile error keeps the previous\n"
        "program. A copy that cannot see those files uses the packed page.\n",
        out);
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

static int mtim_eq(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static int mtim_newer(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) {
        return a->tv_sec > b->tv_sec;
    }
    return a->tv_nsec > b->tv_nsec;
}

static char *absolute_file(const char *path) {
    if (!path || access(path, R_OK) != 0) {
        return NULL;
    }
    return realpath(path, NULL);
}

static char *exe_dir(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) {
        return NULL;
    }
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) {
        return NULL;
    }
    *slash = 0;
    return xstrdup(exe);
}

/* Returns a malloc'd absolute path, or NULL. Sets *missing when GROVE_PAGE is unreadable. */
static char *find_live_page(int *missing) {
    *missing = 0;
    const char *env = getenv("GROVE_PAGE");
    if (env && *env) {
        char *abs = absolute_file(env);
        if (!abs) {
            fprintf(stderr, "grove-ui: cannot read %s\n", env);
            *missing = 1;
        }
        return abs;
    }
    char *dir = exe_dir();
    if (dir) {
        char *rel = path_join(dir, "ui/page.html");
        char *abs = absolute_file(rel);
        free(rel);
        if (!abs) {
            rel = path_join(dir, "page.html");
            abs = absolute_file(rel);
            free(rel);
        }
        free(dir);
        if (abs) {
            return abs;
        }
    }
    char *abs = absolute_file("ui/page.html");
    if (abs) {
        return abs;
    }
    return absolute_file("page.html");
}

static int split_page(char *abs, char **root, char **name) {
    char *slash = strrchr(abs, '/');
    if (!slash || slash == abs || !slash[1]) {
        return -1;
    }
    *name = xstrdup(slash + 1);
    *root = xstrndup(abs, (size_t)(slash - abs));
    if (!*name || !*root) {
        free(*name);
        free(*root);
        *name = NULL;
        *root = NULL;
        return -1;
    }
    return 0;
}

static char *find_ui_dir(void) {
    char *dir = exe_dir();
    if (dir) {
        char *ui = path_join(dir, "ui");
        char *main_c = ui ? path_join(ui, "main.c") : NULL;
        free(dir);
        if (main_c && access(main_c, R_OK) == 0) {
            free(main_c);
            return ui;
        }
        free(main_c);
        free(ui);
    }
    if (access("ui/main.c", R_OK) == 0) {
        return absolute_file("ui");
    }
    if (access("main.c", R_OK) == 0) {
        return absolute_file(".");
    }
    return NULL;
}

static void print_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        fputs(buf, stderr);
    }
    fclose(f);
}

static int compile_mod(const char *ui_dir, const char *out, const char *err_path) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            _exit(127);
        }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) {
            close(fd);
        }
        char outarg[PATH_MAX];
        int nw = snprintf(outarg, sizeof outarg, "OUT=%s", out);
        if (nw < 0 || (size_t)nw >= sizeof outarg) {
            _exit(127);
        }
        execlp("make", "make", "-C", ui_dir, "live-mod", outarg, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        return -1;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

typedef int (*attach_fn)(const GroveUi *);
typedef void (*detach_fn)(void);

typedef struct Mod {
    void *handle;
    attach_fn attach;
    detach_fn detach;
    char *path;
} Mod;

static int load_mod(Mod *m, const char *path) {
    memset(m, 0, sizeof *m);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "grove-ui: %s\n", dlerror());
        return -1;
    }
    attach_fn attach = (attach_fn)dlsym(h, "grove_ui_attach");
    detach_fn detach = (detach_fn)dlsym(h, "grove_ui_detach");
    if (!attach || !detach) {
        fprintf(stderr, "grove-ui: %s\n", dlerror());
        dlclose(h);
        return -1;
    }
    m->handle = h;
    m->attach = attach;
    m->detach = detach;
    m->path = xstrdup(path);
    return m->path ? 0 : -1;
}

static void close_mod(Mod *m, int unlink_file) {
    if (!m->handle) {
        free(m->path);
        m->path = NULL;
        return;
    }
    m->detach();
    dlclose(m->handle);
    m->handle = NULL;
    if (unlink_file && m->path) {
        unlink(m->path);
    }
    free(m->path);
    m->path = NULL;
}

static GroveUi host_ui;
static Mod host_mod;
static char *ui_dir;
static char *err_path;
static volatile int c_stop;
static pthread_mutex_t swap_mu = PTHREAD_MUTEX_INITIALIZER;
static int mod_seq;

static int swap_in(const char *so_path, int unlink_on_fail) {
    Mod neu;
    if (load_mod(&neu, so_path) != 0) {
        if (unlink_on_fail) {
            unlink(so_path);
        }
        return -1;
    }
    if (neu.attach(&host_ui) != 0) {
        dlclose(neu.handle);
        free(neu.path);
        if (unlink_on_fail) {
            unlink(so_path);
        }
        return -1;
    }
    Mod old = host_mod;
    host_mod = neu;
    if (old.handle) {
        old.detach();
        dlclose(old.handle);
        if (old.path) {
            unlink(old.path);
        }
        free(old.path);
    }
    return 0;
}

static void *c_watch(void *arg) {
    const char *main_c = arg;
    struct stat st;
    struct timespec seen;
    memset(&seen, 0, sizeof seen);
    if (stat(main_c, &st) == 0) {
        seen = st.st_mtim;
    }
    struct timespec pending;
    int have_pending = 0;
    memset(&pending, 0, sizeof pending);
    while (!c_stop) {
        if (stat(main_c, &st) != 0 || mtim_eq(&st.st_mtim, &seen)) {
            have_pending = 0;
            usleep(100000);
            continue;
        }
        if (!(have_pending && mtim_eq(&st.st_mtim, &pending))) {
            pending = st.st_mtim;
            have_pending = 1;
            usleep(100000);
            continue;
        }
        seen = st.st_mtim;
        have_pending = 0;
        pthread_mutex_lock(&swap_mu);
        if (!c_stop) {
            char out[PATH_MAX];
            int nw = snprintf(out, sizeof out, "%s/live/module-%d.so", ui_dir, ++mod_seq);
            if (nw < 0 || (size_t)nw >= sizeof out) {
                fprintf(stderr, "grove-ui: module path is too long\n");
                fflush(stderr);
                pthread_mutex_unlock(&swap_mu);
                continue;
            }
            fprintf(stderr, "grove-ui: building %s\n", main_c);
            fflush(stderr);
            if (compile_mod(ui_dir, out, err_path) != 0) {
                fprintf(stderr, "grove-ui: build failed, keeping the running program\n");
                print_file(err_path);
                fflush(stderr);
                unlink(out);
            } else if (swap_in(out, 1) != 0) {
                fprintf(stderr, "grove-ui: could not load the new program, keeping the previous one\n");
                fflush(stderr);
            } else {
                fprintf(stderr, "grove-ui: updated %s\n", main_c);
                fflush(stderr);
            }
        }
        pthread_mutex_unlock(&swap_mu);
        usleep(100000);
    }
    return NULL;
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

    char *index_dir = index_arg ? xstrdup(index_arg) : default_index_dir();
    if (!index_dir) {
        fprintf(stderr, "grove-ui: out of memory\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    char open_err[768];
    open_err[0] = 0;
    Index *ix = index_open(index_dir, 0, &e);
    if (!ix) {
        snprintf(open_err, sizeof open_err, "%s", e.msg[0] ? e.msg : "could not open the index");
        fprintf(stderr, "grove-ui: %s\n", open_err);
    }

    int page_missing = 0;
    char *page_path = find_live_page(&page_missing);
    if (page_missing) {
        if (ix) {
            index_close(ix);
        }
        free(index_dir);
        return 1;
    }
    char *page_root = NULL;
    char *page_name = NULL;
    if (page_path && split_page(page_path, &page_root, &page_name) != 0) {
        free(page_path);
        page_path = NULL;
    }

    ui_dir = find_ui_dir();
    char mod_path[PATH_MAX];
    mod_path[0] = 0;
    if (ui_dir) {
        int nw = snprintf(mod_path, sizeof mod_path, "%s/live/module.so", ui_dir);
        if (nw < 0 || (size_t)nw >= sizeof mod_path) {
            fprintf(stderr, "grove-ui: module path is too long\n");
            mod_path[0] = 0;
        }
        err_path = path_join(ui_dir, "live/build.err");
        char *main_c = path_join(ui_dir, "main.c");
        struct stat src, so;
        int src_ok = main_c && stat(main_c, &src) == 0;
        int so_ok = stat(mod_path, &so) == 0;
        if (src_ok && (!so_ok || mtim_newer(&src.st_mtim, &so.st_mtim))) {
            if (!err_path || compile_mod(ui_dir, mod_path, err_path) != 0) {
                fprintf(stderr, "grove-ui: could not build %s\n", main_c ? main_c : "ui/main.c");
                if (err_path) {
                    print_file(err_path);
                }
                free(main_c);
                if (ix) {
                    index_close(ix);
                }
                free(index_dir);
                free(page_path);
                free(page_root);
                free(page_name);
                free(ui_dir);
                free(err_path);
                return 1;
            }
        }
        free(main_c);
    }
    if (!mod_path[0] || access(mod_path, R_OK) != 0) {
        fprintf(stderr, "grove-ui: no program module. Run make ui from the source tree.\n");
        if (ix) {
            index_close(ix);
        }
        free(index_dir);
        free(page_path);
        free(page_root);
        free(page_name);
        free(ui_dir);
        free(err_path);
        return 1;
    }

    size_t win = webui_new_window();
    webui_set_size(win, 1040, 840);
    webui_set_center(win);
    /* Own Firefox profile. The WebUI default name is already taken when another
     * program is open, and Firefox will not start a second copy of it. */
    char *ff_profile = NULL;
    const char *home = getenv("HOME");
    if (home && *home) {
        char *share = path_join(home, ".local/share/grove");
        ff_profile = share ? path_join(share, "firefox") : NULL;
        free(share);
    }
    if (ff_profile) {
        webui_set_profile(win, "Grove", ff_profile);
        free(ff_profile);
    }
    if (port && !webui_set_port(win, port)) {
        fprintf(stderr, "grove-ui: port %zu is not available\n", port);
        return 1;
    }
    if (page_root && !webui_set_root_folder(win, page_root)) {
        fprintf(stderr, "grove-ui: could not serve %s\n", page_root);
        free(page_path);
        free(page_root);
        free(page_name);
        page_path = NULL;
        page_root = NULL;
        page_name = NULL;
    }

    memset(&host_ui, 0, sizeof host_ui);
    host_ui.ix = ix;
    host_ui.index_dir = index_dir;
    host_ui.open_err = open_err;
    host_ui.window = win;
    host_ui.page_path = page_path;
    host_ui.page_root = page_root;
    host_ui.page_name = page_name;
    host_ui.smoke = smoke;

    if (load_mod(&host_mod, mod_path) != 0 || host_mod.attach(&host_ui) != 0) {
        close_mod(&host_mod, 0);
        webui_clean();
        if (ix) {
            index_close(ix);
        }
        free(index_dir);
        free(page_path);
        free(page_root);
        free(page_name);
        free(ui_dir);
        free(err_path);
        return 1;
    }

    pthread_t cthread;
    int cwatching = 0;
    char *main_c = ui_dir && !smoke ? path_join(ui_dir, "main.c") : NULL;
    if (main_c) {
        fprintf(stderr, "grove-ui: watching %s\n", main_c);
        fflush(stderr);
        cwatching = pthread_create(&cthread, NULL, c_watch, main_c) == 0;
    }

    webui_wait();
    c_stop = 1;
    if (cwatching) {
        pthread_mutex_lock(&swap_mu);
        pthread_mutex_unlock(&swap_mu);
        pthread_join(cthread, NULL);
    }
    close_mod(&host_mod, 0);
    webui_clean();
    if (ix) {
        index_close(ix);
    }
    free(main_c);
    free(index_dir);
    free(page_path);
    free(page_root);
    free(page_name);
    free(ui_dir);
    free(err_path);
    return 0;
}
