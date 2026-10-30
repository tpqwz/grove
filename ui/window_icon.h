#ifndef GROVE_WINDOW_ICON_H
#define GROVE_WINDOW_ICON_H

#include <stddef.h>

/* Paint Grove's mark onto the browser window WebUI opened.
 * The call returns at once. grove_window_icon_stop waits for it. */
void grove_window_icon_start(size_t window);
void grove_window_icon_stop(void);

#endif
