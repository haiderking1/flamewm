#!/bin/sh
# end to end: a headless flame per case, real clients, fake pointer input
# (flamectl pointer, FLAME_TEST_INPUT=1). the clipboard and drag and drop,
# every way between X and wayland apps. make e2e
# usage: run.sh FLAMEWM FLAMECTL CLIENTS_DIR
# ONLY='share picker*' runs just the cases matching that pattern
FLAME=$(realpath "$1")
CTL=$(realpath "$2")
BIN=$(realpath "$3")
WORK=$(mktemp -d /tmp/flame-e2e.XXXXXX)
failed=0

# cases run side by side, JOBS at a time (each its own headless flame, its
# own dir: a copy of $WORK's files as they were when the case was set up).
# the ones timing frames run alone, nothing else on the cpu and gpu then
JOBS=${JOBS:-6}
n=0

alone() {
	case "$1" in
	pacing:*|tearing:*|allow_tearing*|"a fullscreen X game shows its own buffers"*) return 0 ;;
	"an app busy for a while"*|"sharing a wayland window on a hidden workspace"*) return 0 ;;
	esac
	return 1
}

# till fewer than $1 cases are running
wait_below() {
	while [ "$(jobs -p | wc -l)" -ge "$1" ]; do
		sleep 0.1
	done
}

# the case in dir $1, named $2, flame's args $3, what flame runs inside $4
# (CASE_WRAP: dbus-run-session for a bus of its own). its result printed in
# one go, a block the others running don't tear up
run_case() {
	dir=$1
	timeout 30 $4 env WLR_BACKENDS=headless FLAME_TEST_INPUT=1 \
		FLAME_SETTINGS="$dir/settings.lua" $CASE_ENV "$FLAME" $3 \
		-s "$dir/case.sh" > "$dir/flame.log" 2>&1
	ok=1
	while IFS= read -r want; do
		grep -qF -- "$want" "$dir/out" || ok=0
	done < "$dir/wants"
	if grep -qa "ERROR: AddressSanitizer\|runtime error" "$dir/flame.log"; then
		ok=0
	fi
	if [ $ok = 1 ]; then
		echo "ok   $2"
		rm -rf "$dir"
		return
	fi
	{
		echo "FAIL $2"
		sed 's/^/    /' "$dir/out"
		grep -a -A8 "ERROR: AddressSanitizer\|runtime error" "$dir/flame.log" | sed 's/^/    /'
	} > "$dir/result"
	touch "$WORK/failed"
	cat "$dir/result"
}

# one case: the script runs inside flame, its output gets checked for every
# string after the name
case_run() {
	name=$1
	case "$name" in ${ONLY:-*}) ;; *) return ;; esac
	n=$((n + 1))
	dir="$WORK/case.$n"
	mkdir "$dir"
	# $WORK's files as they are now, paths into it pointing at the copies.
	# big ones just linked, nothing points into them
	for f in "$WORK"/*; do
		[ -f "$f" ] || continue
		if [ "$(wc -c < "$f")" -lt 65536 ]; then
			sed "s|$WORK/|$dir/|g" "$f" > "$dir/${f##*/}"
			chmod --reference="$f" "$dir/${f##*/}"
		else
			ln -s "$f" "$dir/${f##*/}"
		fi
	done
	script=$(printf '%s\n' "$2" | sed "s|$WORK/|$dir/|g")
	args=$(printf '%s' "$CASE_ARGS" | sed "s|$WORK/|$dir/|g")
	shift 2
	: > "$dir/wants"
	for want in "$@"; do
		printf '%s\n' "$want" >> "$dir/wants"
	done
	cat > "$dir/case.sh" <<EOF
#!/bin/sh
exec > "$dir/out" 2>&1
sleep 0.5
cd "$dir"
$script
$CTL quit > /dev/null 2>&1
EOF
	chmod +x "$dir/case.sh"
	: > "$dir/out"
	if alone "$name"; then
		wait
		run_case "$dir" "$name" "$args" "$CASE_WRAP"
	else
		wait_below "$JOBS"
		run_case "$dir" "$name" "$args" "$CASE_WRAP" &
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
case_run "share picker as the portal's dmenu chooser: only what the app asked for, answered in its lines" "
foot -T one > /dev/null 2>&1 &
sleep 1.2
$CTL pointer move 640 360 > /dev/null
printf 'Monitor: HEADLESS-1 a screen\\n' | $CTL share-pick --dmenu > a.out 2>&1
echo \"screen asked [\$(cat a.out)]\"
$CTL share-pick > raw.out 2>&1 &
sleep 0.5
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.3
ID=\$(sed 's/^Window: //' raw.out)
printf 'Window: one (%s)\\nWindow: other (nope)\\n' \"\$ID\" | $CTL share-pick --dmenu > w.out 2>&1 &
sleep 0.5
$CTL key Right press > /dev/null; $CTL key Right release > /dev/null
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.3
[ \"\$(cat w.out)\" = \"Window: one (\$ID)\" ] && echo 'window asked: its line back' || echo \"window asked: [\$(cat w.out)]\"
printf 'Monitor: HEADLESS-1 a screen\\nWindow: one (%s)\\n' \"\$ID\" | $CTL share-pick --dmenu > b.out 2>&1 &
sleep 0.5
$CTL key Right press > /dev/null; $CTL key Right release > /dev/null
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.3
echo \"both asked, screen picked [\$(cat b.out)]\"" "screen asked [Monitor: HEADLESS-1 a screen]" \
	"window asked: its line back" "both asked, screen picked [Monitor: HEADLESS-1 a screen]"
# flame's own portal on a bus of its own, called the way xdg-desktop-portal
# calls it, streams taken from the running pipewire like apps take them
CASE_WRAP="dbus-run-session --"
CASE_ENV="FLAME_PORTAL=1"
MIDDLE="python3 -I -c \"
import sys
d = open(sys.argv[1], 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
i = ((h // 2) * w + w // 2) * 3
print('%02x%02x%02x' % (px[i], px[i + 1], px[i + 2]))
\""
case_run "portal: a screen shared straight away, gpu buffers, the same picture as the screen, asked again: no pick" "
foot -o colors-dark.background=aa2233 > /dev/null 2>&1 &
sleep 1.2
$BIN/portalcast screen again gpu > cast.out 2>&1
grim -t ppm shot.ppm
cat cast.out
[ \"\$(sed -n 's/^first: middle pixel //p' cast.out)\" = \"\$($MIDDLE shot.ppm)\" ] && echo 'stream matches the screen'" \
	"first: start 0, right away" "first: source screen, 1280x720 at 0,0, kept yes" \
	"first: frames came, 1280x720 in gpu buffers" "first: first frame right away" \
	"again: start 0, right away" "again: frames came, 1280x720 in gpu buffers" \
	"stream matches the screen"
case_run "portal: a window picked in flame's picker, shared at its size, in shared memory too, a cancel says so" "
foot -T left > /dev/null 2>&1 &
sleep 1
foot -T right > /dev/null 2>&1 &
sleep 1.2
$CTL pointer move 300 360 > /dev/null
$BIN/portalcast window again > cast.out 2>&1 &
sleep 0.8
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 5
cat cast.out
$BIN/portalcast window > cancel.out 2>&1 &
sleep 0.8
$CTL key Escape press > /dev/null; $CTL key Escape release > /dev/null
sleep 0.5
cat cancel.out" \
	"first: start 0" "first: source window, 627x704 at 0,0, kept yes" \
	"first: frames came, 627x704 in shared memory" "again: start 0, right away" \
	"again: frames came, 627x704 in shared memory" "first: start 1"
case_run "portal: a shared window that closes ends its share, flame says so" "
foot -T gone > /dev/null 2>&1 &
sleep 1.2
$BIN/portalcast window closed > cast.out 2>&1 &
sleep 0.8
$CTL key Return press > /dev/null; $CTL key Return release > /dev/null
sleep 0.8
$CTL close > /dev/null
sleep 1.5
cat cast.out" "first: start 0" "first: closed by flame"
case_run "portal: a screenshot, a png of the screen" "
busctl --user call org.freedesktop.impl.portal.desktop.flamewm /org/freedesktop/portal/desktop \
	org.freedesktop.impl.portal.Screenshot Screenshot ossa{sv} /r/1 test '' 0 > shot.out 2>&1
cat shot.out
f=\$(grep -o '/tmp/flamewm-screenshot-[A-Za-z0-9]*.png' shot.out)
file \"\$f\"
rm -f \"\$f\"" '"uri" s "file:///tmp/flamewm-screenshot-' "PNG image data, 1280 x 720"
CASE_WRAP=
CASE_ENV=
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

# sharp stripes behind, a see-through window with glass floating over them:
# inside it they're smoothed, around it they stay sharp
ROUGH="python3 -I -c \"
import sys
d = open(sys.argv[1], 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
def rough(x0, x1, y):
	return sum(abs(px[(y * w + x) * 3 + c] - px[(y * w + x + 1) * 3 + c])
		for x in range(x0, x1) for c in range(3)) / (x1 - x0)
inside, outside = rough(480, 800, 330), rough(40, 400, 330)
print(sys.argv[2], 'smooth' if inside < outside / 3 else 'sharp', 'over sharp' if outside > 3 else 'over nothing')
\""
cat > "$WORK/stripes.sh" <<'EOF'
for i in $(seq 1 40); do
	for j in $(seq 1 20); do printf '\033[41m  \033[42m  \033[44m  \033[0m  '; done
	echo
done
sleep 30
EOF
case_run "glass for apps: flame-eyecandy and ext-background-effect blur what's behind a see-through window" "
foot sh stripes.sh > /dev/null 2>&1 &
sleep 1.2
$BIN/wlglass candy 3 &
sleep 1.2
grim -t ppm candy.ppm
sleep 2.2
$BIN/wlglass blur 3 &
sleep 1.2
grim -t ppm blur.ppm
$ROUGH candy.ppm candy
$ROUGH blur.ppm blur" "glass up" "candy smooth over sharp" "blur smooth over sharp"
# the color in the middle of a ppm screenshot, by name
CENTER="python3 -I -c \"
import sys
d = open(sys.argv[1], 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
i = ((h // 2) * w + w // 2) * 3
r, g, b = px[i], px[i + 1], px[i + 2]
print(sys.argv[2], 'red' if r > 200 and g < 40 and b < 40 else 'blue' if b > 200 and r < 40
	else 'green' if g > 100 and r < 40 and b < 40 else (r, g, b))
\""
case_run "wallpaper: flame draws a png or a jpeg over the background, refuses what isn't a picture, unset takes it off" "
$CTL set background '#ff0000' > /dev/null; sleep 0.5
grim red.png; grim -t jpeg red.jpg
$CTL set background '#0000ff' > /dev/null; sleep 0.4
$CTL set wallpaper \$PWD/red.png; sleep 1.2
grim -t ppm a.ppm; $CENTER a.ppm png
$CTL set wallpaper \$PWD/red.jpg > /dev/null; sleep 1.2
grim -t ppm b.ppm; $CENTER b.ppm jpeg
$CTL set wallpaper /etc/passwd
$CTL settings | python3 -I -c \"import json, sys; print('still', json.load(sys.stdin)['values']['wallpaper'].endswith('red.jpg'))\"
$CTL unset wallpaper > /dev/null; sleep 1.2
grim -t ppm c.ppm; $CENTER c.ppm none" '{"ok":true}' "png red" "jpeg red" \
	"not a jpeg, png or webp it can read" "still True" "none blue"
# flame's apps keep how they show things in XDG_CONFIG_HOME, the trash in
# XDG_DATA_HOME, thumbnails in XDG_CACHE_HOME: never the user's. their
# temporary files in a TMPDIR of their own: the cases' files (under /tmp)
# aren't temporary to them
mkdir -p "$WORK/apps/tmp"
CASE_ENV="XDG_CONFIG_HOME=$WORK/apps/config XDG_DATA_HOME=$WORK/apps/data XDG_CACHE_HOME=$WORK/apps/cache XDG_STATE_HOME=$WORK/apps/state TMPDIR=$WORK/apps/tmp"
case_run "flamesettings: the Wallpaper page shows the wallpaper's folder, a click on a picture puts it on" "
mkdir walls
$CTL set background '#ff0000' > /dev/null; sleep 0.5; grim walls/red.png
$CTL set background '#0000ff' > /dev/null; sleep 0.5; grim walls/blue.png
$CTL set wallpaper \$PWD/walls/red.png > /dev/null; sleep 0.5
$(dirname $CTL)/flamesettings wallpaper > /dev/null 2>&1 &
sleep 2
$CTL pointer move 538 390 > /dev/null; sleep 0.3
$CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null
sleep 0.6
$CTL settings | python3 -I -c \"import json, sys; print('picked', json.load(sys.stdin)['values']['wallpaper'].split('/')[-1])\"" \
	"picked blue.png"
case_run "flamefiles: super+e opens it, a new folder named, a copy pasted into it, the trash and back" "
$CTL key Super_L press > /dev/null; $CTL key e press > /dev/null
$CTL key e release > /dev/null; $CTL key Super_L release > /dev/null
sleep 1.5
$CTL state | grep -o '\"app_id\":\"flamefiles\"'
$CTL key Super_L press > /dev/null; $CTL key c press > /dev/null
$CTL key c release > /dev/null; $CTL key Super_L release > /dev/null
sleep 0.5
mkdir box; echo hi > box/notes.txt; echo x > box/a.txt
export XDG_DATA_HOME=\$PWD/data XDG_CONFIG_HOME=\$PWD/config
$(dirname $CTL)/flamefiles \$PWD/box > /dev/null 2>&1 &
sleep 2
key() { for k in \"\$@\"; do $CTL key \$k press > /dev/null; $CTL key \$k release > /dev/null; done; }
combo() { for k in \"\$@\"; do $CTL key \$k press > /dev/null; done; for k in \"\$@\"; do $CTL key \$k release > /dev/null; done; }
click() { $CTL pointer move \$1 \$2 > /dev/null; sleep 0.2; for i in \$(seq \${3:-1}); do $CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null; done; sleep 0.5; }
combo Control_L Shift_L n; sleep 0.5
for i in \$(seq 16); do key BackSpace; done
key d o c s Return; sleep 0.5
echo folder \$(ls box | tr '\n' ' ')
click 394 110; combo Control_L c
click 278 110 2
combo Control_L v; sleep 1
echo pasted \$(ls box/docs)
combo Alt_L Left; sleep 0.5
click 509 110; key Delete; sleep 0.5
echo trashed \$(ls box | grep -c notes) \$(ls data/Trash/files) \$(grep -c \"Path=\$PWD/box/notes.txt\" data/Trash/info/notes.txt.trashinfo)
click 68 330; click 278 110
$CTL pointer button right press > /dev/null; $CTL pointer button right release > /dev/null; sleep 0.4
click 310 136
echo back \$(ls box | grep -c notes) \$(ls data/Trash/files | wc -l)" \
	'"app_id":"flamefiles"' "folder a.txt docs notes.txt" "pasted a.txt" "trashed 0 notes.txt 1" \
	"back 1 0"
case_run "flamefiles --pick: only what it asks for can be picked, a double click prints its path" "
mkdir pics; grim pics/shot.png; echo hi > pics/a.txt
export XDG_DATA_HOME=\$PWD/data XDG_CONFIG_HOME=\$PWD/config
$(dirname $CTL)/flamefiles --pick --images \$PWD/pics > picked.txt &
pick=\$!
sleep 2
$CTL state | python3 -I -c \"import json, sys; w = json.load(sys.stdin)['window']; print(w['app_id'], w['floating'])\"
dbl() { $CTL pointer move \$1 \$2 > /dev/null; sleep 0.2; for i in 1 2; do $CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null; done; sleep 0.8; }
dbl 488 184
kill -0 \$pick && echo still choosing
dbl 614 184
wait \$pick; echo exit \$?
echo picked \$(basename \$(cat picked.txt))" "flamefiles-chooser True" "still choosing" "exit 0" \
	"picked shot.png"
case_run "flamefiles: drag and drop: onto a folder it moves, out to an app as paths, in from one (same drive: moved)" "
mkdir -p box/docs config/flame; echo a > box/a.txt; echo b > box/b.txt; echo s > src.txt
printf 'view=list\n' > config/flame/files.conf
export XDG_DATA_HOME=\$PWD/data XDG_CONFIG_HOME=\$PWD/config XDG_CACHE_HOME=\$PWD/cache
$(dirname $CTL)/flamefiles \$PWD/box > /dev/null 2>&1 &
sleep 2
drag() { $CTL pointer move \$1 \$2 > /dev/null; sleep 0.3; $CTL pointer button left press > /dev/null; sleep 0.1
	shift 2; for p in \"\$@\"; do $CTL pointer move \$p > /dev/null; sleep 0.08; done; sleep 0.5
	$CTL pointer button left release > /dev/null; sleep 1; }
drag 300 126 '300 120' '300 112' '300 104' '300 102'
echo docs \$(ls box/docs) top \$(ls box | tr '\n' ' ')
$BIN/wldnd target '' 10 > dst.out 2>&1 &
sleep 1.2
drag 300 126 '320 140' '500 250' '800 360' '950 360'
grep -o 'dropped: .*' dst.out
$BIN/wldnd source \"file://\$PWD/src.txt\" 10 > src.out 2>&1 &
sleep 1.2
$CTL pointer move 950 360 > /dev/null; sleep 0.3; $CTL pointer button left press > /dev/null; sleep 0.2
for p in '900 380' '700 420' '500 460' '300 500'; do $CTL pointer move \$p > /dev/null; sleep 0.1; done; sleep 0.6
$CTL pointer button left release > /dev/null; sleep 1.2
echo moved \$(ls box | grep -c src.txt) \$(ls | grep -c src.txt)" "docs a.txt top b.txt docs" \
	"box/b.txt" "moved 1 0"
case_run "flamefiles: the clipboard: copied out the way file managers do, pasted in, a cut moved, a field pastes" "
mkdir -p box/docs elsewhere config/flame; echo a > box/a.txt; echo x > elsewhere/x.txt; echo y > elsewhere/y.txt
printf 'view=list\n' > config/flame/files.conf
export XDG_DATA_HOME=\$PWD/data XDG_CONFIG_HOME=\$PWD/config XDG_CACHE_HOME=\$PWD/cache
$(dirname $CTL)/flamefiles \$PWD/box > /dev/null 2>&1 &
sleep 2
click() { $CTL pointer move \$1 \$2 > /dev/null; sleep 0.2; for i in \$(seq \${3:-1}); do $CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null; done; sleep 0.5; }
combo() { for k in \"\$@\"; do $CTL key \$k press > /dev/null; done; for k in \"\$@\"; do $CTL key \$k release > /dev/null; done; sleep 0.4; }
key() { for k in \"\$@\"; do $CTL key \$k press > /dev/null; $CTL key \$k release > /dev/null; done; }
click 300 126; combo Control_L c
echo gnome \$(wl-paste -n -t x-special/gnome-copied-files | head -1) \$(wl-paste -n -t x-special/gnome-copied-files | tail -1 | sed 's|.*/||')
echo text \$(wl-paste -n | sed 's|.*/||')
click 300 102 2
combo Control_L v; sleep 1
echo docs \$(ls box/docs | tr '\n' ' ')
wl-copy -t text/uri-list \"file://\$PWD/elsewhere/x.txt\"; sleep 0.3
combo Control_L v; sleep 1
echo docs \$(ls box/docs | tr '\n' ' ') elsewhere \$(ls elsewhere | tr '\n' ' ')
printf 'cut\nfile://%s/elsewhere/y.txt' \"\$PWD\" | wl-copy -t x-special/gnome-copied-files; sleep 0.3
combo Control_L v; sleep 1
echo docs \$(ls box/docs | tr '\n' ' ') elsewhere \$(ls elsewhere | tr '\n' ' ') clipboard \$(wl-paste -l 2>/dev/null | wc -l)
wl-copy renamed; sleep 0.3
click 300 126; key F2; sleep 0.3
for i in \$(seq 8); do key BackSpace; done
combo Control_L v; sleep 0.5; key Return; sleep 0.8
echo docs \$(ls box/docs | tr '\n' ' ')" \
	"gnome copy a.txt" "text a.txt" "docs a.txt" "docs a.txt x.txt elsewhere x.txt y.txt" \
	"docs a.txt x.txt y.txt elsewhere x.txt clipboard 0" "docs a.txt renamed y.txt"
case_run "flamefiles: a video's frame and a song's cover for previews, kept in the thumbnail cache" "
mkdir media; grim -t jpeg cover.jpg
ffmpeg -v error -f lavfi -i testsrc2=size=640x360:rate=30:duration=3 -c:v libx264 -pix_fmt yuv420p media/clip.mp4
ffmpeg -v error -f lavfi -i sine=duration=1 -i cover.jpg -map 0:a -map 1:v -c:a libmp3lame -c:v mjpeg -disposition:v attached_pic media/song.mp3
ffmpeg -v error -f lavfi -i sine=duration=1 -c:a libmp3lame media/plain.mp3
export XDG_CACHE_HOME=\$PWD/cache XDG_CONFIG_HOME=\$PWD/config
$(dirname $CTL)/flamefiles \$PWD/media > /dev/null 2>&1 &
sleep 3
python3 -I -c \"
import hashlib, os, sys
for name in ['clip.mp4', 'song.mp3', 'plain.mp3']:
    path = os.path.join(sys.argv[1], 'media', name)
    uri = 'file://' + path
    f = os.path.join(sys.argv[1], 'cache/thumbnails/large', hashlib.md5(uri.encode()).hexdigest() + '.png')
    ok = os.path.exists(f) and ('Thumb::MTime\\0' + str(int(os.stat(path).st_mtime))).encode() in open(f, 'rb').read()
    print(name, 'kept' if ok else 'none')
\" \$PWD" "clip.mp4 kept" "song.mp3 kept" "plain.mp3 none"
case_run "flamesettings: super+comma opens it, borderless glass, a toggle and a slider change flame live" "
$CTL key Super_L press > /dev/null; $CTL key comma press > /dev/null
$CTL key comma release > /dev/null; $CTL key Super_L release > /dev/null
sleep 1.5
$CTL state | grep -o '\"app_id\":\"flamesettings\"'
$CTL state | python3 -I -c \"import json, sys; w = json.load(sys.stdin)['window']; print('window', w['width'], w['height'])\"
$CTL settings | python3 -I -c \"import json, sys; v = json.load(sys.stdin)['values']; print('before', v['animations.enabled'], v['gap'])\"
$CTL pointer move 1028 440 > /dev/null; sleep 0.2
$CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null
sleep 0.2
$CTL pointer move 814 184 > /dev/null; sleep 0.1
$CTL pointer button left press > /dev/null
for x in 830 860 900 940 980; do $CTL pointer move \$x 184 > /dev/null; sleep 0.03; done
$CTL pointer button left release > /dev/null
sleep 0.6
$CTL settings | python3 -I -c \"import json, sys; v = json.load(sys.stdin)['values']; print('after', v['animations.enabled'], v['gap'])\"" \
	'"app_id":"flamesettings"' "window 900 600" "before True 6" "after False 39"
case_run "flamesettings: a search finds settings on any page, the window resizes but no smaller than it draws" "
$CTL key Super_L press > /dev/null; $CTL key comma press > /dev/null
$CTL key comma release > /dev/null; $CTL key Super_L release > /dev/null
sleep 1.5
$CTL pointer move 640 400 > /dev/null
for k in a n i; do $CTL key \$k press > /dev/null; $CTL key \$k release > /dev/null; done
sleep 0.4
$CTL pointer move 1028 184 > /dev/null; sleep 0.2
$CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null
sleep 0.4
$CTL settings | python3 -I -c \"import json, sys; print('found', json.load(sys.stdin)['values']['animations.enabled'])\"
win() { $CTL state | python3 -I -c \"import json, sys; w = json.load(sys.stdin)['window']; print('\$1', w['width'], w['height'], w['floating'])\"; }
win opened
$CTL pointer move 1000 600 > /dev/null
$CTL key Super_L press > /dev/null; $CTL pointer button right press > /dev/null
for x in 1040 1080 1120 1160; do $CTL pointer move \$x 640 > /dev/null; sleep 0.05; done
$CTL pointer button right release > /dev/null; $CTL key Super_L release > /dev/null
sleep 0.6
win bigger
$CTL pointer move 1100 620 > /dev/null
$CTL key Super_L press > /dev/null; $CTL pointer button right press > /dev/null
for x in 900 700 500 300; do $CTL pointer move \$x 200 > /dev/null; sleep 0.05; done
$CTL pointer button right release > /dev/null; $CTL key Super_L release > /dev/null
sleep 0.6
win squeezed" "found False" "opened 900 600 True" "bigger 1060 640 True" "squeezed 780 480 True"
printf 'flame.set{ gap = 4 }\n' > "$WORK/pin.lua"
CASE_ARGS="-c $WORK/pin.lua"
# decoded on the gpu where it can be (vulkan), and on the cpu
for hwdec in "" no; do
case_run "flameplay${hwdec:+ (hwdec=$hwdec)}: a video floats at its size, pauses, seeks to the exact frame, goes fullscreen and back; a song shows its cover" "
export FLAMEPLAY_HWDEC=$hwdec
ffmpeg -v error -f lavfi -i 'color=c=red:size=640x360:rate=30:duration=5[r];color=c=blue:size=640x360:rate=30:duration=5[b];[r][b]concat=n=2:v=1' -c:v libx264 -pix_fmt yuv420p redblue.mp4
ffmpeg -v error -f lavfi -i color=c=green:size=300x300 -frames:v 1 cover.png
ffmpeg -v error -f lavfi -i sine=duration=3 -i cover.png -map 0:a -map 1:v -c:a libmp3lame -c:v png -disposition:v attached_pic song.mp3
key() { $CTL key \$1 press > /dev/null; $CTL key \$1 release > /dev/null; sleep \${2:-0.5}; }
win() { $CTL state | python3 -I -c \"import json, sys; w = json.load(sys.stdin)['window']; print('\$1', w['app_id'], w['floating'], w['width'], w['height'], w.get('fullscreen'))\"; }
$(dirname $CTL)/flameplay \$PWD/redblue.mp4 > /dev/null 2>&1 &
play=\$!
sleep 2
win opened
grim -t ppm a.ppm; $CENTER a.ppm start
key space
key Right 0.8
grim -t ppm b.ppm; $CENTER b.ppm seeked
key f 1
win full
key Escape 1
win back
key q
wait \$play; echo quit \$?
$(dirname $CTL)/flameplay \$PWD/song.mp3 > /dev/null 2>&1 &
sleep 2
grim -t ppm c.ppm; $CENTER c.ppm song" \
	"opened flameplay True 640 360 False" "start red" "seeked blue" "full flameplay True 1280 720 True" \
	"back flameplay True 640 360 False" "quit 0" "song green"
done
case_run "flameplay: fullscreen with nothing over it goes straight to the screen, the bar back composes it again" "
ffmpeg -v error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=20 -c:v libx264 -pix_fmt yuv420p wide.mp4
scanout() { $CTL state | python3 -I -c \"import json, sys; print('\$1', json.load(sys.stdin)['outputs'][0]['scanout'])\"; }
$(dirname $CTL)/flameplay \$PWD/wide.mp4 > /dev/null 2>&1 &
sleep 2
$CTL pointer move 640 360 > /dev/null
$CTL key f press > /dev/null; $CTL key f release > /dev/null
sleep 1
scanout bar
sleep 4.5
scanout watching
$CTL pointer move 600 300 > /dev/null
sleep 0.5
scanout moved" \
	"bar False" "watching True" "moved False"
case_run "flamebar: windows start under it, the control center opens over them without moving them, a click outside closes it" "
win() { $CTL state | python3 -I -c \"import json, sys; w = json.load(sys.stdin)['window']; print('\$1', w['app_id'], w['y'] >= 32, w['x'], w['y'], w['width'], w['height'])\"; }
# the tiles' corner of the screen, how bright: the control center there or not
bright() { grim -t ppm \$1.ppm; python3 -I -c \"
import sys
d = open('\$1.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
n = sum(px[(y * w + x) * 3] for y in range(200, 260, 2) for x in range(w - 320, w - 20, 2))
print(n)
\"; }
click() { $CTL pointer move \$1 \$2 > /dev/null; sleep 0.2; $CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null; sleep 0.8; }
# the control center's button: the first thing that's lit right of the
# bar's middle, the time after it (where depends on how wide the date is)
button() { grim -t ppm bar.ppm; python3 -I -c \"
d = open('bar.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
print(next(x for x in range(w // 2, w) if px[(16 * w + x) * 3] > 200) + 7)
\"; }
# no session bus: the user's would be found through XDG_RUNTIME_DIR, and
# its notifications would come here
DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/flame-e2e $(dirname $CTL)/flamebar > /dev/null 2>&1 &
sleep 1
$(dirname $CTL)/flamefiles > /dev/null 2>&1 &
sleep 1.5
win under
a=\$(bright a)
click \$(button) 16
win open
b=\$(bright b)
click 500 500
win closed
c=\$(bright c)
echo opened \$([ \$b -gt \$((a + a / 20)) ] && echo brighter)
echo closed \$([ \$c -lt \$((b - b / 40)) ] && echo darker)" \
	"under flamefiles True 4 36 1272 680" "open flamefiles True 4 36 1272 680" \
	"closed flamefiles True 4 36 1272 680" "opened brighter" "closed darker"
case_run "flamebar: notifications: a card for each, its action back to the app, clicks beside it go to the windows" "
win() { $CTL state | python3 -I -c \"import json, sys; print(json.load(sys.stdin)['window']['x'])\"; }
# the cards' corner of the screen, how bright: a card there or not
bright() { grim -t ppm \$1.ppm; python3 -I -c \"
d = open('\$1.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
print(sum(px[(y * w + x) * 3] for y in range(46, 96, 2) for x in range(w - 320, w - 20, 2)))
\"; }
click() { $CTL pointer move \$1 \$2 > /dev/null; sleep 0.2; $CTL pointer button left press > /dev/null; $CTL pointer button left release > /dev/null; sleep 0.6; }
# a session bus of the case's own: never the user's
bus=\$(dbus-daemon --session --fork --print-address=1 --print-pid=1 --nopidfile)
export DBUS_SESSION_BUS_ADDRESS=\$(echo \"\$bus\" | sed -n 1p)
buspid=\$(echo \"\$bus\" | sed -n 2p)
$(dirname $CTL)/flamebar > /dev/null 2>&1 &
sleep 1
gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.GetServerInformation
$(dirname $CTL)/flamefiles > /dev/null 2>&1 &
sleep 1.2
$(dirname $CTL)/flamefiles > /dev/null 2>&1 &
sleep 1.5
click 1000 400
echo focused \$([ \$(win) -gt 100 ] && echo right)
a=\$(bright a)
notify-send -a Test 'hello' 'from the tests'
sleep 1
b=\$(bright b)
echo card \$([ \$b -gt \$((a + a / 20)) ] && echo shown)
# the bar's taller for the card, all across: the left window under it still
# takes the click
click 300 80
echo beside \$(win)
(notify-send -a Test 'click me' 'please' --action=default=Open > act.txt &)
sleep 1.2
click 1100 70
sleep 0.5
echo action \$(cat act.txt)
kill \$buspid" \
	"('flame', 'flame', '0.1', '1.2')" "focused right" "card shown" "beside 4" "action default"
case_run "flamesearch: a sum's answer copied, run again it's the same one up, a folder found opens in flamefiles" "
key() { $CTL key \$1 press > /dev/null; $CTL key \$1 release > /dev/null; }
shifted() { $CTL key Shift_L press > /dev/null; key \$1; $CTL key Shift_L release > /dev/null; }
# a home of the case's own: what it finds is what's here
mkdir -p home/Work/flamereports
HOME=\$PWD/home $(dirname $CTL)/flamesearch > search.log 2>&1 &
pid=\$!
sleep 1.5
key 2; shifted equal; key 3; shifted 8; key 4
sleep 0.3
key Return
sleep 0.6
echo sum \$(wl-paste -n)
echo running \$(kill -0 \$pid 2>/dev/null && echo still)
# run again: the one running comes up, this one's gone at once
HOME=\$PWD/home $(dirname $CTL)/flamesearch
echo told \$?
sleep 0.5
for c in f l a m e r e p; do key \$c; done
sleep 0.5
key Return
sleep 2
$CTL state | python3 -I -c \"import json, sys; print('opened', json.load(sys.stdin)['window']['app_id'])\"
echo running \$(kill -0 \$pid 2>/dev/null && echo still)
cat search.log" \
	"sum 14" "running still" "told 0" "opened flamefiles"
case_run "lock: only the locker shows and gets keys, a second one's turned away, one that dies leaves it locked, a new one unlocks" "
# the middle of the screen: the locker's red, the cover's black, else the window
center() { grim -t ppm c.ppm; python3 -I -c \"
d = open('c.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
r, g, b = px[(h // 2 * w + w // 2) * 3:][:3]
print('\$1', 'red' if (r, g, b) == (255, 0, 0) else 'black' if (r, g, b) == (0, 0, 0) else 'the window')
\"; }
key() { $CTL key \$1 press > /dev/null; $CTL key \$1 release > /dev/null; }
windows() { $CTL state | python3 -I -c \"import json, sys; print('\$1', sum(w['windows'] for w in json.load(sys.stdin)['workspaces']))\"; }
$(dirname $CTL)/flamefiles > /dev/null 2>&1 &
sleep 1.5
center before
$BIN/wllock unlock 2500 > one.txt 2>&1 &
sleep 0.8
center locked
key a; key b
$BIN/wllock second
sleep 2.2
center unlocked
cat one.txt
$BIN/wllock crash
sleep 0.3
center crashed
# a bind does nothing while it's locked (super+e: another flamefiles)
$CTL key Super_L press > /dev/null; key e; $CTL key Super_L release > /dev/null
sleep 1
windows still
$BIN/wllock unlock 300
sleep 0.4
center back" \
	"before the window" "locked red" "finished" "unlocked the window" "locked" "key 30" "key 48" \
	"unlocked" "crashed black" "still 1" "back the window"
case_run "flamelock: super+l locks with the time over the background, killed it stays locked" "
# the time's white at the top middle: how much of it there is
clock() { grim -t ppm c.ppm; python3 -I -c \"
d = open('c.ppm', 'rb').read()
w, h = map(int, d.split(b'\\\\n')[1].split())
px = d[-(w * h * 3):]
top = int(h * 0.09)
n = sum(1 for y in range(top, top + 140, 2) for x in range(w // 2 - 160, w // 2 + 160, 2) if px[(y * w + x) * 3] > 200)
c = px[(h // 2 * w + w // 2) * 3:][:3]
print('\$1', 'clock' if n > 300 else 'no clock', 'black' if tuple(c) == (0, 0, 0) else '')
\"; }
key() { $CTL key \$1 press > /dev/null; $CTL key \$1 release > /dev/null; }
windows() { $CTL state | python3 -I -c \"import json, sys; print('\$1', sum(w['windows'] for w in json.load(sys.stdin)['workspaces']))\"; }
$(dirname $CTL)/flamefiles > /dev/null 2>&1 &
sleep 1.5
clock before
$CTL key Super_L press > /dev/null; key l; $CTL key Super_L release > /dev/null
sleep 1.5
clock locked
$CTL key Super_L press > /dev/null; key e; $CTL key Super_L release > /dev/null
sleep 1
windows still
for p in \$(pgrep -x flamelock); do grep -qzx \"WAYLAND_DISPLAY=\$WAYLAND_DISPLAY\" /proc/\$p/environ && kill -9 \$p; done
sleep 0.5
clock killed
$BIN/wllock unlock 200 > /dev/null
sleep 0.4
clock back" \
	"before no clock" "locked clock" "still 1" "killed no clock black" "back no clock"
case_run "media keys: to whoever takes them, volume again and again while it's held, play once" "
cat > keys.py <<'PY'
import json, os, socket, subprocess, sys, time
ctl = sys.argv[1]
def key(name, how):
    subprocess.run([ctl, 'key', name, how], stdout=subprocess.DEVNULL)
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ['FLAMEWM_SOCKET'])
s.sendall(b'media-keys\\n')
time.sleep(0.3)
key('XF86AudioMute', 'press'); key('XF86AudioMute', 'release')
key('XF86AudioPlay', 'press'); time.sleep(0.6); key('XF86AudioPlay', 'release')
key('XF86AudioRaiseVolume', 'press'); time.sleep(0.8); key('XF86AudioRaiseVolume', 'release')
time.sleep(0.3)
s.setblocking(False)
data = b''
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
except BlockingIOError:
    pass
counts = {}
for line in data.decode().splitlines():
    event = json.loads(line)
    if event.get('event') == 'key':
        counts[event['key']] = counts.get(event['key'], 0) + 1
print('mute', counts.get('mute'))
print('play-pause', counts.get('play-pause'))
print('volume-up', 'again and again' if counts.get('volume-up', 0) > 5 else counts.get('volume-up'))
PY
python3 -I keys.py $CTL" \
	"mute 1" "play-pause 1" "volume-up again and again"
case_run "flameplay: subtitles: a file beside it on from the start, off, the video's own (paused)" "
printf '1\\n00:00:00,300 --> 00:00:30,000\\nINSIDE THE VIDEO\\n' > own.srt
printf '1\\n00:00:00,300 --> 00:00:30,000\\nBESIDE IT IN A FILE\\n' > subs.fr.srt
ffmpeg -v error -f lavfi -i color=c=black:size=640x360:rate=10:duration=30 -i own.srt -map 0 -map 1 -c:v libx264 -pix_fmt yuv420p -c:s srt subs.mkv
key() { $CTL key \$1 press > /dev/null; $CTL key \$1 release > /dev/null; sleep \${2:-0.8}; }
# bright pixels just over the bar (it's up, paused): the subtitles
words() { grim -t ppm \$1.ppm; $CTL state | python3 -I -c \"
import json, sys
w = json.load(sys.stdin)['window']
d = open('\$1.ppm', 'rb').read()
sw, sh = map(int, d.split(b'\\\\n')[1].split())
px = d[-(sw * sh * 3):]
n = sum(1 for y in range(w['y'] + 190, w['y'] + 270) for x in range(w['x'], w['x'] + w['width'])
	if min(px[(y * sw + x) * 3:(y * sw + x) * 3 + 3]) > 200)
print('\$1', 'words' if n > 150 else 'none')
\"; }
$(dirname $CTL)/flameplay \$PWD/subs.mkv > /dev/null 2>&1 &
sleep 2
$CTL pointer move 640 300 > /dev/null
key space
words file
key s
words off
key s 1.5
words own" \
	"file words" "off none" "own words"
case_run "flameplay: a video goes on from where it was left (as loud), from the start once it's left near it" "
ffmpeg -v error -f lavfi -i 'color=c=red:size=640x360:rate=5:duration=30[r];color=c=blue:size=640x360:rate=5:duration=60[b];[r][b]concat=n=2:v=1' -c:v libx264 -pix_fmt yuv420p long.mp4
key() { $CTL key \$1 press > /dev/null; $CTL key \$1 release > /dev/null; sleep \${2:-0.5}; }
play() { $(dirname $CTL)/flameplay \$PWD/long.mp4 > /dev/null 2>&1 & sleep 2; grim -t ppm \$1.ppm; $CENTER \$1.ppm \$1; }
play first
key l; key l; key l 1
key Down; key Down
key q 1
echo loud \$(cat \$XDG_STATE_HOME/flame/flameplay-volume)
play again
echo kept \$(wc -l < \$XDG_STATE_HOME/flame/flameplay-positions)
key Home 1
key q 1
echo still \$(cat \$XDG_STATE_HOME/flame/flameplay-volume)
play over" \
	"first red" "loud 0.900 1.000" "again blue" "kept 1" "still 0.900 1.000" "over red"
CASE_ENV=
case_run "settings: changed live into settings.lua, init.lua's win, unset goes back, bad values refused" "
$CTL set border_width 5 > /dev/null
$CTL set keyboard.layout us,ara > /dev/null
$CTL set output.HEADLESS-1.scale 1.5 > /dev/null
echo \"pinned: \$($CTL set gap 9 2>&1)\"
echo \"too big: \$($CTL set border_width 99 2>&1)\"
echo \"no such: \$($CTL set nope 1 2>&1)\"
sleep 0.3
$CTL settings | python3 -I -c \"
import json, sys
d = json.load(sys.stdin)
v = d['values']
print('border', v['border_width'], 'layout', v['keyboard.layout'], 'gap', v['gap'])
print('pinned', d['pinned'], 'scale', d['outputs'][0]['scale'])
\"
grep -c 'flame.set' settings.lua
$CTL unset border_width > /dev/null
sleep 0.3
$CTL settings | python3 -I -c \"import json, sys; print('unset', json.load(sys.stdin)['values']['border_width'])\"" \
	"border 5 layout us,ara gap 4" "pinned ['gap'] scale 1.5" "unset 2" \
	'pinned: {"ok":false,"error":"init.lua sets it, it wins"}' \
	'too big: {"ok":false,"error":"not a whole number in range"}' \
	'no such: {"ok":false,"error":"no such setting"}'
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

wait
[ -e "$WORK/failed" ] && failed=1
rm -rf "$WORK"
exit $failed
