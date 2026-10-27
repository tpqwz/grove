#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void err_clear(Err *e) {
    if (e) {
        e->msg[0] = 0;
    }
}

void err_set(Err *e, const char *fmt, ...) {
    if (!e) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
}

void buf_init(Buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_clear(Buf *b) {
    b->len = 0;
    if (b->data) {
        b->data[0] = 0;
    }
}

int buf_reserve(Buf *b, size_t need) {
    if (need <= b->cap) {
        return 0;
    }
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2)) {
            return -1;
        }
        cap *= 2;
    }
    char *p = realloc(b->data, cap);
    if (!p) {
        return -1;
    }
    b->data = p;
    b->cap = cap;
    return 0;
}

int buf_append(Buf *b, const void *p, size_t n) {
    if (buf_reserve(b, b->len + n + 1) != 0) {
        return -1;
    }
    if (n) {
        memcpy(b->data + b->len, p, n);
    }
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

int buf_puts(Buf *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

int buf_putc(Buf *b, int c) {
    char ch = (char)c;
    return buf_append(b, &ch, 1);
}

int buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(aq);
        return -1;
    }
    if (buf_reserve(b, b->len + (size_t)n + 1) != 0) {
        va_end(aq);
        return -1;
    }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, aq);
    va_end(aq);
    b->len += (size_t)n;
    return 0;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *path_join(const char *a, const char *b) {
    size_t na = strlen(a);
    size_t nb = strlen(b);
    while (nb && b[0] == '/') {
        b++;
        nb--;
    }
    int slash = na && a[na - 1] != '/';
    char *s = malloc(na + nb + 2);
    if (!s) {
        return NULL;
    }
    memcpy(s, a, na);
    size_t n = na;
    if (slash) {
        s[n++] = '/';
    }
    memcpy(s + n, b, nb);
    s[n + nb] = 0;
    return s;
}

int mkdir_p(const char *path) {
    char *tmp = xstrdup(path);
    if (!tmp) {
        return -1;
    }
    size_t n = strlen(tmp);
    if (n == 0) {
        free(tmp);
        return -1;
    }
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            free(tmp);
            return -1;
        }
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int write_file_atomic(const char *path, const void *data, size_t n, Err *e) {
    size_t lp = strlen(path);
    char *tp = malloc(lp + 5);
    if (!tp) {
        err_set(e, "out of memory");
        return -1;
    }
    memcpy(tp, path, lp);
    memcpy(tp + lp, ".tmp", 5);
    FILE *f = fopen(tp, "wb");
    if (!f) {
        err_set(e, "cannot write %s: %s", tp, strerror(errno));
        free(tp);
        return -1;
    }
    if (n && fwrite(data, 1, n, f) != n) {
        err_set(e, "short write %s", tp);
        fclose(f);
        unlink(tp);
        free(tp);
        return -1;
    }
    if (fflush(f) != 0) {
        err_set(e, "flush %s: %s", tp, strerror(errno));
        fclose(f);
        unlink(tp);
        free(tp);
        return -1;
    }
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    if (fclose(f) != 0) {
        err_set(e, "close %s: %s", tp, strerror(errno));
        unlink(tp);
        free(tp);
        return -1;
    }
    if (rename(tp, path) != 0) {
        err_set(e, "rename %s: %s", path, strerror(errno));
        unlink(tp);
        free(tp);
        return -1;
    }
    free(tp);
    return 0;
}

int read_entire_file(const char *path, unsigned char **out, size_t *n, size_t cap, Err *e) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        err_set(e, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        err_set(e, "seek %s", path);
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        err_set(e, "tell %s", path);
        fclose(f);
        return -1;
    }
    if ((size_t)sz > cap) {
        err_set(e, "%s is larger than %zu bytes", path, cap);
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        err_set(e, "rewind %s", path);
        fclose(f);
        return -1;
    }
    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        err_set(e, "out of memory");
        fclose(f);
        return -1;
    }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        err_set(e, "short read %s", path);
        free(buf);
        fclose(f);
        return -1;
    }
    buf[sz] = 0;
    fclose(f);
    *out = buf;
    *n = (size_t)sz;
    return 0;
}

void wr_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 255);
    p[1] = (unsigned char)((v >> 8) & 255);
}

void wr_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 255);
    p[1] = (unsigned char)((v >> 8) & 255);
    p[2] = (unsigned char)((v >> 16) & 255);
    p[3] = (unsigned char)((v >> 24) & 255);
}

void wr_u64(unsigned char *p, uint64_t v) {
    wr_u32(p, (uint32_t)v);
    wr_u32(p + 4, (uint32_t)(v >> 32));
}

uint16_t rd_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t rd_u64(const unsigned char *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

size_t varint_put(unsigned char *dst, uint32_t v) {
    size_t n = 0;
    while (v >= 0x80) {
        dst[n++] = (unsigned char)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    dst[n++] = (unsigned char)v;
    return n;
}

int varint_get(const unsigned char *p, const unsigned char *end, uint32_t *v, const unsigned char **next) {
    uint32_t out = 0;
    int shift = 0;
    while (p < end && shift <= 28) {
        unsigned char b = *p++;
        out |= (uint32_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *v = out;
            *next = p;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

int ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb + 32);
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == 0 && *b == 0;
}

void ascii_lower_inplace(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s + 32);
        }
    }
}

int starts_with(const char *s, const char *pfx) {
    size_t n = strlen(pfx);
    return strncmp(s, pfx, n) == 0;
}

size_t utf8_prefix_bytes(const char *s, size_t n, size_t max_bytes) {
    if (n > max_bytes) {
        n = max_bytes;
    }
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) {
        n--;
    }
    return n;
}
