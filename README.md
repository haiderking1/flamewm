# flamewm

tiling wayland compositor in C on wlroots 0.20. raven but without waiting on rustc.

## build + run

deps are in [deps.md](deps.md).

```sh
make            # build/flamewm
make run        # nested inside your current compositor
make asan       # build-asan/flamewm, address + ub sanitizers
make release    # build-release/flamewm, -O2 + lto
make clangd     # compile_flags.txt so clangd stops yelling
```

`flamewm -s foot` runs a command once it's up, `-d` turns on debug logs.

## layout

one directory, one job. nothing in here should turn into a god file.

```
src/
  main.c        args, start, run, tear down
  core/         server root, init/teardown order, unix signals
  backend/      wlr_backend + session, vt switching
  render/       renderer, allocator, scene graph, z-order layers
  output/       monitors: modes, frame callbacks, output layout, background
  protocols/    globals wlroots runs on its own (compositor, clipboard, screencopy...)
  shell/        xdg-shell: toplevels become views, popups
  layers/       layer-shell: bars, launchers, wallpapers. exclusive zones, keyboard grabs
  desktop/      window model: views, workspaces, focus. protocol agnostic
  layout/       bspwm style tree, pure data + math
  transaction/  layout changes land in one frame, raven style resize batches
  input/        seat, keyboards, cursor
  bindings/     keybind matching + the actions they run
  config/       compiled in defaults, lua later
  util/         listener helpers, spawn, macros

protocols/      protocol xml wlroots doesn't ship headers for, generated at build
```

how stuff connects:

- `core/server.c` brings every subsystem up in dependency order and down in
  the order wlroots needs (it asserts on leftover listeners, so every
  `listener_connect` has a `listener_disconnect`)
- shells implement `struct flame_view_impl`, desktop never knows which protocol
  a window came from. xwayland will just be another impl
- windows get their final tile size in the very first configure and stay
  hidden until a transaction places them, so nothing spawns at 0,0 and grows
- the scene graph is layered bottom to top: background, bottom, tiled,
  floating, top, overlay. every workspace is a tree under `tiled` that gets
  turned on and off

## binds

mod is super.

| bind | does |
| --- | --- |
| `super+q` / `super+enter` | foot |
| `super+c` | close window |
| `super+j` / `super+k` | focus next / prev |
| `super+f` | toggle fullscreen |
| `super+1..9` | go to workspace |
| `super+shift+1..9` | send window to workspace |
| `super` + left drag | drop a window on another to swap them |
| `super` + right drag | resize in place, the edges nearest to where you grab follow the cursor |
| `super+shift+q` | quit |
| `ctrl+alt+f1..f12` | switch vt (tty only) |

focus follows the mouse like hyprland's `follow_mouse = 1`: moving onto a
window focuses it, a window that moves under a still cursor doesn't. holding
a mouse button, an open launcher or a fullscreen window keep focus where it is.
`focus_follows_mouse` in `config/defaults.c` turns it off.

## status

single output, bspwm tiling only. xdg-shell and layer-shell (waybar, rofi,
fuzzel, swaybg, mako...). no floating, no fullscreen yet. no xwayland on purpose.

## license

MIT, see [LICENSE](LICENSE).
