# config

flame's config is one lua file, `~/.config/flame/flame.lua`. flame writes
a first one the first time it starts, and reloads it the moment you save
it. every option is in [examples/flame.lua](../examples/flame.lua), each
block saying what it's for.

```lua
flame.look { gap = 8, border_width = 2 }
flame.keyboard { layout = "us,ara", options = "grp:alt_shift_toggle" }
flame.mouse { focus_follows = true }
flame.lockscreen { command = "flamelock" }  -- what super+l runs, "" for nothing
```

every block is its own function: `flame.look`, `flame.wallpaper`,
`flame.lockscreen`, `flame.keyboard`, `flame.mouse`, `flame.x11`, `flame.blur`, `flame.animations`. all of them with every option
in [examples/flame.lua](../examples/flame.lua).

## binds

mod is super.

| bind | does |
| --- | --- |
| `super+q` | terminal (foot) |
| `super+c` | close window |
| `super+e` | files |
| `super+space` | search |
| `super+l` | lock (whatever `lock` says) |
| `super+j` / `super+k` | focus next / previous |
| `super+f` | fullscreen |
| `super+shift+space` | float / tile |
| `super+1..0` | go to workspace |
| `super+shift+1..0` | send window to workspace |
| `super` + left drag | swap two windows |
| `super` + right drag | resize |
| `super+shift+q` | quit |

## ipc

`flamectl workspace 3`, `flamectl fullscreen`, `flamectl subscribe` (the
whole state as one json line, and again on every change). the socket is
in `$FLAMEWM_SOCKET`, so quickshell and friends can talk to it directly.

waybar's built in workspace module just works:

```jsonc
"ext/workspaces": { "format": "{name}", "on-click": "activate", "sort-by-id": true }
```

## when frames feel off

`flamectl frame-timing on` (or `FLAME_FRAME_TIMING=1`) logs a line per
screen every 2 seconds: flips, scanout, tearing, vrr, a game's pace and its
hitches. every workspace switch writes a few lines too: how many frames made
it, how long the gpu took, and which apps were drawing during it. `grep
workspace $XDG_RUNTIME_DIR/flamewm.log`. off again with `flamectl
frame-timing off`, off by default. `flamectl frame-pipeline adaptive|immediate|deadline`
changes when frames start drawing, live, if you want to compare.
