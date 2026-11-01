#include "cli.h"
#include "fetch.h"
#include "index.h"
#include "mod.h"
#include "os.h"
#include "webui.h"
#include "window_icon.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32)
#define RTLD_NOW 0
#define RTLD_LOCAL 0
static void *dlopen(const char *path, int flags) {
    (void)path;
    (void)flags;
    return NULL;
}
static void *dlsym(void *handle, const char *name) {
    (void)handle;
    (void)name;
    return NULL;
}
static int dlclose(void *handle) {
    (void)handle;
    return 0;
}
static char *dlerror(void) {
    return "live reload is not built on Windows";
}
#else
extern char **environ;
#endif

#if !defined(__linux__)
void grove_window_icon_start(size_t window) {
    (void)window;
}
void grove_window_icon_stop(void) {}
#endif

static char *default_index_dir(void) {
    return grove_index_dir();
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
    return grove_realpath(path, NULL);
}

static char *exe_dir(void) {
    return grove_self_dir();
}

/* Returns a malloc'd absolute path, or NULL. Sets *missing when GROVE_PAGE is unreadable. */
static char *find_live_page(int *missing) {
    *missing = 0;
    const char *env = getenv("GROVE_PAGE");
    if (env && *env) {
        char *abs = absolute_file(env);
        if (!abs) {
            fprintf(stderr, "grove: cannot read %s\n", env);
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

#if !defined(_WIN32)
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
#endif

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
    int fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(fd);
        return -1;
    }
    int act = 0;
    act |= posix_spawn_file_actions_adddup2(&fa, fd, STDOUT_FILENO);
    act |= posix_spawn_file_actions_adddup2(&fa, fd, STDERR_FILENO);
    act |= posix_spawn_file_actions_addclose(&fa, fd);
    char outarg[PATH_MAX];
    int nw = snprintf(outarg, sizeof outarg, "OUT=%s", out);
    char *args[] = {"make", "-C", (char *)ui_dir, "live-mod", outarg, NULL};
    pid_t pid = 0;
    int rc = -1;
    if (nw > 0 && (size_t)nw < sizeof outarg && act == 0) {
        rc = posix_spawnp(&pid, "make", &fa, NULL, args, environ);
    }
    posix_spawn_file_actions_destroy(&fa);
    close(fd);
    if (rc != 0) {
        return -1;
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
        fprintf(stderr, "grove: %s\n", dlerror());
        return -1;
    }
    attach_fn attach = (attach_fn)dlsym(h, "grove_ui_attach");
    detach_fn detach = (detach_fn)dlsym(h, "grove_ui_detach");
    if (!attach || !detach) {
        fprintf(stderr, "grove: %s\n", dlerror());
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
        seen = grove_mtim(&st);
    }
    struct timespec pending;
    int have_pending = 0;
    memset(&pending, 0, sizeof pending);
    while (!c_stop) {
        if (stat(main_c, &st) != 0) {
            have_pending = 0;
            usleep(100000);
            continue;
        }
        struct timespec now = grove_mtim(&st);
        if (mtim_eq(&now, &seen)) {
            have_pending = 0;
            usleep(100000);
            continue;
        }
        if (!(have_pending && mtim_eq(&now, &pending))) {
            pending = now;
            have_pending = 1;
            usleep(100000);
            continue;
        }
        seen = now;
        have_pending = 0;
        pthread_mutex_lock(&swap_mu);
        if (!c_stop) {
            char out[PATH_MAX];
            int nw = snprintf(out, sizeof out, "%s/live/module-%d.so", ui_dir, ++mod_seq);
            if (nw < 0 || (size_t)nw >= sizeof out) {
                fprintf(stderr, "grove: module path is too long\n");
                fflush(stderr);
                pthread_mutex_unlock(&swap_mu);
                continue;
            }
            fprintf(stderr, "grove: building %s\n", main_c);
            fflush(stderr);
            if (compile_mod(ui_dir, out, err_path) != 0) {
                fprintf(stderr, "grove: build failed, keeping the running program\n");
                print_file(err_path);
                fflush(stderr);
                unlink(out);
            } else if (swap_in(out, 1) != 0) {
                fprintf(stderr, "grove: could not load the new program, keeping the previous one\n");
                fflush(stderr);
            } else {
                fprintf(stderr, "grove: updated %s\n", main_c);
                fflush(stderr);
            }
        }
        pthread_mutex_unlock(&swap_mu);
        usleep(100000);
    }
    return NULL;
}

static int wants_cli(int argc, char **argv) {
    int version = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--index") == 0 || strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "--smoke") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }
            i++;
            continue;
        }
        if (strcmp(argv[i], "--quiet") == 0) {
            continue;
        }
        if (strcmp(argv[i], "--version") == 0) {
            version = 1;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            continue;
        }
        if (argv[i][0] != '-') {
            return 1;
        }
    }
    return version;
}

static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;
static int job_running;
static int job_kind;
static int job_ok;
static IndexStats job_st;
static char job_err[768];

typedef struct JobArg {
    char *dir;
    char *path;
    char *url;
} JobArg;

static void job_arg_free(JobArg *a) {
    if (!a) {
        return;
    }
    free(a->dir);
    free(a->path);
    free(a->url);
    free(a);
}

static void *job_main(void *arg) {
    JobArg *a = arg;
    Err e;
    err_clear(&e);
    IndexStats st;
    memset(&st, 0, sizeof st);
    int rc = -1;
    Index *w = index_open(a->dir, 1, &e);
    if (w) {
        if (a->url) {
            rc = grove_fetch_into(w, a->dir, a->url, &st, &e);
        } else if (a->path) {
            rc = index_add(w, a->path, &st, &e);
        } else {
            rc = index_shelf_build(w, &e);
        }
        index_close(w);
    }
    pthread_mutex_lock(&job_mu);
    job_running = 0;
    job_ok = rc == 0;
    job_st = st;
    snprintf(job_err, sizeof job_err, "%s", rc == 0 ? "" : (e.msg[0] ? e.msg : "could not update the library"));
    pthread_mutex_unlock(&job_mu);
    job_arg_free(a);
    return NULL;
}

static int job_start(const char *dir, const char *path, const char *url, int kind) {
    if (!dir || !dir[0] || (path && !path[0]) || (url && !url[0]) || (path && url)) {
        return -1;
    }
    JobArg *a = calloc(1, sizeof *a);
    if (!a) {
        return -1;
    }
    a->dir = xstrdup(dir);
    a->path = path ? xstrdup(path) : NULL;
    a->url = url ? xstrdup(url) : NULL;
    if (!a->dir || (path && !a->path) || (url && !a->url)) {
        job_arg_free(a);
        return -1;
    }
    pthread_mutex_lock(&job_mu);
    if (job_running) {
        pthread_mutex_unlock(&job_mu);
        job_arg_free(a);
        return -1;
    }
    job_running = 1;
    job_kind = kind;
    job_ok = 0;
    job_err[0] = 0;
    memset(&job_st, 0, sizeof job_st);
    pthread_mutex_unlock(&job_mu);
    pthread_t th;
    if (pthread_create(&th, NULL, job_main, a) != 0) {
        pthread_mutex_lock(&job_mu);
        job_running = 0;
        job_ok = 0;
        snprintf(job_err, sizeof job_err, "could not start");
        pthread_mutex_unlock(&job_mu);
        job_arg_free(a);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

int grove_host_prepare(const char *index_dir) {
    return job_start(index_dir, NULL, NULL, 1);
}

int grove_host_import(const char *index_dir, const char *path) {
    return job_start(index_dir, path, NULL, 2);
}

int grove_host_fetch(const char *index_dir, const char *url) {
    return job_start(index_dir, NULL, url, 3);
}

void grove_host_job(GroveJob *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&job_mu);
    out->running = job_running;
    out->kind = job_kind;
    out->ok = job_ok;
    out->indexed = job_st.indexed;
    out->unchanged = job_st.unchanged;
    out->removed = job_st.removed;
    out->skipped = job_st.skipped;
    snprintf(out->error, sizeof out->error, "%s", job_err);
    pthread_mutex_unlock(&job_mu);
}

int main(int argc, char **argv) {
    if (wants_cli(argc, argv)) {
        return grove_cli(argc, argv);
    }
    return grove_open_window(argc, argv);
}

int grove_open_window(int argc, char **argv) {
    const char *index_arg = NULL;
    size_t port = 0;
    const char *smoke = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            grove_usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--index") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove: --index needs a directory\n");
                return 1;
            }
            index_arg = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove: --port needs a number\n");
                return 1;
            }
            errno = 0;
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (!end || end == argv[i] || *end || errno != 0 || v < 1 || v > 65535) {
                fprintf(stderr, "grove: --port must be from 1 to 65535\n");
                return 1;
            }
            port = (size_t)v;
            continue;
        }
        if (strcmp(argv[i], "--smoke") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "grove: --smoke needs a query\n");
                return 1;
            }
            smoke = argv[++i];
            continue;
        }
        fprintf(stderr, "grove: unknown option %s\n", argv[i]);
        return 1;
    }

    char *index_dir = index_arg ? xstrdup(index_arg) : default_index_dir();
    if (!index_dir) {
        fprintf(stderr, "grove: out of memory\n");
        return 1;
    }
    Err e;
    err_clear(&e);
    char open_err[768];
    open_err[0] = 0;
    Index *ix = index_open(index_dir, 0, &e);
    if (!ix) {
        snprintf(open_err, sizeof open_err, "%s", e.msg[0] ? e.msg : "could not open the index");
        fprintf(stderr, "grove: %s\n", open_err);
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

#if defined(_WIN32)
    /* The page is linked in. Windows does not dlopen a side module. */
    ui_dir = NULL;
#else
    ui_dir = find_ui_dir();
#endif
    char mod_path[PATH_MAX];
    mod_path[0] = 0;
    if (ui_dir) {
        int nw = snprintf(mod_path, sizeof mod_path, "%s/live/module.so", ui_dir);
        if (nw < 0 || (size_t)nw >= sizeof mod_path) {
            fprintf(stderr, "grove: module path is too long\n");
            mod_path[0] = 0;
        }
        err_path = path_join(ui_dir, "live/build.err");
        char *main_c = path_join(ui_dir, "main.c");
        struct stat src, so;
        int src_ok = main_c && stat(main_c, &src) == 0;
        int so_ok = mod_path[0] && stat(mod_path, &so) == 0;
        struct timespec src_m;
        struct timespec so_m;
        memset(&src_m, 0, sizeof src_m);
        memset(&so_m, 0, sizeof so_m);
        if (src_ok) {
            src_m = grove_mtim(&src);
        }
        if (so_ok) {
            so_m = grove_mtim(&so);
        }
        if (src_ok && (!so_ok || mtim_newer(&src_m, &so_m))) {
            if (!err_path || compile_mod(ui_dir, mod_path, err_path) != 0) {
                fprintf(stderr, "grove: could not build %s\n", main_c ? main_c : "ui/main.c");
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
    int builtin = 0;
    if (!mod_path[0] || access(mod_path, R_OK) != 0) {
        builtin = 1;
    }

    size_t win = webui_new_window();
    webui_set_size(win, 1040, 840);
    webui_set_center(win);
    /* Own Firefox profile. The WebUI default name is already taken when another
     * program is open, and Firefox will not start a second copy of it. */
    char *ff_profile = NULL;
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = getenv("USERPROFILE");
    }
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
        fprintf(stderr, "grove: port %zu is not available\n", port);
        return 1;
    }
    if (page_root && !webui_set_root_folder(win, page_root)) {
        fprintf(stderr, "grove: could not serve %s\n", page_root);
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

    if (builtin) {
        if (grove_ui_attach(&host_ui) != 0) {
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
    } else if (load_mod(&host_mod, mod_path) != 0 || host_mod.attach(&host_ui) != 0) {
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
    char *main_c = !builtin && ui_dir && !smoke ? path_join(ui_dir, "main.c") : NULL;
    if (main_c) {
        fprintf(stderr, "grove: watching %s\n", main_c);
        fflush(stderr);
        cwatching = pthread_create(&cthread, NULL, c_watch, main_c) == 0;
    }
    grove_window_icon_start(win);

    webui_wait();
    c_stop = 1;
    grove_window_icon_stop();
    if (cwatching) {
        pthread_mutex_lock(&swap_mu);
        pthread_mutex_unlock(&swap_mu);
        pthread_join(cthread, NULL);
    }
    if (builtin) {
        grove_ui_detach();
    } else {
        close_mod(&host_mod, 0);
    }
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
