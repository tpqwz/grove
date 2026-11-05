#ifndef GROVE_WINDOW_ICON_H
#define GROVE_WINDOW_ICON_H

#include <stddef.h>

/* Write the PNG WebUI can hand to its own window, and the desktop entry.
 * path receives the PNG path. Returns 0 when that file is in place. */
int grove_window_icon_file(char *path, size_t cap);

/* Paint Grove's mark onto a browser window WebUI opened.
 * log is 1 when the program should say that it did.
 * The call returns at once. grove_window_icon_stop waits for it. */
void grove_window_icon_start(size_t window, int log);
void grove_window_icon_stop(void);

#endif
