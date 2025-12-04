CC      = gcc
BIN     = layer
SRC     = layer.c
CFLAGS  = -O2 -march=native -Wall -Wextra
LDFLAGS = -lncurses -ltinfo
PREFIX  = /usr

.PHONY: all install uninstall clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)
