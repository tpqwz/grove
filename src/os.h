#ifndef GROVE_OS_H
#define GROVE_OS_H

#include <stddef.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <sys/types.h>
#ifndef O_CLOEXEC
#ifdef O_NOINHERIT
#define O_CLOEXEC O_NOINHERIT
#else
#define O_CLOEXEC 0
#endif
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) (0)
#endif
#ifndef WIFEXITED
#define WIFEXITED(status) (((status) & 0x7f) == 0)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif

/* Incomplete on purpose. posix_spawn_file_actions_t only stores a pointer. */
typedef struct GroveSpawnAct GroveSpawnAct;

typedef struct {
    GroveSpawnAct *act;
    int n;
    int cap;
} posix_spawn_file_actions_t;

typedef void *posix_spawnattr_t;

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src, int dst);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode);
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
int pipe(int fds[2]);
int waitpid(pid_t pid, int *status, int flags);
int kill(pid_t pid, int sig);
/* Close the process handle and leave the process running. */
int grove_disown(pid_t pid);
#endif

/* The running program's path. Fails when it does not fit. */
int grove_self_path(char *buf, size_t cap);

/* Directory of that path, or NULL. The caller frees it. */
char *grove_self_dir(void);

/* Index directory from GROVE_INDEX, or the platform data directory. */
char *grove_index_dir(void);

/* Absolute path with forward slashes, or NULL. resolved may be NULL.
   Fails when the path does not exist. */
char *grove_realpath(const char *path, char *resolved);

/* Identity of a path. The caller frees it.
   An existing path is its real path. A missing tail keeps the real
   directory above it, so /tmp and /private/tmp stay the same place. */
char *grove_path_key(const char *path);

static inline int grove_lstat(const char *path, struct stat *st) {
#if defined(_WIN32)
    return stat(path, st);
#else
    return lstat(path, st);
#endif
}

static inline struct tm *grove_localtime(const time_t *t, struct tm *out) {
#if defined(_WIN32)
    return localtime_s(out, t) == 0 ? out : NULL;
#else
    return localtime_r(t, out);
#endif
}

/* Modification and access time. Windows keeps whole seconds. */
static inline struct timespec grove_atim(const struct stat *st) {
    struct timespec t;
#if defined(__APPLE__)
    t = st->st_atimespec;
#elif defined(_WIN32)
    t.tv_sec = st->st_atime;
    t.tv_nsec = 0;
#else
    t = st->st_atim;
#endif
    return t;
}

static inline struct timespec grove_mtim(const struct stat *st) {
    struct timespec t;
#if defined(__APPLE__)
    t = st->st_mtimespec;
#elif defined(_WIN32)
    t.tv_sec = st->st_mtime;
    t.tv_nsec = 0;
#else
    t = st->st_mtim;
#endif
    return t;
}

#endif
