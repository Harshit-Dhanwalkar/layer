#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX 4096
#define VERSION "0.1"
static char list[MAX][4096];
static int n, sel, top;
static char current_dir[4096] = "";
static char wallsetter[256] = "swaybg"; // feh
static char imageviewer_cmd[256] = "./imageviewer";
static int first_time = 1;

static void clean_filepath(char *path);
static void set_wallpaper_from_file(const char *file);

static int compare(const void *a, const void *b) {
  const char *file_a = strrchr(*(char (*)[4096])a, '/') + 1;
  const char *file_b = strrchr(*(char (*)[4096])b, '/') + 1;
  return strcmp(file_a, file_b);
}

static void expand_path(char *path) {
  if (path[0] == '~') {
    char *home = getenv("HOME");
    if (home) {
      char expanded[4096];
      snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
      strcpy(path, expanded);
    }
  }
}

static void save_config() {
  char config_path[4096];
  snprintf(config_path, sizeof(config_path), "%s/.layer_config",
           getenv("HOME"));
  FILE *f = fopen(config_path, "w");
  if (f) {
    fprintf(f, "DIR=%s\n", current_dir);
    fprintf(f, "SETTER=%s\n", wallsetter);
    fprintf(f, "VIEWER=%s\n", imageviewer_cmd);
    fclose(f);
  }
}

static void save_last_wallpaper(const char *wallpaper) {
  char last_path[4096];
  snprintf(last_path, sizeof(last_path), "%s/.layer_last_wallpaper",
           getenv("HOME"));
  FILE *f = fopen(last_path, "w");
  if (f) {
    fprintf(f, "%s\n", wallpaper);
    fclose(f);
  }
}

static char *load_last_wallpaper() {
  static char last_wallpaper[4096];
  char last_path[4096];
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
  char config_path[4096];
  snprintf(config_path, sizeof(config_path), "%s/.layer_config",
           getenv("HOME"));
  FILE *f = fopen(config_path, "r");
  if (f) {
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "DIR=", 4) == 0) {
        strcpy(current_dir, line + 4);
        current_dir[strcspn(current_dir, "\n")] = 0;
        expand_path(current_dir);
      } else if (strncmp(line, "SETTER=", 7) == 0) {
        strcpy(wallsetter, line + 7);
        wallsetter[strcspn(wallsetter, "\n")] = 0;
      } else if (strncmp(line, "VIEWER=", 7) == 0) {
        strcpy(imageviewer_cmd, line + 7);
        imageviewer_cmd[strcspn(imageviewer_cmd, "\n")] = 0;
      }
    }
    fclose(f);
    first_time = 0;
  } else {
    char *xdg_session = getenv("XDG_SESSION_TYPE");
    char *wayland_display = getenv("WAYLAND_DISPLAY");

    if ((xdg_session && strcasecmp(xdg_session, "wayland") == 0) ||
        wayland_display) {
      strcpy(wallsetter, "swaybg");
    } else {
      strcpy(wallsetter, "feh");
    }
  }
}

static int scan(const char *p) {
  n = 0;
  DIR *d = opendir(p);
  if (!d) {
    return 0;
  }
  struct dirent *e;
  while ((e = readdir(d)) && n < MAX) {
    char *x = strrchr(e->d_name, '.');
    if (x && (!strcasecmp(x, ".jpg") || !strcasecmp(x, ".jpeg") ||
              !strcasecmp(x, ".png") || !strcasecmp(x, ".gif") ||
              !strcasecmp(x, ".bmp") || !strcasecmp(x, ".webp"))) {
      size_t needed = snprintf(NULL, 0, "%s/%s", p, e->d_name);
      if (needed < sizeof(list[n])) {
        snprintf(list[n], sizeof(list[n]), "%s/%s", p, e->d_name);
        clean_filepath(list[n]);
        n++;
      } else {
        fprintf(stderr, "Warning: Path too long, skipping: %s/%s\n", p,
                e->d_name);
      }
    }
  }
  closedir(d);
  qsort(list, n, 4096, compare);
  return n;
}

static void draw() {
  clear();
  mvprintw(0, 0, "Directory: %s | Setter: %s | Viewer: %s", current_dir,
           wallsetter, imageviewer_cmd);
  mvprintw(1, 0,
           "d:Change dir | Enter:Set wallpaper | v:View| m:Dmenu | k:Kill wallpaper | "
           "q:Quit");

  if (n == 0) {
    mvprintw(3, 0, "No images found in directory");
    mvprintw(4, 0, "Press d to change directory");
  } else {
    int max_display = LINES - 3;
    for (int i = top; i < n && i < top + max_display; i++) {
      char *filename = strrchr(list[i], '/') + 1;
      char display_name[256];
      strncpy(display_name, filename, sizeof(display_name) - 1);
      display_name[sizeof(display_name) - 1] = '\0';

      /* if ((int)strlen(display_name) > COLS - 10) { */
      if (strlen(display_name) > (size_t)(COLS - 10)) {
        display_name[COLS - 13] = '.';
        display_name[COLS - 12] = '.';
        display_name[COLS - 11] = '.';
        display_name[COLS - 10] = '\0';
      }

      mvprintw(i - top + 2, 0, "%s %s", i == sel ? ">" : " ", display_name);
    }
    if (n > max_display) {
      mvprintw(LINES - 1, 0, "... %d more images", n - (top + max_display));
    }
  }
  refresh();
}

static int imageviewer_exists() {
  if (access("./imageviewer", X_OK) == 0) {
    return 1;
  }

  char layer_path[4096];
  if (readlink("/proc/self/exe", layer_path, sizeof(layer_path) - 1) != -1) {
    char *last_slash = strrchr(layer_path, '/');
    if (last_slash) {
      *last_slash = '\0';
      char full_path[4096];
      snprintf(full_path, sizeof(full_path), "%s/imageviewer", layer_path);
      if (access(full_path, X_OK) == 0) {
        return 1;
      }
    }
  }

  char *path = getenv("PATH");
  if (path) {
    char *path_copy = strdup(path);
    char *dir = strtok(path_copy, ":");
    while (dir) {
      char full_path[4096];
      snprintf(full_path, sizeof(full_path), "%s/imageviewer", dir);
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

static void show() {
  if (n == 0)
    return;

  if (strcmp(imageviewer_cmd, "./imageviewer") == 0 && !imageviewer_exists()) {
    mvprintw(LINES - 1, 0,
             "Error: imageviewer not found. Build it with 'make'");
    clrtoeol();
    refresh();
    getch();
    return;
  }

  def_prog_mode();
  endwin();

  pid_t pid = fork();
  if (pid == 0) {
    char clean_path[4096];
    strncpy(clean_path, list[sel], sizeof(clean_path) - 1);
    clean_path[sizeof(clean_path) - 1] = '\0';
    clean_filepath(clean_path);

    // Child process
    if (strcmp(imageviewer_cmd, "./imageviewer") == 0) {
      // Use built-in imageviewer
      char *args[] = {imageviewer_cmd, clean_path, NULL};
      execvp(args[0], args);
      // If built-in fails, try from PATH
      char *args2[] = {"imageviewer", clean_path, NULL};
      execvp("imageviewer", args2);
      perror("imageviewer");
    } else {
      // Use external image viewer (sxiv, viu, etc.)
      char *args[] = {imageviewer_cmd, clean_path, NULL};
      execvp(imageviewer_cmd, args);
      perror(imageviewer_cmd);
    }

    // Last resort: try xdg-open
    fprintf(stderr, "Falling back to xdg-open...\n");
    char *args3[] = {"xdg-open", clean_path, NULL};
    execvp("xdg-open", args3);
    exit(1);
  } else if (pid > 0) {
    // Parent process
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
      int exit_status = WEXITSTATUS(status);
      if (exit_status != 0) {
        fprintf(stderr, "Image viewer exited with status %d\n", exit_status);
      }
    }
  }

  printf("\nPress Enter to continue...");
  fflush(stdout);

  // Clear any remaining input
  int ch;
  while ((ch = getchar()) != '\n' && ch != EOF) {
    // Discard all characters including the newline
  }

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

static void clean_filepath(char *path) {
  if (!path || !*path)
    return;

  char *src = path;
  char *dst = path;
  int last_was_slash = 0;

  while (*src) {
    if (*src == '/') {
      if (!last_was_slash) {
        *dst++ = *src;
        last_was_slash = 1;
      }
    } else {
      *dst++ = *src;
      last_was_slash = 0;
    }
    src++;
  }
  *dst = '\0';

  // Remove trailing slash if present (except for root '/')
  if (dst > path + 1 && *(dst - 1) == '/') {
    *(dst - 1) = '\0';
  }
}

static void set_wallpaper_from_file(const char *file) {
  // Remove double slashes if any
  char clean_path[4096];
  strncpy(clean_path, file, sizeof(clean_path) - 1);
  clean_path[sizeof(clean_path) - 1] = '\0';
  clean_filepath(clean_path);
  /* printf("DEBUG: Setting wallpaper: %s\n", clean_path); */
  /* printf("DEBUG: Using setter: %s\n", wallsetter); */

  struct stat st;
  if (stat(clean_path, &st) != 0) {
    fprintf(stderr, "Error: File not found: %s\n", clean_path);
    if (isatty(STDOUT_FILENO)) {
      mvprintw(LINES - 1, 0, "Error: File not found!");
      clrtoeol();
      refresh();
    }
    return;
  }

  kill_wallpaper_processes();
  usleep(100000);

  pid_t pid = fork();
  if (pid == 0) {
    setsid();

    // Close all file descriptors
    int maxfd = sysconf(_SC_OPEN_MAX);
    for (int fd = 0; fd < maxfd; fd++) {
      close(fd);
    }

    // Child process
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > 2)
        close(devnull);
    }
    // Detach from terminal
    setsid();

    if (strcmp(wallsetter, "feh") == 0) {
      char *args[] = {"feh", "--bg-scale", clean_path, NULL};
      execvp("feh", args);
      perror("feh failed");
    } else {
      char *args[] = {"swaybg", "-m", "fill", "-i", clean_path, NULL};
      execvp("swaybg", args);
      perror("swaybg failed");
    }
    exit(1);
  } else if (pid > 0) {
    // Parent process - daemonized
    /* waitpid(pid, NULL, 0); */
    save_last_wallpaper(clean_path);
    usleep(50000); // 50ms

    if (isatty(STDOUT_FILENO)) {
      char *filename = strrchr(clean_path, '/');
      if (filename) {
        filename++;
      } else {
        filename = clean_path;
      }
    } else {
      printf("Wallpaper set: %s\n", clean_path);
    }
  }
}

static void set_wallpaper() {
  if (n == 0)
    return;
  set_wallpaper_from_file(list[sel]);
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
    char *filename = strrchr(list[i], '/') + 1;
    dprintf(input_fd, "%s\n", filename);
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
      char *filename = strrchr(list[i], '/') + 1;
      if (strcmp(filename, selected) == 0) {
        set_wallpaper_from_file(list[i]);
        break;
      }
    }
  }
}

static void change_config() {
  def_prog_mode();
  endwin();

  printf("\nCurrent directory: %s\n", current_dir);
  printf("Enter new directory: ");
  fflush(stdout);
  char new_dir[4096];
  if (fgets(new_dir, sizeof(new_dir), stdin) == NULL) {
    new_dir[0] = '\0';
  }
  new_dir[strcspn(new_dir, "\n")] = 0;
  if (strlen(new_dir) > 0) {
    expand_path(new_dir);
    strcpy(current_dir, new_dir);
  }

  printf("\nCurrent wallpaper setter: %s\n", wallsetter);
  printf("Enter new setter (feh or swaybg): ");
  fflush(stdout);
  char new_setter[256];
  if (fgets(new_setter, sizeof(new_setter), stdin) == NULL) {
    new_setter[0] = '\0';
  }
  new_setter[strcspn(new_setter, "\n")] = 0;
  if (strlen(new_setter) > 0 &&
      (strcmp(new_setter, "feh") == 0 || strcmp(new_setter, "swaybg") == 0))
    strcpy(wallsetter, new_setter);

  printf("\nCurrent image viewer: %s\n", imageviewer_cmd);
  printf("Enter new viewer (./imageviewer, sxiv, viu, etc.): ");
  fflush(stdout);
  char new_viewer[256];
  if (fgets(new_viewer, sizeof(new_viewer), stdin) == NULL) {
    new_viewer[0] = '\0';
  }
  new_viewer[strcspn(new_viewer, "\n")] = 0;
  if (strlen(new_viewer) > 0)
    strcpy(imageviewer_cmd, new_viewer);

  save_config();
  n = scan(current_dir);
  sel = 0;
  top = 0;

  printf("\nPress any key to continue...");
  fflush(stdout);
  getchar();

  reset_prog_mode();
  refresh();
}

static void first_time_setup() {
  def_prog_mode();
  endwin();

  char default_dir[4096];
  snprintf(default_dir, sizeof(default_dir), "%s/Pictures", getenv("HOME"));

  printf("Welcome to Layer!\n\n");
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

  printf("Use default directory (%s)? (y/n): ", default_dir);
  fflush(stdout);

  /* if (!imageviewer_exists()) { */
  /*   printf("Note: The image viewer is not built yet.\n"); */
  /* } */

  char choice[10];
  if (fgets(choice, sizeof(choice), stdin) == NULL) {
    choice[0] = '\0';
  }

  if (choice[0] == 'y' || choice[0] == 'Y' || choice[0] == '\n') {
    strcpy(current_dir, default_dir);
  } else {
    printf("\nEnter directory with images: ");
    fflush(stdout);
    if (fgets(current_dir, sizeof(current_dir), stdin) == NULL) {
      current_dir[0] = '\0';
    }
    current_dir[strcspn(current_dir, "\n")] = 0;
    expand_path(current_dir);
  }

  // now wallpaper setter is auto-detected, no need to ask user
  // printf("\nChoose wallpaper setter (1 for feh, 2 for swaybg): ");
  // fflush(stdout);
  // if (fgets(choice, sizeof(choice), stdin) == NULL) {
  //   choice[0] = '\0';
  // }
  // if (choice[0] == '2')
  //   strcpy(wallsetter, "swaybg");
  // else
  //   strcpy(wallsetter, "feh");

  printf("\nChoose image viewer:\n");
  printf("1. Built-in viewer (default)\n");
  printf("2. sxiv (simple X image viewer)\n");
  printf("3. viu (terminal image viewer)\n");
  printf("4. Custom command\n");
  printf("Enter choice (1-4): ");
  fflush(stdout);
  if (fgets(choice, sizeof(choice), stdin) == NULL) {
    choice[0] = '\0';
  }

  if (choice[0] == '2')
    strcpy(imageviewer_cmd, "sxiv");
  else if (choice[0] == '3')
    strcpy(imageviewer_cmd, "viu");
  else if (choice[0] == '4') {
    printf("Enter custom viewer command: ");
    fflush(stdout);
    char custom_viewer[256];
    if (fgets(custom_viewer, sizeof(custom_viewer), stdin) == NULL) {
      custom_viewer[0] = '\0';
    }
    custom_viewer[strcspn(custom_viewer, "\n")] = 0;
    strcpy(imageviewer_cmd, custom_viewer);
  } else {
    strcpy(imageviewer_cmd, "./imageviewer");
  }

  save_config();
  first_time = 0;

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
  printf("  -r, --restore  Restore last set wallpaper\n");
  printf("  -i VIEWER      Set default image viewer (e.g., sxiv, viu, "
         "./imageviewer)\n");
  printf("  -m, --dmenu    Launch dmenu for wallpaper selection\n");
  printf(
      "\nIf DIRECTORY is provided, it will be set as the image directory.\n");
  printf(
      "Otherwise, the program starts with the saved or default directory.\n");
}

static void print_version() { printf("layer version %s\n", VERSION); }

int main(int argc, char **argv) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);

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
               strcmp(argv[i], "-r") == 0) {
      restore_last_wallpaper();
      return 0;
    } else if (strcmp(argv[i], "--dmenu") == 0 ||
               strcmp(argv[i], "-m") == 0) {
      dmenu_mode = 1;
    } else if (strcmp(argv[i], "-i") == 0) {
      if (i + 1 < argc) {
        strcpy(imageviewer_cmd, argv[i + 1]);
        save_config();
        i++;
      } else {
        fprintf(stderr, "Error: -i requires an argument (viewer command)\n");
        return 1;
      }
    } else if (argv[i][0] != '-') {
      strcpy(current_dir, argv[i]);
      expand_path(current_dir);
      save_config();
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_help();
      return 1;
    }
  }

  if (first_time && argc < 2) {
    first_time_setup();
  }

  n = scan(current_dir);

  if (dmenu_mode) {
    if (n == 0) {
      fprintf(stderr, "No images found in directory: %s\n", current_dir);
      return 1;
    }
    set_wallpaper_dmenu();
    return 0;
  }

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);

  sel = 0;
  top = 0;
  draw();

  int ch;
  while ((ch = getch()) != 'q') {
    if (n > 0) {
      if (ch == KEY_DOWN && sel + 1 < n) {
        sel++;
        if (sel >= top + LINES - 3)
          top++;
        draw();
      }
      if (ch == KEY_UP && sel > 0) {
        sel--;
        if (sel < top)
          top--;
        draw();
      }
      if (ch == '\n') {
        set_wallpaper();
      }
      if (ch == 'v') {
        show();
        draw();
      }
      if (ch == 'k') {
        kill_wallpaper_processes();
        mvprintw(LINES - 1, 0, "Wallpaper killed");
        clrtoeol();
        refresh();
      }
    }
    if (ch == 'd') {
      change_config();
      draw();
    }
    if (ch == 'm') {
      set_wallpaper_dmenu();
      draw();
    }
  }

  endwin();
  return 0;
}
