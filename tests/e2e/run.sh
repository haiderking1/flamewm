#!/bin/sh
# end to end: a headless flame per case, real clients, fake pointer input
# (flamectl pointer, FLAME_TEST_INPUT=1). the clipboard and drag and drop,
# every way between X and wayland apps. make e2e
# usage: run.sh FLAMEWM FLAMECTL CLIENTS_DIR
FLAME=$(realpath "$1")
CTL=$(realpath "$2")
BIN=$(realpath "$3")
WORK=$(mktemp -d /tmp/flame-e2e.XXXXXX)
failed=0

# one case: the script runs inside flame, its output gets checked for every
# string after the name
case_run() {
	name=$1
	script=$2
	shift 2
	cat > "$WORK/case.sh" <<EOF
#!/bin/sh
exec > "$WORK/out" 2>&1
sleep 0.5
cd "$WORK"
$script
$CTL quit > /dev/null 2>&1
EOF
	chmod +x "$WORK/case.sh"
	: > "$WORK/out"
	timeout 30 env WLR_BACKENDS=headless FLAME_TEST_INPUT=1 "$FLAME" $CASE_ARGS \
		-s "$WORK/case.sh" > "$WORK/flame.log" 2>&1
	ok=1
	for want in "$@"; do
		grep -qF -- "$want" "$WORK/out" || ok=0
	done
	if grep -qa "ERROR: AddressSanitizer\|runtime error" "$WORK/flame.log"; then
		ok=0
	fi
	if [ $ok = 1 ]; then
		echo "ok   $name"
	else
		echo "FAIL $name"
		sed 's/^/    /' "$WORK/out"
		grep -a -A8 "ERROR: AddressSanitizer\|runtime error" "$WORK/flame.log" | sed 's/^/    /'
		failed=1
	fi
}

# a drag from the left window's middle to the right one's, release there
DRAG="
sleep 0.8
$CTL pointer move 320 360 >/dev/null
$CTL pointer button left press >/dev/null
for x in 400 500 600 700 800 900 960; do $CTL pointer move \$x 360 >/dev/null; sleep 0.1; done
sleep 0.3
$CTL pointer move 965 365 >/dev/null
sleep 0.2
$CTL pointer button left release >/dev/null
sleep 1.5
cat src.out dst.out"

# both windows open, left first
two() {
	echo "$1 > src.out 2>&1 &
sleep 0.8
$2 > dst.out 2>&1 &
$DRAG"
}

head -c 300000 /dev/urandom | base64 -w0 > "$WORK/big.txt"
printf 'from X ✓' > "$WORK/small.txt"

case_run "clipboard: X copies, wayland pastes" "
$BIN/xclip copy small.txt 3 & sleep 0.5
wl-paste -n; echo
wait" "from X ✓"
case_run "clipboard: X copies big (INCR), wayland pastes" "
$BIN/xclip copy big.txt 4 & sleep 0.5
wl-paste -n > got.txt
cmp got.txt big.txt && echo same
wait" "same"
case_run "clipboard: wayland copies, X pastes (UTF8_STRING, and STRING in Latin-1)" "
printf 'from wayland ✓ café' | wl-copy
sleep 0.3
$BIN/xclip paste
echo
$BIN/xclip paste STRING | iconv -f latin1 -t utf-8" "from wayland ✓ café" "from wayland ? café"
case_run "clipboard: wayland copies big, X pastes" "
wl-copy < big.txt
sleep 0.3
$BIN/xclip paste > got.txt
cmp got.txt big.txt && echo same" "same"
case_run "clipboard: wayland takes it back from X" "
$BIN/xclip copy small.txt 3 & sleep 0.5
printf 'wayland again' | wl-copy
sleep 0.3
wait
$BIN/xclip paste" "lost the clipboard" "wayland again"

case_run "dnd: wayland to wayland" "$(two "$BIN/wldnd source 'wl to wl' 10" "$BIN/wldnd target '' 10")" \
	"dropped: wl to wl" "source: finished"
case_run "dnd: X to X" "$(two "$BIN/xdnd source 'x to x' 10" "$BIN/xdnd target 10")" \
	"dropped: x to x" "finished, accepted"
case_run "dnd: X to wayland" "$(two "$BIN/xdnd source 'x to wl' 10" "$BIN/wldnd target '' 10")" \
	"dropped: x to wl" "finished, accepted"
case_run "dnd: wayland to X" "$(two "$BIN/wldnd source 'wl to x' 10" "$BIN/xdnd target 10")" \
	"dropped: wl to x" "source: finished"
case_run "dnd: X to nothing" "$(two "$BIN/xdnd source 'x' 6" "sleep 1")" "source: cancelled"
case_run "dnd: wayland to nothing" "$(two "$BIN/wldnd source 'wl' 6" "sleep 1")" "source: cancelled"
case_run "dnd: wayland to an X window without XDND" \
	"$(two "$BIN/wldnd source 'wl' 6" "$BIN/xdnd source 'other' 6")" "source: cancelled"
case_run "dnd: X to a wayland window that won't take it" \
	"$(two "$BIN/xdnd source 'x' 6" "$BIN/wldnd source 'not a target' 6")" "source: cancelled"

case_run "X windows on a hidden workspace are out of X's way, and stay where they are" "
$BIN/xdnd target 8 > /dev/null 2>&1 &
sleep 0.8
echo before: \$($BIN/xwins | grep target)
$CTL workspace 2 > /dev/null
sleep 0.3
echo hidden: \$($BIN/xwins | grep -c target)
echo where: \$($BIN/xwins all | grep target)
$CTL workspace 1 > /dev/null
sleep 0.3
echo back: \$($BIN/xwins | grep target)" "hidden: 0" "where: target 8,8" "unviewable" "back: target 8,8"

case_run "nothing's under the pointer on an empty workspace, not a hidden X window" "
$BIN/xdnd target 8 > /dev/null 2>&1 &
sleep 0.8
$CTL pointer move 640 360 > /dev/null
$BIN/xwins under | sed 's/^/shown, /'
$CTL workspace 2 > /dev/null
sleep 0.3
$BIN/xwins under | sed 's/^/hidden, /'" "shown, under: target" "hidden, under: nothing"

case_run "focus and workspaces with X menus open: flame keeps up, menus stay on top" "
$BIN/xdnd target 12 > /dev/null 2>&1 &
sleep 0.5
$BIN/xdnd target 12 > /dev/null 2>&1 &
sleep 0.5
$BIN/xdnd popup 12 > /dev/null 2>&1 &
$BIN/xdnd popup 12 > /dev/null 2>&1 &
sleep 0.8
for i in 1 2 3 4 5; do
	$CTL focus next > /dev/null; $CTL workspace 2 > /dev/null; $CTL workspace 1 > /dev/null
done
timeout 2 $CTL state > /dev/null && echo responsive
$BIN/xwins | tail -2 | grep -c popup | sed 's/^/popups on top: /'" "responsive" "popups on top: 2"

# x and y of an X window as xwins says, "X Y W H"
XGEO="sed -n 's/.* \\(-*[0-9]*\\),\\(-*[0-9]*\\) \\([0-9]*\\)x\\([0-9]*\\)/\\1 \\2 \\3 \\4/p'"

case_run "floating: an X dialog floats, centered at its own size" "
$BIN/xdnd dialog 8 > /dev/null 2>&1 &
sleep 0.8
$BIN/xwins | grep dialog
$CTL state | grep -o '\"floating\":[a-z]*'" "dialog 440,210 400x300" '"floating":true'

case_run "floating: a fixed size wayland window floats" "
$BIN/wldnd fixed '' 8 > /dev/null 2>&1 &
sleep 0.8
$CTL state | grep -o '\"floating\":[a-z]*,\"x\":[0-9]*,\"y\":[0-9]*,\"width\":[0-9]*,\"height\":[0-9]*'" \
	'"floating":true,"x":478,"y":238,"width":324,"height":244'

case_run "vt switch dropping the screen: windows from before keep drawing and typing" "
foot sh -c 'timeout 3 yes; clear; cat > typed' > /dev/null 2>&1 &
sleep 4
$CTL test unplug > /dev/null
sleep 0.5
$CTL test plug > /dev/null
sleep 1
grim before.png
for k in h i Return; do $CTL key \$k press > /dev/null; $CTL key \$k release > /dev/null; done
sleep 1
grim after.png
cat typed
cmp -s before.png after.png || echo redrew" "hi" "redrew"

case_run "EWMH: flame is X's window manager, and composites" "
$BIN/xewmh check" "wm flame, check points at itself: yes" "compositor: yes" \
	"supports fullscreen: yes"
case_run "EWMH: an X window goes fullscreen and back, asked to" "
$BIN/xewmh fullscreen" "state: FOCUSED MAXIMIZED_VERT MAXIMIZED_HORZ" \
	"state: FULLSCREEN FOCUSED" "geometry 0,0 1280x720"
case_run "EWMH: an X window that starts fullscreen does" "
$BIN/xewmh startfull" "state: FULLSCREEN FOCUSED" "geometry 0,0 1280x720"
case_run "a tile asking to move: kept where it is, told the ICCCM way" "
$BIN/xewmh configure" "kept: yes" "told: yes"
case_run "an X app moving the pointer moves flame's, only when it's theirs to move" "
$BIN/xewmh warp" "pointer in window 50,60" "motion 50,60" "after another app tried: 50,60"
case_run "an X grab confining the pointer to a window keeps flame's cursor in it" "
$BIN/xewmh confine $CTL" "grabbed: yes" "moved inside: yes" "mouse pushed out: 100,100" \
	"let go: 2,3"
case_run "X cursors: a window's shows, hides, theme shapes by name, a grab's wins" "
$BIN/xcursor $CTL" "window's: 16x16 name []" "shown: red" "hidden: white" "again: red" \
	"font's: 1x1 name [text]" "grab's: 1x1 name [crosshair]" "after the grab: 1x1 name [text]" \
	"argb: 24x20 name []" "argb shot: green"
case_run "keys typed in a wayland window: X apps see them like on Xorg, push to talk, hotkeys" "
foot sh -c 'cat > typed' > /dev/null 2>&1 &
sleep 1.5
$BIN/xkeys $CTL
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.3
echo \"foot got: \$(head -c1 typed), F9 \$(grep -o '\\[20~' typed | wc -l) time\"" \
	"hotkey grabbed: yes" "taken twice: no" "held while typed in wayland: yes" "up after: yes" \
	"raw events: 2" "hotkey presses: 1" "hotkey presses after ungrab: 0" "foot got: a, F9 1 time"
case_run "a fullscreen X game shows its own buffers (direct scanout) and never stalls" "
$BIN/glxfull 3 > full.out 2>&1 &
sleep 0.8
$CTL pointer move 300 300 > /dev/null
sleep 0.7
$CTL state | grep -o '\"scanout\":[a-z]*'
grim -t ppm full.ppm
wait
cat full.out
python3 -I -c \"
d = open('full.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):][(h // 2 * w + w // 2) * 3:][:3]
print('center', *px)
\"" '"scanout":true' "kept going: yes" "center 0 255 0"
case_run "tearing: an X game with vsync off tears, scanned out, and runs unthrottled" "
$BIN/glxfull 2 vsyncoff > full.out 2>&1 &
sleep 0.6
$CTL pointer move 300 300 > /dev/null
sleep 0.8
$CTL state | grep -o '\"tearing\":[a-z]*'
wait
cat full.out" '"tearing":true' "kept going: yes"
case_run "tearing: a wayland game asking for async tears, one with vsync doesn't" "
$BIN/wlgame 1.5 vsync > /dev/null 2>&1 &
sleep 1
echo \"vsync \$($CTL state | grep -o '\"tearing\":[a-z]*')\"
wait
$BIN/wlgame 1.5 vsyncoff > /dev/null 2>&1 &
sleep 1
echo \"vsyncoff \$($CTL state | grep -o '\"tearing\":[a-z]*')\"
wait" 'vsync "tearing":false' 'vsyncoff "tearing":true'
printf 'flame.set { allow_tearing = false, vrr = "always" }\n' > "$WORK/sync.lua"
CASE_ARGS="-c $WORK/sync.lua"
case_run "allow_tearing = false wins over a game asking, vrr on a screen without it stays off" "
$BIN/glxfull 2 vsyncoff > full.out 2>&1 &
sleep 0.6
$CTL pointer move 300 300 > /dev/null
sleep 0.8
$CTL state | grep -o '\"tearing\":[a-z]*,\"vrr\":[a-z]*,\"vrr_capable\":[a-z]*'
wait
cat full.out" '"tearing":false,"vrr":false,"vrr_capable":false' "kept going: yes"
CASE_ARGS=
printf 'flame.set { pace = "always", pace_fps = 30 }\n' > "$WORK/pace.lua"
CASE_ARGS="-c $WORK/pace.lua"
case_run "pacing: an uneven game's frames come out even, at the pace asked" "
$BIN/glxfull 4 vsyncoff jitter > full.out 2>&1 &
sleep 2
$CTL state | grep -oE '\"pace_fps\":[0-9.]+' | awk -F: '{ print \"paced:\", (\$2 > 25 && \$2 < 31) ? \"yes\" : \$2 }'
wait
cat full.out" "paced: yes" "even: yes" "kept going: yes"
CASE_ARGS=
case_run "an app busy for a while while flame has lots to tell it: still connected after" "
$BIN/wlstall 20000" "mapped" "after the stall: still connected"
case_run "share picker: picked on the windows themselves, the bar switches to the screen or cancels" "
foot -T left > /dev/null 2>&1 &
sleep 1
foot -T right > /dev/null 2>&1 &
sleep 1.2
$CTL pointer move 960 400 > /dev/null
grim -t ppm before.ppm
$CTL share-pick > pick.out 2> pick.err &
sleep 0.6
grim -t ppm open.ppm
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.3
echo \"enter [\$(cat pick.out)]\"
$CTL share-pick > pick.out 2> pick.err &
sleep 0.5
$CTL key Right press > /dev/null; $CTL key Right release > /dev/null
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.3
echo \"screen [\$(cat pick.out)]\"
$CTL share-pick > pick.out 2> pick.err &
sleep 0.5
$CTL key Escape press > /dev/null; $CTL key Escape release > /dev/null
sleep 0.3
echo \"escape [\$(cat pick.out)] \$(cat pick.err)\"
$CTL share-pick > pick.out 2> pick.err &
sleep 0.5
$CTL pointer move 300 400 > /dev/null
$CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null
sleep 0.3
echo \"click [\$(cat pick.out)]\"
$CTL share-pick > pick.out 2> pick.err &
sleep 0.5
$CTL key Super_L press > /dev/null; $CTL key 2 press > /dev/null
$CTL key 2 release > /dev/null; $CTL key Super_L release > /dev/null
sleep 0.4
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.3
echo \"empty workspace, enter: [\$(cat pick.out)]\"
$CTL pointer move 740 36 > /dev/null
$CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null
sleep 0.3
echo \"cancel [\$(cat pick.out)] \$(cat pick.err)\"
python3 -I -c \"
def px(p):
	d = open(p, 'rb').read()
	return d[-(1280 * 720 * 3):]
def avg(px, x0, x1):
	s = [px[(y * 1280 + x) * 3] for y in range(100, 700, 7) for x in range(x0, x1, 7)]
	return sum(s) / len(s)
b, o = px('before.ppm'), px('open.ppm')
print('left dimmed' if avg(o, 20, 600) < avg(b, 20, 600) * 0.75 else 'left not dimmed')
print('right lit' if avg(o, 680, 1260) > avg(b, 680, 1260) * 0.85 else 'right dimmed')
\"" "enter [Window: " "screen [Monitor: HEADLESS-1]" \
	"escape [] {\"ok\":false,\"error\":\"cancelled\"}" "click [Window: " \
	"empty workspace, enter: []" "cancel [] {\"ok\":false,\"error\":\"cancelled\"}" \
	"left dimmed" "right lit"
case_run "sharing a wayland window on a hidden workspace: drawn, and frames keep coming" "
foot --title capme sh -c 'echo hello; sleep 30' > /dev/null 2>&1 &
sleep 1.5
$BIN/wlcapture $CTL capme" "listed: yes" "drawn pixels: 889856 of 889856" \
	"unchanged window, next frame: yes"
case_run "sharing a window into gpu buffers, the portal's way: each copy done before it's ready" "
$BIN/wlcapture $CTL gpu" "gpu buffer: yes" "gpu frames ready: 3"
case_run "sharing one window: listed, captured on its own, even on a hidden workspace" "
$BIN/wlcapture $CTL" "protocols: yes" "listed: yes" "hidden window captured: red" \
	"unchanged window, next frame: yes" "closed while shared, session stopped: yes"
case_run "an X grab breaks when the user leaves the app: the mouse is theirs again" "
$BIN/xewmh grabaway $CTL" "grabbed: yes" "away, the mouse goes: 2,3" "another app can grab: yes"
case_run "an X window on another workspace isn't minimized (steam came back white)" "
$BIN/xewmh away $CTL" "away, state: MAXIMIZED_VERT MAXIMIZED_HORZ"
case_run "an X app can't take the focus from another app the user is in (a game from a tool)" "
$BIN/xewmh steal" "the user is in this app: yes" "the other app asked, still this one: yes" \
	"the other app was told who's active: yes" "its own window back: yes"
case_run "a wayland window in front: X apps hear another app is active, not None (wine)" "
$BIN/xewmh wayland '$BIN/wlgame 3 > /dev/null 2>&1 &'" "active is this one: yes" \
	"a wayland window in front, active is: flame's window"
case_run "a minimize request (wine's on losing focus): refused out loud, WM_STATE stays Normal" "
$BIN/xewmh iconify" "iconify: told yes, WM_STATE 1" "still shown: yes"
case_run "a fullscreen game dropping fullscreen as the user leaves stays fullscreen (wine)" "
$BIN/xewmh keepfull $CTL" "away, asked out of it: state: FULLSCREEN" "back: geometry 0,0 1280x720" \
	"in it, asked out: state: FOCUSED MAXIMIZED_VERT MAXIMIZED_HORZ"
case_run "WM_TAKE_FOCUS windows (wine's): WM_STATE first, a click offers focus again" "
$BIN/xewmh takefocus $CTL" "WM_STATE before the offer: yes" \
	"offers on a click, not taken: 1" "offers on a click, taken: 0"
case_run "ICCCM WM_STATE: Normal while managed, Withdrawn once unmapped" "
$BIN/xewmh wmstate" "mapped: 1" "unmapped: 0, type is WM_STATE: yes"

case_run "a pointer between pixels: flame and X agree which window it's in" "
$BIN/xewmh subpixel > sub.out 2>&1 &
sleep 1
$CTL pointer move 80.3 199.6 > /dev/null
sleep 0.5
$CTL pointer move 80.3 200.4 > /dev/null
wait
cat sub.out" "ready" "window row 199" "popup row 200"

case_run "EWMH: activating and closing the EWMH way" "
$BIN/xewmh activate
$BIN/xewmh close" "active is the new one: yes" "active is the first one: yes" "clients: 2" \
	"asked to close: yes"

case_run "GTK client side decorations on X: shadows outside, click through, headerbar drags" "
GDK_BACKEND=x11 $BIN/gtkcsd tiled > /dev/null 2>&1 &
sleep 1.5
GDK_BACKEND=x11 $BIN/gtkcsd dialog dialog > /dev/null 2>&1 &
sleep 1.5
$BIN/xwins
$CTL state | grep -o '\"floating\":true,\"x\":[0-9]*,\"y\":[0-9]*,\"width\":[0-9]*,\"height\":[0-9]*'
set -- \$($CTL state | grep -o '\"floating\":true,\"x\":[0-9]*,\"y\":[0-9]*' | grep -o '[0-9]*')
fx=\$1; fy=\$2
# deep in the shadow, left of the dialog: the tile under it takes the click
$CTL pointer move \$((fx - 22)) \$((fy + 120)) > /dev/null
$CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null
sleep 0.3
$CTL state | grep -o '\"title\":\"[a-z]*\"' | head -1
# the headerbar dragged: flame moves it
$CTL pointer move \$((fx + 150)) \$((fy + 20)) > /dev/null
$CTL pointer button left press > /dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do $CTL pointer move \$((fx + 150 + i * 6)) \$((fy + 20 + i * 4)) > /dev/null; sleep 0.03; done
$CTL pointer button left release > /dev/null
sleep 0.3
set -- \$($CTL state | grep -o '\"floating\":true,\"x\":[0-9]*,\"y\":[0-9]*' | grep -o '[0-9]*')
[ \$1 -gt \$((fx + 20)) ] && [ \$2 -gt \$((fy + 10)) ] && echo headerbar moved it" \
	"tiled 8,8 1264x704" '"floating":true' '"title":"tiled"' "headerbar moved it"

case_run "GTK client side decorations on wayland: the headerbar drags the window" "
GDK_BACKEND=wayland $BIN/gtkcsd moving > /dev/null 2>&1 &
sleep 1.5
$CTL togglefloating > /dev/null
sleep 0.5
set -- \$($CTL state | grep -o '\"floating\":true,\"x\":[0-9]*,\"y\":[0-9]*' | grep -o '[0-9]*')
fx=\$1; fy=\$2
$CTL pointer move \$((fx + 150)) \$((fy + 20)) > /dev/null
$CTL pointer button left press > /dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do $CTL pointer move \$((fx + 150 + i * 6)) \$((fy + 20 + i * 4)) > /dev/null; sleep 0.03; done
$CTL pointer button left release > /dev/null
sleep 0.3
set -- \$($CTL state | grep -o '\"floating\":true,\"x\":[0-9]*,\"y\":[0-9]*' | grep -o '[0-9]*')
[ \$1 -gt \$((fx + 20)) ] && [ \$2 -gt \$((fy + 10)) ] && echo headerbar moved it" \
	"headerbar moved it"

printf 'flame.output("*", { scale = 2 })\nflame.set { x_zero_scaling = true }\n' > "$WORK/zero.lua"
CASE_ARGS="-c $WORK/zero.lua"
case_run "X apps at scale 2, drawing at its pixels: dpi told, input lands, headerbars drag" "
$BIN/xewmh dpi
GDK_BACKEND=x11 $BIN/gtkcsd dialog dialog > /dev/null 2>&1 &
sleep 1.5
set -- \$($CTL state | grep -o '\"floating\":true,\"x\":[0-9]*,\"y\":[0-9]*' | grep -o '[0-9]*')
fx=\$1; fy=\$2
$CTL pointer move \$((fx + 100)) \$((fy + 12)) > /dev/null
$CTL pointer button left press > /dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do $CTL pointer move \$((fx + 100 + i * 4)) \$((fy + 12 + i * 3)) > /dev/null; sleep 0.03; done
$CTL pointer button left release > /dev/null
sleep 0.3
set -- \$($CTL state | grep -o '\"floating\":true,\"x\":[0-9]*,\"y\":[0-9]*' | grep -o '[0-9]*')
[ \$1 -gt \$((fx + 15)) ] && [ \$2 -gt \$((fy + 10)) ] && echo headerbar moved it" \
	"Xft.dpi:	192" "xsettings: yes" "screen 1280x720" "headerbar moved it"
CASE_ARGS=

case_run "GL on X: a multisampled srgb config, made current, frames swapped and seen" "
$BIN/glx > glx.out 2>&1 &
sleep 0.9
grim -t ppm shot.ppm
wait
cat glx.out
python3 -I -c \"
d = open('shot.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):][(h // 2 * w + w // 2) * 3:][:3]
print('screen center', *px)
\"" "samples 4" "pixel 255 0 0" "screen center 255 0 0"

case_run "GL in child windows: each where X has it, stacked and cut like X" "
$BIN/glxchild > child.out 2>&1 &
sleep 1.2
grim -t ppm shot.ppm
cat child.out
set -- \$($BIN/xwins | grep glxchild | $XGEO)
python3 -I -c \"
d = open('shot.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
pixels = d[-(w * h * 3):]
x0, y0 = \$1, \$2
for name, x, y in (('top', 20, 20), ('green', 60, 50), ('blue', 150, 110),
		('yellow', 375, 105), ('cut', 420, 150)):
	at = ((y0 + y) * w + x0 + x) * 3
	print(name, *pixels[at:at + 3])
\"
wait" "drawn" "top 255 0 0" "green 0 255 0" "blue 0 0 255" "yellow 255 255 0" "cut 255 0 0"

case_run "GL child unmapped and mapped again: its old picture doesn't come back" "
$BIN/glxchild remap > child.out 2>&1 &
sleep 1.6
grim -t ppm shot.ppm
cat child.out
set -- \$($BIN/xwins | grep glxchild | $XGEO)
python3 -I -c \"
d = open('shot.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
pixels = d[-(w * h * 3):]
at = ((\$2 + 50) * w + \$1 + 60) * 3
print('green gone' if pixels[at:at + 3] != bytes([0, 255, 0]) else 'green still there')
\"
wait" "drawn" "green gone"

case_run "floating: toggle, super+drag moves it, super+right drag resizes it, toggle back" "
$BIN/xdnd target 12 > /dev/null 2>&1 &
sleep 0.8
$CTL togglefloating > /dev/null
sleep 0.3
set -- \$($BIN/xwins | grep target | $XGEO)
x=\$1; y=\$2; w=\$3; h=\$4
$CTL state | grep -o '\"floating\":[a-z]*'
$CTL key super press > /dev/null
$CTL pointer move \$((x + 20)) \$((y + 20)) > /dev/null
$CTL pointer button left press > /dev/null
$CTL pointer move \$((x + 80)) \$((y + 60)) > /dev/null
$CTL pointer button left release > /dev/null
sleep 0.3
set -- \$($BIN/xwins | grep target | $XGEO)
echo moved by \$((\$1 - x)),\$((\$2 - y)) size \$((\$3 - w)),\$((\$4 - h))
x=\$1; y=\$2; w=\$3; h=\$4
$CTL pointer move \$((x + w - 10)) \$((y + h - 10)) > /dev/null
$CTL pointer button right press > /dev/null
$CTL pointer move \$((x + w + 40)) \$((y + h + 20)) > /dev/null
sleep 0.1
$CTL pointer button right release > /dev/null
$CTL key super release > /dev/null
sleep 0.5
set -- \$($BIN/xwins | grep target | $XGEO)
echo resized by \$((\$3 - w)),\$((\$4 - h)) moved \$((\$1 - x)),\$((\$2 - y))
$CTL togglefloating > /dev/null
sleep 0.3
$CTL state | grep -o '\"floating\":[a-z]*,\"x\":[0-9]*,\"y\":[0-9]*,\"width\":[0-9]*'" \
	'"floating":true' "moved by 60,40 size 0,0" "resized by 50,30 moved 0,0" \
	'"floating":false,"x":6,"y":6,"width":1268'

cat > "$WORK/init.lua" <<LUA
flame.set { gap = 20, border_width = 4, border_focused_color = "#00ff00" }
flame.bind("super", "t", function() flame.workspace(3) end)
flame.rule({ title = "^target\$" }, { floating = true })
flame.autostart("echo autostarted > $WORK/auto.txt")
LUA
CASE_ARGS="-c $WORK/init.lua"
case_run "config: init.lua sets things, binds keys, floats by rule, autostarts, reloads" "
$BIN/xdnd source x 10 > /dev/null 2>&1 &
sleep 0.5
$BIN/xdnd target 10 > /dev/null 2>&1 &
sleep 0.8
cat auto.txt
$CTL state | grep -o '\"floating\":[a-z]*'
$CTL focus next > /dev/null
$CTL state | grep -o '\"floating\":false,\"x\":[0-9]*,\"y\":[0-9]*'
$CTL key super press > /dev/null; $CTL key t press > /dev/null
$CTL key t release > /dev/null; $CTL key super release > /dev/null
$CTL state | grep -o '\"workspace\":[0-9]*' | head -1
$CTL workspace 1 > /dev/null
sed -i 's/gap = 20/gap = 50/' init.lua
$CTL reload > /dev/null
sleep 0.3
$CTL state | grep -o '\"floating\":false,\"x\":[0-9]*'" \
	autostarted '"floating":true' '"floating":false,"x":20,"y":20' '"workspace":3' \
	'"floating":false,"x":50'

printf 'flame.set { gap = 9 }\nthis is not lua\n' > "$WORK/init.lua"
case_run "config: a broken init.lua still gets a working flame" "
$BIN/xdnd target 6 > /dev/null 2>&1 &
sleep 0.8
$CTL state | grep -o '\"floating\":false,\"x\":[0-9]*'" '"floating":false,"x":6'
CASE_ARGS=

# a flame inside this one, its screen 1266 wide: a row of a 24 bit capture
# isn't whole words there, grim used to fail on it
printf 'flame.set { gap = 5 }\n' > "$WORK/gap.lua"
cat > "$WORK/inner.sh" <<EOF
#!/bin/sh
grim "$WORK/inner.png" > "$WORK/grim.out" 2>&1 && echo captured >> "$WORK/grim.out"
$CTL quit > /dev/null 2>&1
EOF
chmod +x "$WORK/inner.sh"
CASE_ARGS="-c $WORK/gap.lua"
case_run "screen capture (grim) of a screen whatever its width" "
WLR_BACKENDS=wayland timeout 20 $FLAME -s $WORK/inner.sh > inner.log 2>&1
cat grim.out" "captured"
CASE_ARGS=

rm -rf "$WORK"
exit $failed
