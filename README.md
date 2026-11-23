# p2pcall

Low-latency 1:1 video/audio calls and screen sharing (C++20 / Qt 6). Linux and macOS.

## Build & test
```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

## Run two instances on one machine
```sh
./build/dev/p2pcall --instance 1        # listens on 47101
./build/dev/p2pcall --instance 2        # listens on 47102; click the peer or call 127.0.0.1:47101
```
Headless/scripted: `p2pcall --headless --instance 2 --auto-accept --exit-after-call 3` and
`p2pcall --headless --instance 1 --connect 127.0.0.1:47102 --exit-after-call 3`
(prints `CALL_ESTABLISHED …` / `STATS …` / `CALL_ENDED …`). `--prefer-av1 camera|screen|both`.

## macOS
Dependencies: `brew install qt ffmpeg libsodium opus miniaudio ninja pkg-config` (Command Line
Tools are enough; no Xcode). The build makes a signed `build/<preset>/p2pcall.app`;
`build/<preset>/p2pcall` links to its executable, so the commands above work unchanged.

Permissions (camera, microphone, local network, Screen Recording) belong to whoever launches the app:
- From a terminal, macOS asks on behalf of the terminal app, once.
- As an app (`open -n build/release/p2pcall.app --args --instance 1`, or Finder), macOS asks on behalf
  of P2P Call. `open --stdout <log> --stderr <log>` keeps the log.

The app is signed ad hoc by default, so macOS knows it by that exact build: an app-launched
p2pcall should be asked again after each rebuild. To keep the permissions, sign with a certificate:
create one in Keychain Access → Certificate Assistant → Create a Certificate (identity type
Self-Signed Root, certificate type Code Signing), then
`cmake --preset release -DP2P_CODESIGN_IDENTITY="<certificate name>"`.

Screen sharing uses the system picker and needs no permission. `--screen-source display` (the main
display, no picker; for scripted runs) needs Screen Recording.

## Video and loss testing
`--video-source camera|pattern|none` (Linux: only one instance can hold the camera; use `pattern`
for the other; macOS: `--camera default|<name>`, access asked on first use), `--video-size 1920x1080 --video-fps 60 --video-bitrate 12000 --camera-format auto|mjpeg|nv12`.
Loss recovery: `--fec auto|off|<pct>`, `--no-nack`. Simulated loss: `--sim-loss <pct> --sim-burst <n>`.
Real delay/jitter needs root: `sudo tests/net/netem.sh on wan|bottleneck` … `sudo tests/net/netem.sh off`.
Congestion control is on by default (`--cc off` = fixed bitrate; `--video-bitrate` is then the maximum,
`--min-video-bitrate` the floor). Simulated bottleneck on outgoing media, no root needed:
`--sim-rate <kbps> --sim-delay <ms> --sim-queue <ms>`, plus `--sim-ce <ms>` for L4S-style CE marking.
Opus DRED needs a libopus built with it: `tools/build-opus-dred.sh`, then re-run `cmake --preset dev`;
`--audio-fec lbrr|dred` picks in-band FEC (one frame, full quality) or DRED (bursts, vocoder quality).

## Status
- [x] Phase 1–2: build, encrypted control channel (X25519 commit/reveal + ChaCha20-Poly1305, 6-digit SAS),
      LAN discovery, codec probe (VA-API + FFmpeg), per-direction codec negotiation, call state machine, QML UI,
      RTT / clock-offset estimation.
- [x] Phase 3: audio (miniaudio, Opus 10 ms + in-band FEC, adaptive jitter buffer).
- [x] Phase 4: camera video (V4L2, HEVC VA-API, fragment/assemble, QQuickRhiItem NV12 renderer).
- [~] Phase 5: NACK retransmission, adaptive Reed-Solomon FEC, keyframe fallback, burst-loss simulation, netem
      script, Opus DRED (`--audio-fec dred`, needs the local libopus build). Not yet: intra refresh / LTR
      (need a native VA-API encoder backend).
- [x] Phase 6: transport-wide feedback (arrival times + ECN), SCReAM-v2-style delay/L4S controller, pacing
      only when the link is the bottleneck, encoder bitrate/resolution ladder, FEC driven by the controller.
- [ ] Phase 7: screen share · 8: AV1 · 9: latency work · 10: macOS (done: VideoToolbox, camera, screen share, app bundle; see plan.md)

## Layout
`src/net` (crypto, control channel, discovery, media transport, FEC, `cc/` congestion control) · `src/media` (codec caps/negotiation) ·
`src/audio` (devices, Opus, jitter buffer) · `src/video` (capture, codecs, packetizer, streams) ·
`src/app` (CallController, MediaSession, VideoView, main) · `qml/` · `shaders/` · `tests/` · `tools/`
