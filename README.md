# p2pcall

Low-latency 1:1 video/audio calls and screen sharing (C++20 / Qt 6). Linux first, macOS later.

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

## Status
- [x] Phase 1–2: build, encrypted control channel (X25519 commit/reveal + ChaCha20-Poly1305, 6-digit SAS),
      LAN discovery, codec probe (VA-API + FFmpeg), per-direction codec negotiation, call state machine, QML UI,
      RTT / clock-offset estimation.
- [ ] Phase 3: audio · 4: camera video · 5: loss resilience · 6: congestion control · 7: screen share ·
      8: AV1 · 9: latency work · 10: macOS

## Layout
`src/net` (crypto, control channel, discovery) · `src/media` (codec caps/negotiation) ·
`src/video` (codec probe) · `src/app` (CallController, main) · `qml/` · `tests/`
