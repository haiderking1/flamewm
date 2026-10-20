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
export DISPLAY=\$FLAMEWM_X11_DISPLAY
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

case_run "X windows on a hidden workspace are out of X's way" "
$BIN/xdnd target 8 > /dev/null 2>&1 &
sleep 0.8
echo before: \$($BIN/xwins | grep target)
$CTL workspace 2 > /dev/null
sleep 0.3
echo hidden: \$($BIN/xwins | grep target)
$CTL workspace 1 > /dev/null
sleep 0.3
echo back: \$($BIN/xwins | grep target)" "hidden: target -32000,-32000" "back: target 8,8"

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

rm -rf "$WORK"
exit $failed
