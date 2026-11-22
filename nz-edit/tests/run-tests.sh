#!/bin/sh
# Scripted tests for nz-edit: no screen and no keyboard needed.
#   tests/run-tests.sh $PWD/nz-edit-sim          (an absolute path)
#   tests/run-tests.sh qemu-aarch64-static -L /usr/aarch64-linux-gnu ./nz-edit     (the device program)
#   SHOTS=dir tests/run-tests.sh ...     keep the screenshots
set -u
[ $# -ge 1 ] || { sed -n '2,5p' "$0"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
S=${SHOTS:-$T/shots}; mkdir -p "$S" "$T/cfg" "$T/run"
export NZE_HEADLESS=1 SDL_VIDEODRIVER=dummy NZK_BATTERY=64+ NZK_CLOCK=15:40 NZE_CONFIG_DIR="$T/cfg" XDG_RUNTIME_DIR="$T/run" HOME="$T"
fail=0; n=0; F=$T/f.txt
run() { keys=$1; shift; NZE_KEYS="$keys" timeout 60 "$@" 2>"$T/log"; }
ok() { n=$((n + 1)); if eval "$2"; then echo "ok   $n $1"; else echo "FAIL $n $1"; fail=1; sed 's/^/     | /' "$T/log" | tail -5; od -c "$F" | head -5 | sed 's/^/     > /'; fi; }
is() { printf "$1" | cmp -s - "$F"; }
new() { printf "$1" > "$F"; }

new 'one\ntwo\n'; run "end,enter,text:new line,C-s" "$@" "$F"
ok "type a line and save" "is 'one\nnew line\ntwo\n'"
new 'one\ntwo\n'; run "text:x" "$@" "$F"
ok "leaving without saving changes nothing" "is 'one\ntwo\n'"
new 'one\ntwo\n'; run "text:x,C-q,shot:$S/01-ask.ppm,enter" "$@" "$F"
ok "Ctrl+Q, Enter saves" "is 'xone\ntwo\n'"
new 'one\ntwo\n'; run "text:x,C-q,d" "$@" "$F"
ok "Ctrl+Q, D does not" "is 'one\ntwo\n'"
new 'one\ntwo'; run "down,end,text:!,C-s" "$@" "$F"
ok "a file without a final line feed stays so" "is 'one\ntwo!'"
new 'one\r\ntwo\r\n'; run "end,enter,text:x,C-s" "$@" "$F"
ok "DOS line ends are kept" "is 'one\r\nx\r\ntwo\r\n'"
new '\357\273\277caf\303\251\n'; run "end,backspace,text:e,C-s" "$@" "$F"
ok "byte order mark kept, a two-byte character deleted whole" "is '\357\273\277cafe\n'"
new 'abc\ndef\n'; run "end,delete,C-s" "$@" "$F"
ok "Delete at a line end joins the lines" "is 'abcdef\n'"
new 'abc\ndef\n'; run "down,backspace,C-s" "$@" "$F"
ok "Backspace at a line start joins the lines" "is 'abcdef\n'"
new 'abc\n'; run "text:12,enter,text:3,C-z,C-z,C-s" "$@" "$F"
ok "undo: the typed character, then the line break" "is '12abc\n'"
new 'abc\n'; run "text:12,C-z,C-y,C-s" "$@" "$F"
ok "redo" "is '12abc\n'"
new 'abc\n'; run "right,backspace,delete,C-z,C-z,C-s" "$@" "$F"
ok "undo of backspace and delete" "is 'abc\n'"
new 'one\ntwo\nthree\n'; run "down,C-x,up,C-v,C-s" "$@" "$F"
ok "cut a line, paste it elsewhere" "is 'two\none\nthree\n'"
ok "the cut text is in the shared clipboard" "printf 'two\n' | cmp -s - $T/run/nz-clipboard"
new 'one two three\n'; run "right,right,right,right,C-b,right,right,right,right,shot:$S/02-selection.ppm,C-c,end,C-v,C-s" "$@" "$F"
ok "select with Ctrl+B, copy, paste" "is 'one two threetwo \n'"
new 'one two three\n'; run "right,right,right,right,C-b,right,right,right,right,backspace,C-s" "$@" "$F"
ok "Backspace removes the selection" "is 'one three\n'"
new 'one\ntwo\n'; run "C-a,C-x,text:z,C-s" "$@" "$F"
ok "select all, cut" "is 'z\n'"
new 'alpha\nbeta\ngamma beta\n'; run "C-f,text:BETA,enter,text:1,C-g,text:2,C-s" "$@" "$F"
ok "find (any case) puts the cursor behind the match, find next goes on" "is 'alpha\nbeta1\ngamma beta2\n'"
new 'a\nb\nc\nd\n'; run "C-l,text:3,enter,text:x,C-s" "$@" "$F"
ok "go to line" "is 'a\nb\nxc\nd\n'"
new '\tindented\n'; run "end,enter,text:x,C-s" "$@" "$F"
ok "a new line keeps the indent" "is '\tindented\n\tx\n'"
new 'x\n'; run "tab,C-s" "$@" "$F"
ok "Tab types a tab" "is '\tx\n'"
python3 -c "print('word ' * 30)" > "$F" 2>/dev/null || awk 'BEGIN{for(i=0;i<30;i++)printf "word ";print ""}' > "$F"
run "down,down,text:X,shot:$S/03-folded.ppm,C-s" "$@" "$F"
ok "Down moves by screen rows in a folded line (39 columns)" "[ \$(cut -c79 $F) = X ]"
new 'one\n'; run "esc,shot:$S/04-menu.ppm,enter" "$@" "$F"; run "text:q,esc,enter" "$@" "$F"
ok "menu: Save" "is 'qone\n'"
rm -f "$T/n.txt"; run "text:fresh,C-s" "$@" "$T/n.txt"
ok "a file that does not exist is made on saving" "printf 'fresh\n' | cmp -s - $T/n.txt"
new 'keep\n'; chmod 640 "$F"; run "text:x,C-s" "$@" "$F"
ok "saving keeps the file's rights" "[ \$(stat -c %a $F) = 640 ]"
new 'target\n'; ln -sf f.txt "$T/link.txt"; run "text:x,C-s" "$@" "$T/link.txt"
ok "saving through a link writes the target and keeps the link" "[ -L $T/link.txt ] && is 'xtarget\n'"
new 'one\ntwo\n'; run "shot:$S/05-view.ppm,text:zzz,e,text:Z,C-s" "$@" --view "$F"
ok "--view: letters do not type; E changes to editing" "is 'Zone\ntwo\n'"
head -c 2000 /dev/urandom > "$T/bin"; printf '\0' >> "$T/bin"; cp "$T/bin" "$T/bin2"; run "shot:$S/06-hex.ppm,e,text:x,C-s" "$@" "$T/bin"
ok "a file that is not text is shown as bytes and cannot be changed" "cmp -s $T/bin $T/bin2"
new 'a\n'; run "+2,shot:$S/07-keys.ppm" "$@" --view "$F"; run "C-h,shot:$S/08-keys-edit.ppm,enter,C-o,shot:$S/09-options.ppm,down,enter,esc,shot:$S/10-small.ppm" "$@" "$F"
ok "options are saved" "grep -q '^small=1\$' $T/cfg/config"
new 'a\n'; (NZE_KEYS="text:x,wait:3000" "$@" "$F" 2>"$T/log" & p=$!; sleep 1; pkill -TERM -f "f.txt" ; wait $p) 2>/dev/null
ok "told to stop with changes: they are kept in FILE.unsaved, the file is untouched" "is 'a\n' && printf 'xa\n' | cmp -s - $F.unsaved"
(cd "$T" && NZE_KEYS="shot:$S/11-chooser.ppm,n,text:made.txt,enter,text:hi,C-s" timeout 60 "$@" 2>"$T/log")
ok "started without a file: chooser, N makes a new file there" "printf 'hi\n' | cmp -s - $T/made.txt"
"$@" --version > "$T/v" 2>&1; ok "--version" "grep -q '^nz-edit [0-9]' $T/v"
ok "no sanitizer or runtime error in the last log" "! grep -qiE 'sanitizer|runtime error' $T/log"
[ $fail -eq 0 ] && echo "all $n tests passed" || echo "some tests FAILED"
exit $fail
