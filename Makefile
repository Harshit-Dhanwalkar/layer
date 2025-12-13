# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -I./include -I./build
LDFLAGS_LAYER = -lncurses
LDFLAGS_IMAGEVIEWER = -lX11 -lwayland-client -lm
LDFLAGS_CLOCK = -lwayland-client -lm -pthread

# Directories
SRC_DIR = src
INCLUDE_DIR = include
PROTOCOLS_DIR = protocols
BUILD_DIR = build
BIN_DIR = build

# Targets
TARGETS = $(BIN_DIR)/layer $(BIN_DIR)/imageviewer $(BIN_DIR)/clock-widget

# Protocol files
XDG_PROTOCOL_H = $(BUILD_DIR)/xdg-shell-client-protocol.h
XDG_PROTOCOL_C = $(BUILD_DIR)/xdg-shell-protocol.c
LAYER_PROTOCOL_H = $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_PROTOCOL_C = $(BUILD_DIR)/wlr-layer-shell-unstable-v1-protocol.c

# Source files
LAYER_SRC = $(SRC_DIR)/layer.c
IMAGEVIEWER_SRC = $(SRC_DIR)/imageviewer.c
CLOCK_SRC = $(SRC_DIR)/clock-widget.c

# Object files
LAYER_OBJ = $(BUILD_DIR)/layer.o
IMAGEVIEWER_OBJ = $(BUILD_DIR)/imageviewer.o
CLOCK_OBJ = $(BUILD_DIR)/clock-widget.o
XDG_PROTOCOL_OBJ = $(BUILD_DIR)/xdg-shell-protocol.o
LAYER_PROTOCOL_OBJ = $(BUILD_DIR)/wlr-layer-shell-unstable-v1-protocol.o

# Default target
all: $(TARGETS)

# Generate xdg-shell protocol files
$(XDG_PROTOCOL_H): $(PROTOCOLS_DIR)/xdg-shell.xml
	@echo "Generating xdg-shell protocol headers..."
	@mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(XDG_PROTOCOL_C): $(PROTOCOLS_DIR)/xdg-shell.xml
	@echo "Generating xdg-shell protocol code..."
	@mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

# Generate layer-shell protocol files
$(LAYER_PROTOCOL_H): $(PROTOCOLS_DIR)/wlr-layer-shell-unstable-v1.xml
	@echo "Generating layer-shell protocol headers..."
	@mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(LAYER_PROTOCOL_C): $(PROTOCOLS_DIR)/wlr-layer-shell-unstable-v1.xml
	@echo "Generating layer-shell protocol code..."
	@mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

# Compile layer
$(BUILD_DIR)/layer.o: $(LAYER_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/layer: $(LAYER_OBJ)
	$(CC) $< -o $@ $(LDFLAGS_LAYER)

# Compile imageviewer
$(BUILD_DIR)/imageviewer.o: $(IMAGEVIEWER_SRC) $(XDG_PROTOCOL_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile clock widget
$(BUILD_DIR)/clock-widget.o: $(CLOCK_SRC) $(LAYER_PROTOCOL_H) $(XDG_PROTOCOL_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile xdg-shell protocol
$(BUILD_DIR)/xdg-shell-protocol.o: $(XDG_PROTOCOL_C) $(XDG_PROTOCOL_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile layer-shell protocol
$(BUILD_DIR)/wlr-layer-shell-unstable-v1-protocol.o: $(LAYER_PROTOCOL_C) $(LAYER_PROTOCOL_H) $(XDG_PROTOCOL_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Link imageviewer
$(BIN_DIR)/imageviewer: $(IMAGEVIEWER_OBJ) $(XDG_PROTOCOL_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS_IMAGEVIEWER)

# Link clock widget - ADD xdg-shell protocol
$(BIN_DIR)/clock-widget: $(CLOCK_OBJ) $(LAYER_PROTOCOL_OBJ) $(XDG_PROTOCOL_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS_CLOCK)

# Clean
clean:
	rm -rf $(BUILD_DIR)/* $(TARGETS)

# Install
install: all
	@echo "Installing to $(HOME)/.local/bin/..."
	@mkdir -p $(HOME)/.local/bin
	@cp $(BIN_DIR)/layer $(HOME)/.local/bin/
	@cp $(BIN_DIR)/imageviewer $(HOME)/.local/bin/
	@cp $(BIN_DIR)/clock-widget $(HOME)/.local/bin/
	@echo "Installation complete."

# Install system-wide
install-system: all
	@echo "Installing system-wide to /usr/local/bin/ (requires sudo)..."
	@sudo cp $(BIN_DIR)/layer /usr/local/bin/
	@sudo cp $(BIN_DIR)/imageviewer /usr/local/bin/
	@sudo cp $(BIN_DIR)/clock-widget /usr/local/bin/
	@echo "System-wide installation complete."

# Uninstall
uninstall:
	@echo "Removing from $(HOME)/.local/bin..."
	@rm -f $(HOME)/.local/bin/layer $(HOME)/.local/bin/imageviewer $(HOME)/.local/bin/clock-widget
	@echo "Uninstallation complete."

# Uninstall system-wide
uninstall-system:
	@echo "Removing from /usr/local/bin... (requires sudo)"
	@sudo rm -f /usr/local/bin/layer /usr/local/bin/imageviewer /usr/local/bin/clock-widget
	@echo "System-wide uninstallation complete."

# Run clock widget in background (for testing)
run-clock:
	@echo "Running clock widget in background..."
	@./clock-widget &

# Kill clock widget
kill-clock:
	@pkill -f clock-widget

# Phony targets
.PHONY: all clean install install-system uninstall uninstall-system run-clock kill-clock
