#ifndef GROVE_SEVEN_H
#define GROVE_SEVEN_H

#include "util.h"

#include <stddef.h>

typedef struct SevenFile {
    char *name;
    unsigned char *data;
    size_t len;
} SevenFile;

typedef struct Seven {
    SevenFile *files;
    size_t n;
} Seven;

int seven_read(const unsigned char *bytes, size_t len, Seven *out, Err *e);
void seven_free(Seven *s);

#endif
