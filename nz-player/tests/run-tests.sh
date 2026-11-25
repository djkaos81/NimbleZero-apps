#!/bin/sh
# Scripted tests for nz-player: no screen, no keyboard, no sound device needed.
#
#   tests/run-tests.sh ./nz-player-sim
#   tests/run-tests.sh qemu-aarch64-static -L /path/to/rootfs ./nz-player     (the device binary)
#
# Needs the ffmpeg command (to make the test clips).  Each test drives the program with
# NZP_KEYS and checks screenshots, saved files and the log.
set -u
[ $# -ge 1 ] || { sed -n '2,8p' "$0"; exit 2; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found"; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/home/sub" "$W/cfg"
FF="ffmpeg -hide_banner -loglevel error -y"
$FF -f lavfi -i testsrc2=s=640x360:r=25 -f lavfi -i sine=f=440:r=48000 -t 90 -c:v mpeg4 -q:v 5 -g 25 -c:a mp2 "$W/home/a-movie.mkv" || exit 2
$FF -f lavfi -i testsrc=s=320x240:r=25 -t 4 -c:v mpeg4 -q:v 5 "$W/home/b-silent.avi" || exit 2
$FF -f lavfi -i sine=f=440:r=44100 -t 5 -c:a mp2 "$W/home/c-sound.mp2" || exit 2
printf '#EXTM3U\n#EXTINF:-1 group-title="One",First, with comma\na-movie.mkv\n#EXTINF:-1 tvg-name="Second" group-title="Two",\nb-silent.avi\n#EXTINF:-1 group-title="Two",Gone\nmissing.ts\n' > "$W/home/list.m3u"
export NZP_HEADLESS=1 NZP_AUDIO=none NZP_CONFIG_DIR="$W/cfg" HOME="$W/home"
fail=0; n=0
run() { NZP_KEYS="$1" timeout 60 "$@" 2>"$W/log"; }
ok() { n=$((n + 1)); if eval "$2"; then echo "ok   $n $1"; else echo "FAIL $n $1"; fail=1; sed 's/^/     | /' "$W/log" | tail -5; fi; }
# number of different colours in a screenshot (a picture has many, a list or an empty screen few)
colours() { ffmpeg -loglevel error -i "$1" -f rawvideo -pix_fmt rgb24 - 2>/dev/null | od -An -v -tx1 -w3 | sort -u | wc -l; }

K="shot:$W/1.ppm,down,enter,wait:2500,shot:$W/2.ppm,right,wait:2000,space,wait:300,shot:$W/3.ppm,esc,wait:100"
NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
ok "selector, play, seek, pause, back: clean exit" "[ $rc -eq 0 ]"
ok "selector screenshot is a list" "[ $(colours "$W/1.ppm") -lt 40 ]"
ok "playing screenshot is a picture" "[ $(colours "$W/2.ppm") -gt 200 ]"
ok "position saved for resume (between 10 and 20 s)" "awk -F'\t' '\$1 > 10 && \$1 < 20 && /a-movie.mkv/ { f = 1 } END { exit !f }' '$W/cfg/resume'"
ok "last folder saved" "grep -q '^dir_movies=$W/home\$' '$W/cfg/config'"

K="down,down,enter,wait:6000,shot:$W/4.ppm,wait:100"
NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
ok "silent clip plays to its end and returns to the selector" "[ $rc -eq 0 ] && [ $(colours "$W/4.ppm") -lt 40 ]"

K="down,down,down,enter,wait:1500,up,up,wait:300,esc,o,down,down,down,right,esc,wait:100"
NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
ok "sound-only file; volume and options are saved" "[ $rc -eq 0 ] && grep -q '^volume=90\$' '$W/cfg/config' && grep -q '^scaler=0\$' '$W/cfg/config'"

# IPTV mode: list.m3u -> groups (All, One, Two) -> Two -> "Second" plays; then "Gone" gives an error screen
K="tab,down,enter,down,down,enter,shot:$W/5.ppm,enter,wait:2000,shot:$W/6.ppm,esc,down,enter,wait:1500,shot:$W/7.ppm,enter,esc,esc,wait:100"
NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
ok "channel list: groups, play an entry, error for a missing one" "[ $rc -eq 0 ] && [ $(colours "$W/6.ppm") -gt 100 ] && [ $(colours "$W/7.ppm") -lt 40 ] && grep -q 'Cannot open' '$W/log'"

# The GPU code path of the player, with the stand-in decoder of tests/hwdec_fake.c ("make fake"):
#   NZP_TEST_FAKE=1 tests/run-tests.sh ./nz-player-fake
# subtitles: a file next to the video (Windows-1252, to test the conversion) and a track inside it
mkdir -p "$W/subs"
printf '1\n00:00:00,500 --> 00:00:20,000\nSubtitle from the file caf\xe9\n' > "$W/subs/s.srt"
printf '1\n00:00:00,500 --> 00:00:20,000\nTrack inside\n' > "$W/in.srt"
if $FF -f lavfi -i testsrc2=s=640x360:r=25 -f lavfi -i sine=f=440:r=48000 -i "$W/in.srt" -t 20 -c:v mpeg4 -q:v 5 -g 25 -c:a mp2 -c:s srt "$W/subs/s.mkv" 2>/dev/null; then
	region() { ffmpeg -loglevel error -i "$1" -vf crop=320:60:0:80 -f rawvideo -pix_fmt rgb24 - 2>/dev/null | cksum; }
	# the file next to the video; chosen while paused (a subtitle file is held complete)
	K="enter,wait:2500,space,wait:2500,shot:$W/s0.ppm,s,wait:300,s,wait:2500,shot:$W/s2.ppm,s,wait:2500,shot:$W/s3.ppm,esc,wait:100"
	mkdir -p "$W/cfg1"
	HOME="$W/subs" NZP_CONFIG_DIR="$W/cfg1" NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
	if grep -q 'text subtitles are not available' "$W/log"; then echo "skip   subtitles (no libass or font on this machine)"
	else
		ok "subtitles from a file next to the video, and off again" "[ $rc -eq 0 ] && [ \"\$(region $W/s2.ppm)\" != \"\$(region $W/s3.ppm)\" ] && [ \"\$(region $W/s0.ppm)\" != \"\$(region $W/s2.ppm)\" ]"
		# the track inside the file, on from the start (option "Subtitles at start")
		mkdir -p "$W/subs2" "$W/cfg2"; cp "$W/subs/s.mkv" "$W/subs2/"; echo "subs=1" > "$W/cfg2/config"
		K="enter,wait:2500,space,wait:2500,shot:$W/s4.ppm,s,wait:2500,shot:$W/s5.ppm,esc,wait:100"
		HOME="$W/subs2" NZP_CONFIG_DIR="$W/cfg2" NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
		ok "subtitle track inside the file, on at start" "[ $rc -eq 0 ] && [ \"\$(region $W/s4.ppm)\" != \"\$(region $W/s5.ppm)\" ]"
	fi
fi

if [ -n "${NZP_TEST_FAKE:-}" ] && $FF -f lavfi -i testsrc2=s=640x360:r=25 -f lavfi -i sine=f=440:r=48000 -t 30 -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -c:a mp2 "$W/home/sub/h.mkv" 2>/dev/null; then
	K="enter,enter,wait:2000,shot:$W/8.ppm,right,wait:1500,space,wait:300,space,a,wait:500,9,wait:5000,shot:$W/9.ppm,wait:100"
	NZP_FAKE_HW=1 NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
	ok "GPU path (stand-in): play, seek, pause, shape, end of file" "[ $rc -eq 0 ] && grep -q 'gpu: decoding' '$W/log' && [ $(colours "$W/8.ppm") -gt 200 ] && [ $(colours "$W/9.ppm") -lt 40 ]"
	K="enter,enter,wait:4000,shot:$W/10.ppm,esc,wait:100"
	NZP_FAKE_HW=1 NZP_FAKE_FAIL_AFTER=40 NZP_KEYS="$K" timeout 60 "$@" 2>"$W/log"; rc=$?
	ok "GPU failure after 40 pictures: playback goes on in software" "[ $rc -eq 0 ] && grep -q 'continuing with the software decoder' '$W/log' && [ $(colours "$W/10.ppm") -gt 200 ]"
fi

"$@" --version >"$W/v" 2>&1; ok "--version" "grep -q '^nz-player [0-9]' '$W/v'"
[ $fail -eq 0 ] && echo "all $n tests passed" || echo "some tests FAILED"
exit $fail
