# install

## arch

the packages aren't on the aur yet, but the PKGBUILD is right here and
makes the same two packages:

```sh
git clone https://github.com/haiderking1/flamewm
cd flamewm/dist/aur/flamewm-git
makepkg -si                         # builds both, then asks what to install
```

`flamewm-git` is the compositor alone: bring your own bar, launcher and
locker. `flame-apps-git` adds the bar, files, settings, the player,
search, the lock screen and the login screen. to install just one of them
after building: `sudo pacman -U flamewm-git-*.pkg.tar.zst`.

## from source

deps are in [deps.md](../deps.md).

```sh
make               # build/flamewm
make run           # nested, inside whatever you're running now
make release       # build-release/flamewm
make test          # unit tests
make e2e           # end to end tests on headless flames
sudo make install       # flame and all its apps
sudo make install-core  # just flame
sudo make install-apps  # the apps, over an installed flame
```

`flamewm -c config -s command -d`: a config file, a command to run once
it's up, debug logs. started from a tty or greetd, the logs go to
`$XDG_RUNTIME_DIR/flamewm.log` (`flamewm-greeter.log` for the login screen).

## the login screen

flamegreet runs on [greetd](https://git.sr.ht/~kennylevinsen/greetd), with
flame itself showing it. after `sudo make install`, put this in
`/etc/greetd/config.toml`:

```toml
[default_session]
command = "flamewm -g flamegreet"
user = "greeter"
```

then `systemctl enable greetd` (and disable your old display manager).
`-g` runs flame as just the login screen: no binds, and it's gone the
moment someone logs in.

everyone picks their own login look in settings (wallpaper, login screen):
a wallpaper (a copy goes to `/var/lib/flamegreet`, no root needed) and blur
on or off. for the default look, give the greeter a config:
`-c /etc/greetd/flame.lua` with `flame.set{ wallpaper = ... }` in it. it
remembers who logged in last and into what.

## no apps

every app is its own program, flamewm doesn't need any of them. the bar
only runs because of one line in `~/.config/flame/init.lua`:

```lua
flame.autostart("flamebar")
```

swap it for `flame.autostart("waybar")` (or nothing) and that's it. search
only starts when you press `super+space`, the rest only when you open them.

without flame's lock screen, tell flame what locks the screen instead
(`super+l` and before the computer sleeps), or `""` for nothing:

```lua
flame.set { lock = "swaylock" }
```

without flamefiles, apps' open and save dialogs are gtk's.
