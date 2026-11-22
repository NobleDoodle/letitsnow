# letitsnow: falling snow for Hyprland, as a user service.
VERSION := $(shell cat VERSION)
PREFIX  ?= /usr
DESTDIR ?=
BINDIR  ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/lib/letitsnow
UNITDIR ?= $(PREFIX)/lib/systemd/user
DOCDIR  ?= $(PREFIX)/share/doc/letitsnow
LICENSEDIR ?= $(PREFIX)/share/licenses/letitsnow
# The document installed as /usr/share/doc/letitsnow/README.md.
README  ?= README.md

# Everything generated goes to $(O), so a package can build from a read-only
# checkout: make O=/some/dir (the Let It Snow plugin's setup does).
O       ?= build

CC      ?= cc
CFLAGS  ?= -O2 -g -pipe
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -DLETITSNOW_VERSION=\"$(VERSION)\" -I$(O)
PKGS     = wayland-client wayland-egl egl glesv2
WP_DIR  := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)

PROTOS = protocols/wlr-layer-shell-unstable-v1.xml \
         $(WP_DIR)/stable/xdg-shell/xdg-shell.xml \
         $(WP_DIR)/stable/viewporter/viewporter.xml \
         $(WP_DIR)/staging/fractional-scale/fractional-scale-v1.xml
PROTO_H = $(patsubst %.xml,$(O)/%-client-protocol.h,$(notdir $(PROTOS)))
PROTO_C = $(patsubst %.xml,$(O)/%-protocol.c,$(notdir $(PROTOS)))
vpath %.xml protocols $(WP_DIR)/stable/xdg-shell $(WP_DIR)/stable/viewporter $(WP_DIR)/staging/fractional-scale

.PHONY: all clean install install-user uninstall-user check

all: $(O)/letitsnowd

$(O):
	mkdir -p $(O)

$(O)/%-client-protocol.h: %.xml | $(O)
	$(SCANNER) client-header $< $@

$(O)/%-protocol.c: %.xml | $(O)
	$(SCANNER) private-code $< $@

$(O)/letitsnowd: src/letitsnowd.c $(PROTO_H) $(PROTO_C) VERSION
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/letitsnowd.c $(PROTO_C) $(shell pkg-config --cflags --libs $(PKGS)) -lm $(LDFLAGS)

clean:
	rm -rf $(O)

# The command and the unit name the renderer's real location.
define fill_in
	sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' -e 's|@VERSION@|$(VERSION)|g' $(1) > $(2)
endef

install: $(O)/letitsnowd
	install -Dm755 $(O)/letitsnowd $(DESTDIR)$(LIBEXECDIR)/letitsnowd
	$(call fill_in,bin/letitsnow,$(O)/letitsnow)
	install -Dm755 $(O)/letitsnow $(DESTDIR)$(BINDIR)/letitsnow
	$(call fill_in,packaging/systemd/letitsnow.service.in,$(O)/letitsnow.service)
	install -Dm644 $(O)/letitsnow.service $(DESTDIR)$(UNITDIR)/letitsnow.service
	install -Dm644 $(README) $(DESTDIR)$(DOCDIR)/README.md
	install -Dm644 LICENSE $(DESTDIR)$(LICENSEDIR)/LICENSE

# No package, no root: everything under ~/.local, the unit where systemd --user looks.
install-user:
	$(MAKE) install PREFIX=$(HOME)/.local UNITDIR=$(HOME)/.local/share/systemd/user
	systemctl --user daemon-reload

uninstall-user:
	-systemctl --user disable --now letitsnow.service
	rm -f $(HOME)/.local/bin/letitsnow $(HOME)/.local/share/systemd/user/letitsnow.service
	rm -rf $(HOME)/.local/lib/letitsnow $(HOME)/.local/share/doc/letitsnow $(HOME)/.local/share/licenses/letitsnow
	systemctl --user daemon-reload

check: $(O)/letitsnowd
	bash -n bin/letitsnow
	tests/cli.test.sh
