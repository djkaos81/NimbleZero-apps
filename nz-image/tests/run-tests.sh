#!/bin/sh
# Scripted tests for nz-image: no screen and no keyboard needed.  Needs python3 with Pillow
# to make the test pictures and to look at the screenshots.
#   tests/run-tests.sh $PWD/nz-image-sim
#   tests/run-tests.sh qemu-aarch64-static -L /usr/aarch64-linux-gnu $PWD/nz-image
#   SHOTS=dir tests/run-tests.sh ...     keep the screenshots
set -u
[ $# -ge 1 ] || { sed -n '2,6p' "$0"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
S=${SHOTS:-$T/shots}; mkdir -p "$S" "$T/cfg" "$T/p"
export NZI_HEADLESS=1 SDL_VIDEODRIVER=dummy NZK_BATTERY=64+ NZK_CLOCK=15:40 NZI_CONFIG_DIR="$T/cfg" HOME="$T"
fail=0; n=0
python3 - "$T/p" <<'PY' || { echo "python3 with Pillow is needed"; exit 2; }
import sys
from PIL import Image
d = sys.argv[1]
# a: left half red, right half blue, 1600x1200.  b: the same stored lying on its side, with the Exif note to turn it
im = Image.new('RGB', (1600, 1200), (255, 0, 0)); im.paste((0, 0, 255), (800, 0, 1600, 1200))
im.save(d + '/a.jpg', quality=95)
ex = Image.Exif(); ex[0x0112] = 6
im.rotate(90, expand=True).save(d + '/b.jpg', quality=95, exif=ex)
Image.new('RGB', (10, 10), (0, 255, 0)).save(d + '/c.png')
Image.new('RGBA', (64, 64), (0, 0, 0, 0)).save(d + '/d.png')
Image.new('RGB', (40, 20), (255, 255, 0)).save(d + '/e.bmp')
Image.new('RGB', (40, 20), (0, 255, 255)).save(d + '/f.gif')
Image.new('RGB', (40, 20), (255, 0, 255)).save(d + '/g.ppm')
open(d + '/h.png', 'wb').write(b'\x89PNG\r\n\x1a\nbroken')
PY
run() { keys=$1; shift; NZI_KEYS="$keys" timeout 120 "$@" 2>"$T/log"; }
ok() { n=$((n + 1)); if eval "$2"; then echo "ok   $n $1"; else echo "FAIL $n $1"; fail=1; sed 's/^/     | /' "$T/log" | tail -5; fi; }
# px FILE X Y -> "r g b" of that screen pixel, rounded to 0 / 128 / 255
px() { python3 -c "
import sys
from PIL import Image
r=Image.open(sys.argv[1]).convert('RGB').getpixel((int(sys.argv[2]),int(sys.argv[3])))
print(' '.join(str(0 if v<70 else 255 if v>185 else 128) for v in r))" "$1" "$2" "$3"; }

run "shot:$S/a.ppm,space,shot:$S/a-full.ppm,space,c,shot:$S/b.ppm,t,shot:$S/b-turned.ppm,c,shot:$S/c.ppm,enter,enter,enter,shot:$S/c-zoom.ppm,esc,c,shot:$S/d.ppm,c,shot:$S/e.ppm,c,shot:$S/f.ppm,c,shot:$S/g.ppm,c,shot:$S/h.ppm,i,enter,h,shot:$S/keys.ppm" "$@" "$T/p/a.jpg"
ok "JPEG: whole picture, centred, left half red and right half blue" "[ \"\$(px $S/a.ppm 100 85)\" = '255 0 0' ] && [ \"\$(px $S/a.ppm 220 85)\" = '0 0 255' ] && [ \"\$(px $S/a.ppm 20 85)\" = '0 0 0' ]"
ok "Space: picture only, using the whole screen" "[ \"\$(px $S/a-full.ppm 100 4)\" = '255 0 0' ] && [ \"\$(px $S/a-full.ppm 220 166)\" = '0 0 255' ]"
ok "a camera picture stored on its side is turned upright (Exif)" "[ \"\$(px $S/b.ppm 100 85)\" = '255 0 0' ] && [ \"\$(px $S/b.ppm 220 85)\" = '0 0 255' ]"
ok "T turns it a quarter to the right: red on top" "[ \"\$(px $S/b-turned.ppm 160 40)\" = '255 0 0' ] && [ \"\$(px $S/b-turned.ppm 160 130)\" = '0 0 255' ]"
ok "a small PNG is shown in its own size, not blown up" "[ \"\$(px $S/c.ppm 160 85)\" = '0 255 0' ] && [ \"\$(px $S/c.ppm 150 85)\" = '0 0 0' ]"
ok "Enter three times: 800 %" "[ \"\$(px $S/c-zoom.ppm 125 60)\" = '0 255 0' ] && [ \"\$(px $S/c-zoom.ppm 195 110)\" = '0 255 0' ]"
ok "see-through PNG: grey squares, not black" "python3 -c \"from PIL import Image; import sys; sys.exit(0 if Image.open('$S/d.ppm').getpixel((160,85))[0] > 30 else 1)\""
ok "BMP" "[ \"\$(px $S/e.ppm 160 85)\" = '255 255 0' ]"
ok "GIF" "[ \"\$(px $S/f.ppm 160 85)\" = '0 255 255' ]"
ok "PPM" "[ \"\$(px $S/g.ppm 160 85)\" = '255 0 255' ]"
ok "a damaged file gives a message, and the app goes on" "[ -s $S/keys.ppm ]"
run "z,x,space" "$@" "$T/p/c.png"; ok "the bars setting is saved" "grep -q '^bars=0\$' $T/cfg/config"
(cd "$T/p" && NZI_KEYS="shot:$S/chooser.ppm,down,enter,shot:$S/chosen.ppm" timeout 60 "$@" 2>"$T/log")
ok "started without a file: chooser, then the picture" "[ \"\$(px $S/chosen.ppm 100 85)\" = '255 0 0' ]"
"$@" --version > "$T/v" 2>&1; ok "--version" "grep -q '^nz-image [0-9]' $T/v"
ok "no sanitizer or runtime error in the last log" "! grep -qiE 'sanitizer|runtime error' $T/log"
[ $fail -eq 0 ] && echo "all $n tests passed" || echo "some tests FAILED"
exit $fail
