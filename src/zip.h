#ifndef GROVE_ZIP_H
#define GROVE_ZIP_H

#include "util.h"

#include <stddef.h>

typedef struct ZipFile {
    char *name;
    unsigned char *data;
    size_t len;
} ZipFile;

typedef struct Zip {
    ZipFile *files;
    size_t n;
} Zip;

int zip_read(const unsigned char *bytes, size_t len, Zip *out, Err *e);
void zip_free(Zip *z);
ZipFile *zip_find(Zip *z, const char *name);
int zip_name_ends(const ZipFile *f, const char *suffix);

#endif
