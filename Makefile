# flamewm

VERSION := 0.1.0-dev
PREFIX  ?= /usr/local
BUILD   ?= build
TARGET  := $(BUILD)/flamewm

# clang only. := so an env CC can't sneak gcc in, `make CC=clang-21` still works
CC := clang

# every lib here has to be in deps.md too
PKGS := wlroots-0.20 wayland-server xkbcommon

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

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(SAN) $(LDFLAGS) -o $@ $^ $(FLAME_LIBS)

$(BUILD)/obj/%.o: src/%.c | $(PROTO_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(FLAME_CFLAGS) -c -o $@ $<

$(BUILD)/gen/%-protocol.h: protocols/%.xml
	@mkdir -p $(dir $@)
	$(WAYLAND_SCANNER) server-header $< $@

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

clean:
	rm -rf build build-asan build-release

.PHONY: all asan release run clangd install clean

-include $(OBJS:.o=.d)
