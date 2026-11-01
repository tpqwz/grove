#ifndef GROVE_UTIL_H
#define GROVE_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Buf {
    char *data;
    size_t len;
    size_t cap;
} Buf;

typedef struct Err {
    char msg[768];
} Err;

void err_clear(Err *e);
#if defined(__MINGW32__)
#define GROVE_PRINTF gnu_printf
#else
#define GROVE_PRINTF printf
#endif
void err_set(Err *e, const char *fmt, ...) __attribute__((format(GROVE_PRINTF, 2, 3)));

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
int buf_reserve(Buf *b, size_t need);
int buf_append(Buf *b, const void *p, size_t n);
int buf_puts(Buf *b, const char *s);
int buf_putc(Buf *b, int c);
int buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(GROVE_PRINTF, 2, 3)));

char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

char *path_join(const char *a, const char *b);
int mkdir_p(const char *path);
int write_file_atomic(const char *path, const void *data, size_t n, Err *e);
int read_entire_file(const char *path, unsigned char **out, size_t *n, size_t cap, Err *e);

void wr_u16(unsigned char *p, uint16_t v);
void wr_u32(unsigned char *p, uint32_t v);
void wr_u64(unsigned char *p, uint64_t v);
uint16_t rd_u16(const unsigned char *p);
uint32_t rd_u32(const unsigned char *p);
uint64_t rd_u64(const unsigned char *p);

size_t varint_put(unsigned char *dst, uint32_t v);
int varint_get(const unsigned char *p, const unsigned char *end, uint32_t *v, const unsigned char **next);

int ascii_ieq(const char *a, const char *b);
void ascii_lower_inplace(char *s);
int starts_with(const char *s, const char *pfx);
size_t utf8_prefix_bytes(const char *s, size_t n, size_t max_bytes);

/* Sizes fail instead of wrapping. A failed grow leaves the old pointer in place. */
static inline int size_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) {
        return -1;
    }
    *out = a + b;
    return 0;
}

static inline int size_mul(size_t a, size_t b, size_t *out) {
    if (b != 0 && a > SIZE_MAX / b) {
        return -1;
    }
    *out = a * b;
    return 0;
}

static inline int size_fits(size_t len, size_t off, size_t n) {
    size_t end;
    if (size_add(off, n, &end) != 0) {
        return 0;
    }
    return end <= len;
}

static inline int cap_grow(int cap, int initial, int *out) {
    if (cap < 0 || initial <= 0) {
        return -1;
    }
    if (cap == 0) {
        *out = initial;
        return 0;
    }
    /* Half of INT_MAX. limits.h is not included here: it defines MAX_INPUT. */
    if ((unsigned)cap > (((unsigned)-1) >> 1) / 2) {
        return -1;
    }
    *out = cap * 2;
    return 0;
}

static inline int cap_grow_size(size_t cap, size_t initial, size_t *out) {
    if (cap == 0) {
        *out = initial;
        return 0;
    }
    return size_mul(cap, 2, out);
}

static inline int grow_alloc(void **ptr, size_t count, size_t elem) {
    size_t bytes;
    if (!ptr || count == 0 || elem == 0 || size_mul(count, elem, &bytes) != 0) {
        return -1;
    }
    void *p = realloc(*ptr, bytes);
    if (!p) {
        return -1;
    }
    *ptr = p;
    return 0;
}

#endif
