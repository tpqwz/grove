#include "fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* One page, saved under the index so the HTML indexer can read it. curl is a
 * separate program and the address is one argument. The body is capped here
 * because curl's own size limit does not stop a chunked response. */

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

static uint32_t rot32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha_block(uint32_t h[8], const unsigned char block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t x = w[i - 15];
        uint32_t y = w[i - 2];
        uint32_t s0 = rot32(x, 7) ^ rot32(x, 18) ^ (x >> 3);
        uint32_t s1 = rot32(y, 17) ^ rot32(y, 19) ^ (y >> 10);
        /* SHA-256 addition is defined mod 2^32. */
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0];
    uint32_t b = h[1];
    uint32_t c = h[2];
    uint32_t d = h[3];
    uint32_t e = h[4];
    uint32_t f = h[5];
    uint32_t g = h[6];
    uint32_t hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rot32(e, 6) ^ rot32(e, 11) ^ rot32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + s1 + ch + SHA_K[i] + w[i];
        uint32_t s0 = rot32(a, 2) ^ rot32(a, 13) ^ rot32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void fetch_sha256_hex(const void *data, size_t n, char out[65]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64];
    size_t filled = 0;
    uint64_t nbytes = 0;
    const unsigned char *p = data ? data : (const unsigned char *)"";
    if (n > UINT64_MAX - nbytes) {
        n = 0;
    }
    while (n) {
        size_t room = 64 - filled;
        size_t take = n < room ? n : room;
        memcpy(block + filled, p, take);
        filled += take;
        p += take;
        n -= take;
        nbytes += take;
        if (filled == 64) {
            sha_block(h, block);
            filled = 0;
        }
    }
    uint64_t bits = nbytes * 8;
    block[filled++] = 0x80;
    if (filled > 56) {
        while (filled < 64) {
            block[filled++] = 0;
        }
        sha_block(h, block);
        filled = 0;
    }
    while (filled < 56) {
        block[filled++] = 0;
    }
    for (int i = 7; i >= 0; i--) {
        block[filled++] = (unsigned char)((bits >> (8 * i)) & 0xff);
    }
    sha_block(h, block);
    static const char *hexd = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out[i * 8 + (28 - shift) / 4] = hexd[(h[i] >> shift) & 0xf];
        }
    }
    out[64] = 0;
}

static int ieq(const char *s, size_t n, const char *lit) {
    size_t m = strlen(lit);
    if (n != m) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        if (c != (unsigned char)lit[i]) {
            return 0;
        }
    }
    return 1;
}

static int parse_port(const char *s, size_t n, int *port) {
    if (n == 0 || n > 5) {
        return -1;
    }
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    if (v < 1 || v > 65535) {
        return -1;
    }
    *port = v;
    return 0;
}

int fetch_normalize(const char *in, char **url, Err *e) {
    if (url) {
        *url = NULL;
    }
    if (!url) {
        err_set(e, "out of memory");
        return -1;
    }
    if (!in) {
        in = "";
    }
    while (*in == ' ' || *in == '\t') {
        in++;
    }
    size_t n = strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t')) {
        n--;
    }
    if (n == 0) {
        err_set(e, "type a page address");
        return -1;
    }
    if (n > 4096) {
        err_set(e, "that address is too long");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c <= 0x20 || c == 0x7f || c == '\\') {
            err_set(e, "that address is not a web page");
            return -1;
        }
    }
    char *s = malloc(n + 1);
    if (!s) {
        err_set(e, "out of memory");
        return -1;
    }
    memcpy(s, in, n);
    s[n] = 0;
    const char *sep = strstr(s, "://");
    size_t scheme = sep ? (size_t)(sep - s) : 0;
    int https = 0;
    if (!sep || scheme == 0) {
        err_set(e, "that address is not a web page");
        free(s);
        return -1;
    }
    if (ieq(s, scheme, "https")) {
        https = 1;
    } else if (!ieq(s, scheme, "http")) {
        err_set(e, "that address is not a web page");
        free(s);
        return -1;
    }
    const char *rest = sep + 3;
    size_t rest_n = strlen(rest);
    const void *mark = memchr(rest, '#', rest_n);
    if (mark) {
        rest_n = (size_t)((const char *)mark - rest);
    }
    const void *slash_at = memchr(rest, '/', rest_n);
    size_t auth_n = slash_at ? (size_t)((const char *)slash_at - rest) : rest_n;
    const char *path = slash_at ? (const char *)slash_at : rest + rest_n;
    size_t path_n = slash_at ? rest_n - auth_n : 0;
    if (auth_n == 0 || memchr(rest, '@', auth_n)) {
        err_set(e, "that address is not a web page");
        free(s);
        return -1;
    }
    size_t host_n = auth_n;
    int port = -1;
    int colons = 0;
    for (size_t i = 0; i < auth_n; i++) {
        if (rest[i] == ':') {
            colons++;
        }
    }
    if (rest[0] == '[') {
        const void *end = memchr(rest, ']', auth_n);
        if (!end || (const char *)end == rest + 1) {
            err_set(e, "that address is not a web page");
            free(s);
            return -1;
        }
        host_n = (size_t)((const char *)end - rest + 1);
        size_t tail = auth_n - host_n;
        if (tail) {
            if (rest[host_n] != ':' || parse_port(rest + host_n + 1, tail - 1, &port) != 0) {
                err_set(e, "that address is not a web page");
                free(s);
                return -1;
            }
        }
    } else if (colons > 1) {
        err_set(e, "that address is not a web page");
        free(s);
        return -1;
    } else if (colons == 1) {
        const char *colon = memchr(rest, ':', auth_n);
        host_n = colon ? (size_t)(colon - rest) : auth_n;
        if (host_n == 0 || parse_port(colon + 1, auth_n - host_n - 1, &port) != 0) {
            err_set(e, "that address is not a web page");
            free(s);
            return -1;
        }
    }
    if (host_n == 0) {
        err_set(e, "that address is not a web page");
        free(s);
        return -1;
    }
    Buf b;
    buf_init(&b);
    int bad = buf_puts(&b, https ? "https://" : "http://") != 0;
    for (size_t i = 0; !bad && i < host_n; i++) {
        unsigned char c = (unsigned char)rest[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        bad = buf_putc(&b, c) != 0;
    }
    int drop_port = port == 80 && !https;
    if (port == 443 && https) {
        drop_port = 1;
    }
    if (!bad && port >= 0 && !drop_port) {
        bad = buf_printf(&b, ":%d", port) != 0;
    }
    if (!bad && path_n) {
        bad = buf_append(&b, path, path_n) != 0;
    }
    free(s);
    if (bad) {
        buf_free(&b);
        err_set(e, "out of memory");
        return -1;
    }
    if (b.len == 0 || b.len > FETCH_URL_MAX) {
        buf_free(&b);
        err_set(e, "that address is too long");
        return -1;
    }
    *url = b.data;
    return 0;
}

int fetch_read_cap(int fd, unsigned char **out, size_t *n, size_t cap, Err *e) {
    unsigned char *buf = NULL;
    size_t len = 0;
    size_t alloc = 0;
    if (out) {
        *out = NULL;
    }
    if (n) {
        *n = 0;
    }
    if (!out || !n) {
        err_set(e, "out of memory");
        return -1;
    }
    if (cap == 0) {
        err_set(e, "that page is too large");
        return -1;
    }
    for (;;) {
        unsigned char tmp[4096];
        size_t room = cap - len;
        size_t chunk = room < sizeof tmp ? room : sizeof tmp;
        if (chunk == 0) {
            unsigned char extra;
            ssize_t one;
            do {
                one = read(fd, &extra, 1);
            } while (one < 0 && errno == EINTR);
            if (one < 0) {
                free(buf);
                err_set(e, "could not fetch that page");
                return -1;
            }
            if (one == 1) {
                free(buf);
                err_set(e, "that page is too large");
                return -1;
            }
            break;
        }
        ssize_t got;
        do {
            got = read(fd, tmp, chunk);
        } while (got < 0 && errno == EINTR);
        if (got < 0) {
            free(buf);
            err_set(e, "could not fetch that page");
            return -1;
        }
        if (got == 0) {
            break;
        }
        size_t need = 0;
        if (size_add(len, (size_t)got, &need) != 0 || need > cap) {
            free(buf);
            err_set(e, "that page is too large");
            return -1;
        }
        if (need > alloc) {
            size_t na = alloc ? alloc : 4096;
            while (na < need) {
                size_t nxt = 0;
                if (size_mul(na, 2, &nxt) != 0) {
                    na = need;
                    break;
                }
                na = nxt;
            }
            if (na > cap) {
                na = cap;
            }
            if (na < need) {
                na = need;
            }
            unsigned char *nb = realloc(buf, na ? na : 1);
            if (!nb) {
                free(buf);
                err_set(e, "out of memory");
                return -1;
            }
            buf = nb;
            alloc = na;
        }
        memcpy(buf + len, tmp, (size_t)got);
        len = need;
    }
    *out = buf;
    *n = len;
    return 0;
}

static int wait_pid(pid_t pid) {
    for (;;) {
        int status = 0;
        pid_t got = waitpid(pid, &status, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return -1;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
        return -1;
    }
}

static void set_curl_error(const char *path, Err *e) {
    int fd = open(path, O_RDONLY);
    unsigned char *buf = NULL;
    size_t n = 0;
    Err ignore;
    err_clear(&ignore);
    if (fd < 0 || fetch_read_cap(fd, &buf, &n, 2048, &ignore) != 0 || n == 0) {
        if (fd >= 0) {
            close(fd);
        }
        free(buf);
        err_set(e, "could not fetch that page");
        return;
    }
    close(fd);
    size_t i = 0;
    if (n >= 5 && memcmp(buf, "curl:", 5) == 0) {
        i = 5;
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) {
            i++;
        }
        if (i < n && buf[i] == '(') {
            while (i < n && buf[i] != ')') {
                i++;
            }
            if (i < n && buf[i] == ')') {
                i++;
            }
            while (i < n && (buf[i] == ' ' || buf[i] == '\t')) {
                i++;
            }
        }
    }
    char msg[180];
    size_t w = 0;
    int gap = 0;
    for (; i < n && w + 1 < sizeof msg; i++) {
        unsigned char c = buf[i];
        if (c <= 0x20 || c == 0x7f) {
            gap = w > 0;
            continue;
        }
        if (gap && w + 1 < sizeof msg) {
            msg[w++] = ' ';
        }
        gap = 0;
        msg[w++] = (char)c;
    }
    free(buf);
    while (w && msg[w - 1] == ' ') {
        w--;
    }
    msg[w] = 0;
    if (!w) {
        err_set(e, "could not fetch that page");
        return;
    }
    err_set(e, "%s", msg);
}

static int media_is_page(const unsigned char *s, size_t n) {
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    size_t cut = n;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ';') {
            cut = i;
            break;
        }
    }
    while (cut && (s[cut - 1] == ' ' || s[cut - 1] == '\t')) {
        cut--;
    }
    return ieq((const char *)s, cut, "text/html") || ieq((const char *)s, cut, "application/xhtml+xml") ||
           ieq((const char *)s, cut, "text/plain");
}

static int headers_are_page(const unsigned char *s, size_t n, Err *e) {
    size_t last = 0;
    size_t i = 0;
    while (i + 1 < n) {
        size_t skip = 0;
        if (i + 3 < n && s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n') {
            skip = 4;
        } else if (s[i] == '\n' && s[i + 1] == '\n') {
            skip = 2;
        }
        if (skip) {
            i += skip;
            if (i < n) {
                last = i;
            }
            continue;
        }
        i++;
    }
    int saw = 0;
    int ok = 0;
    i = last;
    while (i < n && s[i] != '\n' && !(s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')) {
        size_t eol = i;
        while (eol < n && s[eol] != '\n') {
            eol++;
        }
        size_t len = eol - i;
        if (len && s[i + len - 1] == '\r') {
            len--;
        }
        if (len >= 13 && ieq((const char *)s + i, 13, "content-type:")) {
            size_t v = i + 13;
            size_t vend = i + len;
            while (v < vend && (s[v] == ' ' || s[v] == '\t')) {
                v++;
            }
            saw = 1;
            ok = media_is_page(s + v, vend - v);
        }
        i = eol < n && s[eol] == '\n' ? eol + 1 : eol;
        if (i < n && (s[i] == '\n' || (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n'))) {
            break;
        }
    }
    if (!saw || ok) {
        return 0;
    }
    err_set(e, "that address is not a web page");
    return -1;
}

int fetch_save_cap(const char *index_dir, const char *raw, size_t cap, char **saved, char **url, Err *e) {
    if (saved) {
        *saved = NULL;
    }
    if (url) {
        *url = NULL;
    }
    if (!saved || !url || !index_dir || !index_dir[0]) {
        err_set(e, "no index");
        return -1;
    }
    char *norm = NULL;
    if (fetch_normalize(raw, &norm, e) != 0) {
        return -1;
    }
    char *web = path_join(index_dir, "web");
    char hex[65];
    fetch_sha256_hex(norm, strlen(norm), hex);
    char name[80];
    int nw = snprintf(name, sizeof name, "%s.html", hex);
    char *dest = web && nw > 0 && (size_t)nw < sizeof name ? path_join(web, name) : NULL;
    if (!web || !dest || mkdir_p(web) != 0) {
        free(web);
        free(dest);
        free(norm);
        err_set(e, "cannot save that page");
        return -1;
    }
    free(web);
    char err_path[] = "/tmp/grove-fetch-err-XXXXXX";
    char hdr_path[] = "/tmp/grove-fetch-hdr-XXXXXX";
    int err_fd = mkstemp(err_path);
    int hdr_fd = mkstemp(hdr_path);
    int have_err = err_fd >= 0;
    int have_hdr = hdr_fd >= 0;
    int fds[2] = {-1, -1};
    int spawned = 0;
    pid_t pid = 0;
    unsigned char *body = NULL;
    size_t body_n = 0;
    unsigned char *hdr = NULL;
    size_t hdr_n = 0;
    if (err_fd < 0 || hdr_fd < 0 || pipe(fds) != 0) {
        err_set(e, "could not fetch that page");
        goto fail;
    }
    close(err_fd);
    err_fd = -1;
    close(hdr_fd);
    hdr_fd = -1;
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        err_set(e, "could not fetch that page");
        goto fail;
    }
    int act = 0;
    act |= posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    act |= posix_spawn_file_actions_addclose(&fa, fds[0]);
    act |= posix_spawn_file_actions_addclose(&fa, fds[1]);
    act |= posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    char *argv[] = {"curl",
                    "--globoff",
                    "--proto",
                    "=http,https",
                    "--proto-redir",
                    "=http,https",
                    "--fail",
                    "--silent",
                    "--show-error",
                    "--location",
                    "--compressed",
                    "--max-redirs",
                    "5",
                    "--max-time",
                    "60",
                    "--connect-timeout",
                    "15",
                    "--user-agent",
                    "grove/0.1.0-alpha",
                    "-D",
                    hdr_path,
                    "--",
                    norm,
                    NULL};
    int rc = act != 0 ? act : posix_spawnp(&pid, "curl", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        if (rc == ENOENT) {
            err_set(e, "this machine has no curl, so a page cannot be fetched");
        } else {
            err_set(e, "could not fetch that page");
        }
        goto fail;
    }
    spawned = 1;
    close(fds[1]);
    fds[1] = -1;
    int read_rc = fetch_read_cap(fds[0], &body, &body_n, cap, e);
    close(fds[0]);
    fds[0] = -1;
    if (read_rc != 0) {
        kill(pid, SIGKILL);
        wait_pid(pid);
        spawned = 0;
        goto fail;
    }
    if (wait_pid(pid) != 0) {
        spawned = 0;
        set_curl_error(err_path, e);
        goto fail;
    }
    spawned = 0;
    if (body_n == 0) {
        err_set(e, "that page is empty");
        goto fail;
    }
    int hfd = open(hdr_path, O_RDONLY);
    if (hfd < 0) {
        err_set(e, "could not fetch that page");
        goto fail;
    }
    int header_rc = fetch_read_cap(hfd, &hdr, &hdr_n, 65536, e);
    close(hfd);
    if (header_rc != 0) {
        err_set(e, "could not fetch that page");
        goto fail;
    }
    if (headers_are_page(hdr, hdr_n, e) != 0) {
        goto fail;
    }
    if (write_file_atomic(dest, body, body_n, e) != 0) {
        goto fail;
    }
    char *canon = realpath(dest, NULL);
    if (!canon) {
        err_set(e, "cannot save that page");
        goto fail;
    }
    free(body);
    free(hdr);
    free(dest);
    unlink(err_path);
    unlink(hdr_path);
    *saved = canon;
    *url = norm;
    return 0;
fail:
    if (spawned) {
        kill(pid, SIGKILL);
        wait_pid(pid);
    }
    if (fds[0] >= 0) {
        close(fds[0]);
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    if (err_fd >= 0) {
        close(err_fd);
    }
    if (have_err) {
        unlink(err_path);
    }
    if (hdr_fd >= 0) {
        close(hdr_fd);
    }
    if (have_hdr) {
        unlink(hdr_path);
    }
    free(body);
    free(hdr);
    free(dest);
    free(norm);
    return -1;
}

int fetch_save(const char *index_dir, const char *raw, char **saved, char **url, Err *e) {
    return fetch_save_cap(index_dir, raw, (size_t)FETCH_BODY_MAX, saved, url, e);
}

int grove_fetch_into(Index *ix, const char *index_dir, const char *raw, IndexStats *st, Err *e) {
    if (!ix) {
        err_set(e, "no index");
        return -1;
    }
    IndexStats local;
    memset(&local, 0, sizeof local);
    if (!st) {
        st = &local;
    }
    char *saved = NULL;
    char *page = NULL;
    if (fetch_save(index_dir, raw, &saved, &page, e) != 0) {
        return -1;
    }
    uint32_t indexed = st->indexed;
    uint32_t unchanged = st->unchanged;
    int rc = index_add(ix, saved, st, e);
    if (rc == 0 && st->indexed == indexed && st->unchanged == unchanged) {
        err_set(e, "that page had no text");
        rc = -1;
    }
    if (rc == 0) {
        char *canon = realpath(saved, NULL);
        rc = index_page_remember(ix, canon ? canon : saved, page, e);
        free(canon);
    }
    free(saved);
    free(page);
    return rc;
}
