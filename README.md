# flame

flame is a tiling wayland compositor that's as light as it gets, without
giving up a single feature. X11 apps and games run on its own X server,
written from scratch and built right in. no Xwayland anywhere.

**[install](docs/install.md)** · **[configure](docs/config.md)** · **[deps](deps.md)**

## install (arch)

flame with all its apps:

```sh
curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/install.sh | sh
```

just the window manager, barebones:

```sh
curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/install-core.sh | sh
```

to update flame to the newest version, run `flame-update`.

## features

- its own X server, written from scratch: Steam, Proton and X11 apps without Xwayland
- light: ~30MB of its own memory, X server included
- made for games: tearing, vrr, and frame pacing with nothing injected into the game
- its own renderer: glass, blur, rounded corners and animations, all drawn by flame
- its own frame scheduler
- a hardware cursor that actually works on nvidia
- bspwm style tiling, floating windows, 10 workspaces, focus follows the mouse
- screen sharing with its own portal and picker: point at a window, click, done
- its own apps, all optional: settings, file manager (and every app's file dialog), video and music player, search, a bar with a control center and notifications, a lock screen and a login screen. or bring your own bar and config
- every setting live: a lua config at `~/.config/flame/flame.lua`, reloaded the moment you save it, or an optional settings app (`super+,`) if you want them graphical
- ext-workspace for waybar
- and more...

## special thanks

- wlroots - for doing the basic boring stuff

## license

MIT, see [LICENSE](LICENSE).
