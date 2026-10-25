# flame

flame is a tiling wayland compositor that's as light as it gets, without
giving up a single feature. X11 apps and games run on its own X server,
written from scratch and built right in. no Xwayland anywhere.

**[install](docs/install.md)** · **[configure](docs/config.md)** · **[deps](deps.md)**

## features

- its own X server, written from scratch: Steam, Proton and X11 apps without Xwayland
- light: ~30MB of its own memory, X server included
- made for games: tearing, vrr, and frame pacing with nothing injected into the game
- its own renderer: glass, blur, rounded corners and animations, all drawn by flame
- its own frame scheduler: nothing drawn when nothing changes, frames start as late as they can
- direct scanout for fullscreen apps
- a hardware cursor that actually works on nvidia
- bspwm style tiling, floating windows, 10 workspaces, focus follows the mouse
- screen sharing with its own portal and picker: point at a window, click, done
- its own login screen and lock screen, on greetd
- its own apps: settings, file manager (and every app's file dialog), video and music player, search, a bar with a control center and notifications
- all of the apps optional: use waybar, or nothing
- every setting live, from the settings app or `flamectl set`
- lua config for everything else
- socket-based ipc, plus ext-workspace for waybar
- explicit sync, and windows on hidden workspaces told to stop drawing
- frame logs that show where a slow frame's time went
- and more...

## special thanks

- wlroots - for doing the basic boring stuff
- greetd - for handling the login part
- foot - for being the terminal every test runs in

## license

MIT, see [LICENSE](LICENSE).
