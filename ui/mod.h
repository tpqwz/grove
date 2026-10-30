#ifndef GROVE_MOD_H
#define GROVE_MOD_H

#include "index.h"

#include <stddef.h>

/* The host owns the index and the window. The module is the program.
 * A new build replaces the module; the host stays. */
typedef struct GroveUi {
    Index *ix;
    const char *index_dir;
    const char *open_err;
    size_t window;
    const char *page_path;
    const char *page_root;
    const char *page_name;
    const char *smoke;
} GroveUi;

int grove_ui_attach(const GroveUi *ui);
void grove_ui_detach(void);

/* Background work stays in the host, so a rebuilt module can unload while
 * an import is still reading files. kind 0 is idle, 1 prepares the shelf
 * list, 2 adds a folder or a file. */
typedef struct GroveJob {
    int running;
    int kind;
    int ok;
    uint32_t indexed;
    uint32_t unchanged;
    uint32_t removed;
    uint32_t skipped;
    char error[768];
} GroveJob;

int grove_host_prepare(const char *index_dir);
int grove_host_import(const char *index_dir, const char *path);
void grove_host_job(GroveJob *out);

#endif
