#ifndef GROVE_EXTRACT_H
#define GROVE_EXTRACT_H

#include "text.h"
#include "util.h"

typedef enum ExStatus { EX_OK = 0, EX_SKIP = 1, EX_ERR = -1 } ExStatus;

typedef struct Extract {
    char *text;
    char *title;
    char *detail;
    Mark *marks;
    int nmarks;
} Extract;

ExStatus extract_path(const char *path, Extract *out, Err *e);
void extract_free(Extract *x);

#endif
