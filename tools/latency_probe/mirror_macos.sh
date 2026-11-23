#!/bin/zsh
# Glass-to-glass latency on a MacBook with its built-in camera, through a mirror.
#
#   tools/latency_probe/mirror_macos.sh [seconds] [build dir]   (default 60, build/release)
#
# Two p2pcall instances call each other: instance 1 sends the camera (its window shows the self
# view), instance 2 sends nothing and shows instance 1's camera. The probe window flashes in the
# top middle of the screen; hold a mirror in front of the screen so the camera sees the probe window
# in it, using instance 1's self view to aim. Keep the two p2pcall windows (bottom corners) out of
# the mirror's view: if the camera also sees the video windows, their delayed flashes come back
# in the video as echoes and can be measured instead of the real flash.
#
# Then press Enter. The probe asks what to capture: pick the whole display in the system picker.
# Results: one "video window" per call window that follows the flash -- instance 2's remote view
# (the call: camera -> encode -> network -> decode -> screen) and instance 1's self view (camera ->
# screen, no call) -- with the per-flash numbers in <build dir>/latency-mirror.csv.
set -u
secs=${1:-60}
B=${2:-build/release}
cd "${0:A:h}/../.."
[[ -x $B/p2pcall && -x $B/latency_probe ]] || { echo "build first: cmake --build --preset release"; exit 1; }
L=$B/latency-mirror
pa= pb= pf=
mkdir -p $L

# Points on a 1470x956 screen (MacBook Air 13"); other screens: adjust.
# The call windows need room: the side panel (300) and log (140) leave the rest to the video.
probe_geom=560x300+455+40     # top middle, above the call windows
# --on-top: other windows (the terminal) must not cover the call's video.
own=(--no-discovery --on-top --audio-io null --audio-source tone --exit-after-call $((secs + 600)))

$B/p2pcall $own --instance 2 --auto-accept --video-source none -qwindowgeometry 760x500+0+358 > $L/b.log 2>&1 &
pb=$!
sleep 1.5
$B/p2pcall $own --instance 1 --connect 127.0.0.1:47102 --video-source camera --video-size 1280x720 \
    -qwindowgeometry 700x480+770+378 > $L/a.log 2>&1 &
pa=$!
trap 'kill $pa $pb $pf 2>/dev/null' EXIT

# Flash while aiming (same window place as the measurement).
$B/latency_probe --flash-only --seconds 3600 -qwindowgeometry $probe_geom > /dev/null 2>&1 &
pf=$!
echo "Aim the mirror: instance 1's self view (bottom right) should show the flashing window, and"
echo "the camera shouldn't see the two p2pcall windows. Press Enter to measure for ${secs} s."
read -r _
kill $pf 2>/dev/null; wait $pf 2>/dev/null

$B/latency_probe --seconds $secs --csv $B/latency-mirror.csv --dump $L/probe.dump -qwindowgeometry $probe_geom
echo
grep -hE '^video (tx|rx)' $L/a.log $L/b.log | tail -2
