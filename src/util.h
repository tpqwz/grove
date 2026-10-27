#ifndef GROVE_UTIL_H
#define GROVE_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Buf {
    char *data;
    size_t len;
    size_t cap;
} Buf;

typedef struct Err {
    char msg[768];
} Err;

void err_clear(Err *e);
void err_set(Err *e, const char *fmt, ...);

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
int buf_reserve(Buf *b, size_t need);
int buf_append(Buf *b, const void *p, size_t n);
int buf_puts(Buf *b, const char *s);
int buf_putc(Buf *b, int c);
int buf_printf(Buf *b, const char *fmt, ...);

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

#endif
