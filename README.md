# layer

A lightweight terminal-based wallpaper switcher and image viewer written in C, supporting both X11 and Wayland.

## Features

- **Wallpaper Management**: Browse and set wallpapers from any directory
- **Multi-backend Support**: Uses `feh` for X11 and `swaybg` for Wayland
- **Built-in Image Viewer**: Native image viewer for X11 and Wayland
- **Session Detection**: Automatically detects X11 or Wayland session
- **Configuration Persistence**: Remembers your settings and last wallpaper
- **dmenu Integration**: Select wallpapers using dmenu for quick selection

---

## Installation

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt install build-essential libx11-dev libwayland-dev libncurses-dev

# Fedora
sudo dnf install gcc make libX11-devel wayland-devel ncurses-devel

# Arch
sudo pacman -S base-devel libx11 wayland ncurses
```

### Building

```bash
git clone https://github.com/Harshit-Dhanwalkar/layer.git
cd layer

# Build both programs
make
```

This will build:

- `layer` - The wallpaper switcher
- `imageviewer` - The native image Viewer

### Usage

```bash
# Start with default/saved directory
./layer

# Start with specific directory
./layer ~/Pictures/Wallpapers

# Restore last set wallpaper
./layer --restore

# Launch dmenu for wallpaper selection
./layer --dmenu
# or
./layer -m

# Show help
./layer --help

# Show version
./layer --version
```

### Dependencies

- `layer`: ncurses, libX11 (for X11 fallback)
- `imageviewer`: libX11, libwayland-client
- `Runtime`: feh (X11) or swaybg (Wayland) for setting wallpapers
- `dmenu` (Optional): for dmenu integration

---

### Wayland Protocol Files

The image viewer requires Wayland protocol files. These are automatically generated during build:

```bash
# The Makefile automatically runs these commands:
wayland-scanner client-header xdg-shell.xml xdg-shell-client-protocol.h
wayland-scanner private-code xdg-shell.xml xdg-shell-protocol.c
```

This will create:

```bash
xdg-shell-client-protocol.h
xdg-shell-protocol.c
```

Get `xdg-shell.xml` from [wayland-protocols](https://git.uibk.ac.at/csba3673/visualcomputing/-/blob/dev/05/external/glfw/deps/wayland/xdg-shell.xml)
