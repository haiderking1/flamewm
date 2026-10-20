# deps

everything flamewm needs. add stuff here the moment it gets pulled in, not later.

install all of it (arch):

```sh
sudo pacman -S --needed clang wlroots0.20 wayland wayland-protocols libxkbcommon pixman libglvnd libdrm libxshmfence make pkgconf foot
```

## build

| dep | pkg-config | why |
| --- | --- | --- |
| clang (C17) | - | the compiler. clang only, no gcc |
| make | - | build |
| pkgconf | - | finds libs and cflags |
| wayland-protocols | `wayland-protocols` | wlroots headers include its `*-enum.h` files (xdg-shell, cursor-shape, ext-workspace...) |
| wayland-scanner (comes with `wayland`) | `wayland-scanner` | generates headers from `protocols/*.xml`, layer-shell needs it |

## link

| dep | pkg-config | why |
| --- | --- | --- |
| wlroots 0.20 | `wlroots-0.20` | backend, renderer, scene graph, xdg-shell, seat, cursor. pinned to 0.20 (latest stable, 0.20.2), api breaks between minors |
| wayland | `wayland-server` | the wayland server lib itself |
| libxkbcommon | `xkbcommon` | keymaps + keysyms for keybinds |
| pixman | `pixman-1` | region math: damage, occlusion, checking a client's opaque region before showing a resize |
| libEGL (libglvnd) | `egl` | gpu timing: GL_TIME_ELAPSED queries in the renderer's context, GL functions are loaded through it so GLES itself isn't linked |
| libxshmfence | `xshmfence` | flame's X server: the fences X11 apps share with it for their gpu buffers (DRI3), triggered when flame is done with a buffer |
| libdrm | `libdrm` | the hardware cursor: dumb buffers the cpu writes the pointer into, on their own fd (empty lease or reopened + authenticated), handed to the cursor plane as linear ARGB8888. nvidia can't render to linear, so wlroots' gpu cursor never made it there |

`flamectl` and the ipc socket are plain libc (unix sockets), nothing extra.

wlroots also pulls in libinput, libseat (seatd), libliftoff,
libdisplay-info, lcms2, vulkan loader and some xcb utils. pacman handles those.

## tests

`make test` only, flamewm never links these.

| dep | pkg-config | why |
| --- | --- | --- |
| libxcb + xcb-xinput, xcb-randr, xcb-xkb (come with `libxcb`) | `xcb` `xcb-xinput` `xcb-randr` `xcb-xkb` | the X server tests: plain xcb clients against flame's X server, run by `tests/xserver/harness.c` |
| libxkbcommon-x11 (comes with `libxkbcommon`) | `xkbcommon-x11` | the XKB test reads the keymap back the way kitty and alacritty do |

## runtime

| dep | for |
| --- | --- |
| foot | default terminal on `super+q`, change it in `src/config/defaults.c` |
| seatd or logind | only when running on a real tty, nested doesn't need it |

## later (not wired up yet)

| dep | for |
| --- | --- |
| lua 5.4 / luajit | runtime config |
