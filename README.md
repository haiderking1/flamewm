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
make test       # unit tests for the pure logic (the frame scheduler)
```

`flamewm -s foot` runs a command once it's up, `-d` turns on debug logs.

### frame pipeline

flame decides when to draw, not wlroots: one frame waiting for KMS at a time,
plus at most one prepared behind it (raven's scheduler). when nothing
changed nothing gets drawn or flipped, clients still get their frame
callbacks on the refresh grid. a lone fullscreen window whose buffer KMS
can take goes straight to the display (direct scanout), no compositing.

`FLAME_FRAME_PIPELINE` picks the policy:

- `adaptive` (default): draws as soon as the screen can take a frame. when
  frames get expensive (3/4 of a refresh) it starts preparing the next one
  right before the pending flip lands, and stops again below 1/2
- `immediate`: only ever draws once the pending frame flipped
- `deadline`: always prepares ahead, for comparing

anything else stops startup, a typo shouldn't quietly test the wrong thing.

### frame timing

`FLAME_FRAME_TIMING=1 ./flamewm.run` logs a `flame: frame-timing` line per
output every two seconds, only observing, nothing about rendering changes:

- `pipeline` / `overlap` / `lead_ms`: the policy, whether it's preparing
  frames ahead right now, and what it thinks a frame costs (cpu + gpu + margin)
- `flips` / `flip_hz`: pageflips that reached the screen, not app fps
- `composited` / `scanout` / `failed`: frames we drew, client buffers sent
  straight to KMS, and frames KMS refused
- `successors` / `promoted` / `discarded`: frames prepared behind a pending
  flip, sent when it landed, or refused right then
- `timer_late_ms` / `timer_early`: how far off the frame timer fires
- `draws` / `draw_ms`: building + drawing + committing a changed frame (cpu)
- `empty` / `empty_ms`: frames where nothing changed, nothing drawn
- `gpu_batch_max_ms`: our gpu work per frame, the longest finished
  measurement since the last frame (results arrive late, nothing waits)
- `flip_ms`: spacing between kernel flip timestamps, 6.06ms is one 165hz frame
- `seq_steps`: vblanks between flips, 1 is every refresh, more means skipped
- `dispatch_ms`: kernel flip timestamp to flame handling it

`*_ms` are min/mean/max, `n/a` means no samples. `grep 'frame-timing' /tmp/flamewm.log`

## layout

one directory, one job. nothing in here should turn into a god file.

```
src/
  main.c        args, start, run, tear down
  core/         server root, init/teardown order, unix signals
  backend/      wlr_backend + session, vt switching
  scene/        flame's own scene graph: what goes where, which client surfaces changed
  render/       the frame pipeline: elements, occlusion, damage, explicit sync, commits
  output/       monitors: modes, frame callbacks, output layout, background
  protocols/    globals wlroots runs on its own (compositor, clipboard, screencopy...),
                ext-workspace for waybar
  shell/        xdg-shell: toplevels become views, popups
  layers/       layer-shell: bars, launchers, wallpapers. exclusive zones, keyboard grabs
  desktop/      window model: views, workspaces, focus. protocol agnostic
  layout/       bspwm style tree, pure data + math
  transaction/  layout changes land in one frame, raven style resize batches
  input/        seat, keyboards, cursor
  bindings/     keybind matching + the actions they run
  config/       compiled in defaults, lua later
  ipc/          flame's own socket for bars and scripts: commands in, state out
  util/         listener helpers, spawn, macros, string buffers

tools/
  flamectl/     the ipc from a shell

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
  floating, top, fullscreen, grabbing launchers, overlay. every workspace is a
  tree under `tiled` that gets turned on and off
- flame renders by itself, no wlr_scene: every frame `render/` flattens the
  scene into elements, drops what's covered, diffs against the last frame to
  find damage, draws only that through the wlroots renderer and commits. the
  renderer is just "this texture here, this rect there"

## binds

mod is super.

| bind | does |
| --- | --- |
| `super+q` / `super+enter` | foot |
| `super+c` | close window |
| `super+j` / `super+k` | focus next / prev |
| `super+f` | toggle fullscreen |
| `super+1..9`, `super+0` | go to workspace (0 is the 10th) |
| `super+shift+1..9`, `super+shift+0` | send window to workspace |
| `super` + left drag | drop a window on another to swap them |
| `super` + right drag | resize in place, the edges nearest to where you grab follow the cursor |
| `super+shift+q` | quit |
| `ctrl+alt+f1..f12` | switch vt (tty only) |

focus follows the mouse like hyprland's `follow_mouse = 1`: moving onto a
window focuses it, a window that moves under a still cursor doesn't. holding
a mouse button, an open launcher or a fullscreen window keep focus where it is.
`focus_follows_mouse` in `config/defaults.c` turns it off.

## ipc

flame has its own socket, nothing third party in between. everything flame
starts gets it in `$FLAMEWM_SOCKET` (`$XDG_RUNTIME_DIR/flamewm.<wayland display>.sock`).
one line per message: send a command, get `{"ok":true}` or
`{"ok":false,"error":"..."}` back.

| command | does |
| --- | --- |
| `subscribe` | the state now, and again every time it changes |
| `state` | the state now, once |
| `workspace <1-10>` | go there |
| `movetoworkspace <1-10>` | send the focused window there |
| `focus next` / `focus prev` | |
| `fullscreen` | toggle it on the focused window |
| `close` | close the focused window |
| `quit` | |

the state is one json line:

```json
{"event":"state","workspace":1,
 "workspaces":[{"id":1,"name":"1","windows":2,"active":true,"fullscreen":false}, ...],
 "window":{"app_id":"foot","title":"~","fullscreen":false}}
```

`flamectl` does it from a shell: `flamectl workspace 3`, `flamectl subscribe`.
quickshell can talk to the socket straight away with a `Socket`.

### waybar

flame also speaks ext-workspace, so waybar's built in module just works.
empty workspaces you're not on are hidden, `"ignore-hidden": false` shows all 10:

```jsonc
"ext/workspaces": { "format": "{name}", "on-click": "activate", "sort-by-id": true }
```

## status

single output, bspwm tiling only. xdg-shell and layer-shell (waybar, rofi,
fuzzel, swaybg, mako...), fullscreen, 10 workspaces with an ipc socket and
ext-workspace for bars. no floating yet. no xwayland on purpose.

## license

MIT, see [LICENSE](LICENSE).
