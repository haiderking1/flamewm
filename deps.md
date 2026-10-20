# deps

everything flamewm needs. add stuff here the moment it gets pulled in, not later.

install all of it (arch):

```sh
sudo pacman -S --needed clang wlroots0.20 wayland wayland-protocols libxkbcommon pixman make pkgconf foot
```

## build

| dep | pkg-config | why |
| --- | --- | --- |
| clang (C17) | - | the compiler. clang only, no gcc |
| make | - | build |
| pkgconf | - | finds libs and cflags |
| wayland-protocols | `wayland-protocols` | wlroots headers include its `*-enum.h` files (xdg-shell, cursor-shape...) |
| wayland-scanner (comes with `wayland`) | `wayland-scanner` | generates headers from `protocols/*.xml`, layer-shell needs it |

## link

| dep | pkg-config | why |
| --- | --- | --- |
| wlroots 0.20 | `wlroots-0.20` | backend, renderer, scene graph, xdg-shell, seat, cursor. pinned to 0.20 (latest stable, 0.20.2), api breaks between minors |
| wayland | `wayland-server` | the wayland server lib itself |
| libxkbcommon | `xkbcommon` | keymaps + keysyms for keybinds |
| pixman | `pixman-1` | region math: checking a client's opaque region before showing a resize |

wlroots also pulls in libinput, libdrm, libseat (seatd), libliftoff,
libdisplay-info, lcms2, vulkan loader and some xcb utils. pacman handles those.

## runtime

| dep | for |
| --- | --- |
| foot | default terminal on `super+q`, change it in `src/config/defaults.c` |
| seatd or logind | only when running on a real tty, nested doesn't need it |

## later (not wired up yet)

| dep | for |
| --- | --- |
| lua 5.4 / luajit | runtime config |
