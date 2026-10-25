# install

## arch

flame with all its apps:

```sh
curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/install.sh | sh
```

just the window manager, barebones:

```sh
curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/install-core.sh | sh
```

## after installing

`super+q` opens foot, the default terminal (`sudo pacman -S foot` if you
don't have it). want another one: `flame.bind("super", "q", "kitty")` in
your flame.lua.

to update flame to the newest version later, run this in your terminal:

```sh
flame-update
```

it updates what you installed (all of flame, or just the window manager)
and does nothing when there's nothing new. `flame-update --all` / `--core`
switch between the two. under the hood both installs build real packages
(`flamewm-git`, `flame-apps-git`, from [this PKGBUILD](../dist/aur/flamewm-git/PKGBUILD))
and install them with pacman. the aur package is coming once the aur takes
new accounts again.

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
`-c /etc/greetd/flame.lua` with `flame.wallpaper { path = ... }` in it. it
remembers who logged in last and into what.

## barebones

the barebones install has none of flame's apps, so two things work a bit
differently. tell flame what locks the screen (`super+l`, and before the
computer sleeps), or `""` for nothing:

```lua
flame.lockscreen { command = "swaylock" }
```

and apps' open and save dialogs are gtk's instead of flamefiles.
