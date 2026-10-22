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
callbacks on the refresh grid, one turn per window per refresh. a lone
fullscreen window whose buffer KMS can take goes straight to the display
(direct scanout), no compositing. X games too: their presented buffers go
to KMS themselves and go back to the game once a later frame flipped.

windows covered by something opaque still get frame callbacks, four times a
second while they ask, so they don't freeze without burning the gpu, and
their redraws don't repaint the screen. a client buffer the gpu is still
writing waits before it's used (explicit sync clients say so themselves),
the window keeps its last frame instead of one slow app delaying the screen.

`FLAME_FRAME_PIPELINE` picks the policy:

- `adaptive` (default): draws as soon as the screen can take a frame. when
  frames get expensive (3/4 of a refresh) it starts preparing the next one
  right before the pending flip lands, and stops again below 1/2
- `immediate`: only ever draws once the pending frame flipped
- `deadline`: always prepares ahead, for comparing

anything else stops startup, a typo shouldn't quietly test the wrong thing.

### tearing and vrr

a fullscreen game with vsync off asks to tear (wayland: tearing-control
async, X: PresentOptionAsync, what dxvk and vkd3d send): its frames go out
the moment they're ready as async page flips, scanned out or composed. a
flip KMS refuses goes out as a plain one right away, a run of refusals turns
tearing off until the mode changes or a vt switch. apps that don't ask
never tear.

vrr (adaptive sync) lets the display wait for frames. with it on, flame
sends every frame the moment it's ready instead of at the repaint window.
switching it rides on a frame and gets a KMS test first, so a display that
won't switch never costs one, it just stays how it is.

```lua
flame.set { allow_tearing = true, vrr = "fullscreen" } -- the defaults
-- vrr: "off", "fullscreen" (while something fullscreen shows), "always"
```

`flamectl state` says per output whether it's scanning out, tearing and
running vrr right now, so does frame timing. `vrr_capable` is whether the
display can do it at all: false usually means freesync is off in the
display's own menu. turn it on there and flame sees it straight away, no
replug.

### pacing

the fullscreen X game you're in gets its frames on screen at an even pace,
the fastest one it can hold: a flat frametime line, nothing to set. the
pace follows how the frames turn out, whatever the game does inside (dx12
games present from a thread of their own): a frame that needed longer than
the pace once it could start backs it off, frames on time speed it up a
hair. it glides, so with vrr nothing on screen jumps, and settles where
nearly every frame makes its slot. after a hitch (a shader compiling) the
frames that piled up keep the pace instead of rushing out.

it's all on flame's side of the X server, no injecting into the game: a
game starts its next frame when it gets a buffer back, so flame hands one
back just in time for the frame to be ready at its slot (the input it
draws is the freshest it can be, like reflex), and shows it on the slot. a
game that kept a spare buffer (it presents with nothing handed back, a
frame ahead, lag) loses it, and frames lining up for their slots drop to
the newest. a new swapchain (fullscreen, a resize) gets its old buffers
back at once, and a game that waits 100ms on held ones gets them all back
(`pace_stuck` in frame timing counts that).

```lua
flame.set { pace = "vrr", pace_fps = 0 } -- the defaults
-- pace: "off", "vrr" (while the display runs vrr, it shows any pace
-- evenly), "always". pace_fps: a cap on top, 0 for none
```

a hitch is still a hitch: a frame that isn't drawn yet can't be shown.
`flamectl state` and frame timing say the pace (`pace_fps`).

### cursor

flame owns the pointer image, wlroots never draws one. on a real tty it goes
on the cursor plane: the image gets written by the cpu into a dumb buffer
(that's what nvidia's cursor plane takes, it can't take what the gpu renders)
and KMS moves it. moving the mouse redraws nothing, it just sends the frame
that's already on screen again with the plane somewhere else, and a
fullscreen window keeps direct scanout with the pointer over it.

when the plane can't show it (nested, headless, a rotated output, a client
cursor at the wrong scale, a recorder that wants the cursor in its frames)
it's drawn as the topmost thing in the frame instead.
`FLAME_HARDWARE_CURSOR=0` always draws it, `1` (default) uses the plane when
it can, anything else stops startup.

### frame timing

`FLAME_FRAME_TIMING=1 ./flamewm.run` logs a `flame: frame-timing` line per
output every two seconds, only observing, nothing about rendering changes:

- `pipeline` / `overlap` / `lead_ms`: the policy, whether it's preparing
  frames ahead right now, and what it thinks a frame costs (cpu + gpu + margin)
- `asap` / `vrr`: frames go out the moment they're ready (a game tearing,
  or vrr on), and whether the display runs adaptive sync
- `pace_fps` / `pace_stuck`: the fullscreen game's pace (see pacing), 0 when
  none is paced, and times it waited on buffers the pacer held
- `flips` / `flip_hz`: pageflips that reached the screen, not app fps
- `composited` / `scanout` / `cursor` / `failed`: frames we drew, client
  buffers sent straight to KMS, the same buffer again because only the
  cursor plane moved, and frames KMS refused
- `tearing`: frames that went out as tearing flips
- `successors` / `promoted` / `discarded`: frames prepared behind a pending
  flip, sent when it landed, or refused right then
- `timer_late_ms` / `timer_early`: how far off the frame timer fires
- `draws` / `draw_ms`: building + drawing + committing a changed frame (cpu)
- `empty` / `empty_ms`: frames where nothing got drawn (cursor only counts here)
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
  cursor/       the pointer image (theme or client surface) and the cursor plane it goes on
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
  game/         what's for games: the pacer (an even frame pace, the fastest held)
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
 "window":{"app_id":"foot","title":"~","fullscreen":false},
 "outputs":[{"name":"DP-2","scanout":false,"tearing":false,"vrr":false,"vrr_capable":true,
   "pace_fps":0.0}]}
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
