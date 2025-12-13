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
#include "../build//xdg-shell-client-protocol.h"
#include "../include/stb_image.h"

typedef struct {
    unsigned char *data;
    int width;
    int height;
    int channels;
} ImageData;

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
    char *wl = getenv("WAYLAND_DISPLAY");
    return (xdg && strcasecmp(xdg, "wayland") == 0) || wl;
}

static int is_x11() {
    char *disp = getenv("DISPLAY");
    return disp && strlen(disp) > 0;
}

static int run_wayland_viewer(const char *path, int requested_width,
                              int requested_height);
static int run_x11_viewer(const char *path, int requested_width,
                          int requested_height);
static int run_wayland_grid_viewer(const char **paths, int num_paths, 
                                   int requested_width, int requested_height,
                                   int grid_cols, int grid_rows);
static int run_x11_grid_viewer(const char **paths, int num_paths,
                               int requested_width, int requested_height,
                               int grid_cols, int grid_rows);

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

// Wayland callbacks
static void xdg_surface_handle_configure(void *data,
                                         struct xdg_surface *surface,
                                         uint32_t serial) {
    int *wh = data;
    xdg_surface_ack_configure(surface, serial);
    xdg_surface_set_window_geometry(surface, 0, 0, wh[0], wh[1]);
    configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_handle_configure};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states) {
    (void)toplevel;
    (void)states;
    int *wh = data;
    if (width > 0 && height > 0) {
        wh[0] = width;
        wh[1] = height;
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data;
    (void)toplevel;
    running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = xdg_toplevel_configure, .close = xdg_toplevel_close};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

// Keyboard listener functions
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int fd, uint32_t size) {
    (void)data;
    (void)keyboard;
    (void)format;
    (void)fd;
    (void)size;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)time;

    // state: 0 = released, 1 = pressed
    if (state == 1) { // Key press
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
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
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
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_handler(void *data, struct wl_registry *reg, uint32_t id,
                             const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, "wl_compositor") == 0)
        compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    else if (strcmp(interface, "wl_shm") == 0)
        shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    else if (strcmp(interface, "xdg_wm_base") == 0) {
        wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_remover(void *data, struct wl_registry *reg, uint32_t id) {
    (void)data;
    (void)reg;
    (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handler, .global_remove = registry_remover};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    wl_callback_destroy(cb);

    if (running) {
        struct wl_surface *surface = data;
        struct wl_callback *new_cb = wl_surface_frame(surface);
        static const struct wl_callback_listener listener = {.done = frame_done};
        wl_callback_add_listener(new_cb, &listener, surface);
        wl_surface_attach(surface, global_buffer, 0, 0);
        wl_surface_commit(surface);
    }
}

// Load and scale a single image for grid view
static ImageData* load_and_scale_image(const char *path, int target_width, int target_height) {
    int w, h, ch;
    unsigned char *img = stbi_load(path, &w, &h, &ch, 4);
    if (!img) {
        fprintf(stderr, "[imageviewer] Failed to load image: %s\n", path);
        return NULL;
    }

    ImageData *image = malloc(sizeof(ImageData));
    if (!image) {
        stbi_image_free(img);
        return NULL;
    }

    image->width = target_width;
    image->height = target_height;
    image->channels = 4;

    // Allocate memory for scaled image
    image->data = malloc(target_width * target_height * 4);
    if (!image->data) {
        stbi_image_free(img);
        free(image);
        return NULL;
    }

    // nearest-neighbor scaling for grid thumbnail
    for (int y = 0; y < target_height; ++y) {
        for (int x = 0; x < target_width; ++x) {
            int src_x = (x * w) / target_width;
            int src_y = (y * h) / target_height;
            int src_idx = (src_y * w + src_x) * 4;
            int dst_idx = (y * target_width + x) * 4;

            image->data[dst_idx + 0] = img[src_idx + 0];
            image->data[dst_idx + 1] = img[src_idx + 1];
            image->data[dst_idx + 2] = img[src_idx + 2];
            image->data[dst_idx + 3] = img[src_idx + 3];
        }
    }

    stbi_image_free(img);
    return image;
}

// Wayland grid viewer
static int run_wayland_grid_viewer(const char **paths, int num_paths, 
                                   int requested_width, int requested_height,
                                   int grid_cols, int grid_rows) {
    if (num_paths == 0) {
        fprintf(stderr, "[imageviewer] No images provided for grid view\n");
        return 1;
    }

    // Calculate total grid dimensions
    int cell_width = requested_width > 0 ? requested_width / grid_cols : 400;
    int cell_height = requested_height > 0 ? requested_height / grid_rows : 300;
    int display_w = cell_width * grid_cols;
    int display_h = cell_height * grid_rows;

    if (requested_width <= 0) display_w = cell_width * grid_cols;
    if (requested_height <= 0) display_h = cell_height * grid_rows;

    fprintf(stderr, "[imageviewer] Grid view: %dx%d cells, %d images\n", 
            grid_cols, grid_rows, num_paths);
    fprintf(stderr, "[imageviewer] Cell size: %dx%d, Total: %dx%d\n",
            cell_width, cell_height, display_w, display_h);

    // Load all images
    ImageData **images = malloc(num_paths * sizeof(ImageData*));
    if (!images) {
        fprintf(stderr, "[imageviewer] Memory allocation failed\n");
        return 1;
    }

    int loaded_images = 0;
    for (int i = 0; i < num_paths; i++) {
        images[i] = load_and_scale_image(paths[i], cell_width, cell_height);
        if (images[i]) {
            loaded_images++;
        } else {
            fprintf(stderr, "[imageviewer] Failed to load: %s\n", paths[i]);
            images[i] = NULL;
        }
    }

    if (loaded_images == 0) {
        fprintf(stderr, "[imageviewer] No images could be loaded\n");
        free(images);
        return 1;
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "[imageviewer] wl_display_connect failed\n");
        for (int i = 0; i < num_paths; i++) {
            if (images[i]) {
                free(images[i]->data);
                free(images[i]);
            }
        }
        free(images);
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "[imageviewer] Missing Wayland globals\n");
        for (int i = 0; i < num_paths; i++) {
            if (images[i]) {
                free(images[i]->data);
                free(images[i]);
            }
        }
        free(images);
        wl_display_disconnect(display);
        return 1;
    }
    wl_display_roundtrip(display);

    // Create composite buffer for grid
    int stride = display_w * 4;
    int size = stride * display_h;
    int fd = create_shm_file(size);
    if (fd < 0) {
        fprintf(stderr, "[imageviewer] create_shm_file failed\n");
        for (int i = 0; i < num_paths; i++) {
            if (images[i]) {
                free(images[i]->data);
                free(images[i]);
            }
        }
        free(images);
        wl_display_disconnect(display);
        return 1;
    }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("[imageviewer] mmap");
        close(fd);
        for (int i = 0; i < num_paths; i++) {
            if (images[i]) {
                free(images[i]->data);
                free(images[i]);
            }
        }
        free(images);
        wl_display_disconnect(display);
        return 1;
    }

    // Clear background (dark gray)
    uint32_t *dst = map;
    for (int y = 0; y < display_h; ++y) {
        for (int x = 0; x < display_w; ++x) {
            dst[y * display_w + x] = 0xFF202020; // ARGB: dark gray
        }
    }

    // Composite all images into grid
    int images_placed = 0;
    for (int row = 0; row < grid_rows; row++) {
        for (int col = 0; col < grid_cols; col++) {
            if (images_placed >= num_paths) break;

            int img_index = row * grid_cols + col;
            if (img_index >= num_paths || !images[img_index]) continue;

            int start_x = col * cell_width;
            int start_y = row * cell_height;

            ImageData *img = images[img_index];
            uint32_t *src_pixels = (uint32_t*)img->data;

            // Copy image to grid cell
            for (int y = 0; y < cell_height && y < img->height; y++) {
                for (int x = 0; x < cell_width && x < img->width; x++) {
                    int dst_idx = (start_y + y) * display_w + (start_x + x);
                    int src_idx = y * img->width + x;

                    // Only copy if pixel is not fully transparent
                    if ((src_pixels[src_idx] >> 24) > 0) {
                        dst[dst_idx] = src_pixels[src_idx];
                    }
                }
            }
            images_placed++;
        }
        if (images_placed >= num_paths) break;
    }

    // Free image data
    for (int i = 0; i < num_paths; i++) {
        if (images[i]) {
            free(images[i]->data);
            free(images[i]);
        }
    }
    free(images);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, display_w, display_h, stride, WL_SHM_FORMAT_ARGB8888);
    global_buffer = buffer;
    wl_shm_pool_destroy(pool);
    close(fd);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    int wh[2] = {display_w, display_h};
    struct xdg_surface *xdg_surface =
        xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, wh);

    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_set_title(toplevel, "Image Viewer - Grid");
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, wh);

    // Set window geometry
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
    static const struct wl_callback_listener listener = {.done = frame_done};
    wl_callback_add_listener(cb, &listener, surface);
    wl_surface_commit(surface);
    wl_display_flush(display);

    // Set up signal handler
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    fprintf(stderr,
            "[imageviewer] Grid view shown (%dx%d, %d images). Press 'q' or ESC to exit.\n",
            display_w, display_h, loaded_images);

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

// X11 grid viewer
static int run_x11_grid_viewer(const char **paths, int num_paths,
                               int requested_width, int requested_height,
                               int grid_cols, int grid_rows) {

    if (num_paths == 0) {
        fprintf(stderr, "[imageviewer] No images provided for grid view\n");
        return 1;
    }

    // Calculate total grid dimensions
    int cell_width = requested_width > 0 ? requested_width / grid_cols : 400;
    int cell_height = requested_height > 0 ? requested_height / grid_rows : 300;
    int display_w = cell_width * grid_cols;
    int display_h = cell_height * grid_rows;

    if (requested_width <= 0) display_w = cell_width * grid_cols;
    if (requested_height <= 0) display_h = cell_height * grid_rows;

    fprintf(stderr, "[imageviewer] Grid view: %dx%d cells, %d images\n", 
            grid_cols, grid_rows, num_paths);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[imageviewer] Cannot open X11 display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win =
        XCreateSimpleWindow(dpy, root, 50, 50, display_w, display_h, 1,
                          BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapWindow(dpy, win);

    // Create composite image for grid
    unsigned char *composite_img = malloc(display_w * display_h * 4);
    if (!composite_img) {
        fprintf(stderr, "[imageviewer] Memory allocation failed\n");
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }

    // Clear background (dark gray)
    for (int i = 0; i < display_w * display_h * 4; i += 4) {
        composite_img[i] = 32;     // R
        composite_img[i+1] = 32;   // G
        composite_img[i+2] = 32;   // B
        composite_img[i+3] = 255;  // A
    }

    // Load and composite images
    int images_placed = 0;
    for (int row = 0; row < grid_rows; row++) {
        for (int col = 0; col < grid_cols; col++) {
            if (images_placed >= num_paths) break;

            const char *path = paths[images_placed];
            int w, h, ch;
            unsigned char *img = stbi_load(path, &w, &h, &ch, 4);
            if (!img) {
                fprintf(stderr, "[imageviewer] Failed to load: %s\n", path);
                images_placed++;
                continue;
            }

            int start_x = col * cell_width;
            int start_y = row * cell_height;

            // Simple scaling
            for (int y = 0; y < cell_height; y++) {
                for (int x = 0; x < cell_width; x++) {
                    int src_x = (x * w) / cell_width;
                    int src_y = (y * h) / cell_height;
                    int src_idx = (src_y * w + src_x) * 4;
                    int dst_idx = ((start_y + y) * display_w + (start_x + x)) * 4;

                    // Only copy if pixel is not fully transparent
                    if (img[src_idx + 3] > 0) {
                        composite_img[dst_idx] = img[src_idx];
                        composite_img[dst_idx + 1] = img[src_idx + 1];
                        composite_img[dst_idx + 2] = img[src_idx + 2];
                        composite_img[dst_idx + 3] = img[src_idx + 3];
                    }
                }
            }
            stbi_image_free(img);
            images_placed++;
        }
    }

    XImage *xim =
        XCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen),
                   ZPixmap, 0, (char*)composite_img, display_w, display_h, 32, 0);
    if (!xim) {
        fprintf(stderr, "[imageviewer] XCreateImage failed\n");
        free(composite_img);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }

    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (!gc) {
        fprintf(stderr, "[imageviewer] XCreateGC failed\n");
        XDestroyImage(xim);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }

    XEvent ev;
    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose)
            XPutImage(dpy, win, gc, xim, 0, 0, 0, 0, display_w, display_h);
        if (ev.type == KeyPress || ev.type == ButtonPress)
            break;
    }

    XDestroyImage(xim);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

// Wayland viewer for single image
static int run_wayland_viewer(const char *path, int requested_width,
                              int requested_height) {
    int w, h, ch;
    unsigned char *img = stbi_load(path, &w, &h, &ch, 4);
    if (!img) {
        fprintf(stderr, "[imageviewer] Failed to load image: %s\n", path);
        return 1;
    }

    // Calculate display dimensions
    int display_w = w;
    int display_h = h;

    // If specified dimensions, resize maintaining aspect ratio
    if (requested_width > 0 || requested_height > 0) {
        if (requested_width > 0 && requested_height > 0) {
            // Both specified
            display_w = requested_width;
            display_h = requested_height;
        } else if (requested_width > 0) {
            // Only width specified, calculate height maintaining aspect ratio
            display_w = requested_width;
            display_h = (int)((float)h * requested_width / w);
        } else if (requested_height > 0) {
            // Only height specified, calculate width maintaining aspect ratio
            display_h = requested_height;
            display_w = (int)((float)w * requested_height / h);
        }

        fprintf(stderr, "[imageviewer] Using requested size: %dx%d\n", display_w,
                display_h);
    } else {
        // Default scaling: limit to 800x600 if image is too large
        int max_width = 800;
        int max_height = 600;

        if (w > max_width || h > max_height) {
            float scale_w = (float)max_width / w;
            float scale_h = (float)max_height / h;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;

            display_w = (int)(w * scale);
            display_h = (int)(h * scale);

            fprintf(stderr, "[imageviewer] Scaling image from %dx%d to %dx%d\n", w, h,
                    display_w, display_h);
        } else {
            fprintf(stderr, "[imageviewer] Original: %dx%d, Display: %dx%d\n", w, h,
                    display_w, display_h);
        }
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
            uint8_t r = img[idx + 0], g = img[idx + 1], b = img[idx + 2],
                    a = img[idx + 3];
            dst[y * display_w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                   ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    stbi_image_free(img);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, display_w, display_h, stride, WL_SHM_FORMAT_ARGB8888);
    global_buffer = buffer;
    wl_shm_pool_destroy(pool);
    close(fd);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    int wh[2] = {display_w, display_h};
    struct xdg_surface *xdg_surface =
        xdg_wm_base_get_xdg_surface(wm_base, surface);
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
    static const struct wl_callback_listener listener = {.done = frame_done};
    wl_callback_add_listener(cb, &listener, surface);
    wl_surface_commit(surface);
    wl_display_flush(display);

    // Set up signal handler
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    fprintf(stderr,
            "[imageviewer] Image shown (%dx%d). Press 'q' or ESC to exit.\n",
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

// X11 viewer for single image
static int run_x11_viewer(const char *path, int requested_width,
                          int requested_height) {
    int w, h, ch;
    unsigned char *img = stbi_load(path, &w, &h, &ch, 4);
    if (!img) {
        fprintf(stderr, "[imageviewer] Failed to load: %s\n", path);
        return 1;
    }

    // Calculate display dimensions
    int display_w = w;
    int display_h = h;

    if (requested_width > 0 || requested_height > 0) {
        if (requested_width > 0 && requested_height > 0) {
            display_w = requested_width;
            display_h = requested_height;
        } else if (requested_width > 0) {
            display_w = requested_width;
            display_h = (int)((float)h * requested_width / w);
        } else if (requested_height > 0) {
            display_h = requested_height;
            display_w = (int)((float)w * requested_height / h);
        }
    } else {
        // Default scaling
        int max_width = 800;
        int max_height = 600;

        if (w > max_width || h > max_height) {
            float scale_w = (float)max_width / w;
            float scale_h = (float)max_height / h;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;

            display_w = (int)(w * scale);
            display_h = (int)(h * scale);
        }
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[imageviewer] Cannot open X11 display\n");
        stbi_image_free(img);
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win =
        XCreateSimpleWindow(dpy, root, 50, 50, display_w, display_h, 1,
                          BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapWindow(dpy, win);

    // Scale image to display dimensions
    unsigned char *scaled_img = malloc(display_w * display_h * 4);
    if (!scaled_img) {
        fprintf(stderr, "[imageviewer] Memory allocation failed\n");
        stbi_image_free(img);
        return 1;
    }

    // Simple nearest-neighbor scaling
    for (int y = 0; y < display_h; ++y) {
        for (int x = 0; x < display_w; ++x) {
            int src_x = (x * w) / display_w;
            int src_y = (y * h) / display_h;
            int src_idx = (src_y * w + src_x) * 4;
            int dst_idx = (y * display_w + x) * 4;

            scaled_img[dst_idx + 0] = img[src_idx + 0];
            scaled_img[dst_idx + 1] = img[src_idx + 1];
            scaled_img[dst_idx + 2] = img[src_idx + 2];
            scaled_img[dst_idx + 3] = img[src_idx + 3];
        }
    }

    stbi_image_free(img);

    XImage *xim =
        XCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen), ZPixmap, 0, scaled_img, display_w, display_h, 32, 0);
    if (!xim) {
        fprintf(stderr, "[imageviewer] XCreateImage failed\n");
        free(scaled_img);
        return 1;
    }

    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (!gc) {
        fprintf(stderr, "[imageviewer] XCreateGC failed\n");
        XDestroyImage(xim);
        return 1;
    }

    XEvent ev;
    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose)
            XPutImage(dpy, win, gc, xim, 0, 0, 0, 0, display_w, display_h);
        if (ev.type == KeyPress || ev.type == ButtonPress)
            break;
    }

    XDestroyImage(xim);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

int main(int argc, char **argv) {
    int width = 0;
    int height = 0;
    int grid_mode = 0;
    int grid_cols = 3;
    int grid_rows = 2;

    // Store all image paths
    const char *paths[256];
    int num_paths = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--grid") == 0 || strcmp(argv[i], "-g") == 0) {
            grid_mode = 1;
        } else if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) {
            grid_cols = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
            grid_rows = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: imageviewer [OPTIONS] <image1> [image2 ...]\n");
            printf("Options:\n");
            printf("  -w WIDTH     Set window width\n");
            printf("  -h HEIGHT    Set window height\n");
            printf("  -g, --grid   Enable grid view for multiple images\n");
            printf("  --cols N     Set grid columns (default: 3)\n");
            printf("  --rows N     Set grid rows (default: 2)\n");
            printf("  --help       Show this help\n");
            printf("\nExamples:\n");
            printf("  imageviewer image.jpg           # View single image\n");
            printf("  imageviewer -g *.jpg            # View all JPGs in grid\n");
            printf("  imageviewer -g --cols 4 img*.png # 4-column grid\n");
            return 0;
        } else if (argv[i][0] != '-') {
            if (num_paths < 256) {
                paths[num_paths++] = argv[i];
            } else {
                fprintf(stderr, "Too many images (max 256)\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (num_paths == 0) {
        fprintf(stderr, "Usage: imageviewer [OPTIONS] <image1> [image2 ...]\n");
        fprintf(stderr, "Try 'imageviewer --help' for more information.\n");
        return 1;
    }

    if (grid_mode) {
        fprintf(stderr, "[imageviewer] Grid mode: %d images, %dx%d grid\n", 
                num_paths, grid_cols, grid_rows);

        if (is_wayland()) {
            printf("[imageviewer] Detected Wayland session (grid view)\n");
            return run_wayland_grid_viewer(paths, num_paths, width, height, 
                                          grid_cols, grid_rows);
        } else if (is_x11()) {
            printf("[imageviewer] Detected X11 session (grid view)\n");
            return run_x11_grid_viewer(paths, num_paths, width, height,
                                      grid_cols, grid_rows);
        }
    } else {
        // Single image mode (use first image)
        if (num_paths > 1) {
            fprintf(stderr, "[imageviewer] Note: Only showing first image. Use --grid for multiple images.\n");
        }

        if (is_wayland()) {
            printf("[imageviewer] Detected Wayland session\n");
            return run_wayland_viewer(paths[0], width, height);
        } else if (is_x11()) {
            printf("[imageviewer] Detected X11 session\n");
            return run_x11_viewer(paths[0], width, height);
        }
    }
    fprintf(stderr,
            "[imageviewer] No display detected (neither X11 nor Wayland)\n");
    return 1;
}
