# flamewm

a tiling wayland compositor in C on wlroots 0.20, built for games.

X11 apps and games run through flame's own X server, built in. no
Xwayland needed, Steam and Proton games included.

## what you get

- **bspwm style tiling**, 10 workspaces, focus follows the mouse
- **its own frame scheduler**: nothing drawn when nothing changed, a
  fullscreen app goes straight to the display (direct scanout)
- **tearing** for games with vsync off, **vrr** while a game's fullscreen,
  no hitch going to the browser and back
- **frame pacing**: the fullscreen game you're in gets its frames on screen
  at an even pace, the fastest it can hold. a flat frametime line with
  nothing to set and no injecting into the game
- **hardware cursor** that works on nvidia
- **screen sharing** through its own portal and picker: the window you
  want lights up under the mouse, a glass panel on it, click to share.
  an app asking twice (browsers do) doesn't make you pick twice. flame
  draws everything itself, blur and glass included
- **its own apps**, on its glass: settings (`super+,`) and a file manager,
  flamefiles (`super+e`)
- an **ipc socket** for bars and scripts, and ext-workspace for waybar

## build + run

deps are in [deps.md](deps.md).

```sh
make            # build/flamewm
make run        # nested inside your current compositor
make release    # build-release/flamewm
make test       # unit tests
make e2e        # end to end tests on headless flames
sudo make install  # flamewm, flamectl, its apps, the portal files screen sharing needs
```

`flamewm -c config -s command -d`: a config file, a command to run once
it's up, debug logs.

## config

none needed. **flamesettings** (`super+,`) changes things live, on flame's
glass, so does `flamectl set gap 8` (`flamectl settings` lists them all).
flame keeps them in `~/.config/flame/settings.lua`. the wallpaper too: flame
draws it itself, pick one in flamesettings or `flamectl set wallpaper ~/walls/x.jpg`.

for more, lua at `~/.config/flame/init.lua`, see
[examples/init.lua](examples/init.lua). what it sets wins over settings.lua.

```lua
flame.set {
  allow_tearing = true, -- games with vsync off tear
  vrr = "fullscreen",   -- "off", "fullscreen", "always"
  pace = "vrr",         -- "off", "vrr", "always"
  pace_fps = 0,         -- a cap, 0 for none
}
```

## binds

mod is super.

| bind | does |
| --- | --- |
| `super+q` / `super+enter` | foot |
| `super+c` | close window |
| `super+,` | settings |
| `super+e` | files |
| `super+j` / `super+k` | focus next / prev |
| `super+f` | fullscreen |
| `super+1..0` | go to workspace |
| `super+shift+1..0` | send window to workspace |
| `super` + left drag | swap two windows |
| `super` + right drag | resize |
| `super+shift+q` | quit |

## ipc

`flamectl workspace 3`, `flamectl fullscreen`, `flamectl subscribe` (the
state as one json line, again on every change). the socket is in
`$FLAMEWM_SOCKET`, so quickshell and friends can talk to it directly.

for waybar the built in workspace module just works:

```jsonc
"ext/workspaces": { "format": "{name}", "on-click": "activate", "sort-by-id": true }
```

## debugging frames

`FLAME_FRAME_TIMING=1` logs a timing line per output every 2 seconds:
flips, scanout, tearing, vrr, the game's pace and its hitches.
`grep frame-timing /tmp/flamewm.log`

## license

MIT, see [LICENSE](LICENSE).
