# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -I./include -I./build
LDFLAGS_LAYER = -lncurses
LDFLAGS_IMAGEVIEWER = -lX11 -lwayland-client -lm

# Directories
SRC_DIR = src
INCLUDE_DIR = include
PROTOCOLS_DIR = protocols
BUILD_DIR = build
BIN_DIR = .

# Targets
TARGETS = $(BIN_DIR)/layer $(BIN_DIR)/imageviewer

# Protocol files
PROTOCOL_H = $(BUILD_DIR)/xdg-shell-client-protocol.h
PROTOCOL_C = $(BUILD_DIR)/xdg-shell-protocol.c

# Source files
LAYER_SRC = $(SRC_DIR)/layer.c
IMAGEVIEWER_SRC = $(SRC_DIR)/imageviewer.c

# Object files
LAYER_OBJ = $(BUILD_DIR)/layer.o
IMAGEVIEWER_OBJ = $(BUILD_DIR)/imageviewer.o
PROTOCOL_OBJ = $(BUILD_DIR)/xdg-shell-protocol.o

# Default target
all: $(TARGETS)

# Generate protocol files
$(PROTOCOL_H): $(PROTOCOLS_DIR)/xdg-shell.xml
	@echo "Generating Wayland protocol headers..."
	@mkdir -p $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(PROTOCOL_C): $(PROTOCOLS_DIR)/xdg-shell.xml
	@echo "Generating Wayland protocol code..."
	@mkdir -p $(BUILD_DIR)
	wayland-scanner private-code $< $@

# Compile imageviewer
$(BUILD_DIR)/imageviewer.o: $(IMAGEVIEWER_SRC) $(PROTOCOL_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/xdg-shell-protocol.o: $(PROTOCOL_C) $(PROTOCOL_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/imageviewer: $(IMAGEVIEWER_OBJ) $(PROTOCOL_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS_IMAGEVIEWER)

# Compile layer
$(BUILD_DIR)/layer.o: $(LAYER_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/layer: $(LAYER_OBJ)
	$(CC) $< -o $@ $(LDFLAGS_LAYER)

# Clean
clean:
	rm -rf $(BUILD_DIR)/* $(TARGETS)

# Install
install: all
	@echo "Installing to /usr/local/bin/"
	@sudo cp $(BIN_DIR)/layer /usr/local/bin/
	@sudo cp $(BIN_DIR)/imageviewer /usr/local/bin/
	@echo "Installation complete."

# Uninstall
uninstall:
	@echo "Removing from /usr/local/bin..."
	@sudo rm -f /usr/local/bin/layer /usr/local/bin/imageviewer
	@echo "Uninstallation complete."

# Phony targets
.PHONY: all clean install uninstall
