#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <wayland-client.h>

// Configuration
typedef struct {
  int width;
  int height;
  uint32_t bg_color;
  uint32_t text_color;
  int layer;
  int anchor;
  int margin_top;
  int margin_right;
  int margin_bottom;
  int margin_left;
  int show_date;
  int show_seconds;
  int font_size;
  int corner_radius;
  int padding;
} Config;

static struct wl_display *display = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *wl_shm = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;
static struct wl_surface *surface = NULL;
static struct zwlr_layer_surface_v1 *layer_surface = NULL;
static struct wl_buffer *buffer = NULL;
static struct wl_callback *frame_callback = NULL;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t should_exit = 0;
static int configured = 0;
static int needs_redraw = 1;
static time_t last_drawn_time = 0;
static int display_fd = -1;

// Signal handler function
static void signal_handler(int signo) {
  (void)signo;
  should_exit = 1;
  running = 0;
  fprintf(stderr, "Signal received, exiting...\n");
}

static Config config = {.width = 180,
                        .height = 50,
                        .bg_color = 0xAA222222,
                        // .text_color = 0xFFFFFFFF,
                        .text_color = 0x89B4FAFF,
                        .layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                        .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                        .margin_top = 40,
                        .margin_right = 20,
                        .margin_bottom = 0,
                        .margin_left = 0,
                        .show_date = 1,
                        .show_seconds = 1,
                        .font_size = 18,
                        .corner_radius = 15,
                        .padding = 12};

// 7-segment digit
void draw_digit(uint32_t *pixels, char digit, int x, int y, int width,
                int height, uint32_t color) {
  if (digit < '0' || digit > '9')
    return;

  int segments[10][7] = {
      {1, 1, 1, 1, 1, 1, 0}, // 0
      {0, 1, 1, 0, 0, 0, 0}, // 1
      {1, 1, 0, 1, 1, 0, 1}, // 2
      {1, 1, 1, 1, 0, 0, 1}, // 3
      {0, 1, 1, 0, 0, 1, 1}, // 4
      {1, 0, 1, 1, 0, 1, 1}, // 5
      {1, 0, 1, 1, 1, 1, 1}, // 6
      {1, 1, 1, 0, 0, 0, 0}, // 7
      {1, 1, 1, 1, 1, 1, 1}, // 8
      {1, 1, 1, 1, 0, 1, 1}  // 9
  };

  int *seg = segments[digit - '0'];
  int thickness = width / 7;
  if (thickness < 2)
    thickness = 2;

  int seg_length = width - thickness * 2;

  // Segment positions (relative to digit bounds)
  // a: top horizontal
  if (seg[0]) {
    for (int i = 0; i < thickness; i++) {
      for (int px = x + thickness; px < x + width - thickness; px++) {
        int py = y + i;
        if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }

  // b: top-right vertical
  if (seg[1]) {
    for (int i = 0; i < thickness; i++) {
      for (int py = y + thickness; py < y + height / 2 - thickness / 2; py++) {
        int px = x + width - thickness + i;
        if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }

  // c: bottom-right vertical
  if (seg[2]) {
    for (int i = 0; i < thickness; i++) {
      for (int py = y + height / 2 - thickness / 2; py < y + height - thickness;
           py++) {
        int px = x + width - thickness + i;
        if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }

  // d: bottom horizontal
  if (seg[3]) {
    for (int i = 0; i < thickness; i++) {
      for (int px = x + thickness; px < x + width - thickness; px++) {
        int py = y + height - thickness + i;
        if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }

  // e: bottom-left vertical
  if (seg[4]) {
    for (int i = 0; i < thickness; i++) {
      for (int py = y + height / 2 - thickness / 2; py < y + height - thickness;
           py++) {
        int px = x + i;
        if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }

  // f: top-left vertical
  if (seg[5]) {
    for (int i = 0; i < thickness; i++) {
      for (int py = y + thickness; py < y + height / 2 - thickness / 2; py++) {
        int px = x + i;
        if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }

  // g: middle horizontal
  if (seg[6]) {
    for (int i = 0; i < thickness; i++) {
      for (int px = x + thickness; px < x + width - thickness; px++) {
        int py = y + height / 2 - thickness / 2 + i;
        if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }
}

void draw_colon(uint32_t *pixels, int x, int y, int size, uint32_t color) {
  int dot_size = size / 4;
  if (dot_size < 3)
    dot_size = 3;

  int spacing = size / 3;

  // Top dot
  for (int dy = -dot_size / 2; dy <= dot_size / 2; dy++) {
    for (int dx = -dot_size / 2; dx <= dot_size / 2; dx++) {
      int px = x + dx;
      int py = y - spacing / 2 + dy;
      if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
        if (dx * dx + dy * dy <= (dot_size / 2) * (dot_size / 2)) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }

  // Bottom dot
  for (int dy = -dot_size / 2; dy <= dot_size / 2; dy++) {
    for (int dx = -dot_size / 2; dx <= dot_size / 2; dx++) {
      int px = x + dx;
      int py = y + spacing / 2 + dy;
      if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
        if (dx * dx + dy * dy <= (dot_size / 2) * (dot_size / 2)) {
          pixels[py * config.width + px] = color;
        }
      }
    }
  }
}

void draw_time(uint32_t *pixels, const char *time_str, int x, int y) {
  int digit_width = config.font_size;
  int digit_height = config.font_size * 2;
  int spacing = digit_width / 3;
  int colon_spacing = digit_width / 4;

  for (int i = 0; time_str[i]; i++) {
    if (time_str[i] == ':') {
      draw_colon(pixels, x + colon_spacing / 2, y + digit_height / 2,
                 digit_width / 2, config.text_color);
      x += colon_spacing * 2;
    } else if (isdigit(time_str[i])) {
      draw_digit(pixels, time_str[i], x, y, digit_width, digit_height,
                 config.text_color);
      x += digit_width + spacing;
    }
  }
}

void draw_date(uint32_t *pixels, const char *date_str, int x, int y) {
  int char_width = config.font_size / 2;
  int char_height = config.font_size;

  for (int i = 0; date_str[i]; i++) {
    char c = date_str[i];

    // Skip spaces
    if (c == ' ') {
      x += char_width;
      continue;
    }

    // Draw character
    for (int cy = 0; cy < char_height; cy++) {
      for (int cx = 0; cx < char_width; cx++) {
        int draw_pixel = 0;

        // Border
        if (cy == 0 || cy == char_height - 1) {
          if (cx > 0 && cx < char_width - 1)
            draw_pixel = 1;
        }
        // Middle bar for some letters
        else if (cy == char_height / 2) {
          if (strchr("AEIO", toupper(c)) && cx > 0 && cx < char_width - 1)
            draw_pixel = 1;
        }
        // Vertical bars
        else if (cx == 0 || cx == char_width - 1) {
          draw_pixel = 1;
        }
        // For 'M' and 'W'
        else if ((toupper(c) == 'M' || toupper(c) == 'W') &&
                 (cx == char_width / 4 || cx == 3 * char_width / 4)) {
          draw_pixel = 1;
        }

        if (draw_pixel) {
          int px = x + cx;
          int py = y + cy;
          if (px >= 0 && px < config.width && py >= 0 && py < config.height) {
            pixels[py * config.width + px] = config.text_color;
          }
        }
      }
    }

    if (isdigit(c)) {
      x += char_width + 2;
    } else {
      x += char_width + 1;
    }
  }
}

void draw_rounded_rect(uint32_t *pixels, uint32_t color) {
  int r = config.corner_radius;

  for (int y = 0; y < config.height; y++) {
    for (int x = 0; x < config.width; x++) {
      int inside = 1;
      float alpha = 1.0;

      if (x < r && y < r) {
        // Top-left corner
        float dx = r - x - 0.5;
        float dy = r - y - 0.5;
        float distance = sqrtf(dx * dx + dy * dy);
        if (distance > r + 0.5) {
          inside = 0;
        } else if (distance > r - 0.5) {
          alpha = (r + 0.5) - distance;
        }
      } else if (x > config.width - r - 1 && y < r) {
        // Top-right corner
        float dx = x - (config.width - r - 1) - 0.5;
        float dy = r - y - 0.5;
        float distance = sqrtf(dx * dx + dy * dy);
        if (distance > r + 0.5) {
          inside = 0;
        } else if (distance > r - 0.5) {
          alpha = (r + 0.5) - distance;
        }
      } else if (x < r && y > config.height - r - 1) {
        // Bottom-left corner
        float dx = r - x - 0.5;
        float dy = y - (config.height - r - 1) - 0.5;
        float distance = sqrtf(dx * dx + dy * dy);
        if (distance > r + 0.5) {
          inside = 0;
        } else if (distance > r - 0.5) {
          alpha = (r + 0.5) - distance;
        }
      } else if (x > config.width - r - 1 && y > config.height - r - 1) {
        // Bottom-right corner
        float dx = x - (config.width - r - 1) - 0.5;
        float dy = y - (config.height - r - 1) - 0.5;
        float distance = sqrtf(dx * dx + dy * dy);
        if (distance > r + 0.5) {
          inside = 0;
        } else if (distance > r - 0.5) {
          alpha = (r + 0.5) - distance;
        }
      } else if (x < 0 || x >= config.width || y < 0 || y >= config.height) {
        inside = 0;
      }

      if (inside) {
        // Apply alpha
        if (alpha < 1.0) {
          uint8_t a = (color >> 24) & 0xFF;
          uint8_t r = (color >> 16) & 0xFF;
          uint8_t g = (color >> 8) & 0xFF;
          uint8_t b = color & 0xFF;

          a = (uint8_t)(a * alpha);
          pixels[y * config.width + x] = (a << 24) | (r << 16) | (g << 8) | b;
        } else {
          pixels[y * config.width + x] = color;
        }
      }
    }
  }
}

// Get current time and date as strings
void get_time_and_date(char *time_buf, size_t time_size, char *date_buf,
                       size_t date_size) {
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);

  if (config.show_seconds) {
    strftime(time_buf, time_size, "%H:%M:%S", tm);
  } else {
    strftime(time_buf, time_size, "%H:%M", tm);
  }

  if (config.show_date) {
    // Format: "Day Date Month" (as "Mon 08 Dec")
    strftime(date_buf, date_size, "%a %d %b", tm);
  } else {
    date_buf[0] = '\0';
  }
}

// Create shared memory buffer
static struct wl_buffer *create_buffer(void) {
  int stride = config.width * 4;
  int size = stride * config.height;

  char template[] = "/tmp/clock-widget-shm-XXXXXX";
  int fd = mkstemp(template);
  if (fd < 0) {
    fprintf(stderr, "Failed to create shm file\n");
    return NULL;
  }

  unlink(template);
  if (ftruncate(fd, size) < 0) {
    fprintf(stderr, "Failed to truncate shm file\n");
    close(fd);
    return NULL;
  }

  uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    fprintf(stderr, "Failed to mmap shm file\n");
    close(fd);
    return NULL;
  }

  // Clear to transparent
  for (int i = 0; i < config.width * config.height; i++) {
    data[i] = 0x00000000;
  }

  // Draw rounded rectangle background
  draw_rounded_rect(data, config.bg_color);

  // Get time and date
  char time_str[32];
  char date_str[32];
   get_time_and_date(time_str, sizeof(time_str), date_str, sizeof(date_str));

  // Calculate positions with padding
  int content_width = config.width - config.padding * 2;
  int content_height = config.height - config.padding * 2;

  // Draw time (centered horizontally, 1/3 from top)
  int time_str_len = strlen(time_str);
  int time_chars = time_str_len;
  int digit_width = config.font_size;
  int spacing = digit_width / 3;
  int colon_spacing = digit_width / 4;

  // Calculate time width
  int time_width = 0;
  for (int i = 0; time_str[i]; i++) {
    if (time_str[i] == ':') {
      time_width += colon_spacing * 2;
    } else {
      time_width += digit_width + spacing;
    }
  }
  time_width -= spacing;

  int time_x = config.padding + (content_width - time_width) / 2;
  int time_y = config.padding + content_height / 3 - config.font_size;
  draw_time(data, time_str, time_x, time_y);

  // Draw date if enabled
if (config.show_date && date_str[0]) {
    const int DATE_FONT_SCALE = 2; 
    int date_font_size = config.font_size / DATE_FONT_SCALE;
    if (date_font_size < 4) date_font_size = 4;

    int date_len = strlen(date_str);
    int date_char_width_base = date_font_size / 2;
    int date_width = 0;
    for (int i = 0; date_str[i]; i++) {
        if (date_str[i] == ' ') {
            date_width += date_char_width_base;
        } else if (isdigit(date_str[i])) {
            date_width += date_char_width_base + 2;
        } else {
            date_width += date_char_width_base + 1;
        }
    }

    int date_x = config.padding + (content_width - date_width) / 2;
    int date_y = config.padding + content_height * 2 / 3;
    int original_font_size = config.font_size;
    config.font_size = date_font_size;

    draw_date(data, date_str, date_x, date_y);

    // Restore global font size
    config.font_size = original_font_size;
  }

  munmap(data, size);

  struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      pool, 0, config.width, config.height, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  return buf;
}

// Draw current frame
void draw_frame(void) {
  if (!configured) {
    fprintf(stderr, "Not drawing frame: not configured yet\n");
    return;
  }

  struct wl_buffer *new_buffer = create_buffer();
  if (!new_buffer) {
    fprintf(stderr, "Failed to create buffer\n");
    return;
  }

  wl_surface_attach(surface, new_buffer, 0, 0);
  wl_surface_damage(surface, 0, 0, config.width, config.height);

  // Destroy old buffer if it exists
  if (buffer) {
    wl_buffer_destroy(buffer);
  }
  buffer = new_buffer;

  wl_surface_commit(surface);
  needs_redraw = 0;
  last_drawn_time = time(NULL);

  fprintf(stderr, "Frame drawn at %ld\n", last_drawn_time);
}

// Parse command line arguments - ADD padding option
void parse_args(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      printf("Clock Widget - Display a clock above wallpaper\n");
      printf("Usage: %s [options]\n", argv[0]);
      printf("Options:\n");
      printf("  --top-left         Position at top-left\n");
      printf("  --top-right        Position at top-right (default)\n");
      printf("  --bottom-left      Position at bottom-left\n");
      printf("  --bottom-right     Position at bottom-right\n");
      printf("  --width W          Set width (default: 320)\n");
      printf("  --height H         Set height (default: 120)\n");
      printf("  --margin M         Set all margins (default: 20)\n");
      printf("  --margin-top M     Set top margin\n");
      printf("  --margin-right M   Set right margin\n");
      printf("  --padding P        Set internal padding (default: 15)\n");
      printf("  --no-date          Hide date\n");
      printf("  --no-seconds       Hide seconds\n");
      printf("  --font-size N      Set font size (default: 28)\n");
      printf("  --corner-radius R  Set corner radius (default: 15)\n");
      printf("  --transparency N   Set transparency (0-255, default: 170)\n");
      printf("  --debug            Enable debug output\n");
      printf("  --help             Show this help\n");
      exit(0);
    } else if (strcmp(argv[i], "--top-left") == 0) {
      config.anchor =
          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    } else if (strcmp(argv[i], "--top-right") == 0) {
      config.anchor =
          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    } else if (strcmp(argv[i], "--bottom-left") == 0) {
      config.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    } else if (strcmp(argv[i], "--bottom-right") == 0) {
      config.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
      config.width = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
      config.height = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--margin") == 0 && i + 1 < argc) {
      int m = atoi(argv[++i]);
      config.margin_top = config.margin_right = config.margin_bottom =
          config.margin_left = m;
    } else if (strcmp(argv[i], "--margin-top") == 0 && i + 1 < argc) {
      config.margin_top = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--margin-right") == 0 && i + 1 < argc) {
      config.margin_right = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--padding") == 0 && i + 1 < argc) {
      config.padding = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--no-date") == 0) {
      config.show_date = 0;
    } else if (strcmp(argv[i], "--no-seconds") == 0) {
      config.show_seconds = 0;
    } else if (strcmp(argv[i], "--font-size") == 0 && i + 1 < argc) {
      config.font_size = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--corner-radius") == 0 && i + 1 < argc) {
      config.corner_radius = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--transparency") == 0 && i + 1 < argc) {
      int alpha = atoi(argv[++i]);
      if (alpha < 0)
        alpha = 0;
      if (alpha > 255)
        alpha = 255;
      // Keep the color but change alpha
      config.bg_color = (alpha << 24) | 0x222222;
    } else if (strcmp(argv[i], "--debug") == 0) {
      // Debug is enabled by default with stderr output
    }
  }
}

// Wayland registry handlers
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  (void)data;
  (void)version;

  fprintf(stderr, "Registry global: %s (name: %u)\n", interface, name);

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    fprintf(stderr, "Got compositor\n");
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    fprintf(stderr, "Got shm\n");
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    layer_shell =
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4);
    fprintf(stderr, "Got layer shell\n");
  }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
  fprintf(stderr, "Registry global removed: %u\n", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// Layer surface handlers
static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
  (void)data;
  fprintf(stderr, "Layer surface configure: %ux%u (serial: %u)\n", width,
          height, serial);

  zwlr_layer_surface_v1_ack_configure(surface, serial);
  configured = 1;

  // Draw initial frame
  draw_frame();
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
  (void)data;
  (void)surface;
  fprintf(stderr, "Layer surface closed\n");
  running = 0;
  should_exit = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

// Frame callback handler
static void frame_callback_handler(void *data, struct wl_callback *callback,
                                   uint32_t timestamp) {
  (void)data;
  (void)callback;
  (void)timestamp;

  fprintf(stderr, "Frame callback\n");

  if (frame_callback) {
    wl_callback_destroy(frame_callback);
    frame_callback = NULL;
  }

  // Check if we need to redraw
  time_t now = time(NULL);
  if (now != last_drawn_time) {
    draw_frame();
  }

  // Request next frame
  frame_callback = wl_surface_frame(surface);
  static const struct wl_callback_listener listener = {
      .done = frame_callback_handler};
  wl_callback_add_listener(frame_callback, &listener, NULL);
}

// Clean up function
static void cleanup(void) {
  fprintf(stderr, "Cleaning up...\n");

  if (frame_callback) {
    wl_callback_destroy(frame_callback);
    frame_callback = NULL;
  }
  if (layer_surface) {
    zwlr_layer_surface_v1_destroy(layer_surface);
    layer_surface = NULL;
  }
  if (surface) {
    wl_surface_destroy(surface);
    surface = NULL;
  }
  if (buffer) {
    wl_buffer_destroy(buffer);
    buffer = NULL;
  }
  if (display) {
    wl_display_disconnect(display);
    display = NULL;
  }
}

int main(int argc, char *argv[]) {
  fprintf(stderr, "Starting clock widget...\n");

  parse_args(argc, argv);

  // Set up signal handlers
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  // handle SIGTSTP (Ctrl+Z) to clean up before stopping
  struct sigaction sa_tstp;
  sa_tstp.sa_handler = signal_handler;
  sigemptyset(&sa_tstp.sa_mask);
  sa_tstp.sa_flags = 0;
  sigaction(SIGTSTP, &sa_tstp, NULL);

  // Connect to Wayland display
  display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "Failed to connect to Wayland display\n");
    return 1;
  }
  fprintf(stderr, "Connected to Wayland display\n");

  // Get registry
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  wl_display_flush(display);

  if (!compositor || !wl_shm || !layer_shell) {
    fprintf(stderr, "Missing required Wayland interfaces\n");
    fprintf(stderr, "compositor: %p, shm: %p, layer_shell: %p\n",
            (void *)compositor, (void *)wl_shm, (void *)layer_shell);
    return 1;
  }

  // Create surface
  surface = wl_compositor_create_surface(compositor);
  if (!surface) {
    fprintf(stderr, "Failed to create surface\n");
    cleanup();
    return 1;
  }
  fprintf(stderr, "Surface created\n");

  // Create layer surface
  layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, surface, NULL, config.layer, "clock-widget");

  if (!layer_surface) {
    fprintf(stderr, "Failed to create layer surface\n");
    cleanup();
    return 1;
  }
  fprintf(stderr, "Layer surface created\n");

  // Configure layer surface
  zwlr_layer_surface_v1_set_size(layer_surface, config.width, config.height);
  zwlr_layer_surface_v1_set_anchor(layer_surface, config.anchor);
  zwlr_layer_surface_v1_set_margin(layer_surface, config.margin_top,
                                   config.margin_right, config.margin_bottom,
                                   config.margin_left);
  zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, config.height);
  zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener,
                                     NULL);
  // Disable keyboard input
  zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 0);

  // Commit surface
  wl_surface_commit(surface);
  wl_display_roundtrip(display);
  wl_display_flush(display);

  fprintf(stderr, "Surface committed, waiting for configure...\n");

  // Set up frame callback once we're configured
  while (running && !configured && !should_exit) {
    // Use poll() to avoid blocking indefinitely
    struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};

    int ret = poll(&pfd, 1, 100); // 100ms timeout
    if (ret < 0) {
      if (errno == EINTR) {
        // Interrupted by signal
        continue;
      }
      perror("poll");
      break;
    } else if (ret == 0) {
      // Timeout
      continue;
    }

    if (pfd.revents & POLLIN) {
      wl_display_dispatch(display);
    }
    wl_display_flush(display);
  }

  if (!configured) {
    fprintf(stderr, "Exiting before configuration\n");
    cleanup();
    return 1;
  }

  fprintf(stderr, "Configured, setting up frame callback\n");

  // Set up frame callback for animation
  frame_callback = wl_surface_frame(surface);
  static const struct wl_callback_listener listener = {
      .done = frame_callback_handler};
  wl_callback_add_listener(frame_callback, &listener, NULL);
  wl_surface_commit(surface);
  wl_display_flush(display);

  // Main loop
  fprintf(stderr, "Entering main loop\n");
  fprintf(stderr, "Press Ctrl+C to exit\n");

  display_fd = wl_display_get_fd(display);

  while (running && !should_exit) {
    // Check if time has changed
    time_t now = time(NULL);
    if (now != last_drawn_time) {
      fprintf(stderr, "Time changed: %ld -> %ld\n", last_drawn_time, now);
      if (frame_callback) {
        wl_callback_destroy(frame_callback);
        frame_callback = NULL;
      }
      frame_callback = wl_surface_frame(surface);
      wl_callback_add_listener(frame_callback, &listener, NULL);
      wl_surface_commit(surface);
      wl_display_flush(display);
    }

    struct pollfd pfd = {.fd = display_fd, .events = POLLIN};
    long timeout_ms;

    // Calculate timeout based on configuration
    if (config.show_seconds) {
      timeout_ms = 1000;
    } else {
      long seconds_to_next_minute = 60 - (now % 60);
      timeout_ms = seconds_to_next_minute * 1000;
    }

    if (timeout_ms <= 0)
      timeout_ms = 100; // Minimum timeout

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
      if (errno == EINTR) {
        // Interrupted by signal
        continue;
      }
      perror("poll");
      break;
    } else if (ret == 0) {
      // Timeout
      continue;
    }

    if (pfd.revents & POLLIN) {
      wl_display_dispatch(display);
    }

    wl_display_flush(display);
    wl_display_dispatch_pending(display);
  }

  fprintf(stderr, "Exiting...\n");

  cleanup();

  return 0;
}
