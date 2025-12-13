#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX 4096
#define VERSION "0.2.0" // Major.Minor.Patch
#define PATH_MAX_LEN 4096
#define MAX_VIEWERS 10

// Global State Refactoring for Sorting and Directory Management
typedef enum { FILE_IMAGE, FILE_DIR, FILE_PARENT } FileType;
typedef enum { SORT_NAME, SORT_SIZE, SORT_DATE } SortMode;

typedef struct {
  char path[PATH_MAX_LEN];
  char name[PATH_MAX_LEN];
  long size;
  time_t mtime;
  FileType type;
  int stats_fetched; // 0: Not fetched (lazy), 1: Fetched
} FileEntry;

typedef struct {
  char name[32];
  char command[256];
  int priority;
} ViewerOption;

static FileEntry list[MAX];
static int n, sel, top;
static char current_dir[PATH_MAX_LEN] = "";
static char wallsetter[256] = "swaybg"; // feh
static char viewer[256] = "imageviewer";
static SortMode current_sort = SORT_NAME;
static int first_time = 1;

static int scan(const char *p);
static int is_image(const char *filename);
static void draw_menu();
static void set_wallpaper_from_file(const char *file);
static void handle_resize(int sig);
static void set_random_wallpaper();
static void enter_directory();
static void kill_wallpaper_processes();
static void show_preview();
static void save_config();

// Sorting Logic
static const char *get_base_name(const char *full_path) {
  const char *base = strrchr(full_path, '/');
  return (base ? base + 1 : full_path);
}

// Image viewer options
static ViewerOption viewer_options[MAX_VIEWERS] = {
    {"imageviewer", "imageviewer", 100}, // Built-in viewer
    {"sxiv", "sxiv", 90},
    {"imv", "imv", 85},
    {"feh", "feh", 80},
    {"viu", "viu -w %d -h %d", 70},
    {"chafa", "chafa -f sixel", 60},
    {"mpv", "mpv --loop --no-osc --no-border", 50},
    {"qview", "qview", 40},
    {"gpicview", "gpicview", 30},
    {"eog", "eog", 20}};
static int viewer_count = MAX_VIEWERS;

static int is_image(const char *path) {
    char *ext = strrchr(path, '.');
    if (!ext) return 0;

    const char *image_exts[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", NULL};
    for (int i = 0; image_exts[i] != NULL; i++) {
        if (strcasecmp(ext, image_exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// Sort by Name
static int compare_by_name(const void *a, const void *b) {
  const FileEntry *entry_a = (const FileEntry *)a;
  const FileEntry *entry_b = (const FileEntry *)b;

  if (entry_a->type == FILE_PARENT)
    return -1;
  if (entry_b->type == FILE_PARENT)
    return 1;
  if (entry_a->type == FILE_DIR && entry_b->type == FILE_IMAGE)
    return -1;
  if (entry_a->type == FILE_IMAGE && entry_b->type == FILE_DIR)
    return 1;

  // Natural sorting (case-insensitive for files)
  return strcasecmp(entry_a->name, entry_b->name);
}

static void fetch_stats(FileEntry *entry) {
  if (entry->stats_fetched == 1)
    return;

  struct stat st;
  if (lstat(entry->path, &st) < 0) {
    entry->size = 0;
    entry->mtime = 0;
  } else {
    entry->size = st.st_size;
    entry->mtime = st.st_mtime;
  }
  entry->stats_fetched = 1;
}

// Sort by Size (Ascending)
static int compare_by_size(const void *a, const void *b) {
  const FileEntry *fa = (const FileEntry *)a;
  const FileEntry *fb = (const FileEntry *)b;

  fetch_stats((FileEntry *)fa);
  fetch_stats((FileEntry *)fb);

  if (fa->type != fb->type)
    return compare_by_name(a, b);
  if (fa->size > fb->size)
    return 1;
  if (fa->size < fb->size)
    return -1;

  return compare_by_name(a, b); // Tie-breaker by name
}

// Sort by Date Modified (Newest first)
static int compare_by_date(const void *a, const void *b) {
  const FileEntry *entry_a = (const FileEntry *)a;
  const FileEntry *entry_b = (const FileEntry *)b;

  if (entry_a->type != entry_b->type)
    return compare_by_name(a, b);
  if (entry_a->mtime < entry_b->mtime)
    return 1;
  if (entry_a->mtime > entry_b->mtime)
    return -1;
  return compare_by_name(a, b); // Tie-breaker by name
}

static int get_current_comparator(const void *a, const void *b) {
  switch (current_sort) {
  case SORT_NAME:
    return compare_by_name(a, b);
  case SORT_SIZE:
    return compare_by_size(a, b);
  case SORT_DATE:
    return compare_by_date(a, b);
  }
  return 0;
}

static void apply_sort() {
  if (n > 0) {
    qsort(list, n, sizeof(FileEntry), get_current_comparator);
    sel = 0; // Reset selection after sorting
    top = 0;
  }
}

// Path and Config Functions
static void expand_path(char *path) {
  if (path[0] == '~') {
    char *home = getenv("HOME");
    if (home) {
      char expanded[PATH_MAX_LEN];
      snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
      strncpy(path, expanded, PATH_MAX_LEN - 1);
      path[PATH_MAX_LEN - 1] = '\0';
    }
  }
}

static void save_config() {
  char config_path[PATH_MAX_LEN];
  snprintf(config_path, sizeof(config_path), "%s/.layer_config",
           getenv("HOME"));
  FILE *f = fopen(config_path, "w");
  if (f) {
    fprintf(f, "DIR=%s\n", current_dir);   // save default wallpaper directory
    fprintf(f, "SETTER=%s\n", wallsetter); // save wallpaper setter
    fprintf(f, "VIEWER=%s\n", viewer);     // save viewer
    fprintf(f, "SEL=%d\n", sel);           // Save scroll position
    fprintf(f, "SORT=%d\n", current_sort); // Save sort mode
    fclose(f);
  }
}

static void save_last_wallpaper(const char *wallpaper) {
  char last_path[PATH_MAX_LEN];
  snprintf(last_path, sizeof(last_path), "%s/.layer_last_wallpaper",
           getenv("HOME"));
  FILE *f = fopen(last_path, "w");
  if (f) {
    fprintf(f, "%s\n", wallpaper);
    fclose(f);
  }
}

static char *load_last_wallpaper() {
  static char last_wallpaper[PATH_MAX_LEN];
  char last_path[PATH_MAX_LEN];
  snprintf(last_path, sizeof(last_path), "%s/.layer_last_wallpaper",
           getenv("HOME"));
  FILE *f = fopen(last_path, "r");
  if (f) {
    if (fgets(last_wallpaper, sizeof(last_wallpaper), f)) {
      last_wallpaper[strcspn(last_wallpaper, "\n")] = 0;
      fclose(f);
      return last_wallpaper;
    }
    fclose(f);
  }
  return NULL;
}

static void load_config() {
  char config_path[PATH_MAX_LEN];
  snprintf(config_path, sizeof(config_path), "%s/.layer_config",
           getenv("HOME"));

  FILE *f = fopen(config_path, "r");
  if (f) {
    char line[PATH_MAX_LEN];
    int loaded_sel = 0;
    int loaded_sort = 0;

    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "DIR=", 4) == 0) {
        strncpy(current_dir, line + 4, sizeof(current_dir) - 1);
        current_dir[strcspn(current_dir, "\n")] = 0;
        expand_path(current_dir);
      } else if (strncmp(line, "SETTER=", 7) == 0) {
        strncpy(wallsetter, line + 7, sizeof(wallsetter) - 1);
        wallsetter[strcspn(wallsetter, "\n")] = 0;
      } else if (strncmp(line, "VIEWER=", 7) == 0) {
        strncpy(viewer, line + 7, sizeof(viewer) - 1);
        viewer[strcspn(viewer, "\n")] = 0;
      } else if (strncmp(line, "SEL=", 4) == 0) {
        loaded_sel = atoi(line + 4);
      } else if (strncmp(line, "SORT=", 5) == 0) {
        loaded_sort = atoi(line + 5);
      }
    }
    fclose(f);
    first_time = 0;

    sel = (loaded_sel >= 0) ? loaded_sel : 0;
    if (loaded_sort >= SORT_NAME && loaded_sort <= SORT_DATE) {
      current_sort = (SortMode)loaded_sort;
    }
  }
}

// Directory Scanning
static int scan(const char *p) {
  char canonical_dir[PATH_MAX_LEN];
  if (realpath(p, canonical_dir) == NULL) {
    fprintf(stderr, "Error: Could not resolve path %s\n", p);
    return 0;
  }
  strncpy(current_dir, canonical_dir, sizeof(current_dir) - 1);
  current_dir[sizeof(current_dir) - 1] = '\0';

  n = 0;
  DIR *d = opendir(current_dir);
  if (!d) {
    fprintf(stderr, "Error: Cannot open directory %s\n", current_dir);
    return 0;
  }

  if (strcmp(current_dir, "/") != 0 && n < MAX) {
    FileEntry *entry = &list[n++];
    snprintf(entry->path, sizeof(entry->path), "%s", current_dir);
    strcpy(entry->name, "..");
    entry->type = FILE_PARENT;
    entry->stats_fetched = 1;
    entry->size = 0;
    entry->mtime = 0;
  }

  struct dirent *e;
  while ((e = readdir(d)) != NULL && n < MAX) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }

    char full_path[PATH_MAX_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", canonical_dir, e->d_name);

    struct stat st;
    if (lstat(full_path, &st) < 0) {
      if (e->d_name[0] != '.') {
        fprintf(stderr, "Error stating file %s: %s\n", full_path,
                strerror(errno));
      }
      continue; // Skip inaccessible files
    }

    FileEntry *entry = &list[n];
    strncpy(entry->path, full_path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    strncpy(entry->name, e->d_name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';

    if (S_ISDIR(st.st_mode)) {
      entry->type = FILE_DIR;
      entry->size = 0;            // Size is irrelevant for directories
      entry->mtime = st.st_mtime; // for directory sorting
      entry->stats_fetched = 1;
      n++;
    } else if (is_image(full_path)) {
      entry->type = FILE_IMAGE;
      entry->size = 0;
      entry->mtime = 0;
      entry->stats_fetched = 0;
      n++;
    }
    // Skip non-image, non-directory files
  }
  closedir(d);

  apply_sort();

  if (sel >= n)
    sel = (n > 0) ? n - 1 : 0;
  if (sel < 0)
    sel = 0;

  return n;
}

// UI
static const char *get_sort_name(SortMode mode) {
  switch (mode) {
  case SORT_NAME:
    return "NAME";
  case SORT_SIZE:
    return "SIZE";
  case SORT_DATE:
    return "DATE";
  }
  return "UNKNOWN";
}

static const char *format_size(long bytes) {
  static char buffer[32];
  const char *units[] = {"B", "KB", "MB", "GB"};
  int unit = 0;
  double size = bytes;

  while (size >= 1024 && unit < 3) {
    size /= 1024;
    unit++;
  }

  if (unit == 0) {
    snprintf(buffer, sizeof(buffer), "%ld %s", bytes, units[unit]);
  } else {
    snprintf(buffer, sizeof(buffer), "%.1f %s", size, units[unit]);
  }
  return buffer;
}

static void draw_menu() {
  clear();
  mvprintw(0, 0,
           "[j/k or Arrow Keys] Navigate | [Enter] Select/Set | [r] Random | "
           "[s] Sort: %s | [F1] Config | [q] Quit",
           get_sort_name(current_sort));
  mvprintw(1, 0, "Dir: %s | Setter: %s | Viewer: %s", current_dir, wallsetter,
           viewer);

  if (n == 0) {
    mvprintw(3, 0, "No images or subdirectories found in: %s", current_dir);
    mvprintw(5, 0, "Press 'F1' to change directory/config.");
  } else {
    int max_display = LINES - 3;
    for (int i = top; i < n && i < top + max_display; i++) {
      FileEntry *entry = &list[i];
      int y = i - top + 2;

      if (i == sel) {
        attron(A_REVERSE);
      }

      if (entry->type == FILE_DIR || entry->type == FILE_PARENT) {
        attron(A_BOLD);
        mvprintw(y, 0, "%s %s/", i == sel ? ">" : " ", entry->name);
        attroff(A_BOLD);
      } else {
        // Display file size/date based on sort mode
        char details[100] = "";
        if (current_sort == SORT_SIZE) {
          snprintf(details, sizeof(details), " (%s)", format_size(entry->size));
        } else if (current_sort == SORT_DATE) {
          char time_str[30];
          strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M",
                   localtime(&entry->mtime));
          snprintf(details, sizeof(details), " (%s)", time_str);
        }
        mvprintw(y, 0, "%s %s%s", i == sel ? ">" : " ", entry->name, details);
      }

      if (i == sel) {
        attroff(A_REVERSE);
      }
    }
  }
  refresh();
}

// --- Action Functions
static void notify_wallpaper_set(const char *file) {
  char command[PATH_MAX_LEN + 256];
  const char *filename = get_base_name(file);

  snprintf(command, sizeof(command),
           "if command -v dunstify >/dev/null 2>&1; then "
           "dunstify -h string:x-dunst-stack-tag:sheet-wp -t 3000 \"Wallpaper "
           "Set\" \"\\\"%s\\\" set via %s\"; "
           "elif command -v notify-send >/dev/null 2>&1; then "
           "notify-send -t 3000 \"Wallpaper Set\" \"\\\"%s\\\" set via %s\"; "
           "fi",
           filename, wallsetter, filename, wallsetter);

  // Fork process to run notification command and detach it
  if (fork() == 0) {
    setsid();
    int ret = system(command);
    (void)ret;
    exit(0);
  }
}

static int detect_available_viewers(char *available_viewers[], int max_count) {
  int count = 0;
  char *path_env = getenv("PATH");

  if (!path_env)
    return 0;

  for (int i = 0; i < viewer_count; i++) {
    char command[256];
    snprintf(command, sizeof(command), "command -v %s >/dev/null 2>&1",
             viewer_options[i].name);

    if (system(command) == 0) {
      available_viewers[count] = viewer_options[i].name;
      count++;
      if (count >= max_count)
        break;
    }
  }

  return count;
}

static int imageviewer_exists() {
  // Check binary in current dir
  if (access("./imageviewer", X_OK) == 0) {
    return 1;
  }

  // Check in build directory
  if (access("./build/imageviewer", X_OK) == 0) {
    return 1;
  }

  // Check executables in layer dir
  char layer_path[4096];
  if (readlink("/proc/self/exe", layer_path, sizeof(layer_path) - 1) != -1) {
    char *last_slash = strrchr(layer_path, '/');
    if (last_slash) {
      *last_slash = '\0';
      char full_path[4096];
      snprintf(full_path, sizeof(full_path), "%s/imageviewer", layer_path);
      full_path[sizeof(full_path) - 1] = '\0';
      if (access(full_path, X_OK) == 0) {
        return 1;
      }
      snprintf(full_path, sizeof(full_path), "%s/build/imageviewer",
               layer_path);
      full_path[sizeof(full_path) - 1] = '\0';
      if (access(full_path, X_OK) == 0) {
        return 1;
      }
    }
  }

  // Check in PATH
  char *path = getenv("PATH");
  if (path) {
    char *path_copy = strdup(path);
    char *dir = strtok(path_copy, ":");
    while (dir) {
      char full_path[4096];
      snprintf(full_path, sizeof(full_path), "%s/imageviewer", dir);
      full_path[sizeof(full_path) - 1] = '\0';
      if (access(full_path, X_OK) == 0) {
        free(path_copy);
        return 1;
      }
      dir = strtok(NULL, ":");
    }
    free(path_copy);
  }

  return 0;
}

static void show_preview() {
  if (n == 0 || list[sel].type != FILE_IMAGE)
    return;

  def_prog_mode();
  endwin();

  const char *file = list[sel].path;
  printf("\nOpening image: %s\n", file);
  fflush(stdout);

  int viewer_launched = 0;

  // First, try the configured viewer
  if (strcmp(viewer, "imageviewer") == 0 && imageviewer_exists()) {
    printf("Trying built-in imageviewer...\n");
    fflush(stdout);

    char imageviewer_path[4096] = "imageviewer"; // default

    if (access("./build/imageviewer", X_OK) == 0) {
      strcpy(imageviewer_path, "./build/imageviewer");
    } else if (access("./imageviewer", X_OK) == 0) {
      strcpy(imageviewer_path, "./imageviewer");
    }

    // Calculate preview size based on terminal size
    struct winsize ws;
    int term_width = 80;
    int term_height = 24;

    if (isatty(STDOUT_FILENO)) {
      if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1) {
        term_width = ws.ws_col;
        term_height = ws.ws_row;
      }
    }

    // Use about 70% of terminal size, but max 1000x700
    int preview_width = (term_width * 7) / 10 * 10;
    int preview_height = (term_height * 7) / 10 * 20;

    if (preview_width > 1000)
      preview_width = 1000;
    if (preview_height > 700)
      preview_height = 700;
    if (preview_width < 400)
      preview_width = 400;
    if (preview_height < 300)
      preview_height = 300;

    char command[PATH_MAX_LEN + 100];
    snprintf(command, sizeof(command), "%s \"%s\"", imageviewer_path, file);

    printf("Running: %s\n", command);
    fflush(stdout);

    int ret = system(command);
    if (ret != 32512 && ret != 127 && ret != -1) {
      viewer_launched = 1;
      printf("imageviewer exited with status %d\n", WEXITSTATUS(ret));
    } else {
      printf("system() returned error: %d\n", ret);
    }
  }

  // If not launched yet, try other viewers
  /* if (!viewer_launched) { */
  /*   printf("\n[ERROR] Could not launch imageviewer!\n"); */
  /*   printf("Imageviewer paths tried:\n"); */
  /*   for (int i = 0; paths[i] != NULL; i++) { */
  /*     printf("  %s\n", paths[i]); */
  /*   } */
  /*   printf("\nMake sure imageviewer is built and accessible.\n"); */
  /* } */

  printf("\nPress Enter to return to layer...");
  fflush(stdout);

  // Clear input buffer
  int c;
  while ((c = getchar()) != '\n' && c != EOF)
    ;

  reset_prog_mode();
  refresh();
}

static void kill_wallpaper_processes() {
  pid_t pid = fork();
  if (pid == 0) {
    execlp("pkill", "pkill", "-9", "feh", NULL);
    exit(0);
  }
  waitpid(pid, NULL, 0);

  pid = fork();
  if (pid == 0) {
    execlp("pkill", "pkill", "-9", "swaybg", NULL);
    exit(0);
  }
  waitpid(pid, NULL, 0);
}

static void set_wallpaper_from_file(const char *file) {
  if (strlen(file) == 0)
    return;
  kill_wallpaper_processes();

  pid_t pid = fork();
  if (pid == 0) {
    // Child process: Detach and execute wallpaper setter
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull != STDOUT_FILENO && devnull != STDERR_FILENO) {
        close(devnull);
      }
    }

    setsid();

    if (strcmp(wallsetter, "swaybg") == 0) {
      char *args[] = {"swaybg", "-m", "fill", "-i", (char *)file, NULL};
      execvp("swaybg", args);
    } else {
      char *args[] = {"feh", "--bg-scale", (char *)file, NULL};
      execvp("feh", args);
    }
    exit(1);
  } else if (pid > 0) {
    save_last_wallpaper(file);
    // Parent
    if (!isendwin()) { // draw only if ncurses is active
      mvprintw(LINES - 1, 0, "Wallpaper set: %s", get_base_name(file));
      clrtoeol();
      refresh();
    }
    // Send desktop notification
    notify_wallpaper_set(file);
  }
}

static void set_wallpaper() {
  if (n == 0 || list[sel].type != FILE_IMAGE)
    return;
  set_wallpaper_from_file(list[sel].path);
}

static void set_random_wallpaper() {
  if (n == 0)
    return;

  int image_indices[MAX];
  int image_count = 0;
  for (int i = 0; i < n; i++) {
    if (list[i].type == FILE_IMAGE) {
      image_indices[image_count++] = i;
    }
  }

  if (image_count == 0) {
    if (!isendwin()) {
      mvprintw(LINES - 1, 0, "No images available for random selection.");
      clrtoeol();
      refresh();
    } else {
      fprintf(stderr, "No images found for random selection.\n");
    }
    return;
  }

  // Seed rand with current time
  srand(time(NULL) * getpid());
  int random_index_in_list = image_indices[rand() % image_count];

  if (!isendwin()) {
    sel = random_index_in_list;
    int max_display = LINES - 3;
    if (sel < top || sel >= top + max_display) {
      top = sel - (max_display / 2) + 1;
      if (top < 0)
        top = 0;
    }
    draw_menu();
  }

  set_wallpaper_from_file(list[random_index_in_list].path);
}

static void restore_last_wallpaper() {
  char *last_wallpaper = load_last_wallpaper();
  if (last_wallpaper) {
    struct stat st;
    if (stat(last_wallpaper, &st) == 0) {
      set_wallpaper_from_file(last_wallpaper);
    } else {
      fprintf(stderr, "Last wallpaper file not found: %s\n", last_wallpaper);
    }
  } else {
    fprintf(stderr, "No last wallpaper saved.\n");
  }
}

static void set_wallpaper_dmenu() {
  if (n == 0) {
    fprintf(stderr, "No images found in directory: %s\n", current_dir);
    return;
  }

  char temp_file[] = "/tmp/layer_dmenu_XXXXXX";
  int temp_fd = mkstemp(temp_file);
  if (temp_fd == -1) {
    perror("mkstemp");
    return;
  }

  char input_file[] = "/tmp/layer_input_XXXXXX";
  int input_fd = mkstemp(input_file);
  if (input_fd == -1) {
    close(temp_fd);
    unlink(temp_file);
    perror("mkstemp");
    return;
  }

  for (int i = 0; i < n; i++) {
    if (list[i].type == FILE_IMAGE) {
      dprintf(input_fd, "%s\n", list[i].name);
    }
  }
  close(input_fd);

  // Run dmenu
  char command[512];
  snprintf(command, sizeof(command),
           "cat '%s' | dmenu -l 20 -p 'Select wallpaper:' > '%s'", input_file,
           temp_file);

  int ret = system(command);
  if (ret == -1) {
    perror("system");
    unlink(temp_file);
    unlink(input_file);
    return;
  }

  // Read selected filename
  FILE *fp = fopen(temp_file, "r");
  char selected[256] = "";
  if (fp) {
    if (fgets(selected, sizeof(selected), fp)) {
      selected[strcspn(selected, "\n")] = '\0';
    }
    fclose(fp);
  }

  // Clean up temp files
  unlink(temp_file);
  unlink(input_file);

  // Find and set the selected wallpaper
  if (selected[0]) {
    for (int i = 0; i < n; i++) {
      if (list[i].type == FILE_IMAGE && strcmp(list[i].name, selected) == 0) {
        set_wallpaper_from_file(list[i].path);
        break;
      }
    }
  }
}

static void enter_directory() {
  if (n == 0)
    return;

  if (list[sel].type == FILE_DIR || list[sel].type == FILE_PARENT) {
    char new_dir[PATH_MAX_LEN];

    if (list[sel].type == FILE_PARENT) {
      char *last_slash = strrchr(current_dir, '/');
      if (last_slash) {
        if (strcmp(current_dir, "/") == 0) {
          return;
        }
        *last_slash = '\0';
        if (strlen(current_dir) == 0) {
          strcpy(current_dir, "/");
        }
      }
      strcpy(new_dir, current_dir);
    } else {
      strncpy(new_dir, list[sel].path, sizeof(new_dir) - 1);
      new_dir[sizeof(new_dir) - 1] = '\0';
    }

    if (scan(new_dir) > 0) {
      strcpy(current_dir, new_dir);
      save_config();
      draw_menu();
    } else {
      scan(current_dir);
      if (!isendwin()) {
        mvprintw(LINES - 1, 0, "Error opening directory");
        clrtoeol();
        refresh();
        getch();
        draw_menu();
      }
    }
  } else if (list[sel].type == FILE_IMAGE) {
    set_wallpaper();
  }
}

static void change_config() {
  def_prog_mode();
  endwin();

  char new_dir[PATH_MAX_LEN];
  printf("\nCurrent directory: %s\n", current_dir);
  printf("Enter new directory (empty to keep): ");
  fflush(stdout);
  if (fgets(new_dir, sizeof(new_dir), stdin) == NULL) {
    new_dir[0] = '\0';
  }
  new_dir[strcspn(new_dir, "\n")] = 0;
  if (strlen(new_dir) > 0) {
    expand_path(new_dir);
    char temp_dir[PATH_MAX_LEN];
    strncpy(temp_dir, new_dir, sizeof(temp_dir) - 1);
    if (scan(temp_dir) > 0) { // Test scan success before updating config
      strncpy(current_dir, temp_dir, sizeof(current_dir) - 1);
    } else {
      printf(
          "Error: New directory '%s' is invalid or empty. Keeping current.\n",
          temp_dir);
    }
  }

  char new_setter[256];
  printf("\nCurrent wallpaper setter: %s\n", wallsetter);
  printf("Enter new setter (feh or swaybg, empty to keep): ");
  fflush(stdout);
  if (fgets(new_setter, sizeof(new_setter), stdin) == NULL) {
    new_setter[0] = '\0';
  }
  new_setter[strcspn(new_setter, "\n")] = 0;
  if (strlen(new_setter) > 0 &&
      (strcmp(new_setter, "feh") == 0 || strcmp(new_setter, "swaybg") == 0))
    strncpy(wallsetter, new_setter, sizeof(wallsetter) - 1);

  /* char new_viewer[256]; */
  /* printf("\nCurrent image viewer: %s\n", viewer); */
  /* printf("Enter new viewer (./imageviewer, sxiv, viu, etc.): "); */
  /* fflush(stdout); */
  /* if (fgets(new_viewer, sizeof(new_viewer), stdin) == NULL) { */
  /*   new_viewer[0] = '\0'; */
  /* } */
  /* new_viewer[strcspn(new_viewer, "\n")] = 0; */
  /* if (strlen(new_viewer) > 0) */
  /*   strncpy(viewer, new_viewer, sizeof(viewer) - 1); */
  printf("\nCurrent image viewer: %s\n", viewer);
  printf("\nAvailable viewer options:\n");

  char *available_viewers[16];
  int avail_count = detect_available_viewers(available_viewers, 16);

  if (avail_count == 0) {
    printf("  No image viewers found in PATH!\n");
  } else {
    printf("  Found: ");
    for (int i = 0; i < avail_count; i++) {
      printf("%s ", available_viewers[i]);
    }
    printf("\n");
  }

  if (imageviewer_exists()) {
    printf("  imageviewer (built-in) is available\n");
  }

  printf("\nEnter new viewer (or 'auto' for auto-detection): ");

  char new_viewer[256];
  fflush(stdout);
  if (fgets(new_viewer, sizeof(new_viewer), stdin) == NULL) {
    new_viewer[0] = '\0';
  }
  new_viewer[strcspn(new_viewer, "\n")] = 0;

  if (strlen(new_viewer) > 0) {
    // Validate viewer exists if not "auto"
    if (strcmp(new_viewer, "auto") == 0) {
      strncpy(viewer, new_viewer, sizeof(viewer) - 1);
    } else {
      // Check if viewer exists
      char test_cmd[512];
      snprintf(test_cmd, sizeof(test_cmd), "command -v %s >/dev/null 2>&1",
               new_viewer);

      if (system(test_cmd) == 0 ||
          (strcmp(new_viewer, "imageviewer") == 0 && imageviewer_exists())) {
        strncpy(viewer, new_viewer, sizeof(viewer) - 1);
      } else {
        printf("Warning: Viewer '%s' not found. Keeping '%s'\n", new_viewer,
               viewer);
      }
    }
  }

  save_config();
  n = scan(current_dir);
  sel = 0;
  top = 0;

  printf("\nConfiguration updated. Press Enter to continue...");
  fflush(stdout);
  getchar();

  reset_prog_mode();
  refresh();
}

static void first_time_setup() {
  def_prog_mode();
  endwin();

  printf("Welcome to Layer (v%s)!\n\n", VERSION);

  // Auto-detect session type
  char *xdg_session = getenv("XDG_SESSION_TYPE");
  char *wayland_display = getenv("WAYLAND_DISPLAY");
  int is_wayland = (xdg_session && strcasecmp(xdg_session, "wayland") == 0) ||
                   wayland_display;

  if (is_wayland) {
    printf("Detected: Wayland session (using swaybg as default)\n");
    strcpy(wallsetter, "swaybg");
  } else {
    printf("Detected: X11 session (using feh as default)\n");
    strcpy(wallsetter, "feh");
  }

  // Set default viewer to imageviewer
  if (!imageviewer_exists()) {
    printf("Note: The image viewer is not built yet.\n");
  }
  strcpy(viewer, "imageviewer");

  char default_dir[PATH_MAX_LEN];
  snprintf(default_dir, sizeof(default_dir), "%s/Pictures", getenv("HOME"));

  char dir_choice[10];
  printf("Use default directory (%s)? (y/n): ", default_dir);
  fflush(stdout);
  if (fgets(dir_choice, sizeof(dir_choice), stdin) &&
      (dir_choice[0] == 'y' || dir_choice[0] == 'Y')) {
    strcpy(current_dir, default_dir);
  } else {
    printf("Enter directory with images: ");
    fflush(stdout);
    if (fgets(current_dir, sizeof(current_dir), stdin) == NULL) {
      current_dir[0] = '\0';
    }
    current_dir[strcspn(current_dir, "\n")] = 0;
    expand_path(current_dir);
  }

  char setter_choice[10];
  printf("\n[1] swaybg (Wayland)\n[2] feh (X11)\n");
  printf("Choose wallpaper setter (1/2): ");
  fflush(stdout);
  if (fgets(setter_choice, sizeof(setter_choice), stdin)) {
    if (setter_choice[0] == '1')
      strcpy(wallsetter, "swaybg");
    else
      strcpy(wallsetter, "feh");
  }

  save_config();
  first_time = 0;

  printf("\nUsing %s as wallpaper setter and directory %s.\n", wallsetter,
         current_dir);
  printf("\nPress any key to launch...");
  fflush(stdout);
  getchar();

  reset_prog_mode();
  refresh();
}

static void print_help() {
  printf("Layer - Terminal wallpaper selector\n");
  printf("Version: %s\n\n", VERSION);
  printf("Usage: layer [OPTION] [DIRECTORY]\n\n");
  printf("Options:\n");
  printf("  -h, --help     Display this help message\n");
  printf("  -v, --version  Display version information\n");
  printf("  -r, --random   Set a random wallpaper from the configured "
         "directory and exit\n");
  printf("  -i VIEWER      Set default image viewer (e.g., sxiv, viu, "
         "./imageviewer)\n");
  printf("  -m, --dmenu    Launch dmenu for wallpaper selection\n");
  printf(
      "\nIf DIRECTORY is provided, it will be set as the image directory.\n");
  printf(
      "Otherwise, the program starts with the saved or default directory.\n");
}

static void print_version() { printf("layer version %s\n", VERSION); }

// Terminal Resize Handler
static void handle_resize(int sig) {
  (void)sig;
  if (!isendwin()) {
    endwin();
    refresh();
    clear();
    draw_menu();
  }
}

static void ncurses_exit_handler(int sig) {
  (void)sig;
  if (!isendwin()) {
    endwin();
  }
  exit(0);
}

int main(int argc, char **argv) {
  signal(SIGINT, ncurses_exit_handler);
  signal(SIGTERM, ncurses_exit_handler);
  signal(SIGWINCH, handle_resize);

  int dmenu_mode = 0;

  load_config();

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help();
      return 0;
    } else if (strcmp(argv[i], "--version") == 0 ||
               strcmp(argv[i], "-v") == 0) {
      print_version();
      return 0;
    } else if (strcmp(argv[i], "--restore") == 0 ||
               strcmp(argv[i], "-R") == 0) {
      restore_last_wallpaper();
      return 0;
    } else if (strcmp(argv[i], "--random") == 0 || strcmp(argv[i], "-r") == 0) {
      if (strlen(current_dir) == 0) {
        snprintf(current_dir, sizeof(current_dir), "%s/Pictures",
                 getenv("HOME"));
      }
      n = scan(current_dir);
      set_random_wallpaper();
      return 0;
    } else if (strcmp(argv[i], "--dmenu") == 0 || strcmp(argv[i], "-m") == 0) {
      dmenu_mode = 1;
    } else if (argv[i][0] != '-') {
      char temp_dir[PATH_MAX_LEN];
      strncpy(temp_dir, argv[i], sizeof(temp_dir) - 1);
      expand_path(temp_dir);
      if (scan(temp_dir) > 0) {
        strncpy(current_dir, temp_dir, sizeof(current_dir) - 1);
        save_config();
      } else {
        fprintf(stderr,
                "Error: Directory %s not found or is empty. Using current "
                "saved directory.\n",
                temp_dir);
      }
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_help();
      return 1;
    }
  }

  if (dmenu_mode) {
    if (strlen(current_dir) == 0) {
      snprintf(current_dir, sizeof(current_dir), "%s/Pictures", getenv("HOME"));
    }
    n = scan(current_dir);

    if (n == 0) {
      fprintf(stderr, "No images found in directory: %s\n", current_dir);
      return 1;
    }
    set_wallpaper_dmenu();
    return 0;
  }

  // initialize ncurses if not in dmenu mode
  if (first_time && argc < 2) {
    first_time_setup();
  }
  if (strlen(current_dir) == 0) {
    snprintf(current_dir, sizeof(current_dir), "%s/Pictures", getenv("HOME"));
  }

  n = scan(current_dir);

  // ncurses initialization
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);

  if (sel >= n)
    sel = (n > 0) ? n - 1 : 0;
  if (sel < 0)
    sel = 0;

  int max_display = LINES - 3;
  if (sel >= max_display) {
    top = sel - (max_display / 2);
  } else {
    top = 0;
  }

  // Initiate draw
  draw_menu();

  if (dmenu_mode) {
    if (n == 0) {
      fprintf(stderr, "No images found in directory: %s\n", current_dir);
      return 1;
    }
    set_wallpaper_dmenu();
    return 0;
  }

  int ch;
  while (1) {
    ch = getch();
    if (ch == 'q' || ch == 'Q')
      break;

    max_display = LINES - 3;

    if (ch == KEY_DOWN || ch == 'j') {
      if (sel + 1 < n) {
        sel++;
        if (sel >= top + max_display)
          top++;
        draw_menu();
      }
    } else if (ch == KEY_UP || ch == 'k') {
      if (sel > 0) {
        sel--;
        if (sel < top)
          top--;
        draw_menu();
      }
    } else if (ch == '\n' || ch == KEY_RIGHT || ch == 'l') {
      enter_directory();
    } else if (ch == KEY_LEFT || ch == 'h') {
      for (int i = 0; i < n; i++) {
        if (list[i].type == FILE_PARENT) {
          sel = i;
          enter_directory();
          break;
        }
      }
    } else if (ch == 'v' && n > 0 && list[sel].type == FILE_IMAGE) {
      show_preview();
      draw_menu();
    } else if (ch == 'r') {
      set_random_wallpaper();
    } else if (ch == 's') {
      current_sort = (current_sort + 1) % 3;
      apply_sort();
      draw_menu();
    } else if (ch == 'K') {
      kill_wallpaper_processes();
      mvprintw(LINES - 1, 0, "Wallpaper killed");
      clrtoeol();
      refresh();
    } else if (ch == KEY_F(1)) {
      change_config();
      draw_menu();
    } else if (ch == 'd') {
      change_config();
      draw_menu();
    } else if (ch == 'm') {
      set_wallpaper_dmenu();
      draw_menu();
    }
  }

  save_config();
  endwin();
  return 0;
}
