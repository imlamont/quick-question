# qq — build with `make`, test with `make test`, sanitizer run with `make debug`.

CFLAGS  ?= -O2
LDLIBS   = -lcurl -lcjson
PREFIX  ?= /usr/local
QQFLAGS  = -std=c17 -Wall -Wextra -Wpedantic -Wshadow -D_XOPEN_SOURCE=700

# Out-of-tree objects so the release and sanitizer builds never mix.
B   ?= build
BIN ?= qq

ifdef SAN
CFLAGS   = -O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
endif

OBJ = $(B)/buf.o $(B)/config.o $(B)/prompt.o $(B)/proc.o $(B)/http.o $(B)/mcp.o \
      $(B)/tools.o $(B)/openai.o
HDR = $(wildcard src/*.h)

all: $(BIN)

$(BIN): $(B)/qq.o $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(B)/unit: $(B)/unit.o $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lm

$(B)/%.o: src/%.c $(HDR) | $(B)
	$(CC) $(QQFLAGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B)/unit.o: tests/unit.c $(HDR) | $(B)
	$(CC) $(QQFLAGS) -Isrc $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(B):
	mkdir -p $@

test: $(BIN) $(B)/unit
	$(B)/unit
	tests/run.sh $(BIN)

debug:
	$(MAKE) SAN=1 B=build/debug BIN=build/debug/qq test

# INSTALL_STRIP= installs unstripped, for packaging tools that strip separately.
INSTALL_STRIP ?= -s

install: $(BIN)
	install -Dm755 $(INSTALL_STRIP) $(BIN) $(DESTDIR)$(PREFIX)/bin/qq
	install -Dm644 docs/qq.1 $(DESTDIR)$(PREFIX)/share/man/man1/qq.1

clean:
	rm -rf build qq

.PHONY: all test debug install clean
