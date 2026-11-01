#include "os.h"

#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

int grove_self_path(char *buf, size_t cap) {
    if (!buf || cap < 2) {
        return -1;
    }
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)cap);
    if (n == 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
#elif defined(__APPLE__)
    uint32_t need = (uint32_t)cap;
    if (_NSGetExecutablePath(buf, &need) != 0) {
        return -1;
    }
    char *real = realpath(buf, NULL);
    if (!real) {
        return -1;
    }
    size_t len = strlen(real);
    if (len + 1 > cap) {
        free(real);
        return -1;
    }
    memcpy(buf, real, len + 1);
    free(real);
    return 0;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0 || (size_t)n >= cap) {
        return -1;
    }
    buf[n] = 0;
    const char *gone = " (deleted)";
    size_t gn = strlen(gone);
    if ((size_t)n > gn && memcmp(buf + (size_t)n - gn, gone, gn) == 0) {
        buf[(size_t)n - gn] = 0;
    }
    return 0;
#endif
}

char *grove_self_dir(void) {
    char exe[4096];
    if (grove_self_path(exe, sizeof exe) != 0) {
        return NULL;
    }
    char *slash = strrchr(exe, '/');
    char *bslash = strrchr(exe, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
    if (!slash || slash == exe) {
        return NULL;
    }
    return xstrndup(exe, (size_t)(slash - exe));
}

char *grove_index_dir(void) {
    const char *env = getenv("GROVE_INDEX");
    if (env && *env) {
        return xstrdup(env);
    }
#if defined(_WIN32)
    const char *home = getenv("LOCALAPPDATA");
    if (!home || !*home) {
        home = getenv("USERPROFILE");
    }
    if (!home || !*home) {
        home = getenv("HOME");
    }
    if (!home || !*home) {
        return xstrdup("grove");
    }
    return path_join(home, "grove");
#else
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
#endif
}

char *grove_realpath(const char *path, char *resolved) {
#if !defined(_WIN32)
    return realpath(path, resolved);
#else
    if (!path || !path[0]) {
        errno = EINVAL;
        return NULL;
    }
    char buf[4096];
    DWORD n = GetFullPathNameA(path, (DWORD)sizeof buf, buf, NULL);
    if (n == 0 || n >= sizeof buf) {
        errno = n == 0 ? EINVAL : ENAMETOOLONG;
        return NULL;
    }
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES) {
        errno = ENOENT;
        return NULL;
    }
    for (char *p = buf; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    size_t len = strlen(buf);
    if (resolved) {
        if (len >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        memcpy(resolved, buf, len + 1);
        return resolved;
    }
    return xstrdup(buf);
#endif
}

#if defined(_WIN32)
struct GroveSpawnAct {
    int kind;
    int fd;
    int src;
    int oflag;
    mode_t mode;
    char *path;
};

enum { SPAWN_DUP2 = 1, SPAWN_CLOSE = 2, SPAWN_OPEN = 3, SPAWN_CAP = 32 };

static int spawn_add(posix_spawn_file_actions_t *fa, struct GroveSpawnAct item) {
    if (!fa) {
        return EINVAL;
    }
    if (fa->n >= SPAWN_CAP) {
        return ENOMEM;
    }
    if (fa->n == fa->cap) {
        int nc = fa->cap == 0 ? 4 : fa->cap * 2;
        if (nc > SPAWN_CAP) {
            nc = SPAWN_CAP;
        }
        struct GroveSpawnAct *na = realloc(fa->act, (size_t)nc * sizeof *na);
        if (!na) {
            return ENOMEM;
        }
        fa->act = na;
        fa->cap = nc;
    }
    fa->act[fa->n++] = item;
    return 0;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
    if (!fa) {
        return EINVAL;
    }
    fa->act = NULL;
    fa->n = 0;
    fa->cap = 0;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
    if (!fa) {
        return EINVAL;
    }
    for (int i = 0; i < fa->n; i++) {
        free(fa->act[i].path);
    }
    free(fa->act);
    fa->act = NULL;
    fa->n = 0;
    fa->cap = 0;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src, int dst) {
    struct GroveSpawnAct item;
    memset(&item, 0, sizeof item);
    item.kind = SPAWN_DUP2;
    item.fd = dst;
    item.src = src;
    return spawn_add(fa, item);
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
    struct GroveSpawnAct item;
    memset(&item, 0, sizeof item);
    item.kind = SPAWN_CLOSE;
    item.fd = fd;
    return spawn_add(fa, item);
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode) {
    if (!path) {
        return EINVAL;
    }
    struct GroveSpawnAct item;
    memset(&item, 0, sizeof item);
    item.kind = SPAWN_OPEN;
    item.fd = fd;
    item.oflag = oflag;
    item.mode = mode;
    item.path = xstrdup(path);
    if (!item.path) {
        return ENOMEM;
    }
    int rc = spawn_add(fa, item);
    if (rc != 0) {
        free(item.path);
    }
    return rc;
}

enum { KID_MAX = 32 };

typedef struct Kid {
    DWORD pid;
    HANDLE proc;
} Kid;

static Kid kids[KID_MAX];
static CRITICAL_SECTION kid_mu;
static INIT_ONCE kid_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK kid_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&kid_mu);
    return TRUE;
}

static void kid_lock(void) {
    InitOnceExecuteOnce(&kid_once, kid_init, NULL, NULL);
    EnterCriticalSection(&kid_mu);
}

static void kid_unlock(void) {
    LeaveCriticalSection(&kid_mu);
}

static int kid_put(DWORD pid, HANDLE proc) {
    kid_lock();
    int rc = EAGAIN;
    for (int i = 0; i < KID_MAX; i++) {
        if (!kids[i].proc) {
            kids[i].pid = pid;
            kids[i].proc = proc;
            rc = 0;
            break;
        }
    }
    kid_unlock();
    return rc;
}

static HANDLE kid_take(DWORD pid) {
    HANDLE proc = NULL;
    kid_lock();
    for (int i = 0; i < KID_MAX; i++) {
        if (kids[i].proc && kids[i].pid == pid) {
            proc = kids[i].proc;
            kids[i].proc = NULL;
            kids[i].pid = 0;
            break;
        }
    }
    kid_unlock();
    return proc;
}

int grove_disown(pid_t pid) {
    HANDLE proc = kid_take((DWORD)pid);
    if (!proc) {
        errno = ESRCH;
        return -1;
    }
    CloseHandle(proc);
    return 0;
}

int waitpid(pid_t pid, int *status, int flags) {
    if (flags != 0) {
        errno = EINVAL;
        return -1;
    }
    HANDLE proc = kid_take((DWORD)pid);
    if (!proc) {
        errno = ESRCH;
        return -1;
    }
    if (WaitForSingleObject(proc, INFINITE) != WAIT_OBJECT_0) {
        CloseHandle(proc);
        errno = ECHILD;
        return -1;
    }
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (status) {
        *status = (int)((code & 0xffu) << 8);
    }
    return (int)pid;
}

int kill(pid_t pid, int sig) {
    (void)sig;
    HANDLE proc = NULL;
    kid_lock();
    for (int i = 0; i < KID_MAX; i++) {
        if (kids[i].proc && kids[i].pid == (DWORD)pid) {
            proc = kids[i].proc;
            break;
        }
    }
    kid_unlock();
    if (!proc) {
        errno = ESRCH;
        return -1;
    }
    if (!TerminateProcess(proc, 1)) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int pipe(int fds[2]) {
    if (!fds) {
        errno = EINVAL;
        return -1;
    }
    if (_pipe(fds, 4096, _O_BINARY) != 0) {
        return -1;
    }
    return 0;
}

/* Worst case is two output bytes per input byte, plus the quotes. */
static int quote_arg(const char *arg, char *dst, size_t cap, size_t *used) {
    size_t n = 0;
    int plain = arg[0] != 0;
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '"') {
            plain = 0;
        }
        n++;
    }
    size_t need = plain ? n : n * 2 + 2;
    size_t at = *used;
    if (need > cap - at) {
        return -1;
    }
    if (plain) {
        memcpy(dst + at, arg, n);
        *used = at + n;
        return 0;
    }
    dst[at++] = '"';
    size_t bs = 0;
    for (size_t i = 0;; i++) {
        int end = i == n;
        if (!end && arg[i] == '\\') {
            bs++;
            continue;
        }
        size_t out_bs = bs;
        if (end) {
            out_bs = bs * 2;
        } else if (arg[i] == '"') {
            out_bs = bs * 2 + 1;
        }
        if (at + out_bs + 1 > cap) {
            return -1;
        }
        for (size_t k = 0; k < out_bs; k++) {
            dst[at++] = '\\';
        }
        bs = 0;
        if (end) {
            break;
        }
        dst[at++] = arg[i];
    }
    dst[at++] = '"';
    *used = at;
    return 0;
}

static char *join_argv(char *const argv[]) {
    size_t cap = 1;
    for (int i = 0; argv[i]; i++) {
        size_t n = strlen(argv[i]);
        size_t one = n * 2 + 3;
        if (size_add(cap, one, &cap) != 0) {
            errno = E2BIG;
            return NULL;
        }
    }
    if (cap > 32767) {
        errno = E2BIG;
        return NULL;
    }
    char *cmd = malloc(cap);
    if (!cmd) {
        errno = ENOMEM;
        return NULL;
    }
    size_t used = 0;
    for (int i = 0; argv[i]; i++) {
        if (i && used + 1 < cap) {
            cmd[used++] = ' ';
        }
        if (quote_arg(argv[i], cmd, cap, &used) != 0) {
            free(cmd);
            errno = E2BIG;
            return NULL;
        }
    }
    cmd[used] = 0;
    return cmd;
}

static int apply_actions(const posix_spawn_file_actions_t *fa, STARTUPINFOA *si, HANDLE opened[], int *nopened) {
    si->dwFlags |= STARTF_USESTDHANDLES;
    si->hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si->hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si->hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!fa) {
        return 0;
    }
    /* Closes first, then duplicates. A close of the pipe's write end must not
     * clear the handle after it has been made the child's stdout. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < fa->n; i++) {
            const struct GroveSpawnAct *a = &fa->act[i];
            if (pass == 0 && a->kind == SPAWN_CLOSE) {
                intptr_t h = _get_osfhandle(a->fd);
                if (h != (intptr_t)-1) {
                    SetHandleInformation((HANDLE)h, HANDLE_FLAG_INHERIT, 0);
                }
            }
            if (pass == 0 || a->kind == SPAWN_CLOSE) {
                continue;
            }
            if (a->kind == SPAWN_DUP2) {
                if (a->fd < 0 || a->fd > 2) {
                    return EINVAL;
                }
                intptr_t h = _get_osfhandle(a->src);
                if (h == (intptr_t)-1) {
                    return EBADF;
                }
                SetHandleInformation((HANDLE)h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                if (a->fd == 0) {
                    si->hStdInput = (HANDLE)h;
                } else if (a->fd == 1) {
                    si->hStdOutput = (HANDLE)h;
                } else {
                    si->hStdError = (HANDLE)h;
                }
                continue;
            }
            if (a->kind == SPAWN_OPEN) {
                if (a->fd < 0 || a->fd > 2 || *nopened >= 8) {
                    return EINVAL;
                }
                DWORD access = 0;
                DWORD disp = OPEN_EXISTING;
                if (a->oflag & O_WRONLY) {
                    access = GENERIC_WRITE;
                } else if (a->oflag & O_RDWR) {
                    access = GENERIC_READ | GENERIC_WRITE;
                } else {
                    access = GENERIC_READ;
                }
                if (a->oflag & O_CREAT) {
                    disp = (a->oflag & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
                } else if (a->oflag & O_TRUNC) {
                    disp = TRUNCATE_EXISTING;
                }
                SECURITY_ATTRIBUTES sa;
                memset(&sa, 0, sizeof sa);
                sa.nLength = sizeof sa;
                sa.bInheritHandle = TRUE;
                HANDLE h = CreateFileA(a->path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, disp,
                                       FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE) {
                    return ENOENT;
                }
                opened[(*nopened)++] = h;
                if (a->fd == 0) {
                    si->hStdInput = h;
                } else if (a->fd == 1) {
                    si->hStdOutput = h;
                } else {
                    si->hStdError = h;
                }
                continue;
            }
            return EINVAL;
        }
    }
    return 0;
}

static int spawn_create(pid_t *pid, const char *file, int search, const posix_spawn_file_actions_t *fa, char *const argv[]) {
    if (!file || !argv || !argv[0]) {
        return EINVAL;
    }
    char *cmd = join_argv(argv);
    if (!cmd) {
        return errno ? errno : ENOMEM;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    HANDLE opened[8];
    int nopened = 0;
    int inherit = fa != NULL;
    int arc = 0;
    if (fa) {
        arc = apply_actions(fa, &si, opened, &nopened);
    }
    if (arc != 0) {
        for (int i = 0; i < nopened; i++) {
            CloseHandle(opened[i]);
        }
        free(cmd);
        return arc;
    }
    BOOL ok = CreateProcessA(search ? NULL : file, cmd, NULL, NULL, inherit ? TRUE : FALSE, 0, NULL, NULL, &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    free(cmd);
    for (int i = 0; i < nopened; i++) {
        CloseHandle(opened[i]);
    }
    if (!ok) {
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return ENOENT;
        }
        return EIO;
    }
    CloseHandle(pi.hThread);
    if (!pid) {
        CloseHandle(pi.hProcess);
        return 0;
    }
    int put = kid_put(pi.dwProcessId, pi.hProcess);
    if (put != 0) {
        CloseHandle(pi.hProcess);
        return put;
    }
    *pid = (pid_t)pi.dwProcessId;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    (void)attr;
    (void)envp;
    return spawn_create(pid, path, 0, fa, argv);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    (void)attr;
    (void)envp;
    return spawn_create(pid, file, 1, fa, argv);
}
#endif
