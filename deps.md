# deps

everything flamewm needs. add stuff here the moment it gets pulled in, not later.

install all of it (arch):

```sh
sudo pacman -S --needed clang wlroots0.20 wayland wayland-protocols libxkbcommon pixman libglvnd libdrm libxshmfence lua54 make pkgconf foot
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
| lua 5.4 | `lua5.4` | the config: `~/.config/flame/init.lua`, see `src/config/lua.c` for what it can say |
| libdrm | `libdrm` | the hardware cursor: dumb buffers the cpu writes the pointer into, on their own fd (empty lease or reopened + authenticated), handed to the cursor plane as linear ARGB8888. nvidia can't render to linear, so wlroots' gpu cursor never made it there |

`flamectl` and the ipc socket are plain libc (unix sockets), nothing extra.

wlroots also pulls in libinput, libseat (seatd), libliftoff,
libdisplay-info, lcms2, vulkan loader and some xcb utils. pacman handles those.

## runtime

not linked, flame works without them, these features don't.

| dep | why |
| --- | --- |
| xdg-desktop-portal, xdg-desktop-portal-wlr, pipewire | screen sharing (obs, discord, browsers): the portal asks wlr's backend, it captures a screen or one window through flame's ext-image-copy-capture into pipewire. `data/flamewm-portals.conf` (installed by `make install`) points the portal at it |
| rofi | picks what to share, screens and windows by title (`data/xdg-desktop-portal-wlr-flamewm`, installed to `/etc/xdg/xdg-desktop-portal-wlr/flamewm`). any dmenu-like works, change `chooser_cmd` |
| xdg-desktop-portal-gtk | the rest of the portal: file pickers and such |

## tests

`make test` and `make e2e` only, flamewm never links these.

| dep | pkg-config | why |
| --- | --- | --- |
| libxcb and its extension libs (come with `libxcb`) | `xcb` `xcb-xinput` `xcb-randr` `xcb-xkb` `xcb-xfixes` `xcb-xinerama` `xcb-screensaver` `xcb-dpms` `xcb-xtest` `xcb-shape` `xcb-render` | the X server tests: plain xcb clients against flame's X server, run by `tests/xserver/harness.c`, and `make e2e`'s X cursors |
| libxkbcommon-x11 (comes with `libxkbcommon`) | `xkbcommon-x11` | the XKB test reads the keymap back the way kitty and alacritty do |
| wayland (client side) | `wayland-client` | `make e2e`: the wayland drag and drop client |
| wl-clipboard | - | `make e2e`: wl-copy and wl-paste, the wayland end of the clipboard tests |
| libGL, libX11 (libglvnd) | `gl`, `x11` | `make e2e`: an X GL app, multisampled, swapping frames |
| gtk3 | `gtk+-3.0` | `make e2e`: client side decorations, on X and wayland: shadows, headerbar drags |
| foot, grim | - | `make e2e`: a terminal that has to keep drawing across a vt switch, and screenshots to tell |

## data

| what | where | why |
| --- | --- | --- |
| misc-fixed 6x13 (xorg's font/misc-misc, public domain: "Public domain font. Share and enjoy.") | `src/xserver/fixed.c`, made by `tools/fonts/bdf2c.py 6x13.bdf` | flame's X server's built-in "fixed", the font core text draws with, like Xorg's. iso8859-1, 256 cells baked in, nothing read at runtime |

## runtime

| dep | for |
| --- | --- |
| foot | default terminal on `super+q`, bind another in `~/.config/flame/init.lua` (`examples/init.lua`) |
| seatd or logind | only when running on a real tty, nested doesn't need it |

