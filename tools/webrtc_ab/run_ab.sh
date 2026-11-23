#!/bin/bash
# Same-sitting glass-to-glass A/B: p2pcall flash source vs Chrome WebRTC loopback.
# run_ab.sh <outdir> [secs] [control]   (needs build/dev/p2pcall + latency_probe, google-chrome;
# keep the screen uncovered: windows flash black/white for ~5 min)
R=$(cd "$(dirname "$0")/../.." && pwd)
W=$(cd "$(dirname "$0")" && pwd)
O=$1; S=${2:-45}; mkdir -p "$O"; O=$(cd "$O" && pwd)
p2p() { # tag size
  local L=$O/$1; rm -f $L.flash
  setsid $R/build/dev/p2pcall --instance 2 --video-source none --auto-accept > $L.b.log 2>&1 &
  sleep 1
  setsid $R/build/dev/p2pcall --headless --instance 1 --video-source flash --video-size $2 --flash-log $L.flash --connect 127.0.0.1:47102 > $L.a.log 2>&1 &
  sleep 6
  $R/build/dev/latency_probe --seconds $S --source-log $L.flash > $L.probe 2>&1
  pkill -x p2pcall; sleep 2
}
web() { # tag codec
  local L=$O/$1; mkdir -p $L.d; rm -f $L.d/*
  python3 -I $W/server.py 47611 $L.d > /dev/null 2>&1 & local SRV=$!
  sleep 0.5
  setsid google-chrome --user-data-dir=$O/chrome-profile --no-first-run --no-default-browser-check \
    --autoplay-policy=no-user-gesture-required --window-size=1340,820 \
    --app="http://127.0.0.1:47611/page.html?${2}" > $L.chrome.log 2>&1 & local CH=$!
  sleep 8
  $R/build/dev/latency_probe --seconds $S --source-log $L.d/flash.txt > $L.probe 2>&1
  kill -TERM -- -$CH 2>/dev/null; sleep 2; kill $SRV 2>/dev/null; sleep 1
}
case "$3" in
control)
  web web-local "mode=local"
  p2p p2p-1080 1920x1080
  web web-vp8 ""
  web web-local2 "mode=local"
  ;;
*)
  p2p p2p-1080 1920x1080
  web web-vp8 ""
  p2p p2p-720 1280x720
  web web-h264 "codec=H264"
  web web-av1 "codec=AV1"
  ;;
esac
echo done
