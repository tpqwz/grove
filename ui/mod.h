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

#endif
