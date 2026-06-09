# ============================================================
# Syshash v2.1.0 — Makefile
# ============================================================

BINARY     := syshash
VERSION    := 2.1.0
PREFIX     ?= /usr/local
BINDIR     := $(PREFIX)/bin

CC         ?= gcc
SRCDIR     := src
SRCS       := $(SRCDIR)/main.c \
              $(SRCDIR)/sha3.c \
              $(SRCDIR)/db.c   \
              $(SRCDIR)/scan.c \
              $(SRCDIR)/ui.c
OBJS       := $(SRCS:.c=.o)

CFLAGS     := -std=c11 -Wall -Wextra -Wpedantic \
              -O2 -D_FILE_OFFSET_BITS=64        \
              -D_POSIX_C_SOURCE=200809L          \
              -DVERSION=\"$(VERSION)\"
LDFLAGS    :=

# ── Targets ─────────────────────────────────────────────────

.PHONY: all clean install uninstall

all: $(BINARY)

$(BINARY): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "  Built: $(BINARY) v$(VERSION)"
	@echo ""

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BINARY)
	@echo "  Cleaned build artifacts."

install: $(BINARY)
	@echo "  Installing $(BINARY) to $(BINDIR)…"
	@install -d $(BINDIR)
	@install -m 0755 $(BINARY) $(BINDIR)/$(BINARY)
	@echo "  Installed: $(BINDIR)/$(BINARY)"
	@echo ""
	@echo "  Run 'syshash' from any directory to start."

uninstall:
	@if [ -f $(BINDIR)/$(BINARY) ]; then \
	    rm -f $(BINDIR)/$(BINARY); \
	    echo "  Uninstalled: $(BINDIR)/$(BINARY)"; \
	else \
	    echo "  Nothing to remove at $(BINDIR)/$(BINARY)"; \
	fi
