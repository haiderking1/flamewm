# flamewm

VERSION := 0.1.0-dev
PREFIX  ?= /usr/local
BUILD   ?= build
TARGET  := $(BUILD)/flamewm
CTL     := $(BUILD)/flamectl

# clang only. := so an env CC can't sneak gcc in, `make CC=clang-21` still works
CC := clang

# every lib here has to be in deps.md too
PKGS := wlroots-0.20 wayland-server xkbcommon pixman-1 egl libdrm xshmfence

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
TESTS := $(BUILD)/tests/schedule_test $(BUILD)/tests/hiccup_test

$(BUILD)/tests/schedule_test: tests/schedule_test.c src/render/schedule.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

$(BUILD)/tests/hiccup_test: tests/hiccup_test.c src/render/hiccup.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -fsanitize=address,undefined -Isrc -o $@ $^

# flame's X server on its own, a test client against it through the harness.
# the clients are plain xcb (test-only deps, see deps.md)
XSERVER_SRCS := $(wildcard src/xserver/*.c) src/util/strbuf.c
XTEST_PKGS := xcb xcb-xinput xcb-randr xcb-xkb xkbcommon-x11 xkbcommon
XTESTS := $(BUILD)/tests/xinput_test $(BUILD)/tests/xkb_test

$(BUILD)/tests/harness: tests/xserver/harness.c $(XSERVER_SRCS)
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) $(DEFS) -fsanitize=address,undefined -Isrc $(PKG_CFLAGS) \
		-o $@ $^ $(PKG_LIBS) -lm

$(BUILD)/tests/%_test: tests/xserver/%_test.c
	@mkdir -p $(dir $@)
	$(CC) -std=c17 -O1 -g $(WARN) -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined \
		-o $@ $< $(shell pkg-config --cflags --libs $(XTEST_PKGS))

test: $(TESTS) $(BUILD)/tests/harness $(XTESTS)
	@for t in $(TESTS); do $$t || exit 1; done
	@for t in $(XTESTS); do $(BUILD)/tests/harness $$t || exit 1; done

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

clean:
	rm -rf build build-asan build-release

.PHONY: all asan release run clangd install clean test

-include $(OBJS:.o=.d)
