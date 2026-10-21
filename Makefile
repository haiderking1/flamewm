# flamewm

VERSION := 0.1.0-dev
PREFIX  ?= /usr/local
BUILD   ?= build
TARGET  := $(BUILD)/flamewm
CTL     := $(BUILD)/flamectl

# clang only. := so an env CC can't sneak gcc in, `make CC=clang-21` still works
CC := clang

# every lib here has to be in deps.md too
PKGS := wlroots-0.20 wayland-server xkbcommon pixman-1 egl libdrm xshmfence lua5.4

# clean shouldn't need any deps installed
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifneq ($(shell pkg-config --exists $(PKGS) wayland-scanner && echo ok),ok)
$(error missing deps: $(shell pkg-config --print-errors --exists $(PKGS) wayland-scanner 2>&1 | head -1). see deps.md)
endif
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))
WAYLAND_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
endif

# protocol xml wlroots implements but doesn't ship headers for
PROTOCOLS     := $(wildcard protocols/*.xml)
PROTO_HEADERS := $(PROTOCOLS:protocols/%.xml=$(BUILD)/gen/%-protocol.h)

OPT  ?= -O2 -g
WARN := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wmissing-prototypes \
	-Werror=implicit-function-declaration -Werror=return-type
DEFS := -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -DFLAME_VERSION=\"$(VERSION)\"

# CFLAGS/LDFLAGS from the command line get appended, never replace ours
FLAME_CFLAGS = -std=c17 $(OPT) $(WARN) $(DEFS) -Isrc -I$(BUILD)/gen $(PKG_CFLAGS) -MMD -MP $(CFLAGS)
FLAME_LIBS   = $(PKG_LIBS) $(LDLIBS)

SRCS := $(shell find src -name '*.c' | sort)
OBJS := $(SRCS:src/%.c=$(BUILD)/obj/%.o)

all: $(TARGET) $(CTL)

$(TARGET): $(OBJS)
	$(CC) $(SAN) $(LDFLAGS) -o $@ $^ $(FLAME_LIBS)

$(BUILD)/obj/%.o: src/%.c | $(PROTO_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(FLAME_CFLAGS) -c -o $@ $<

# the ipc client, plain libc
$(CTL): tools/flamectl/flamectl.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 $(OPT) $(WARN) -D_POSIX_C_SOURCE=200809L $(SAN) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD)/gen/%-protocol.h: protocols/%.xml
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) server-header $< $@

# pure logic modules get unit tests, sanitizers on
TESTS := $(BUILD)/tests/schedule_test $(BUILD)/tests/hiccup_test $(BUILD)/tests/sync_test

$(BUILD)/tests/schedule_test: tests/schedule_test.c src/render/schedule.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

$(BUILD)/tests/sync_test: tests/sync_test.c src/render/sync.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

$(BUILD)/tests/hiccup_test: tests/hiccup_test.c src/render/hiccup.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

# flame's X server on its own, a test client against it through the harness.
# the clients are plain xcb (test-only deps, see deps.md)
XSERVER_SRCS := $(wildcard src/xserver/*.c) src/util/strbuf.c
XTEST_PKGS := xcb xcb-xinput xcb-randr xcb-xkb xcb-xfixes xcb-xinerama xcb-screensaver \
	xcb-dpms xcb-xtest xcb-shape xcb-render xcb-damage xcb-composite xcb-glx xkbcommon-x11 \
	xkbcommon
XTESTS := $(BUILD)/tests/xinput_test $(BUILD)/tests/xkb_test $(BUILD)/tests/selection_test \
	$(BUILD)/tests/ids_test $(BUILD)/tests/misc_test $(BUILD)/tests/xfixes_test \
	$(BUILD)/tests/damage_test $(BUILD)/tests/composite_test $(BUILD)/tests/font_test \
	$(BUILD)/tests/focus_test $(BUILD)/tests/grab_test \
	$(BUILD)/tests/fuzz_test

$(BUILD)/tests/harness: tests/xserver/harness.c $(XSERVER_SRCS)
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) $(DEFS) -fsanitize=address,undefined -Isrc $(PKG_CFLAGS) \
		-o $@ $^ $(PKG_LIBS) -lm

$(BUILD)/tests/%_test: tests/xserver/%_test.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined \
		-o $@ $< $(shell pkg-config --cflags --libs $(XTEST_PKGS))

# records real apps' requests for the fuzzer: FUZZ_CORPUS=DIR replays them
$(BUILD)/tests/xrecord: tests/xserver/xrecord.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -D_GNU_SOURCE -o $@ $<

test: $(TESTS) $(BUILD)/tests/harness $(XTESTS)
	@for t in $(TESTS); do $$t || exit 1; done
	@for t in $(XTESTS); do $(BUILD)/tests/harness $$t || exit 1; done

# end to end: headless flames, real X and wayland clients, fake pointer input.
# the clipboard and drag and drop every way. needs wl-clipboard (deps.md)
E2E := $(BUILD)/e2e
XDG_SHELL_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

$(E2E)/xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) client-header $< $@

$(E2E)/xdg-shell-protocol.c: $(XDG_SHELL_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) private-code $< $@

# staging protocols the screen sharing test speaks
STAGING_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging
CAPTURE_PROTOCOLS := ext-foreign-toplevel-list ext-image-capture-source ext-image-copy-capture
CAPTURE_HEADERS := $(CAPTURE_PROTOCOLS:%=$(E2E)/%-v1-client-protocol.h)
CAPTURE_CODE := $(CAPTURE_PROTOCOLS:%=$(E2E)/%-v1-protocol.c)

define capture_protocol
$(E2E)/$(1)-v1-client-protocol.h: $(STAGING_XML)/$(1)/$(1)-v1.xml
	@mkdir -p $$(dir $$@)
	$(WAYLAND_SCANNER) client-header $$< $$@

$(E2E)/$(1)-v1-protocol.c: $(STAGING_XML)/$(1)/$(1)-v1.xml
	@mkdir -p $$(dir $$@)
	$(WAYLAND_SCANNER) private-code $$< $$@
endef
$(foreach p,$(CAPTURE_PROTOCOLS),$(eval $(call capture_protocol,$(p))))

DMABUF_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/linux-dmabuf/linux-dmabuf-v1.xml

$(E2E)/linux-dmabuf-v1-client-protocol.h: $(DMABUF_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) client-header $< $@

$(E2E)/linux-dmabuf-v1-protocol.c: $(DMABUF_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) private-code $< $@

$(E2E)/wlcapture: tests/e2e/wlcapture.c $(CAPTURE_HEADERS) $(CAPTURE_CODE) \
		$(E2E)/linux-dmabuf-v1-client-protocol.h $(E2E)/linux-dmabuf-v1-protocol.c
	$(CC) -std=c17 -O1 -g -D_DEFAULT_SOURCE -I$(E2E) -o $@ tests/e2e/wlcapture.c $(CAPTURE_CODE) \
		$(E2E)/linux-dmabuf-v1-protocol.c \
		$(shell pkg-config --cflags --libs wayland-client xcb gbm libdrm)

TEARING_XML := $(STAGING_XML)/tearing-control/tearing-control-v1.xml

$(E2E)/tearing-control-v1-client-protocol.h: $(TEARING_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) client-header $< $@

$(E2E)/tearing-control-v1-protocol.c: $(TEARING_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) private-code $< $@

$(E2E)/wlgame: tests/e2e/wlgame.c $(E2E)/xdg-shell-protocol.c $(E2E)/xdg-shell-client-protocol.h \
		$(E2E)/tearing-control-v1-protocol.c $(E2E)/tearing-control-v1-client-protocol.h
	$(CC) -std=c17 -O1 -g -I$(E2E) -o $@ tests/e2e/wlgame.c $(E2E)/xdg-shell-protocol.c \
		$(E2E)/tearing-control-v1-protocol.c $(shell pkg-config --cflags --libs wayland-client)

$(E2E)/wldnd: tests/e2e/wldnd.c $(E2E)/xdg-shell-protocol.c $(E2E)/xdg-shell-client-protocol.h
	$(CC) -std=c17 -O1 -g -I$(E2E) -o $@ tests/e2e/wldnd.c $(E2E)/xdg-shell-protocol.c \
		$(shell pkg-config --cflags --libs wayland-client)

$(E2E)/glx: tests/e2e/glx.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -o $@ $< $(shell pkg-config --cflags --libs gl x11)

$(E2E)/glxchild: tests/e2e/glxchild.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -D_DEFAULT_SOURCE -o $@ $< $(shell pkg-config --cflags --libs gl x11)

$(E2E)/glxfull: tests/e2e/glxfull.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -D_POSIX_C_SOURCE=200809L -o $@ $< \
		$(shell pkg-config --cflags --libs gl x11)

$(E2E)/gtkcsd: tests/e2e/gtkcsd.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -o $@ $< $(shell pkg-config --cflags --libs gtk+-3.0)

$(E2E)/xcursor: tests/e2e/xcursor.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -D_POSIX_C_SOURCE=200809L -o $@ $< $(shell pkg-config --cflags --libs xcb xcb-xfixes xcb-render)

$(E2E)/xkeys: tests/e2e/xkeys.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -D_POSIX_C_SOURCE=200809L -o $@ $< $(shell pkg-config --cflags --libs xcb xcb-xinput)

$(E2E)/%: tests/e2e/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -D_POSIX_C_SOURCE=200809L -o $@ $< $(shell pkg-config --cflags --libs xcb)

e2e: $(TARGET) $(CTL) $(E2E)/wldnd $(E2E)/xdnd $(E2E)/xclip $(E2E)/xwins $(E2E)/glx \
		$(E2E)/glxchild $(E2E)/xewmh $(E2E)/gtkcsd $(E2E)/xcursor $(E2E)/xkeys $(E2E)/wlcapture $(E2E)/glxfull $(E2E)/wlgame
	@sh tests/e2e/run.sh $(TARGET) $(CTL) $(E2E)

# address + ub sanitizers, own build dir so objects never mix
asan:
	@$(MAKE) --no-print-directory BUILD=build-asan \
		OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
		SAN="-fsanitize=address,undefined"

release:
	@$(MAKE) --no-print-directory BUILD=build-release OPT="-O2 -flto -DNDEBUG" SAN="-flto"

# nested inside whatever compositor you're in right now
run: $(TARGET)
	$(TARGET) $(ARGS)

# clangd picks this up, no bear needed
clangd: $(PROTO_HEADERS)
	@printf '%s\n' -xc $(filter-out -MMD -MP,$(FLAME_CFLAGS)) > compile_flags.txt
	@echo "wrote compile_flags.txt"

install: release
	install -Dm755 build-release/flamewm $(DESTDIR)$(PREFIX)/bin/flamewm
	install -Dm755 build-release/flamectl $(DESTDIR)$(PREFIX)/bin/flamectl
	install -Dm644 data/flamewm-portals.conf \
		$(DESTDIR)$(PREFIX)/share/xdg-desktop-portal/flamewm-portals.conf
	install -Dm644 data/xdg-desktop-portal-wlr-flamewm \
		$(DESTDIR)/etc/xdg/xdg-desktop-portal-wlr/flamewm

clean:
	rm -rf build build-asan build-release

.PHONY: all asan release run clangd install clean test e2e

-include $(OBJS:.o=.d)
