# flamewm

VERSION := 0.1.0-dev
PREFIX  ?= /usr/local
BUILD   ?= build
TARGET  := $(BUILD)/flamewm
CTL     := $(BUILD)/flamectl
SETTINGS := $(BUILD)/flamesettings
FILES    := $(BUILD)/flamefiles
PLAY     := $(BUILD)/flameplay
BAR      := $(BUILD)/flamebar

# clang only. := so an env CC can't sneak gcc in, `make CC=clang-21` still works
CC := clang

# every lib here has to be in deps.md too
PKGS := wlroots-0.20 wayland-server xkbcommon pixman-1 egl libdrm xshmfence lua5.4 pangocairo \
	libpipewire-0.3 libsystemd libturbojpeg libpng libwebp

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

# protocols flame implements itself: wlroots has no code for them, their
# interfaces get generated and linked in too
BG_EFFECT_XML := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-background-effect/ext-background-effect-v1.xml
OWN_PROTOCOLS := flame-eyecandy-v1 ext-background-effect-v1
PROTO_HEADERS += $(BUILD)/gen/ext-background-effect-v1-protocol.h
OWN_PROTO_OBJS := $(OWN_PROTOCOLS:%=$(BUILD)/obj/gen/%-protocol.o)

OPT  ?= -O2 -g
WARN := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wmissing-prototypes \
	-Werror=implicit-function-declaration -Werror=return-type
DEFS := -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -DFLAME_VERSION=\"$(VERSION)\"

# CFLAGS/LDFLAGS from the command line get appended, never replace ours
FLAME_CFLAGS = -std=c17 $(OPT) $(WARN) $(DEFS) -Isrc -I$(BUILD)/gen $(PKG_CFLAGS) -MMD -MP $(CFLAGS)
FLAME_LIBS   = $(PKG_LIBS) $(LDLIBS)

SRCS := $(shell find src -name '*.c' | sort)
OBJS := $(SRCS:src/%.c=$(BUILD)/obj/%.o)

all: $(TARGET) $(CTL) $(SETTINGS) $(FILES) $(PLAY) $(BAR)

$(TARGET): $(OBJS) $(OWN_PROTO_OBJS)
	$(CC) $(SAN) $(LDFLAGS) -o $@ $^ $(FLAME_LIBS)

$(BUILD)/obj/%.o: src/%.c | $(PROTO_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(FLAME_CFLAGS) -c -o $@ $<

# flame's own apps: flameui (tools/flameui) and what's built on it
CLIENT_GEN := $(BUILD)/gen-client
WL_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
UI_PROTOCOLS := $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml \
	$(WL_PROTOCOLS)/staging/fractional-scale/fractional-scale-v1.xml \
	$(WL_PROTOCOLS)/stable/viewporter/viewporter.xml \
	$(WL_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml \
	$(WL_PROTOCOLS)/staging/content-type/content-type-v1.xml \
	$(WL_PROTOCOLS)/stable/linux-dmabuf/linux-dmabuf-v1.xml \
	$(WL_PROTOCOLS)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml \
	protocols/wlr-layer-shell-unstable-v1.xml \
	$(WL_PROTOCOLS)/stable/tablet/tablet-v2.xml \
	protocols/flame-eyecandy-v1.xml
UI_PROTO_NAMES := $(basename $(notdir $(UI_PROTOCOLS)))
UI_PROTO_HEADERS := $(UI_PROTO_NAMES:%=$(CLIENT_GEN)/%-client-protocol.h)
UI_PROTO_CODE := $(UI_PROTO_NAMES:%=$(CLIENT_GEN)/%-protocol.c)
UI_PKGS := wayland-client pangocairo xkbcommon libturbojpeg libpng libwebp libavformat \
	libavcodec libswscale libavutil librsvg-2.0 gio-2.0

define ui_protocol
$(CLIENT_GEN)/$(basename $(notdir $(1)))-client-protocol.h: $(1)
	@mkdir -p $$(dir $$@)
	$$(WAYLAND_SCANNER) client-header $$< $$@

$(CLIENT_GEN)/$(basename $(notdir $(1)))-protocol.c: $(1)
	@mkdir -p $$(dir $$@)
	$$(WAYLAND_SCANNER) private-code $$< $$@
endef
$(foreach p,$(UI_PROTOCOLS),$(eval $(call ui_protocol,$(p))))

# flame's own apps: flameui, the pictures it reads, its protocols
FLAMEUI_SRCS := tools/flameui/window.c tools/flameui/ui.c tools/flameui/thumbs.c \
	tools/flameui/video.c tools/flameui/thumbcache.c tools/flameui/data.c tools/flameui/layer.c tools/flameui/icons.c \
	src/image/image.c
FLAMEUI_DEPS := $(FLAMEUI_SRCS) tools/flameui/flameui.h tools/flameui/internal.h \
	src/image/image.h $(UI_PROTO_HEADERS) $(UI_PROTO_CODE)
UI_CC = $(CC) -std=c17 $(OPT) $(WARN) -D_DEFAULT_SOURCE -I$(CLIENT_GEN) -Isrc $(SAN) $(CFLAGS) \
	$(LDFLAGS)
UI_LIBS = $(shell pkg-config --cflags --libs $(UI_PKGS)) -lm -pthread

# flame's ipc for its apps: the socket, the json it answers in
IPC_SRCS := tools/flameipc/ipc.c tools/flameipc/json.c
IPC_DEPS := $(IPC_SRCS) tools/flameipc/ipc.h tools/flameipc/json.h

SETTINGS_SRCS := tools/flamesettings/main.c tools/flamesettings/browser.c $(IPC_SRCS)
$(SETTINGS): $(SETTINGS_SRCS) $(IPC_DEPS) tools/flamesettings/browser.h $(FLAMEUI_DEPS)
	@mkdir -p $(dir $@)
	$(UI_CC) -o $@ $(SETTINGS_SRCS) $(FLAMEUI_SRCS) $(UI_PROTO_CODE) $(UI_LIBS)

FILES_SRCS := tools/flamefiles/main.c tools/flamefiles/folder.c tools/flamefiles/ops.c \
	tools/flamefiles/icons.c
$(FILES): $(FILES_SRCS) tools/flamefiles/folder.h tools/flamefiles/ops.h \
		tools/flamefiles/icons.h $(FLAMEUI_DEPS)
	@mkdir -p $(dir $@)
	$(UI_CC) -o $@ $(FILES_SRCS) $(FLAMEUI_SRCS) $(UI_PROTO_CODE) $(UI_LIBS)

# flameplay's gpu pass: glsl to spir-v, as a c array gpu.c includes
PLAY_GEN := $(BUILD)/gen-play
$(PLAY_GEN)/convert.spv.inc: tools/flameplay/convert.comp
	@mkdir -p $(dir $@)
	glslc --target-env=vulkan1.3 -O -mfmt=c -o $@ $<

PLAY_SRCS := tools/flameplay/main.c tools/flameplay/player.c tools/flameplay/gpu.c \
	tools/flameplay/resume.c tools/flameplay/subs.c
$(PLAY): $(PLAY_SRCS) tools/flameplay/player.h tools/flameplay/gpu.h tools/flameplay/resume.h \
		tools/flameplay/subs.h $(PLAY_GEN)/convert.spv.inc $(FLAMEUI_DEPS)
	@mkdir -p $(dir $@)
	$(UI_CC) -I$(PLAY_GEN) -o $@ $(PLAY_SRCS) $(FLAMEUI_SRCS) $(UI_PROTO_CODE) $(UI_LIBS) \
		$(shell pkg-config --cflags --libs libswresample libpipewire-0.3 libass uchardet)

BAR_SRCS := tools/flamebar/main.c tools/flamebar/control.c tools/flamebar/look.c \
	tools/flamebar/notify.c tools/flamebar/notes.c tools/flamebar/bus.c \
	tools/flamebar/sound.c tools/flamebar/media.c tools/flamebar/network.c \
	tools/flamebar/bluetooth.c $(IPC_SRCS)
BAR_HDRS := tools/flamebar/control.h tools/flamebar/look.h tools/flamebar/notify.h \
	tools/flamebar/notes.h tools/flamebar/bus.h tools/flamebar/sound.h \
	tools/flamebar/media.h tools/flamebar/network.h tools/flamebar/bluetooth.h
$(BAR): $(BAR_SRCS) $(BAR_HDRS) $(IPC_DEPS) $(FLAMEUI_DEPS)
	@mkdir -p $(dir $@)
	$(UI_CC) -o $@ $(BAR_SRCS) $(FLAMEUI_SRCS) $(UI_PROTO_CODE) $(UI_LIBS) \
		$(shell pkg-config --cflags --libs libpulse libsystemd)

# the ipc client, plain libc
$(CTL): tools/flamectl/flamectl.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 $(OPT) $(WARN) -D_POSIX_C_SOURCE=200809L $(SAN) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD)/gen/%-protocol.h: protocols/%.xml
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) server-header $< $@

$(BUILD)/gen/ext-background-effect-v1-protocol.h: $(BG_EFFECT_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) server-header $< $@

$(BUILD)/gen/flame-eyecandy-v1-protocol.c: protocols/flame-eyecandy-v1.xml
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD)/gen/ext-background-effect-v1-protocol.c: $(BG_EFFECT_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD)/obj/gen/%-protocol.o: $(BUILD)/gen/%-protocol.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 $(OPT) $(PKG_CFLAGS) -c -o $@ $<

# pure logic modules get unit tests, sanitizers on
TESTS := $(BUILD)/tests/schedule_test $(BUILD)/tests/hiccup_test $(BUILD)/tests/sync_test \
	$(BUILD)/tests/mode_test $(BUILD)/tests/pacer_test $(BUILD)/tests/gl_test

$(BUILD)/tests/schedule_test: tests/schedule_test.c src/render/schedule.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

$(BUILD)/tests/sync_test: tests/sync_test.c src/render/sync.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

$(BUILD)/tests/hiccup_test: tests/hiccup_test.c src/render/hiccup.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

$(BUILD)/tests/mode_test: tests/mode_test.c src/output/mode.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

$(BUILD)/tests/pacer_test: tests/pacer_test.c src/game/pacer.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

# flame's renderer against wlroots' on a real gpu. no sanitizers: gpu
# drivers trip them on their own
$(BUILD)/tests/gl_test: tests/gl_test.c src/gl/gl.c src/gl/pass.c src/gl/glass.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) $(DEFS) -Isrc $(PKG_CFLAGS) -o $@ $^ $(PKG_LIBS) -lm

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

$(E2E)/wlstall: tests/e2e/wlstall.c $(E2E)/xdg-shell-protocol.c $(E2E)/xdg-shell-client-protocol.h
	$(CC) -std=c17 -O1 -g -I$(E2E) -o $@ tests/e2e/wlstall.c $(E2E)/xdg-shell-protocol.c \
		$(shell pkg-config --cflags --libs wayland-client)

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

# glass for apps: our protocol and the standard one, client side
$(E2E)/flame-eyecandy-v1-client-protocol.h: protocols/flame-eyecandy-v1.xml
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) client-header $< $@

$(E2E)/flame-eyecandy-v1-protocol.c: protocols/flame-eyecandy-v1.xml
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) private-code $< $@

$(E2E)/ext-background-effect-v1-client-protocol.h: $(BG_EFFECT_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) client-header $< $@

$(E2E)/ext-background-effect-v1-protocol.c: $(BG_EFFECT_XML)
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) private-code $< $@

GLASS_CLIENT := $(E2E)/flame-eyecandy-v1-client-protocol.h $(E2E)/flame-eyecandy-v1-protocol.c \
	$(E2E)/ext-background-effect-v1-client-protocol.h $(E2E)/ext-background-effect-v1-protocol.c

$(E2E)/wlglass: tests/e2e/wlglass.c $(GLASS_CLIENT) $(E2E)/xdg-shell-protocol.c \
		$(E2E)/xdg-shell-client-protocol.h
	$(CC) -std=c17 -O1 -g -D_GNU_SOURCE -I$(E2E) -o $@ tests/e2e/wlglass.c \
		$(E2E)/flame-eyecandy-v1-protocol.c $(E2E)/ext-background-effect-v1-protocol.c \
		$(E2E)/xdg-shell-protocol.c $(shell pkg-config --cflags --libs wayland-client)

$(E2E)/portalcast: tests/e2e/portalcast.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -D_GNU_SOURCE -o $@ $< \
		$(shell pkg-config --cflags --libs libpipewire-0.3 libsystemd egl glesv2 libdrm)

$(E2E)/%: tests/e2e/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g -D_POSIX_C_SOURCE=200809L -o $@ $< $(shell pkg-config --cflags --libs xcb)

e2e: $(TARGET) $(CTL) $(E2E)/wldnd $(E2E)/xdnd $(E2E)/xclip $(E2E)/xwins $(E2E)/glx \
		$(E2E)/glxchild $(E2E)/xewmh $(E2E)/gtkcsd $(E2E)/xcursor $(E2E)/xkeys $(E2E)/wlcapture $(E2E)/glxfull $(E2E)/wlgame $(E2E)/wlstall \
		$(E2E)/portalcast $(E2E)/wlglass $(SETTINGS) $(FILES) $(PLAY) $(BAR)
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

# xdg-desktop-portal reads backends from its own data dir only, whatever
# PREFIX is
PORTALDIR ?= /usr/share/xdg-desktop-portal

install: release
	install -Dm755 build-release/flamewm $(DESTDIR)$(PREFIX)/bin/flamewm
	install -Dm755 build-release/flamectl $(DESTDIR)$(PREFIX)/bin/flamectl
	install -Dm755 build-release/flamesettings $(DESTDIR)$(PREFIX)/bin/flamesettings
	install -Dm755 build-release/flamefiles $(DESTDIR)$(PREFIX)/bin/flamefiles
	install -Dm755 build-release/flameplay $(DESTDIR)$(PREFIX)/bin/flameplay
	install -Dm755 build-release/flamebar $(DESTDIR)$(PREFIX)/bin/flamebar
	install -Dm644 data/flamesettings.desktop \
		$(DESTDIR)$(PREFIX)/share/applications/flamesettings.desktop
	install -Dm644 data/flamefiles.desktop $(DESTDIR)$(PREFIX)/share/applications/flamefiles.desktop
	install -Dm644 data/flameplay.desktop $(DESTDIR)$(PREFIX)/share/applications/flameplay.desktop
	install -Dm644 data/flamewm.portal $(DESTDIR)$(PORTALDIR)/portals/flamewm.portal
	install -Dm644 data/flamewm-portals.conf $(DESTDIR)$(PORTALDIR)/flamewm-portals.conf

clean:
	rm -rf build build-asan build-release

.PHONY: all asan release run clangd install clean test e2e

-include $(OBJS:.o=.d)
