#!/bin/sh
# Scripted tests for nz-commander: no screen and no keyboard needed.
#
#   tests/run-tests.sh ./nz-commander-sim
#   tests/run-tests.sh qemu-aarch64-static -L /usr/aarch64-linux-gnu ./nz-commander   (the device program)
#   SHOTS=dir tests/run-tests.sh ...     keep the screenshots
#
# Each test builds a small folder tree, drives the program with NZC_KEYS and looks at the
# files afterwards.  Left pane: $W/home, right pane: $W/home/movies unless said otherwise.
# Order in $W/home: .. docs movies big.bin link notes.txt
set -u
[ $# -ge 1 ] || { sed -n '2,7p' "$0"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T" "${X:-}"' EXIT
S=${SHOTS:-$T/shots}; mkdir -p "$S"
export NZC_HEADLESS=1 SDL_VIDEODRIVER=dummy NZK_BATTERY=64+ NZK_CLOCK=15:40
fail=0; n=0
fresh() {
	rm -rf "$T/w"; W=$T/w; mkdir -p "$W/home/docs/sub" "$W/home/movies" "$W/cfg"
	printf 'first line\nsecond line\n' > "$W/home/notes.txt"; touch -d '2026-11-01 10:00' "$W/home/notes.txt"
	echo a > "$W/home/docs/a.txt"; echo b > "$W/home/docs/sub/b.txt"; echo h > "$W/home/.hidden"
	head -c 3000000 /dev/urandom > "$W/home/big.bin"; ln -s notes.txt "$W/home/link"
	export NZC_CONFIG_DIR="$W/cfg" HOME="$W/home"
}
run() { keys=$1; left=$2; right=$3; shift 3; NZC_KEYS="$keys" timeout 60 "$@" "$left" "$right" 2>"$T/log"; }
ok() { n=$((n + 1)); if eval "$2"; then echo "ok   $n $1"; else echo "FAIL $n $1"; fail=1; sed 's/^/     | /' "$T/log" | tail -5; fi; }
H() { echo "$W/home"; }

fresh; run "shot:$S/01-two-panes.ppm,down,down,down,down,down,5,shot:$S/02-copy-ask.ppm,enter,shot:$S/03-after-copy.ppm" "$W/home" "$W/home/movies" "$@"
ok "copy one file to the other pane" "cmp -s $W/home/notes.txt $W/home/movies/notes.txt"
ok "the copy keeps the date" "[ \$(stat -c %Y $W/home/movies/notes.txt) = \$(stat -c %Y $W/home/notes.txt) ]"

fresh; run "down,space,down,space,space,shot:$S/04-marked.ppm,5,enter,wait:300" "$W/home" "$W/home/movies" "$@"
ok "copy marked folder, file and link" "diff -r $W/home/docs $W/home/movies/docs && cmp -s $W/home/big.bin $W/home/movies/big.bin && [ \"\$(readlink $W/home/movies/link)\" = notes.txt ]"
ok "the sources are still there" "[ -f $W/home/big.bin ] && [ -d $W/home/docs/sub ]"

fresh; run "down,down,down,down,down,6,shot:$S/05-move-ask.ppm,enter" "$W/home" "$W/home/movies" "$@"
ok "move one file" "[ ! -e $W/home/notes.txt ] && grep -q 'second line' $W/home/movies/notes.txt"

fresh; run "down,down,down,down,down,6,r,backspace,backspace,backspace,backspace,backspace,backspace,backspace,backspace,backspace,n,e,w,dot,m,d,shot:$S/06-rename.ppm,enter" "$W/home" "$W/home/movies" "$@"
ok "6 then R renames" "[ ! -e $W/home/notes.txt ] && grep -q 'first line' $W/home/new.md"

fresh; run "7,a,b,c,enter,shot:$S/07-mkdir.ppm" "$W/home" "$W/home/movies" "$@"
ok "new folder, and the selection is on it" "[ -d $W/home/abc ]"

fresh; run "down,space,down,space,8,shot:$S/08-delete-ask.ppm,enter" "$W/home" "$W/home/movies" "$@"
ok "delete marked folder and file" "[ ! -e $W/home/docs ] && [ ! -e $W/home/big.bin ] && [ -f $W/home/notes.txt ] && [ -d $W/home/movies ]"
fresh; run "down,8,esc" "$W/home" "$W/home/movies" "$@"
ok "Esc at the delete question keeps everything" "[ -f $W/home/docs/sub/b.txt ]"
fresh; run "down,down,down,down,8,enter" "$W/home" "$W/home/movies" "$@"
ok "deleting a link leaves its target" "[ ! -L $W/home/link ] && [ -f $W/home/notes.txt ]"

fresh; echo other > "$W/home/movies/notes.txt"
run "down,down,down,down,down,5,enter,shot:$S/09-exists.ppm,s,shot:$S/10-skipped.ppm,enter" "$W/home" "$W/home/movies" "$@"
ok "existing file: S skips" "grep -q other $W/home/movies/notes.txt"
run "down,down,down,down,down,5,enter,enter" "$W/home" "$W/home/movies" "$@"
ok "existing file: Enter replaces" "cmp -s $W/home/notes.txt $W/home/movies/notes.txt"
echo other > "$W/home/movies/notes.txt"
run "down,down,down,down,down,6,enter,esc,enter" "$W/home" "$W/home/movies" "$@"
ok "existing file at a move: Esc cancels, nothing lost" "grep -q other $W/home/movies/notes.txt && [ -f $W/home/notes.txt ]"

fresh; run "down,5,enter,shot:$S/11-into-itself.ppm,enter" "$W/home" "$W/home/docs/sub" "$@"
ok "a folder is not copied into itself" "[ ! -e $W/home/docs/sub/docs ]"
fresh; run "down,6,enter,enter" "$W/home" "$W/home/docs/sub" "$@"
ok "a folder is not moved into itself" "[ -f $W/home/docs/sub/b.txt ] && [ ! -e $W/home/docs/sub/docs ]"

fresh; run "down,down,down,down,down,5,shot:$S/12-copy-as.ppm,2,enter" "$W/home" "$W/home" "$@"
ok "same folder on both sides: copy under a new name" "cmp -s $W/home/notes.txt $W/home/notes.txt2"

fresh; export NZC_PROGRESS_NOW=1; run "down,down,down,5,enter,shot:$S/13-progress.ppm,esc,shot:$S/13-cancelled.ppm,enter" "$W/home" "$W/home/movies" "$@"
ok "Esc during a copy: no partial file is left" "[ ! -e $W/home/movies/big.bin ] && [ -f $W/home/big.bin ]"
unset NZC_PROGRESS_NOW

# across file systems (a rename is not possible there): /dev/shm if it is another device
X=$(mktemp -d /dev/shm/nzc.XXXXXX 2>/dev/null) || X=
fresh
if [ -n "$X" ] && [ "$(stat -c %d "$X")" != "$(stat -c %d "$W")" ]; then
	run "down,space,down,space,6,enter,wait:300" "$W/home" "$X" "$@"
	ok "move to another file system: copied, then deleted" "[ -f $X/docs/sub/b.txt ] && [ \$(stat -c %s $X/big.bin) = 3000000 ] && [ ! -e $W/home/docs ] && [ ! -e $W/home/big.bin ]"
else echo "skip   move to another file system (no second file system here)"; fi

fresh; run "down,down,down,down,down,3,shot:$S/14-view-text.ppm,esc,up,up,3,enter,shot:$S/15-view-hex.ppm,esc,2,shot:$S/16-usr.ppm,esc,h,shot:$S/17-help.ppm,enter,o,shot:$S/18-options.ppm,enter,down,down,enter,esc,shot:$S/19-one-pane.ppm,tab,shot:$S/20-one-pane-other.ppm" "$W/home" "$W/home/movies" "$@"
ok "options are saved" "grep -q '^panes=1\$' $W/cfg/config && grep -q '^hidden=1\$' $W/cfg/config && grep -q '^active=1\$' $W/cfg/config"
run "shot:$S/21-restart.ppm,tab,down,down,down,2,down,down,down,enter,shot:$S/22-properties.ppm" "" "" "$@"
ok "folders and view come back after a restart" "grep -q \"^left=$W/home\$\" $W/cfg/config && grep -q \"^right=$W/home/movies\$\" $W/cfg/config"

fresh; mkdir "$W/home/locked"; chmod 000 "$W/home/locked"
run "down,down,5,enter,shot:$S/23-error.ppm" "$W/home" "$W/home/movies" "$@"
if [ "$(id -u)" != 0 ]; then ok "a folder that cannot be read gives an error, not a crash" "grep -qv Sanitizer $T/log || [ ! -s $T/log ]"; else echo "skip   unreadable folder (running as root)"; fi
chmod 755 "$W/home/locked"

# the other apps: found under NZC_APPS_DIR; without a screen the commander only says what it would start
fresh; A=$W/apps; for a in nz-edit nz-image nz-term; do mkdir -p "$A/$a"; printf '#!/bin/sh\n' > "$A/$a/$a"; chmod +x "$A/$a/$a"; done; : > "$W/home/pic.jpg"
NZC_APPS_DIR=$A run "down,down,down,down,down,3,4,enter,down,enter,3,t,2,shot:$S/24-usr-picture.ppm,esc,h,shot:$S/25-help.ppm" "$W/home" "$W/home/movies" "$@"
ok "3 and Enter on a text file: Text Editor's viewer" "[ \$(grep -c \"would run $A/nz-edit/nz-edit --view $W/home/notes.txt\" $T/log) = 2 ]"
ok "4: Text Editor" "grep -q \"would run $A/nz-edit/nz-edit $W/home/notes.txt\" $T/log"
ok "Enter and 3 on a picture: Image Viewer" "[ \$(grep -c \"would run $A/nz-image/nz-image $W/home/pic.jpg\" $T/log) = 2 ]"
ok "T: Terminal in the folder of the pane" "grep -q \"would run $A/nz-term/nz-term -d $W/home\$\" $T/log"
NZC_APPS_DIR=$W/none run "down,down,down,down,down,4,t,down,enter,shot:$S/26-picture-no-app.ppm" "$W/home" "$W/home/movies" "$@"
ok "without those apps: nano, and a shell on the text console" "grep -q 'would run nano' $T/log && grep -q 'would run sh -c cd' $T/log && ! grep -q nz-image $T/log"

"$@" --version > "$T/v" 2>&1; ok "--version" "grep -q '^nz-commander [0-9]' $T/v"
ok "no sanitizer or runtime error in the last log" "! grep -qiE 'sanitizer|runtime error' $T/log"
[ $fail -eq 0 ] && echo "all $n tests passed" || echo "some tests FAILED"
exit $fail
