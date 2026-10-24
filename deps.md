# deps

everything flamewm needs. add stuff here the moment it gets pulled in, not later.

install all of it (arch):

```sh
sudo pacman -S --needed clang wlroots0.20 wayland wayland-protocols libxkbcommon pixman libglvnd libdrm libxshmfence lua54 pango cairo pipewire systemd-libs libjpeg-turbo libpng libwebp librsvg ffmpeg libass uchardet vulkan-headers shaderc make pkgconf foot
```

## build

| dep | pkg-config | why |
| --- | --- | --- |
| clang (C17) | - | the compiler. clang only, no gcc |
| make | - | build |
| pkgconf | - | finds libs and cflags |
| wayland-protocols | `wayland-protocols` | wlroots headers include its `*-enum.h` files (xdg-shell, cursor-shape, ext-workspace...) |
| wayland-scanner (comes with `wayland`) | `wayland-scanner` | generates headers from `protocols/*.xml`, layer-shell needs it |
| vulkan-headers | - | flameplay's gpu decoding: `vulkan/vulkan.h` (ffmpeg's vulkan frames are vulkan objects). nothing's linked, the functions come through ffmpeg's vulkan device |
| glslc (`shaderc`) | - | flameplay's gpu pass (`tools/flameplay/convert.comp`) built to spir-v, baked into flameplay |

## link

| dep | pkg-config | why |
| --- | --- | --- |
| wlroots 0.20 | `wlroots-0.20` | backend, renderer, scene graph, xdg-shell, seat, cursor. pinned to 0.20 (latest stable, 0.20.2), api breaks between minors |
| wayland | `wayland-server` | the wayland server lib itself |
| libxkbcommon | `xkbcommon` | keymaps + keysyms for keybinds |
| pixman | `pixman-1` | region math: damage, occlusion, checking a client's opaque region before showing a resize |
| libEGL (libglvnd) | `egl` | flame's renderer (`src/gl/`): its shaders draw every frame in the GL context wlroots made, fences through EGL_ANDROID_native_fence_sync. gpu timing too. GL functions are loaded through it so GLES itself isn't linked |
| libxshmfence | `xshmfence` | flame's X server: the fences X11 apps share with it for their gpu buffers (DRI3), triggered when flame is done with a buffer |
| lua 5.4 | `lua5.4` | the config: `~/.config/flame/init.lua`, see `src/config/lua.c` for what it can say |
| pango, cairo | `pangocairo` | the share picker: its labels drawn as text. flamesettings (flameui) draws everything with them |
| wayland (client side), libxkbcommon | `wayland-client` `xkbcommon` | flamesettings: flame's own apps are wayland clients (flameui, `tools/flameui`) |
| pipewire | `libpipewire-0.3` | screen sharing: flame's portal streams what's shared into pipewire itself, drawn straight into the stream's gpu buffers (or shared memory for apps that can't take those) |
| sd-bus (systemd) | `libsystemd` | flame's portal on the session bus, what xdg-desktop-portal calls for screen sharing, screenshots and apps' open and save dialogs (flamefiles). "show in folder" (org.freedesktop.FileManager1) too |
| libjpeg-turbo, libpng, libwebp | `libturbojpeg` `libpng` `libwebp` | wallpapers (`src/image/`): flame reads the picture itself, on a worker thread, a jpeg straight at a fraction of its size when less is needed. flamesettings' wallpaper thumbnails too |
| ffmpeg's libraries | `libavformat` `libavcodec` `libswscale` `libavutil` | flame's apps (flameui) only: videos' previews (a frame a tenth of the way in, not a black one) and songs' covers in flamefiles. md5 for the thumbnail cache's names |
| libswresample, pipewire | `libswresample` `libpipewire-0.3` | flameplay: its sound resampled to 48 kHz and played through pipewire, the clock its picture follows |
| libpulse, sd-bus (systemd) | `libpulse` `libsystemd` | flamebar's control center: the default output's level and mute (pipewire-pulse answers), what's playing (mpris), the network (networkmanager) and bluetooth (bluez) over d-bus. its notifications: it's the session's notification daemon (org.freedesktop.Notifications) over sd-bus too. the brightness keys and the display slider: a laptop's backlight set through logind (SetBrightness, no root) |
| libcurl | `libcurl` | flamebar's control center: a player's art on the web (an https cover, at its full size) fetched once into `~/.cache/flame/art` |
| librsvg, gio | `librsvg-2.0` `gio-2.0` | flame's apps (flameui): icons from the desktop's icon theme (gsettings' theme, else gtk's settings.ini, then hicolor), drawn sharp at their size when they're svg. an app's icon on its notifications |
| gio-unix | `gio-unix-2.0` | flamesearch: the apps there are (their .desktop files, as they're installed or removed), opened the way they ask (a terminal for a terminal app), a file in the app that opens its kind |
| pam | `-lpam` (`pam`) | flamelock: the password checked the way a console login checks it (`/etc/pam.d/flamelock`, which `make install` puts there: login's rules). systemd's logind (sd-bus): flame locks before the computer sleeps |
| sd-bus (systemd) | `libsystemd` | flamegreet: sleep, restart and shut down asked of logind from the login screen |
| uchardet | `uchardet` | flameplay: what encoding a subtitle file that isn't utf-8 is in (old .srt files: windows' arabic, cyrillic, western...), read as that |
| libass | `libass` | flameplay's subtitles: every text kind (srt, ass, webvtt, a video's own) drawn by it, a styled one the way it was made, a plain one in flame's look. fonts through fontconfig and the ones a file brings |
| libdrm | `libdrm` | the hardware cursor: dumb buffers the cpu writes the pointer into, on their own fd (empty lease or reopened + authenticated), handed to the cursor plane as linear ARGB8888. nvidia can't render to linear, so wlroots' gpu cursor never made it there |

`flamectl` and the ipc socket are plain libc (unix sockets), nothing extra.

wlroots also pulls in libinput, libseat (seatd), libliftoff,
libdisplay-info, lcms2, vulkan loader and some xcb utils. pacman handles those.

## runtime

not linked, flame works without them, these features don't.

| dep | why |
| --- | --- |
| xdg-desktop-portal, pipewire, wireplumber | screen sharing and screenshots (obs, discord, browsers): xdg-desktop-portal asks flame's own portal, flame's picker asks you, flame streams it into pipewire. apps' open and save dialogs the same way, flamefiles as the dialog. `make install` puts `data/flamewm.portal` and `data/flamewm-portals.conf` where xdg-desktop-portal looks (`/usr/share/xdg-desktop-portal`) |
| greetd | the login screen: flamegreet is a greetd greeter (`flamewm -g flamegreet` as its `default_session`). greetd does the logging in (pam) and starts the session, flamegreet just asks |
| xdg-desktop-portal-gtk | the rest of the portal: what flame doesn't serve itself (settings, app chooser and such) |
| Inter (`inter-font`) | flamesettings' font, falls back to the system sans without it |
| networkmanager, bluez, an mpris player | what flamebar's control center shows: the network and wi-fi, bluetooth, what's playing. without one its tile says so. neither is started by the bar |
| upower | a laptop's battery in flamebar (its "display device": the system's batteries, never a mouse's). without it the bar reads the kernel's own files. a desktop shows none either way |
| `i2c-dev` loaded, access to the monitors' i2c (`ddcutil`'s udev rule, or the `i2c` group) | flamebar's brightness for monitors on a cable: asked over the cable (ddc/ci), on the graphics card's lines only. `echo i2c-dev \| sudo tee /etc/modules-load.d/i2c-dev.conf`. `FLAMEBAR_NO_DDC=1` leaves monitors alone |
| `noto-fonts-emoji` | emoji (in notifications, file names, subtitles) in colour. without it they're boxes |
| `noto-fonts` | text Inter has no letters for: arabic, hebrew, thai... in flameplay's subtitles and flame's apps, found through fontconfig. without it they come out in whatever font has them, or as boxes |
| a vulkan driver that decodes video (nvidia's, mesa's radv or anv) | flameplay decodes on the gpu (h264, h265, vp9, av1) and hands flame its frames as they are. without one, or for what it can't decode, it decodes on the cpu. `FLAMEPLAY_HWDEC=no` makes it always the cpu |
| (vulkan-validation-layers, debugging only) | `VK_LOADER_LAYERS_ENABLE='*validation'` checks flameplay's gpu pass |
| (xdg-desktop-portal-wlr, if you'd rather) | flame's ext-image-copy-capture still serves it, `flamectl share-pick --dmenu` as its dmenu chooser gets you flame's picker |

## tests

`make test` and `make e2e` only, flamewm never links these.

| dep | pkg-config | why |
| --- | --- | --- |
| ffmpeg (the command) | - | makes the videos and songs flamefiles' preview test reads |
| libxcb and its extension libs (come with `libxcb`) | `xcb` `xcb-xinput` `xcb-randr` `xcb-xkb` `xcb-xfixes` `xcb-xinerama` `xcb-screensaver` `xcb-dpms` `xcb-xtest` `xcb-shape` `xcb-render` | the X server tests: plain xcb clients against flame's X server, run by `tests/xserver/harness.c`, and `make e2e`'s X cursors |
| libxkbcommon-x11 (comes with `libxkbcommon`) | `xkbcommon-x11` | the XKB test reads the keymap back the way kitty and alacritty do |
| wayland (client side) | `wayland-client` | `make e2e`: the wayland clients (drag and drop, window sharing, a fullscreen game asking to tear) |
| gbm (mesa or the nvidia driver), libdrm | `gbm` `libdrm` | `make e2e`: window sharing into gpu buffers, the way the portal takes frames |
| wl-clipboard | - | `make e2e`: wl-copy and wl-paste, the wayland end of the clipboard tests |
| python-gobject | - | `make e2e`: the battery test's fake upower (tests/e2e/fakepower.py), an app asking for file dialogs (tests/e2e/askfile.py), a music player (tests/e2e/fakeplayer.py) |
| libGL, libX11 (libglvnd) | `gl`, `x11` | `make e2e`: X GL apps, multisampled, swapping frames, a fullscreen one scanned out |
| gtk3 | `gtk+-3.0` | `make e2e`: client side decorations, on X and wayland: shadows, headerbar drags |
| foot, grim | - | `make e2e`: a terminal that has to keep drawing across a vt switch, and screenshots to tell |
| slurp | - | `make e2e`: a part of the screen picked for a screenshot, the shot taken right after has nothing of slurp's in it |
| dbus (`dbus-run-session`), libsystemd, libpipewire, libEGL, GLESv2 | `libsystemd` `libpipewire-0.3` `egl` `glesv2` | `make e2e`: flame's portal on a bus of its own, called like xdg-desktop-portal calls it, its streams taken from pipewire like apps take them (gpu buffers imported with EGL). uses the running pipewire |

## data

| what | where | why |
| --- | --- | --- |
| misc-fixed 6x13 (xorg's font/misc-misc, public domain: "Public domain font. Share and enjoy.") | `src/xserver/fixed.c`, made by `tools/fonts/bdf2c.py 6x13.bdf` | flame's X server's built-in "fixed", the font core text draws with, like Xorg's. iso8859-1, 256 cells baked in, nothing read at runtime |

## runtime

| dep | for |
| --- | --- |
| foot | default terminal on `super+q`, bind another in `~/.config/flame/init.lua` (`examples/init.lua`) |
| seatd or logind | only when running on a real tty, nested doesn't need it |

