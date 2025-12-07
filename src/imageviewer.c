#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"
#include "../build//xdg-shell-client-protocol.h"

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct xdg_wm_base *wm_base = NULL;
static volatile sig_atomic_t running = 1;
static struct wl_buffer *global_buffer = NULL;
static volatile int configured = 0;
static struct wl_seat *seat = NULL;
static struct wl_keyboard *keyboard = NULL;
static int has_keyboard = 0;

static int is_wayland() {
    char *xdg = getenv("XDG_SESSION_TYPE");
    char *wl  = getenv("WAYLAND_DISPLAY");
    return (xdg && strcasecmp(xdg, "wayland") == 0) || wl;
}

static int is_x11() {
    char *disp = getenv("DISPLAY");
    return disp && strlen(disp) > 0;
}

static void sigint_handler(int signo) {
    (void)signo;
    running = 0;
}

static int create_shm_file(off_t size) {
    char template[] = "/tmp/imageviewer-shm-XXXXXX";
    int fd = mkstemp(template);
    if (fd >= 0) {
        unlink(template);
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* ---------------- Wayland callbacks ---------------- */
static void xdg_surface_handle_configure(void *data,
                                         struct xdg_surface *surface,
                                         uint32_t serial) {
    int *wh = data;
    xdg_surface_ack_configure(surface, serial);
    xdg_surface_set_window_geometry(surface, 0, 0, wh[0], wh[1]);
    configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_handle_configure
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states) {
    (void)toplevel; (void)states;
    int *wh = data;
    if (width > 0 && height > 0) {
        wh[0] = width;
        wh[1] = height;
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
    running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping
};

/* Keyboard listener functions */
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                           uint32_t format, int fd, uint32_t size) {
    (void)data; (void)keyboard; (void)format; (void)fd; (void)size;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                          uint32_t serial, struct wl_surface *surface,
                          struct wl_array *keys) {
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                          uint32_t serial, struct wl_surface *surface) {
    (void)data; (void)keyboard; (void)serial; (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                        uint32_t serial, uint32_t time, uint32_t key,
                        uint32_t state) {
    (void)data; (void)keyboard; (void)serial; (void)time;

    // state: 0 = released, 1 = pressed
    if (state == 1) {  // Key press
        // fprintf(stderr, "[imageviewer] Key pressed: keycode = %u\n", key);
        // 'q' = 16, Escape = 1
        if (key == 16 || key == 1) {
            running = 0;
        }
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                              uint32_t serial, uint32_t mods_depressed,
                              uint32_t mods_latched, uint32_t mods_locked,
                              uint32_t group) {
    (void)data; (void)keyboard; (void)serial; 
    (void)mods_depressed; (void)mods_latched; 
    (void)mods_locked; (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                int32_t rate, int32_t delay) {
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                             uint32_t capabilities) {
    (void)data;

    // Check for keyboard capability
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!keyboard) {
            keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
            has_keyboard = 1;
        }
    } else {
        if (keyboard) {
            wl_keyboard_destroy(keyboard);
            keyboard = NULL;
            has_keyboard = 0;
        }
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_handler(void *data, struct wl_registry *reg, uint32_t id,
                             const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (strcmp(interface, "wl_compositor") == 0)
        compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    else if (strcmp(interface, "wl_shm") == 0)
        shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    else if (strcmp(interface, "xdg_wm_base") == 0) {
        wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    }
    else if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_remover(void *data, struct wl_registry *reg, uint32_t id) {
    (void)data; (void)reg; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handler,
    .global_remove = registry_remover
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    wl_callback_destroy(cb);

    if (running) {
        struct wl_surface *surface = data;
        struct wl_callback *new_cb = wl_surface_frame(surface);
        static const struct wl_callback_listener listener = { .done = frame_done };
        wl_callback_add_listener(new_cb, &listener, surface);
        wl_surface_attach(surface, global_buffer, 0, 0);
        wl_surface_commit(surface);
    }
}

/* ---------------- Wayland viewer ---------------- */
static int run_wayland_viewer(const char *path) {
    int w, h, ch;
    unsigned char *img = stbi_load(path, &w, &h, &ch, 4);
    if (!img) {
        fprintf(stderr, "[imageviewer] Failed to load image: %s\n", path);
        return 1;
    }

    // Calculate scaled dimensions
    // limit to 1920x1080 if image is too large
    int display_w = w;
    int display_h = h;

    fprintf(stderr, "[imageviewer] Original: %dx%d, Display: %dx%d\n", 
    w, h, display_w, display_h);

    if (w > 1920 || h > 1080) {
        float scale = 1920.0f / w;
        if (h * scale > 1080) {
            scale = 1080.0f / h;
        }
        display_w = (int)(w * scale);
        display_h = (int)(h * scale);
        fprintf(stderr, "[imageviewer] Scaling image from %dx%d to %dx%d\n", 
                w, h, display_w, display_h);
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "[imageviewer] wl_display_connect failed\n");
        stbi_image_free(img);
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "[imageviewer] Missing Wayland globals\n");
        stbi_image_free(img);
        wl_display_disconnect(display);
        return 1;
    }
    wl_display_roundtrip(display);

    // Create scaled image buffer
    int stride = display_w * 4;
    int size = stride * display_h;
    int fd = create_shm_file(size);
    if (fd < 0) {
        fprintf(stderr, "[imageviewer] create_shm_file failed\n");
        stbi_image_free(img);
        wl_display_disconnect(display);
        return 1;
    }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("[imageviewer] mmap");
        close(fd);
        stbi_image_free(img);
        wl_display_disconnect(display);
        return 1;
    }

    // Simple nearest-neighbor scaling
    uint32_t *dst = map;
    for (int y = 0; y < display_h; ++y) {
        for (int x = 0; x < display_w; ++x) {
            int src_x = (x * w) / display_w;
            int src_y = (y * h) / display_h;
            int idx = (src_y * w + src_x) * 4;
            uint8_t r = img[idx+0], g = img[idx+1], b = img[idx+2], a = img[idx+3];
            dst[y * display_w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                     ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    stbi_image_free(img);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, display_w, display_h, stride, 
                                  WL_SHM_FORMAT_ARGB8888);
    global_buffer = buffer;
    wl_shm_pool_destroy(pool);
    close(fd);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    int wh[2] = { display_w, display_h };
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, wh);

    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_set_title(toplevel, "Image Viewer");
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, wh);

    // Set window geometry to match scaled image size
    xdg_surface_set_window_geometry(xdg_surface, 0, 0, display_w, display_h);

    // Set size hints
    xdg_toplevel_set_min_size(toplevel, display_w, display_h);
    xdg_toplevel_set_max_size(toplevel, display_w, display_h);

    // Commit initial state
    wl_surface_commit(surface);
    wl_display_flush(display);

    // Wait for first configure
    configured = 0;
    int timeout = 0;
    while (!configured && timeout < 100) {
        wl_display_dispatch(display);
        wl_display_flush(display);
        usleep(10000);
        timeout++;
    }

    if (!configured) {
        fprintf(stderr, "[imageviewer] Timeout waiting for configure\n");
        munmap(map, size);
        wl_buffer_destroy(buffer);
        if (keyboard) wl_keyboard_destroy(keyboard);
        if (seat) wl_seat_destroy(seat);
        wl_display_disconnect(display);
        return 1;
    }

    // Attach buffer
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    wl_display_flush(display);

    // Set up frame callback
    struct wl_callback *cb = wl_surface_frame(surface);
    static const struct wl_callback_listener listener = { .done = frame_done };
    wl_callback_add_listener(cb, &listener, surface);
    wl_surface_commit(surface);
    wl_display_flush(display);

    // Set up signal handler
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    fprintf(stderr, "[imageviewer] Image shown (%dx%d). Press 'q' or ESC to exit.\n", 
            display_w, display_h);

    // Main loop
    while (running) {
        wl_display_dispatch(display);
        wl_display_flush(display);
        usleep(10000);
    }

    fprintf(stderr, "[imageviewer] Exiting...\n");

    // Cleanup
    munmap(map, size);
    wl_buffer_destroy(buffer);
    if (keyboard) {
        wl_keyboard_destroy(keyboard);
    }
    if (seat) {
        wl_seat_destroy(seat);
    }
    wl_display_disconnect(display);
    return 0;
}

/* ---------------- X11 viewer ---------------- */
static int run_x11_viewer(const char *path) {
    int w, h, ch;
    unsigned char *img = stbi_load(path, &w, &h, &ch, 4);
    if (!img) {
        fprintf(stderr, "[imageviewer] Failed to load: %s\n", path);
        return 1;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[imageviewer] Cannot open X11 display\n");
        stbi_image_free(img);
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, root, 50, 50, w, h, 1,
                                     BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapWindow(dpy, win);

    XImage *xim = XCreateImage(dpy, DefaultVisual(dpy, screen),
                               DefaultDepth(dpy, screen), ZPixmap, 0,
                               malloc(w*h*4), w, h, 32, 0);
    if (!xim) { fprintf(stderr, "[imageviewer] XCreateImage failed\n"); return 1; }

    memcpy(xim->data, img, w*h*4);
    stbi_image_free(img);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (!gc) { fprintf(stderr, "[imageviewer] XCreateGC failed\n"); return 1; }

    XEvent ev;
    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose)
            XPutImage(dpy, win, gc, xim, 0,0,0,0, w,h);
        if (ev.type == KeyPress || ev.type == ButtonPress)
            break;
    }

    XDestroyImage(xim);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

/* ---------------- main ---------------- */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: imageviewer <image>\n");
        return 1;
    }

    const char *path = argv[1];

    if (is_wayland()) {
        printf("[imageviewer] Detected Wayland session\n");
        return run_wayland_viewer(path);
    } else if (is_x11()) {
        printf("[imageviewer] Detected X11 session\n");
        return run_x11_viewer(path);
    }

    fprintf(stderr, "[imageviewer] No display detected (neither X11 nor Wayland)\n");
    return 1;
}
