CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
WAYLAND_LIBS = -lwayland-client -lm
X11_LIBS = -lX11
NCURSES_LIBS = -lncurses

all: layer imageviewer

layer: layer.o
	$(CC) $(CFLAGS) -o $@ $^ $(NCURSES_LIBS) $(X11_LIBS)

layer.o: layer.c
	$(CC) $(CFLAGS) -c layer.c

imageviewer: imageviewer.o xdg-shell-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $(X11_LIBS) $(WAYLAND_LIBS)

imageviewer.o: imageviewer.c xdg-shell-client-protocol.h stb_image.h
	$(CC) $(CFLAGS) -c imageviewer.c

xdg-shell-protocol.o: xdg-shell-protocol.c
	$(CC) $(CFLAGS) -c xdg-shell-protocol.c

xdg-shell-protocol.c xdg-shell-client-protocol.h: xdg-shell.xml
	wayland-scanner client-header $< xdg-shell-client-protocol.h
	wayland-scanner private-code $< xdg-shell-protocol.c

clean:
	rm -f *.o layer imageviewer xdg-shell-protocol.c xdg-shell-client-protocol.h

distclean: clean
	@echo "This will remove all generated files AND user configuration."
	@echo -n "Are you sure? [y/N] " && read ans && [ $${ans:-N} = y ]
	rm -f ~/.layer_config ~/.layer_last_wallpaper

.PHONY: all clean distclean
