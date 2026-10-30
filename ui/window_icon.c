#include "window_icon.h"
#include "util.h"
#include "webui.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* WebUI launches Firefox and can only set a favicon, which this window
 * never shows. The frame reads _NET_WM_ICON. This dock groups by class
 * and then shows the desktop entry's icon, so both are set on the
 * Firefox that is serving this process's port. Firefox writes its own
 * icon at startup, and may write it again, so the window values are
 * put back while the process lives. */

enum { ICON_SCALE = 1000, ICON_SAMPLES = 4, ICON_LONGS = 24652 };

static pthread_t icon_thread;
static int icon_started;
static volatile int icon_stop;
static size_t icon_window;
static int icon_announced;

static int icon_x_error(Display *dpy, XErrorEvent *event) {
    (void)dpy;
    (void)event;
    return 0;
}

static int in_disc(int px, int py, int cx, int cy, int r) {
    long dx = (long)px - cx;
    long dy = (long)py - cy;
    long rr = (long)r * (long)r;
    return dx * dx + dy * dy <= rr;
}

/* Thousandths of the icon side, origin at the top left.
 * Paper disc, a brown rim, then the canopy, one lighter leaf, and the trunk. */
static unsigned icon_sample(int px, int py) {
    if (!in_disc(px, py, 500, 500, 460)) {
        return 0;
    }
    unsigned rgb = 0x00F4F1EAu;
    if (!in_disc(px, py, 500, 500, 400)) {
        rgb = 0x008A4E22u;
    }
    if (in_disc(px, py, 500, 400, 240)) {
        rgb = 0x0035543Cu;
    }
    if (in_disc(px, py, 400, 360, 100)) {
        rgb = 0x004E7052u;
    }
    if (px >= 440 && px < 560 && py >= 500 && py < 800) {
        rgb = 0x008A4E22u;
    }
    return 0xFF000000u | rgb;
}

static int sample_pos(int side, int pixel, int sample) {
    int num = (2 * (pixel * ICON_SAMPLES + sample) + 1) * ICON_SCALE;
    int den = 2 * side * ICON_SAMPLES;
    if (den <= 0) {
        return 0;
    }
    return num / den;
}

static unsigned pixel_at(int side, int x, int y) {
    unsigned sum_r = 0;
    unsigned sum_g = 0;
    unsigned sum_b = 0;
    int opaque = 0;
    for (int sy = 0; sy < ICON_SAMPLES; sy++) {
        int py = sample_pos(side, y, sy);
        for (int sx = 0; sx < ICON_SAMPLES; sx++) {
            unsigned sample = icon_sample(sample_pos(side, x, sx), py);
            if (sample == 0) {
                continue;
            }
            opaque++;
            sum_r += (sample >> 16) & 255u;
            sum_g += (sample >> 8) & 255u;
            sum_b += sample & 255u;
        }
    }
    int area = ICON_SAMPLES * ICON_SAMPLES;
    if (opaque <= 0 || area <= 0) {
        return 0;
    }
    unsigned a = (unsigned)(opaque * 255 / area);
    unsigned r = sum_r / (unsigned)opaque;
    unsigned g = sum_g / (unsigned)opaque;
    unsigned b = sum_b / (unsigned)opaque;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static int paint_icons(long *dst, int *out_n) {
    static const int sides[] = {16, 24, 32, 48, 64, 128};
    size_t at = 0;
    size_t nside = sizeof sides / sizeof sides[0];
    for (size_t i = 0; i < nside; i++) {
        int side = sides[i];
        size_t pix = (size_t)side * (size_t)side;
        if (side < 1 || at + 2 < at || at + 2 + pix < at + 2 || at + 2 + pix > ICON_LONGS) {
            return -1;
        }
        dst[at++] = side;
        dst[at++] = side;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                dst[at++] = (long)pixel_at(side, x, y);
            }
        }
    }
    if (at != ICON_LONGS || at > 2147483647u) {
        return -1;
    }
    *out_n = (int)at;
    return 0;
}

static int cmdline_has(pid_t pid, const char *token) {
    char path[64];
    int nw = snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    if (nw < 0 || (size_t)nw >= sizeof path || pid <= 0) {
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    char buf[4096];
    ssize_t got = read(fd, buf, sizeof buf);
    close(fd);
    if (got <= 0) {
        return 0;
    }
    size_t len = (size_t)got;
    size_t tlen = strlen(token);
    if (tlen == 0 || tlen > len) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0') {
            buf[i] = ' ';
        }
    }
    for (size_t i = 0; i + tlen <= len; i++) {
        char next;
        if (memcmp(buf + i, token, tlen) != 0) {
            continue;
        }
        if (i + tlen == len) {
            return 1;
        }
        next = buf[i + tlen];
        if (next < '0' || next > '9') {
            return 1;
        }
    }
    return 0;
}

static pid_t parent_of(pid_t pid) {
    char path[64];
    FILE *f;
    char line[256];
    pid_t ppid = 0;
    int nw = snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    if (nw < 0 || (size_t)nw >= sizeof path || pid <= 0) {
        return 0;
    }
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        char *end = NULL;
        long v;
        if (strncmp(line, "PPid:", 5) != 0) {
            continue;
        }
        v = strtol(line + 5, &end, 10);
        if (end != line + 5 && v > 0 && v <= 2147483647L) {
            ppid = (pid_t)v;
        }
        break;
    }
    fclose(f);
    return ppid;
}

static int process_owns_port(pid_t pid, const char *token) {
    int depth;
    for (depth = 0; depth < 8 && pid > 1; depth++) {
        pid_t next;
        if (cmdline_has(pid, token)) {
            return 1;
        }
        next = parent_of(pid);
        if (next <= 0 || next == pid) {
            break;
        }
        pid = next;
    }
    return 0;
}

static int title_is_grove(const char *s) {
    static const char *prefix[] = {"Grove — ", "Grove – ", "Grove - "};
    size_t i;
    if (strcmp(s, "Grove") == 0) {
        return 1;
    }
    for (i = 0; i < sizeof prefix / sizeof prefix[0]; i++) {
        size_t n = strlen(prefix[i]);
        if (strncmp(s, prefix[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int window_title(Display *dpy, Window w, char *out, size_t cap) {
    Atom utf8;
    Atom prop;
    Atom actual = None;
    int format = 0;
    unsigned long n = 0;
    unsigned long left = 0;
    unsigned char *data = NULL;
    int ok = 0;
    if (cap < 2) {
        return 0;
    }
    utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    prop = XInternAtom(dpy, "_NET_WM_NAME", False);
    if (utf8 == None || prop == None) {
        return 0;
    }
    if (XGetWindowProperty(dpy, w, prop, 0, 128, False, utf8, &actual, &format, &n, &left, &data) == Success &&
        data && format == 8 && n > 0) {
        size_t take = n;
        if (take >= cap) {
            take = cap - 1;
        }
        memcpy(out, data, take);
        out[take] = '\0';
        ok = title_is_grove(out);
    }
    if (data) {
        XFree(data);
    }
    return ok;
}

static pid_t window_pid(Display *dpy, Window w) {
    Atom prop = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom actual = None;
    int format = 0;
    unsigned long n = 0;
    unsigned long left = 0;
    unsigned char *data = NULL;
    pid_t pid = 0;
    if (prop == None) {
        return 0;
    }
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_CARDINAL, &actual, &format, &n, &left, &data) == Success &&
        data && format == 32 && n >= 1) {
        long v = *(long *)data;
        if (v > 0 && v <= 2147483647L) {
            pid = (pid_t)v;
        }
    }
    if (data) {
        XFree(data);
    }
    return pid;
}

static int icon_is_ours(Display *dpy, Window w, Atom icon, const long *data, int nlong) {
    Atom actual = None;
    int format = 0;
    unsigned long n = 0;
    unsigned long left = 0;
    unsigned char *got = NULL;
    int same = 0;
    if (XGetWindowProperty(dpy, w, icon, 0, nlong, False, XA_CARDINAL, &actual, &format, &n, &left, &got) == Success &&
        got && format == 32 && n == (unsigned long)nlong && left == 0) {
        const long *have = (const long *)got;
        int i;
        same = 1;
        for (i = 0; i < nlong; i++) {
            if ((have[i] & 0xffffffffL) != (data[i] & 0xffffffffL)) {
                same = 0;
                break;
            }
        }
    }
    if (got) {
        XFree(got);
    }
    return same;
}

static int class_is_ours(Display *dpy, Window w) {
    XClassHint hint;
    int same;
    memset(&hint, 0, sizeof hint);
    if (XGetClassHint(dpy, w, &hint) == 0) {
        return 0;
    }
    same = hint.res_name && hint.res_class && strcmp(hint.res_name, "grove-ui") == 0 &&
           strcmp(hint.res_class, "Grove-ui") == 0;
    if (hint.res_name) {
        XFree(hint.res_name);
    }
    if (hint.res_class) {
        XFree(hint.res_class);
    }
    return same;
}

static void set_class(Display *dpy, Window w) {
    char name[] = "grove-ui";
    char klass[] = "Grove-ui";
    XClassHint hint;
    hint.res_name = name;
    hint.res_class = klass;
    XSetClassHint(dpy, w, &hint);
}

static void apply_once(Display *dpy, const char *token, const long *data, int nlong) {
    Atom clients = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    Atom actual = None;
    int format = 0;
    unsigned long n = 0;
    unsigned long left = 0;
    unsigned char *raw = NULL;
    const long *ids;
    unsigned long i;
    if (clients == None || icon == None) {
        return;
    }
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), clients, 0, 4096, False, XA_WINDOW, &actual, &format, &n,
                           &left, &raw) != Success ||
        !raw || format != 32) {
        if (raw) {
            XFree(raw);
        }
        return;
    }
    ids = (const long *)raw;
    for (i = 0; i < n; i++) {
        Window w = (Window)ids[i];
        XWindowAttributes attr;
        pid_t pid;
        char title[256];
        int class_ok;
        int icon_ok;
        memset(&attr, 0, sizeof attr);
        if (XGetWindowAttributes(dpy, w, &attr) == 0) {
            continue;
        }
        if (attr.width < 64 || attr.height < 64) {
            continue;
        }
        pid = window_pid(dpy, w);
        if (pid <= 0 || !process_owns_port(pid, token)) {
            continue;
        }
        if (!window_title(dpy, w, title, sizeof title)) {
            continue;
        }
        class_ok = class_is_ours(dpy, w);
        icon_ok = icon_is_ours(dpy, w, icon, data, nlong);
        if (!class_ok) {
            set_class(dpy, w);
        }
        if (!icon_ok) {
            XChangeProperty(dpy, w, icon, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)data, nlong);
        }
        if (!icon_announced && (!class_ok || !icon_ok)) {
            fprintf(stderr, "grove-ui: the window is using Grove's icon\n");
            fflush(stderr);
            icon_announced = 1;
        }
    }
    XFree(raw);
    XFlush(dpy);
}

static int be32(unsigned char *dst, size_t cap, size_t *at, uint32_t v) {
    if (*at + 4 > cap) {
        return -1;
    }
    dst[(*at)++] = (unsigned char)(v >> 24);
    dst[(*at)++] = (unsigned char)(v >> 16);
    dst[(*at)++] = (unsigned char)(v >> 8);
    dst[(*at)++] = (unsigned char)v;
    return 0;
}

static int png_chunk(unsigned char *dst, size_t cap, size_t *at, const char type[4], const unsigned char *data,
                     size_t n) {
    size_t start;
    unsigned long sum;
    if (n > 0x7fffffffu || be32(dst, cap, at, (uint32_t)n) != 0) {
        return -1;
    }
    start = *at;
    if (*at + 4 > cap) {
        return -1;
    }
    memcpy(dst + *at, type, 4);
    *at += 4;
    if (n > 0) {
        if (*at + n > cap) {
            return -1;
        }
        memcpy(dst + *at, data, n);
        *at += n;
    }
    sum = crc32(0L, Z_NULL, 0);
    sum = crc32(sum, dst + start, (uInt)(*at - start));
    return be32(dst, cap, at, (uint32_t)sum);
}

/* One RGBA PNG. The dock loads this file from the desktop entry. */
static int encode_png(int side, unsigned char **out, size_t *out_n) {
    size_t stride = 0;
    size_t raw_n = 0;
    size_t cap;
    size_t at = 0;
    unsigned char *raw = NULL;
    unsigned char *zbuf = NULL;
    unsigned char *png = NULL;
    uLongf zlen;
    int y;
    int rc = -1;
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13];
    size_t ihdr_n = 0;
    if (side < 1 || size_mul((size_t)side, 4, &stride) != 0 || size_add(stride, 1, &stride) != 0 ||
        size_mul(stride, (size_t)side, &raw_n) != 0) {
        return -1;
    }
    raw = malloc(raw_n);
    if (!raw) {
        return -1;
    }
    for (y = 0; y < side; y++) {
        unsigned char *row = raw + (size_t)y * stride;
        int x;
        row[0] = 0;
        for (x = 0; x < side; x++) {
            unsigned p = pixel_at(side, x, y);
            unsigned char *px = row + 1 + (size_t)x * 4;
            px[0] = (unsigned char)((p >> 16) & 255u);
            px[1] = (unsigned char)((p >> 8) & 255u);
            px[2] = (unsigned char)(p & 255u);
            px[3] = (unsigned char)((p >> 24) & 255u);
        }
    }
    zlen = compressBound((uLong)raw_n);
    zbuf = malloc(zlen);
    if (!zbuf || compress(zbuf, &zlen, raw, (uLong)raw_n) != Z_OK) {
        goto done;
    }
    if (size_add((size_t)zlen, 64, &cap) != 0) {
        goto done;
    }
    png = malloc(cap);
    if (!png || cap < sizeof sig) {
        goto done;
    }
    memcpy(png, sig, sizeof sig);
    at = sizeof sig;
    if (be32(ihdr, sizeof ihdr, &ihdr_n, (uint32_t)side) != 0 || be32(ihdr, sizeof ihdr, &ihdr_n, (uint32_t)side) != 0) {
        goto done;
    }
    ihdr[ihdr_n++] = 8;
    ihdr[ihdr_n++] = 6;
    ihdr[ihdr_n++] = 0;
    ihdr[ihdr_n++] = 0;
    ihdr[ihdr_n++] = 0;
    if (png_chunk(png, cap, &at, "IHDR", ihdr, ihdr_n) != 0 || png_chunk(png, cap, &at, "IDAT", zbuf, (size_t)zlen) != 0 ||
        png_chunk(png, cap, &at, "IEND", NULL, 0) != 0) {
        goto done;
    }
    *out = png;
    *out_n = at;
    png = NULL;
    rc = 0;
done:
    free(raw);
    free(zbuf);
    free(png);
    return rc;
}

static int same_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    size_t got;
    int same;
    if (!f) {
        return 0;
    }
    buf = malloc(n + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    got = fread(buf, 1, n + 1, f);
    fclose(f);
    same = got == n && memcmp(buf, data, n) == 0;
    free(buf);
    return same;
}

static int write_if_new(const char *path, const void *data, size_t n) {
    Err e;
    if (same_bytes(path, data, n)) {
        return 0;
    }
    err_clear(&e);
    if (write_file_atomic(path, data, n, &e) != 0) {
        fprintf(stderr, "grove-ui: %s\n", e.msg[0] ? e.msg : "could not write the icon");
        fflush(stderr);
        return -1;
    }
    return 1;
}

/* The dock matches StartupWMClass and shows Icon. It does not use the
 * window pixmap for the button. */
static void publish_launcher(void) {
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char *root = NULL;
    char *icon_dir = NULL;
    char *app_dir = NULL;
    char *icon_path = NULL;
    char *desk_path = NULL;
    char exe[PATH_MAX];
    char text[PATH_MAX * 2 + 256];
    unsigned char *png = NULL;
    size_t png_n = 0;
    ssize_t en;
    int nw;
    const char *gone = " (deleted)";
    size_t gone_n = strlen(gone);
    if (xdg && xdg[0]) {
        root = xstrdup(xdg);
    } else if (home && home[0]) {
        root = path_join(home, ".local/share");
    }
    if (!root) {
        return;
    }
    en = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (en <= 0 || (size_t)en >= sizeof exe) {
        free(root);
        return;
    }
    exe[en] = '\0';
    if ((size_t)en > gone_n && memcmp(exe + en - (ssize_t)gone_n, gone, gone_n) == 0) {
        exe[en - (ssize_t)gone_n] = '\0';
    }
    if (strchr(exe, '\n') || strchr(exe, '\r')) {
        free(root);
        return;
    }
    icon_dir = path_join(root, "icons");
    app_dir = path_join(root, "applications");
    icon_path = icon_dir ? path_join(icon_dir, "grove-ui.png") : NULL;
    desk_path = app_dir ? path_join(app_dir, "grove-ui.desktop") : NULL;
    if (!icon_dir || !app_dir || !icon_path || !desk_path || mkdir_p(icon_dir) != 0 || mkdir_p(app_dir) != 0) {
        goto done;
    }
    if (encode_png(128, &png, &png_n) != 0) {
        goto done;
    }
    nw = snprintf(text, sizeof text,
                  "[Desktop Entry]\n"
                  "# Written by grove-ui.\n"
                  "Type=Application\n"
                  "Name=Grove\n"
                  "Exec=%s\n"
                  "Icon=%s\n"
                  "StartupWMClass=Grove-ui\n"
                  "Terminal=false\n",
                  exe, icon_path);
    if (nw < 0 || (size_t)nw >= sizeof text) {
        goto done;
    }
    if (write_if_new(icon_path, png, png_n) < 0) {
        goto done;
    }
    write_if_new(desk_path, text, (size_t)nw);
done:
    free(png);
    free(desk_path);
    free(icon_path);
    free(app_dir);
    free(icon_dir);
    free(root);
}

static void *icon_main(void *arg) {
    Display *dpy;
    long data[ICON_LONGS];
    int nlong = 0;
    (void)arg;
    publish_launcher();
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        return NULL;
    }
    XSetErrorHandler(icon_x_error);
    if (paint_icons(data, &nlong) != 0) {
        XCloseDisplay(dpy);
        return NULL;
    }
    while (!icon_stop) {
        size_t port = webui_get_port(icon_window);
        int i;
        if (port >= 1 && port <= 65535) {
            char token[32];
            int nw = snprintf(token, sizeof token, "localhost:%zu", port);
            if (nw > 0 && (size_t)nw < sizeof token) {
                apply_once(dpy, token, data, nlong);
            }
        }
        for (i = 0; i < 5 && !icon_stop; i++) {
            usleep(100000);
        }
    }
    XCloseDisplay(dpy);
    return NULL;
}

void grove_window_icon_start(size_t window) {
    if (icon_started) {
        return;
    }
    icon_window = window;
    icon_stop = 0;
    icon_announced = 0;
    if (pthread_create(&icon_thread, NULL, icon_main, NULL) == 0) {
        icon_started = 1;
    }
}

void grove_window_icon_stop(void) {
    if (!icon_started) {
        return;
    }
    icon_stop = 1;
    pthread_join(icon_thread, NULL);
    icon_started = 0;
}
