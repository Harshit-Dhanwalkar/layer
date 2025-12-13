# layer

A lightweight, terminal-based wallpaper switcher and image viewer written in C, supporting both X11 and Wayland.

## ✨ Features (v0.2.0 Major Update)

- **⚡ Optimized Performance**: Implemented **Lazy Stat Fetching** to dramatically speed up directory navigation (especially in folders with thousands of files).
- **Wallpaper Management**: Browse and set wallpapers from any directory.
- **Multi-backend Support**: Uses `feh` for X11 and `swaybg` for Wayland.
- **Built-in Utilities**:
  - **`imageviewer`**: Native image viewer for quick previews (`v` key).
  - **`clock-widget`**: A separate Wayland-native time/date overlay utility.
- **Session Detection**: Automatically detects X11 or Wayland session.
- **Configuration Persistence**: Remembers your settings, last wallpaper, and preferred directory.
- **dmenu Integration**: Select wallpapers using dmenu for quick selection.
- **File Sorting**: Cycle through **Name**, **Size**, and **Date** sorting modes (`s` key).

---

## 🚀 Installation

### Prerequisites

You will need the standard C build tools, Wayland development libraries, and `ncurses`.

| OS/Distro         | Command                                                                                      |
| :---------------- | :------------------------------------------------------------------------------------------- |
| **Debian/Ubuntu** | `sudo apt install build-essential libx11-dev libwayland-dev libncurses-dev wayland-scanner`  |
| **Fedora**        | `sudo dnf install gcc make libX11-devel wayland-devel ncurses-devel wayland-protocols-devel` |
| **Arch/Manjaro**  | `sudo pacman -S base-devel libx11 wayland ncurses wayland-protocols`                         |

### Building

```bash
git clone [https://github.com/Harshit-Dhanwalkar/layer.git](https://github.com/Harshit-Dhanwalkar/layer.git)
cd layer

# Build all three programs: layer, imageviewer, and clock-widget
make
```

The build process generates:

- layer - The ncurses wallpaper switcher.
- imageviewer - The lightweight X11/Wayland image viewer.
- clock-widget - The simple Wayland clock overlay utility.

### Installation

To install the executables to your local binary path (`$HOME/.local/bin/`):

```bash
make install
```

(You may need to add `$HOME/.local/bin` to your system's `$PATH`.)

---

## Usage

#### Running `layer` (Wallpaper Switcher)

| Command                       | Description                                              |
| ----------------------------- | -------------------------------------------------------- |
| ./layer                       | Start with default/saved directory.                      |
| ./layer ~/Pictures/Wallpapers | Start in a specific directory.                           |
| ./layer --restore             | Restore the last set wallpaper.                          |
| ./layer --dmenu or ./layer -m | Launch dmenu for quick selection from current directory. |
| ./layer                       | --help Show help message.                                |

#### Keybindings in `layer`

| Key               | Action                                                        | Notes         |
| ----------------- | ------------------------------------------------------------- | ------------- |
| Enter / l / Right | Enter the selected directory or set the wallpaper.            |               |
| h / Left          | Go up to the parent directory (..).                           |               |
| j / Down          | Move selection down.                                          |               |
| k / Up            | Move selection up.                                            |               |
| s                 | Cycle Sort Mode: Name -> Size -> Date. New in v0.2.0          |               |
| v                 | Show Preview of the selected image using imageviewer.         | New in v0.2.0 |
| K                 | Kill the current wallpaper setter process (swaybg/feh).       | New in v0.2.0 |
| r                 | Set a random wallpaper from the current directory.            |               |
| F1                | Enter Config Menu to change wallsetter, viewer, or directory. |               |
| q / Q             | Quit the application.                                         |               |

#### Running `imageviewer` (Image Viewer)

| Command                                  | Description                                  |
| ---------------------------------------- | -------------------------------------------- |
| ./imageviewer <image_path>               | View an image in X11 or Wayland.             |
| ./imageviewer --help                     | Show help message.                           |
| ./imageviewer --g or ./imageviewer -grid | View images in a grid layout (Wayland only). |

#### Running `clock-widget` (Wayland Clock Overlay)

The clock widget is a separate binary that can be launched directly:

```bash
./clock-widget
```

---

## Dependencies

| Program      | Core Dependencies                     | Runtime Dependencies                              |
| ------------ | ------------------------------------- | ------------------------------------------------- |
| layer        | "ncurses, libX11 (for X11 fallback)"  | "feh (X11) or swaybg (Wayland), dmenu (optional)" |
| imageviewer  | "libX11, libwayland-client,stb_image" |                                                   |
| clock-widget | libwayland-client                     |                                                   |

---

## Wayland Protocol Files

The Wayland-native utilities (`imageviewer` and `clock-widget`) require client headers and code for the `xdg-shell` and `wlr-layer-shell` protocols.

The Makefile automatically generates these files using `wayland-scanner` during the build process:

```bash
# Generated Protocol Headers
build/xdg-shell-client-protocol.h
build/wlr-layer-shell-unstable-v1-client-protocol.h

# Generated Protocol Code
build/xdg-shell-protocol.c
build/wlr-layer-shell-unstable-v1-protocol.c
```

---

# License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
