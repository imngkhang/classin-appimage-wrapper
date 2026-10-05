#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shm.h>

static FILE *logf = NULL;
static int share_capture_active = 0;
static int logged_xcb_capture = 0;

static void plog(const char *fmt, ...) {
    if (!logf) {
        const char *p = getenv("CLASSIN_PROXY_LOG");
        if (!p) return;
        logf = fopen(p, "a");
        if (!logf) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fflush(logf);
}

static void touch(const char *p) {
    if (!p) return;
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
}

static uint64_t monotonic_ms(void);
static int last_region[4] = {0, 0, 0, 0};
static int have_region = 0;
static uint64_t last_heartbeat_ms = 0;

static void report(int x, int y, int w, int h) {
    const char *rf = getenv("CLASSIN_PROXY_REGION");
    if (rf && (!have_region || last_region[0] != x || last_region[1] != y ||
               last_region[2] != w || last_region[3] != h)) {
        FILE *f = fopen(rf, "w");
        if (f) {
            fprintf(f, "%d %d %d %d", x, y, w, h);
            fclose(f);
        }
        last_region[0] = x; last_region[1] = y;
        last_region[2] = w; last_region[3] = h;
        have_region = 1;
    }
    uint64_t now = monotonic_ms();
    if (now - last_heartbeat_ms >= 1000) {
        touch(getenv("CLASSIN_PROXY_HEARTBEAT"));
        last_heartbeat_ms = now;
    }
}

static void report_debug(const char *msg) {
    plog("%s\n", msg);
}

static void signal_request(void) {
    if (!share_capture_active) return;
    touch(getenv("CLASSIN_PROXY_REQUEST"));
}

static void signal_xcb_capture(void) {
    if (!share_capture_active) {
        share_capture_active = 1;
        if (!logged_xcb_capture) {
            plog("XCB root capture fallback active\n");
            logged_xcb_capture = 1;
        }
    }
    signal_request();
}

typedef void (*screen_capture_method)(void *);
static screen_capture_method real_screen_capture_start = NULL;
static screen_capture_method real_screen_capture_stop = NULL;
typedef void (*screen_grab_method)(void *, void *);
static screen_grab_method real_screen_grab = NULL;
static int logged_share_hook = 0;

void _ZN18ScreenShareCapture5startEv(void *self) {
    if (!real_screen_capture_start)
        real_screen_capture_start = (screen_capture_method)dlsym(RTLD_NEXT, "_ZN18ScreenShareCapture5startEv");
    share_capture_active = 1;
    if (!logged_share_hook) {
        plog("ScreenShareCapture::start hook active\n");
        logged_share_hook = 1;
    }
    signal_request();
    if (real_screen_capture_start) real_screen_capture_start(self);
}

void _ZN18ScreenShareCapture10grabScreenER6QImage(void *self, void *image) {
    if (!real_screen_grab)
        real_screen_grab = (screen_grab_method)dlsym(RTLD_NEXT, "_ZN18ScreenShareCapture10grabScreenER6QImage");
    share_capture_active = 1;
    if (!logged_share_hook) {
        plog("ScreenShareCapture::grabScreen hook active\n");
        logged_share_hook = 1;
    }
    signal_request();
    if (real_screen_grab) real_screen_grab(self, image);
}

void _ZN18ScreenShareCapture4stopEv(void *self) {
    if (!real_screen_capture_stop)
        real_screen_capture_stop = (screen_capture_method)dlsym(RTLD_NEXT, "_ZN18ScreenShareCapture4stopEv");
    share_capture_active = 0;
    touch(getenv("CLASSIN_PROXY_STOP"));
    if (real_screen_capture_stop) real_screen_capture_stop(self);
}

static Display *src_dpy = NULL;
static xcb_connection_t *src_c = NULL;
static xcb_window_t src_root = 0;
static Window src_capture = 0;
static xcb_window_t src_capture_c = 0;
static uint16_t src_capture_w = 0, src_capture_h = 0;
static uint64_t last_capture_refresh_ms = 0;

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void reset_src_c(void) {
    if (src_c) xcb_disconnect(src_c);
    src_c = NULL;
    src_root = 0;
    src_capture_c = 0;
    src_capture_w = src_capture_h = 0;
    last_capture_refresh_ms = 0;
}

static void refresh_src_capture_c(void) {
    if (!src_c || !src_root || (src_capture_c && src_capture_c != src_root))
        return;
    uint64_t now = monotonic_ms();
    if (now - last_capture_refresh_ms < 500)
        return;
    last_capture_refresh_ms = now;
    xcb_query_tree_cookie_t tc = xcb_query_tree(src_c, src_root);
    xcb_query_tree_reply_t *tr = xcb_query_tree_reply(src_c, tc, NULL);
    if (!tr) return;
    int len = xcb_query_tree_children_length(tr);
    xcb_window_t *wins = xcb_query_tree_children(tr);
    uint64_t best_area = 0;
    for (int i = 0; i < len; i++) {
        xcb_get_window_attributes_cookie_t ac = xcb_get_window_attributes(src_c, wins[i]);
        xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(src_c, ac, NULL);
        if (!a || a->map_state != XCB_MAP_STATE_VIEWABLE) {
            free(a);
            continue;
        }
        free(a);
        xcb_get_geometry_cookie_t gc = xcb_get_geometry(src_c, wins[i]);
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(src_c, gc, NULL);
        if (g) {
            uint64_t area = (uint64_t)g->width * (uint64_t)g->height;
            if (area > best_area) {
                src_capture_c = wins[i];
                src_capture_w = g->width;
                src_capture_h = g->height;
                best_area = area;
            }
            free(g);
        }
    }
    free(tr);
}

static void ensure_capture_c_size(uint16_t w, uint16_t h) {
    if (!src_c || !src_capture_c || src_capture_c == src_root) return;
    if (src_capture_w < w || src_capture_h < h) {
        plog("capture window 0x%x too small: %ux%u for %ux%u; rescanning\n",
             src_capture_c, src_capture_w, src_capture_h, w, h);
        src_capture_c = src_root;
        src_capture_w = src_capture_h = 0;
        last_capture_refresh_ms = 0;
        refresh_src_capture_c();
    }
}

static void map_capture_rect_c(int16_t *x, int16_t *y, uint16_t w, uint16_t h) {
    if (!src_c || !src_capture_c) return;
    if (*x < 0 || *y < 0 || (uint32_t)(uint16_t)*x + w > src_capture_w ||
        (uint32_t)(uint16_t)*y + h > src_capture_h) {
        plog("mapping XCB request %d,%d %ux%u from source %ux%u to 0,0\n",
             *x, *y, w, h, src_capture_w, src_capture_h);
        *x = 0;
        *y = 0;
    }
}

/* ximagesink renders into a child window, not the Xvfb root. Pick the
 * largest mapped child so root captures see the actual video frame. */
static Window find_capture_window(Display *dpy, Window root) {
    Window parent, *children = NULL;
    unsigned int n = 0;
    Window best = root;
    unsigned long best_area = 0;
    if (!XQueryTree(dpy, root, &root, &parent, &children, &n))
        return root;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy, children[i], &a) || a.map_state != IsViewable)
            continue;
        unsigned long area = (unsigned long)a.width * (unsigned long)a.height;
        if (area > best_area) {
            best = children[i];
            best_area = area;
        }
    }
    if (children) XFree(children);
    return best;
}

static Drawable current_capture_window(void) {
    Window root = DefaultRootWindow(src_dpy);
    static uint64_t last_refresh_ms = 0;
    uint64_t now = monotonic_ms();
    if ((src_capture == 0 || src_capture == root) && now - last_refresh_ms >= 500) {
        src_capture = find_capture_window(src_dpy, root);
        last_refresh_ms = now;
    }
    return (Drawable)(src_capture ? src_capture : root);
}

static void map_capture_rect(int *x, int *y, unsigned int w, unsigned int h) {
    Window root;
    int rx, ry;
    unsigned int sw, sh, bw, depth;
    Drawable capture = current_capture_window();
    if (!XGetGeometry(src_dpy, capture, &root, &rx, &ry, &sw, &sh, &bw, &depth))
        return;
    if (*x < 0 || *y < 0 || (unsigned int)*x + w > sw || (unsigned int)*y + h > sh) {
        plog("mapping request %d,%d %ux%u from source %ux%u to 0,0\n",
             *x, *y, w, h, sw, sh);
        *x = 0;
        *y = 0;
    }
}

static int (*orig_io_handler)(Display *) = NULL;

static int src_io_handler(Display *d) {
    if (d == src_dpy) {
        plog("src connection broken, dropping cache\n");
        src_dpy = NULL;
        src_capture = 0;
        return 0;
    }
    if (orig_io_handler) return orig_io_handler(d);
    return 1;
}

static Display *ensure_src_dpy(void) {
    if (!src_dpy) {
        const char *n = getenv("CLASSIN_PROXY_SRC");
        if (!n) n = ":99";
        src_dpy = XOpenDisplay(n);
        if (src_dpy) {
            src_capture = find_capture_window(src_dpy, DefaultRootWindow(src_dpy));
            plog("source display %s capture window 0x%lx\n", n,
                 (unsigned long)src_capture);
            orig_io_handler = XSetIOErrorHandler(src_io_handler);
        } else {
            signal_request();
        }
    }
    return src_dpy;
}

static xcb_connection_t *ensure_src_c(void) {
    if (src_c && xcb_connection_has_error(src_c))
        reset_src_c();
    if (!src_c) {
        const char *n = getenv("CLASSIN_PROXY_SRC");
        if (!n) n = ":99";
        int scr = 0;
        src_c = xcb_connect(n, &scr);
        if (src_c && !xcb_connection_has_error(src_c)) {
            xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(src_c));
            if (it.rem) {
                src_root = it.data->root;
                src_capture_c = src_root;
                xcb_query_tree_cookie_t tc = xcb_query_tree(src_c, src_root);
                xcb_query_tree_reply_t *tr = xcb_query_tree_reply(src_c, tc, NULL);
                if (tr) {
                    int len = xcb_query_tree_children_length(tr);
                    xcb_window_t *wins = xcb_query_tree_children(tr);
                    uint64_t best_area = 0;
                    for (int i = 0; i < len; i++) {
                        xcb_get_geometry_cookie_t gc = xcb_get_geometry(src_c, wins[i]);
                        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(src_c, gc, NULL);
                        if (g) {
                            uint64_t area = (uint64_t)g->width * (uint64_t)g->height;
                            if (area > best_area) {
                                src_capture_c = wins[i];
                                src_capture_w = g->width;
                                src_capture_h = g->height;
                                best_area = area;
                            }
                            free(g);
                        }
                    }
                    free(tr);
                }
                plog("source XCB capture window 0x%x\n", src_capture_c);
            }
        } else {
            reset_src_c();
            signal_request();
        }
    }
    return src_c;
}

typedef XImage *(*XGetImage_fn)(Display *, Drawable, int, int, unsigned int,
                                unsigned int, unsigned long, int);
typedef Bool (*XShmGetImage_fn)(Display *, Drawable, XImage *, int, int,
                                unsigned long);

static XGetImage_fn real_XGetImage = NULL;
static XShmGetImage_fn real_XShmGetImage = NULL;

static int is_root(Display *dpy, Drawable d) {
    return d == (Drawable)DefaultRootWindow(dpy);
}

XImage *XGetImage(Display *dpy, Drawable d, int x, int y, unsigned int w,
                  unsigned int h, unsigned long plane_mask, int format) {
    if (!real_XGetImage)
        real_XGetImage = (XGetImage_fn)dlsym(RTLD_NEXT, "XGetImage");
    if (dpy != src_dpy && is_root(dpy, d) && ensure_src_dpy()) {
        map_capture_rect(&x, &y, w, h);
        XImage *im = real_XGetImage(src_dpy, current_capture_window(),
                                    x, y, w, h, plane_mask, format);
        if (im) {
            report(x, y, (int)w, (int)h);
            return im;
        }
    }
    return real_XGetImage(dpy, d, x, y, w, h, plane_mask, format);
}

Bool XShmGetImage(Display *dpy, Drawable d, XImage *image, int x, int y,
                  unsigned long plane_mask) {
    if (!real_XShmGetImage)
        real_XShmGetImage = (XShmGetImage_fn)dlsym(RTLD_NEXT, "XShmGetImage");
    if (!real_XGetImage)
        real_XGetImage = (XGetImage_fn)dlsym(RTLD_NEXT, "XGetImage");
    if (dpy != src_dpy && is_root(dpy, d) && image && ensure_src_dpy()) {
        map_capture_rect(&x, &y, image->width, image->height);
        XImage *im = real_XGetImage(src_dpy, current_capture_window(),
                                    x, y, image->width, image->height,
                                    plane_mask, ZPixmap);
        if (im) {
            size_t copy = im->bytes_per_line < image->bytes_per_line
                              ? im->bytes_per_line
                              : image->bytes_per_line;
            for (unsigned int r = 0; r < (unsigned int)im->height; r++)
                memcpy(image->data + (size_t)r * image->bytes_per_line,
                       im->data + (size_t)r * im->bytes_per_line, copy);
            XDestroyImage(im);
            report(x, y, image->width, image->height);
            return True;
        }
    }
    return real_XShmGetImage(dpy, d, image, x, y, plane_mask);
}

typedef xcb_get_image_cookie_t (*xcb_get_image_fn)(xcb_connection_t *, uint8_t,
                                                   xcb_drawable_t, int16_t,
                                                   int16_t, uint16_t, uint16_t,
                                                   uint32_t);
typedef xcb_get_image_reply_t *(*xcb_get_image_reply_fn)(
    xcb_connection_t *, xcb_get_image_cookie_t, xcb_generic_error_t **);

static xcb_get_image_fn real_xcb_get_image = NULL;
static xcb_get_image_reply_fn real_xcb_get_image_reply = NULL;

#define MAP_SIZE 64
static struct {
    uint64_t seq;
    int valid;
    int16_t x, y;
    uint16_t w, h;
} gi_map[MAP_SIZE];

static struct {
    uint64_t seq;
    int valid;
    int16_t x, y;
    uint16_t w, h;
    uint32_t shmid;
    uint32_t offset;
} shm_map[MAP_SIZE];

static struct {
    xcb_connection_t *c;
    uint32_t seg;
    uint32_t shmid;
} attach_map[16];

static xcb_window_t conn_root(xcb_connection_t *c) {
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    return it.rem ? it.data->root : 0;
}

xcb_get_image_cookie_t xcb_get_image(xcb_connection_t *c, uint8_t format,
                                     xcb_drawable_t drawable, int16_t x,
                                     int16_t y, uint16_t width, uint16_t height,
                                     uint32_t plane_mask) {
    if (!real_xcb_get_image)
        real_xcb_get_image = (xcb_get_image_fn)dlsym(RTLD_NEXT, "xcb_get_image");
    xcb_get_image_cookie_t cookie =
        real_xcb_get_image(c, format, drawable, x, y, width, height, plane_mask);
    if (c != src_c && drawable == conn_root(c)) {
        signal_xcb_capture();
    }
    if (c != src_c && drawable == conn_root(c) && ensure_src_c()) {
        refresh_src_capture_c();
        ensure_capture_c_size(width, height);
        unsigned int slot = cookie.sequence % MAP_SIZE;
        for (unsigned int i = 0; i < MAP_SIZE; i++) {
            unsigned int s = (slot + i) % MAP_SIZE;
            if (!gi_map[s].valid) {
                gi_map[s].valid = 1;
                gi_map[s].seq = cookie.sequence;
                gi_map[s].x = x;
                gi_map[s].y = y;
                gi_map[s].w = width;
                gi_map[s].h = height;
                break;
            }
        }
    }
    return cookie;
}

xcb_get_image_reply_t *xcb_get_image_reply(xcb_connection_t *c,
                                           xcb_get_image_cookie_t cookie,
                                           xcb_generic_error_t **e) {
    if (!real_xcb_get_image_reply)
        real_xcb_get_image_reply =
            (xcb_get_image_reply_fn)dlsym(RTLD_NEXT, "xcb_get_image_reply");
    unsigned int slot = cookie.sequence % MAP_SIZE;
    for (unsigned int i = 0; i < MAP_SIZE; i++) {
        unsigned int s = (slot + i) % MAP_SIZE;
        if (gi_map[s].valid && gi_map[s].seq == cookie.sequence) {
            gi_map[s].valid = 0;
            xcb_generic_error_t *lerr = NULL;
            xcb_get_image_reply_t *rr = real_xcb_get_image_reply(c, cookie, &lerr);
            free(rr);
            int16_t sx = gi_map[s].x, sy = gi_map[s].y;
            map_capture_rect_c(&sx, &sy, gi_map[s].w, gi_map[s].h);
            uint16_t sw = gi_map[s].w < src_capture_w ? gi_map[s].w : src_capture_w;
            uint16_t sh = gi_map[s].h < src_capture_h ? gi_map[s].h : src_capture_h;
            xcb_get_image_cookie_t c2 =
                real_xcb_get_image(src_c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                   src_capture_c ? src_capture_c : src_root,
                                   sx, sy, sw, sh, ~0);
            xcb_get_image_reply_t *r2 = real_xcb_get_image_reply(src_c, c2, NULL);
            if (r2) {
                uint8_t *data = (uint8_t *)(r2 + 1);
                size_t len = (size_t)r2->length * 4;
                xcb_get_image_reply_t *out =
                    malloc(sizeof(xcb_get_image_reply_t) + len);
                memset(out, 0, sizeof(xcb_get_image_reply_t));
                out->response_type = 1;
                out->depth = r2->depth;
                out->sequence = (uint16_t)cookie.sequence;
                out->visual = r2->visual;
                out->length = r2->length;
                memcpy(out + 1, data, len);
                free(r2);
                report(gi_map[s].x, gi_map[s].y, gi_map[s].w, gi_map[s].h);
                return out;
            }
            report_debug("xcb source get_image returned no reply");
            return NULL;
        }
    }
    return real_xcb_get_image_reply(c, cookie, e);
}

static uint32_t find_shmid(xcb_connection_t *c, uint32_t seg) {
    for (unsigned int i = 0; i < 16; i++) {
        if (attach_map[i].c == c && attach_map[i].seg == seg)
            return attach_map[i].shmid;
    }
    return (uint32_t)-1;
}

static void set_shmid(xcb_connection_t *c, uint32_t seg, uint32_t shmid) {
    for (unsigned int i = 0; i < 16; i++) {
        if (attach_map[i].c == NULL) {
            attach_map[i].c = c;
            attach_map[i].seg = seg;
            attach_map[i].shmid = shmid;
            return;
        }
    }
}

xcb_void_cookie_t xcb_shm_attach(xcb_connection_t *c, xcb_shm_seg_t shmseg,
                                 uint32_t shmid, uint8_t read_only) {
    typedef xcb_void_cookie_t (*xcb_shm_attach_fn)(xcb_connection_t *,
                                                   xcb_shm_seg_t, uint32_t,
                                                   uint8_t);
    static xcb_shm_attach_fn real = NULL;
    if (!real)
        real = (xcb_shm_attach_fn)dlsym(RTLD_NEXT, "xcb_shm_attach");
    set_shmid(c, shmseg, shmid);
    return real(c, shmseg, shmid, read_only);
}

typedef xcb_shm_get_image_cookie_t (*xcb_shm_get_image_fn)(
    xcb_connection_t *, xcb_drawable_t, int16_t, int16_t, uint16_t, uint16_t,
    uint32_t, uint8_t, xcb_shm_seg_t, uint32_t);
typedef xcb_shm_get_image_reply_t *(*xcb_shm_get_image_reply_fn)(
    xcb_connection_t *, xcb_shm_get_image_cookie_t, xcb_generic_error_t **);

xcb_shm_get_image_cookie_t xcb_shm_get_image(xcb_connection_t *c,
                                             xcb_drawable_t drawable, int16_t x,
                                             int16_t y, uint16_t width,
                                             uint16_t height, uint32_t plane_mask,
                                             uint8_t format, xcb_shm_seg_t shmseg,
                                             uint32_t offset) {
    static xcb_shm_get_image_fn real = NULL;
    if (!real)
        real = (xcb_shm_get_image_fn)dlsym(RTLD_NEXT, "xcb_shm_get_image");
    xcb_shm_get_image_cookie_t cookie =
        real(c, drawable, x, y, width, height, plane_mask, format, shmseg, offset);
    if (c != src_c && drawable == conn_root(c)) {
        signal_xcb_capture();
    }
    if (c != src_c && drawable == conn_root(c) && ensure_src_c()) {
        unsigned int slot = cookie.sequence % MAP_SIZE;
        for (unsigned int i = 0; i < MAP_SIZE; i++) {
            unsigned int s = (slot + i) % MAP_SIZE;
            if (!shm_map[s].valid) {
                shm_map[s].valid = 1;
                shm_map[s].seq = cookie.sequence;
                shm_map[s].x = x;
                shm_map[s].y = y;
                shm_map[s].w = width;
                shm_map[s].h = height;
                shm_map[s].shmid = find_shmid(c, shmseg);
                shm_map[s].offset = offset;
                break;
            }
        }
    }
    return cookie;
}

xcb_shm_get_image_reply_t *xcb_shm_get_image_reply(
    xcb_connection_t *c, xcb_shm_get_image_cookie_t cookie,
    xcb_generic_error_t **e) {
    static xcb_shm_get_image_reply_fn real = NULL;
    if (!real)
        real = (xcb_shm_get_image_reply_fn)dlsym(RTLD_NEXT, "xcb_shm_get_image_reply");
    unsigned int slot = cookie.sequence % MAP_SIZE;
    for (unsigned int i = 0; i < MAP_SIZE; i++) {
        unsigned int s = (slot + i) % MAP_SIZE;
        if (shm_map[s].valid && shm_map[s].seq == cookie.sequence) {
            shm_map[s].valid = 0;
            xcb_generic_error_t *lerr = NULL;
            xcb_shm_get_image_reply_t *rr = real(c, cookie, &lerr);
            if (!rr) return NULL;
            if (shm_map[s].shmid != (uint32_t)-1) {
                refresh_src_capture_c();
                ensure_capture_c_size(shm_map[s].w, shm_map[s].h);
                if (!real_xcb_get_image)
                    real_xcb_get_image = (xcb_get_image_fn)dlsym(RTLD_NEXT, "xcb_get_image");
                if (!real_xcb_get_image_reply)
                    real_xcb_get_image_reply = (xcb_get_image_reply_fn)dlsym(
                        RTLD_NEXT, "xcb_get_image_reply");
                int16_t sx = shm_map[s].x, sy = shm_map[s].y;
                map_capture_rect_c(&sx, &sy, shm_map[s].w, shm_map[s].h);
                uint16_t sw = shm_map[s].w < src_capture_w ? shm_map[s].w : src_capture_w;
                uint16_t sh = shm_map[s].h < src_capture_h ? shm_map[s].h : src_capture_h;
                xcb_get_image_cookie_t c2 = real_xcb_get_image(
                    src_c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                    src_capture_c ? src_capture_c : src_root,
                    sx, sy, sw, sh, ~0);
                xcb_get_image_reply_t *r2 =
                    real_xcb_get_image_reply(src_c, c2, NULL);
                if (r2) {
                    uint8_t *data = (uint8_t *)(r2 + 1);
                    size_t len = (size_t)r2->length * 4;
                    void *shm = shmat(shm_map[s].shmid, NULL, 0);
                    if (shm != (void *)-1) {
                        memcpy((uint8_t *)shm + shm_map[s].offset, data, len);
                        shmdt(shm);
                    }
                    free(r2);
                    report(shm_map[s].x, shm_map[s].y, shm_map[s].w, shm_map[s].h);
                } else {
                    report_debug("xcb_shm source get_image returned no reply");
                }
            } else {
                report_debug("xcb_shm_get_image missing shmid mapping");
            }
            return rr;
        }
    }
    return real(c, cookie, e);
}
