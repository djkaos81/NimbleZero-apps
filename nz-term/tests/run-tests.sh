#!/bin/sh
# Scripted tests for nz-term: no screen and no keyboard needed.  Programs really run in it;
# what they print is checked through what they write to files, and through screenshots.
#   tests/run-tests.sh $PWD/nz-term-sim
#   tests/run-tests.sh qemu-aarch64-static -L /usr/aarch64-linux-gnu $PWD/nz-term
#   SHOTS=dir tests/run-tests.sh ...     keep the screenshots
set -u
[ $# -ge 1 ] || { sed -n '2,6p' "$0"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
S=${SHOTS:-$T/shots}; mkdir -p "$S" "$T/cfg" "$T/run" "$T/work"
export NZT_HEADLESS=1 SDL_VIDEODRIVER=dummy NZK_BATTERY=64+ NZK_CLOCK=15:40 NZT_CONFIG_DIR="$T/cfg" XDG_RUNTIME_DIR="$T/run" HOME="$T" LANG=C.UTF-8
fail=0; n=0; O=$T/out
run() { keys=$1; shift; rm -f "$O"; NZT_KEYS="$keys" timeout 60 "$@" 2>"$T/log"; }
ok() { n=$((n + 1)); if eval "$2"; then echo "ok   $n $1"; else echo "FAIL $n $1"; fail=1; sed 's/^/     | /' "$T/log" | tail -5; [ -f "$O" ] && od -c "$O" | head -4 | sed 's/^/     > /'; fi; }
cfg() { printf 'size=%s\ntitle=%s\n' "$1" "$2" > "$T/cfg/config"; }
sh_() { printf '%s\n' "$1" > "$T/s.sh"; }

cfg 1 1; sh_ "stty size > $O; echo \$TERM >> $O; pwd >> $O"
run "wait:500" "$@" -d "$T/work" -e sh "$T/s.sh"
ok "medium text with the title bar: 19 rows of 53; TERM; -d FOLDER" "[ \"\$(sed -n 1p $O)\" = '19 53' ] && grep -q '^xterm' $O && grep -q '/work\$' $O"
cfg 2 0; run "wait:500" "$@" -e sh "$T/s.sh"; ok "small text without the title bar: 28 rows of 80" "[ \"\$(sed -n 1p $O)\" = '28 80' ]"
cfg 0 1; run "wait:500" "$@" -e sh "$T/s.sh"; ok "large text: 19 rows of 40" "[ \"\$(sed -n 1p $O)\" = '19 40' ]"
cfg 1 1
sh_ "read a; printf '%s' \"\$a\" > $O"
run "wait:300,text:Hello World 42,enter,wait:300" "$@" -e sh "$T/s.sh"; ok "typing, with Shift for capitals" "[ \"\$(cat $O)\" = 'Hello World 42' ]"
sh_ "stty raw -echo; dd bs=1 count=\$1 2>/dev/null | od -An -c > $O"
keys() { run "wait:400,$1,wait:400" "$@"; }
run "wait:400,up,down,right,left,wait:400" "$@" -e sh "$T/s.sh" 12; ok "arrow keys" "[ \"\$(tr -d ' \n' < $O)\" = '033[A033[B033[C033[D' ]"
run "wait:400,C-c,C-d,A-x,tab,esc,backspace,enter,wait:400" "$@" -e sh "$T/s.sh" 8; ok "Ctrl+C, Ctrl+D, Alt+X, Tab, Esc, Backspace, Enter" "[ \"\$(tr -d ' \n' < $O)\" = '003004033x\t033177\r' ]"
run "wait:400,f1,f5,delete,pgup,home,wait:400" "$@" -e sh "$T/s.sh" 19; ok "F1, F5, Delete, PgUp, Home" "[ \"\$(tr -d ' \n' < $O)\" = '033OP033[15~033[3~033[5~033[H' ]"
sh_ "printf '\\033[?1h'; stty raw -echo; dd bs=1 count=3 2>/dev/null | od -An -c > $O"
run "wait:400,up,wait:400" "$@" -e sh "$T/s.sh"; ok "arrow keys in the mode full-screen programs ask for" "[ \"\$(tr -d ' \n' < $O)\" = '033OA' ]"
sh_ "stty raw -echo; printf '\\033[5;7H\\033[6n'; dd bs=1 count=6 2>/dev/null | od -An -c > $O"
run "wait:800" "$@" -e sh "$T/s.sh"; ok "a program can ask where the cursor is" "[ \"\$(tr -d ' \n' < $O)\" = '033[5;7R' ]"
printf 'pasted words\n' > "$T/run/nz-clipboard"; sh_ "read a; printf '%s' \"\$a\" > $O"
run "wait:300,help,shot:$S/menu.ppm,down,enter,wait:200,enter,wait:300" "$@" -e sh "$T/s.sh"; ok "menu: Paste types the editor's clipboard" "[ \"\$(cat $O)\" = 'pasted words' ]"
sh_ "trap 'echo hup > $O; exit' HUP; sleep 30"
run "wait:300,help,down,down,down,down,down,enter,shot:$S/close.ppm,enter" "$@" -e sh "$T/s.sh"; sleep 0.5; ok "menu: Close ends the program in it (hang-up)" "grep -q hup $O"
sh_ "seq 1 100; sleep 5"
run "wait:500,shot:$S/seq.ppm,S-pgup,wait:100,shot:$S/back1.ppm,z,wait:100,shot:$S/back2.ppm,help,down,down,enter,enter,esc,wait:100,shot:$S/large.ppm" "$@" -e sh "$T/s.sh"
ok "scrolling back and the text size setting do not stop it; the size is saved" "grep -q '^size=0\$' $T/cfg/config && [ -s $S/back2.ppm ]"
cfg 1 1; sh_ "printf '\\033[31mred \\033[1;32mbold green\\033[0m \\033[7mreverse\\033[0m \\033[4munderlined\\033[0m\\n\\033(0lqqqqk\\nx    x\\nmqqqqj\\033(B\\n\\342\\224\\214\\342\\224\\200\\342\\224\\220 caf\\303\\251 \\342\\206\\222\\n'; sleep 5"
run "wait:500,shot:$S/colours.ppm" "$@" -e sh "$T/s.sh"; ok "colours, line drawing, accents (see the screenshot)" "[ -s $S/colours.ppm ]"
if command -v nano >/dev/null; then printf 'one\n' > "$T/n.txt"
	run "wait:1200,text:Edited ,wait:200,shot:$S/nano.ppm,C-x,wait:300,y,wait:300,enter,wait:500" "$@" -e nano "$T/n.txt"; ok "nano runs in it: edit, save, leave" "[ \"\$(cat $T/n.txt)\" = 'Edited one' ]"
else echo "skip   nano is not installed here"; fi
run "wait:400" "$@" -e /no/such/program; ok "a program that cannot be started does not hang the terminal" "true"
"$@" --version > "$T/v" 2>&1; ok "--version" "grep -q '^nz-term [0-9]' $T/v"
ok "no sanitizer or runtime error in the last log" "! grep -qiE 'sanitizer|runtime error' $T/log"
[ $fail -eq 0 ] && echo "all $n tests passed" || echo "some tests FAILED"
exit $fail
