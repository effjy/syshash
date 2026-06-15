# ============================================================
# Syshash v3.0.0 — Makefile
#
#   make            build CLI (syshash) and GUI (syshash-gui)
#   make cli        build only the command-line tool
#   make gui        build only the GTK3 GUI
#   sudo make install     install both, plus icon + desktop entry (global)
#   sudo make uninstall   remove everything installed above
# ============================================================

CLI_BIN    := syshash
GUI_BIN    := syshash-gui
VERSION    := 3.0.0
AUTHOR     := Jean-Francois Lachance-Caumartin

PREFIX     ?= /usr/local
BINDIR     := $(PREFIX)/bin
DATADIR    := $(PREFIX)/share
APPDIR     := $(DATADIR)/applications
ICONDIR    := $(DATADIR)/icons/hicolor/scalable/apps

CC         ?= gcc
SRCDIR     := src

# Shared integrity core (no terminal/GUI dependencies)
CORE_SRCS  := $(SRCDIR)/sha3.c $(SRCDIR)/db.c $(SRCDIR)/scan.c
CORE_OBJS  := $(CORE_SRCS:.c=.o)

# CLI-only sources
CLI_SRCS   := $(SRCDIR)/main.c $(SRCDIR)/ui.c
CLI_OBJS   := $(CLI_SRCS:.c=.o)

# GUI-only sources
GUI_SRCS   := $(SRCDIR)/gui.c
GUI_OBJS   := $(GUI_SRCS:.c=.gui.o)

CFLAGS     := -std=c11 -Wall -Wextra -Wpedantic \
              -O2 -D_FILE_OFFSET_BITS=64        \
              -D_POSIX_C_SOURCE=200809L          \
              -DVERSION=\"$(VERSION)\"
LDFLAGS    :=

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

# ── Targets ─────────────────────────────────────────────────

.PHONY: all cli gui clean install uninstall

all: cli gui

cli: $(CLI_BIN)
gui: $(GUI_BIN)

$(CLI_BIN): $(CORE_OBJS) $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "  Built: $(CLI_BIN) v$(VERSION)"

$(GUI_BIN): $(CORE_OBJS) $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(GTK_LIBS) $(LDFLAGS)
	@echo "  Built: $(GUI_BIN) v$(VERSION)"

# core + CLI objects (no GTK headers)
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

# GUI objects need GTK include flags; relaxed -Wpedantic for GTK macros
$(SRCDIR)/%.gui.o: $(SRCDIR)/%.c
	$(CC) -std=c11 -Wall -Wextra -O2 -DVERSION=\"$(VERSION)\" \
	    -I$(SRCDIR) $(GTK_CFLAGS) -c -o $@ $<

clean:
	rm -f $(CORE_OBJS) $(CLI_OBJS) $(GUI_OBJS) $(CLI_BIN) $(GUI_BIN)
	@echo "  Cleaned build artifacts."

install: all
	@echo "  Installing binaries to $(BINDIR)…"
	@install -d $(DESTDIR)$(BINDIR)
	@install -m 0755 $(CLI_BIN) $(DESTDIR)$(BINDIR)/$(CLI_BIN)
	@install -m 0755 $(GUI_BIN) $(DESTDIR)$(BINDIR)/$(GUI_BIN)
	@echo "  Installing icon to $(ICONDIR)…"
	@install -d $(DESTDIR)$(ICONDIR)
	@install -m 0644 data/syshash.svg $(DESTDIR)$(ICONDIR)/syshash.svg
	@echo "  Installing desktop entry to $(APPDIR)…"
	@install -d $(DESTDIR)$(APPDIR)
	@install -m 0644 data/syshash.desktop $(DESTDIR)$(APPDIR)/syshash.desktop
	@echo "  Refreshing desktop & icon caches…"
	-@update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	-@gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null || true
	@echo ""
	@echo "  Installed Syshash v$(VERSION)."
	@echo "    CLI : run 'syshash' in any directory"
	@echo "    GUI : launch 'Syshash' from your applications menu, or 'syshash-gui'"
	@echo ""

uninstall:
	@echo "  Removing Syshash…"
	@rm -f $(DESTDIR)$(BINDIR)/$(CLI_BIN)
	@rm -f $(DESTDIR)$(BINDIR)/$(GUI_BIN)
	@rm -f $(DESTDIR)$(ICONDIR)/syshash.svg
	@rm -f $(DESTDIR)$(APPDIR)/syshash.desktop
	-@update-desktop-database $(DESTDIR)$(APPDIR) 2>/dev/null || true
	-@gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null || true
	@echo "  Uninstalled."
