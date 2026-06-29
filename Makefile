# stress -- build & install
#
#   make            dynamic build (default)
#   make static     fully static; uses musl-gcc if present, else glibc -static
#   make static-glibc  force a glibc static build
#   make strip      static build, symbols stripped (smallest portable binary)
#   make debug      build with -g and ASan/UBSan for development
#   make test       build, then run smoke tests
#   make install    install to $(PREFIX)/bin    (default PREFIX=/usr/local)
#   make uninstall  remove the installed binary
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
SRC      = stress.c
BIN      = stress

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
INSTALL ?= install

# Prefer musl for static builds: it produces a genuinely dependency-free,
# portable binary with no glibc NSS caveats. Fall back to the system cc.
MUSL := $(shell command -v musl-gcc 2>/dev/null)

.PHONY: all dynamic static static-glibc strip debug test install uninstall clean

all: dynamic

dynamic: $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

# 'static' picks the best available toolchain automatically.
static: $(SRC)
ifneq ($(MUSL),)
	@echo ">> static via musl-gcc (portable, no libc caveats)"
	musl-gcc $(CFLAGS) -static -o $(BIN) $(SRC)
else
	@echo ">> musl-gcc not found; falling back to glibc -static"
	@echo ">> (works here since no NSS/locale calls are used)"
	$(CC) $(CFLAGS) -static -o $(BIN) $(SRC)
endif

# Explicit glibc static build, regardless of musl availability.
static-glibc: $(SRC)
	$(CC) $(CFLAGS) -static -o $(BIN) $(SRC)

# Static + stripped: smallest artifact to ship.
strip: static
	strip -s $(BIN)
	@echo ">> stripped:"; ls -lh $(BIN) | awk '{print "   " $$5, $$9}'

# Development build: debug symbols + sanitizers. Not for time-tight runs
# (ASan adds overhead), but invaluable for catching pipe/fd/memory bugs.
debug: $(SRC)
	$(CC) -g -O0 -Wall -Wextra -fsanitize=address,undefined -o $(BIN) $(SRC)

test: dynamic
	@./test.sh

install: dynamic
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	@echo ">> installed to $(DESTDIR)$(BINDIR)/$(BIN)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	@echo ">> removed $(DESTDIR)$(BINDIR)/$(BIN)"

clean:
	rm -f $(BIN) $(BIN)-static
