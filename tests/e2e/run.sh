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
EOF
	chmod +x "$WORK/case.sh"
	: > "$WORK/out"
	timeout 30 env WLR_BACKENDS=headless FLAME_TEST_INPUT=1 "$FLAME" -s "$WORK/case.sh" \
		> "$WORK/flame.log" 2>&1
	ok=1
	for want in "$@"; do
		grep -qF -- "$want" "$WORK/out" || ok=0
	done
	if grep -qa "AddressSanitizer\|runtime error" "$WORK/flame.log"; then
		ok=0
	fi
	if [ $ok = 1 ]; then
		echo "ok   $name"
	else
		echo "FAIL $name"
		sed 's/^/    /' "$WORK/out"
		grep -a -A8 "AddressSanitizer\|runtime error" "$WORK/flame.log" | sed 's/^/    /'
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
case_run "clipboard: wayland copies, X pastes" "
printf 'from wayland ✓' | wl-copy
sleep 0.3
$BIN/xclip paste" "from wayland ✓"
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

rm -rf "$WORK"
exit $failed
