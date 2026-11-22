# p2pcall — plan and status

1:1 peer-to-peer video/audio call + screen share in C++ / Qt 6 (Qt Quick UI), aiming for lower
latency and better quality than stock WebRTC. LAN first, macOS later. Codecs: HEVC (hardware,
default) and AV1 (software here, optional), negotiated per stream and direction. End-to-end
encrypted from day one.

## Ground rules (from the user)
- Stay in scope: implement what the plan / user asks; no unrequested features or tuning. If an
  extra turns out buggy, disable it rather than polish it.
- No sudo for the agent: ask the user to run `! sudo …` (package installs, `tests/net/netem.sh`).
- Commit only when asked.
- Testing is on **one machine**: two instances (`--instance 1`, `--instance 2`). Only one can
  hold the camera; give the other `--video-source pattern`.
- Browser automation (if ever needed): use the local Linux Chrome, never the macOS one.

## Dev machine
Debian 13, kernel 6.12, 12 cores, AMD RX 5700 XT (Navi 10, Mesa radeonsi). VA-API: HEVC
Main/Main10 encode + decode, JPEG decode; **no AV1 hardware** (AV1 = SVT-AV1 encode, dav1d
decode). Qt 6.8.2, FFmpeg 7.1, libsodium, miniaudio, webrtc-audio-processing-1. Camera:
Logitech StreamCam `/dev/video0` (60 fps only as MJPEG; NV12 ≤ 30 fps).

## Build / run / test
```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev   # 8 suites
tools/build-opus-dred.sh        # optional: libopus 1.5.2 with DRED into build/third_party/opus
                                # (CMake picks it up; clear cache with -U 'OPUS*' -U '__pkg_config_checked_OPUS')
./build/dev/p2pcall --instance 1 --video-source camera
./build/dev/p2pcall --instance 2 --video-source pattern
```
Headless pair: `--headless --no-discovery --auto-accept --exit-after-call N` on one side,
`--connect 127.0.0.1:4710N` on the other; `--audio-io null --audio-source tone|silence`.
Impairment: `--sim-loss <pct> --sim-burst <n>` (in-app), `sudo tests/net/netem.sh on lan|wifi|wan|lossy`
/ `off` (real delay/jitter on loopback; RTT = 2× delay).
Other flags: `--fec auto|off|<pct>`, `--no-nack`, `--audio-fec lbrr|dred`, `--video-size`,
`--video-fps`, `--video-bitrate`, `--camera-format auto|mjpeg|nv12`, `--prefer-av1 camera|screen|both`,
`--aec/--ns/--agc` (all off by default — buggy, deliberately parked).

## Architecture (as built)
- `src/net`: `crypto` (X25519 commit/reveal handshake, 6-digit SAS, ChaCha20-Poly1305, subkeys),
  `control_channel` (TCP, length-prefixed encrypted JSON, ping/pong RTT + clock offset with an
  initial 5-ping burst), `discovery` (multicast beacons 239.255.77.77:47999),
  `media_transport` (UDP, 12-byte header = AEAD AD, packet number = nonce, replay window,
  per-stream media + feedback handlers, rx loss accounting, Gilbert-Elliott sim loss),
  `fec` (GF(256) Cauchy Reed-Solomon erasure code).
- `src/audio`: miniaudio device I/O, Opus 10 ms VOIP 48 kbps + LBRR (or DRED), adaptive jitter
  buffer (p95 jitter target, DRED→FEC→PLC concealment), drift resampler, optional APM.
- `src/video`: V4L2 camera (MJPEG→NV12 via swscale, BT.601→709), test pattern with a 32-bit frame
  code readable after decode, VA-API/x265/SVT-AV1 encoder (CBR, VBV ≈ 1 frame, P-only, IDR on
  demand), VA-API/dav1d decoder, packetizer + `FrameAssembler`, `video_stream` (send/receive).
- `src/app`: `CallController` (state machine, caps/invite/accept), `MediaSession`, `VideoView`
  (`QQuickRhiItem`, NV12 shader in `shaders/`), `main.cpp` CLI. UI in `qml/Main.qml`.

Video fragment header (24 B, inside the encrypted payload): frame id, index, data-fragment count,
capture µs, flags (key / test-pattern / retransmit), codec, display w/h (Mesa pads 1080→1088 with
no conformance window, so the receiver crops), FEC parity-per-block, FEC block size. Video
feedback kinds: PLI, NACK (frame id + index, 0xffff = whole frame), loss report (basis points).

## Status
- [x] **1 Skeleton**, **2 Signalling** — discovery, encrypted control channel, negotiation, UI.
- [x] **3 Audio** — capture→playout hand-off ~25–35 ms on real devices.
- [x] **4 Camera video** — 1080p60 HEVC VA-API; pattern capture→decoded 12–13 ms. Camera path
      ≈ 52 ms of which ~26 ms camera/USB and ~13.5 ms CPU MJPEG decode.
- [~] **5 Loss resilience** — done: NACK (sender keeps 1 s history; receiver retries ×3 spaced by
      RTT; grace window adapts to RTT, 20–300 ms), adaptive RS FEC from receiver loss reports,
      PLI only when unrecoverable (also delayed at call start), burst-loss sim, netem script, Opus
      DRED. Results (720p30, loopback): 5% random loss → 0 frames lost with NACK; FEC-only 3% →
      0 lost; 5% bursts with defaults → 0–1 lost.
      **Open:** (a) intra refresh + LTR recovery need a native VA-API HEVC encoder (FFmpeg's
      hevc_vaapi exposes neither) — proposed to defer to phase 9; (b) user to pick the audio
      default after a listening test: LBRR (default) vs DRED (`--audio-fec dred`; at 48 kbps /
      10 ms LBRR starves DRED's budget; 64 kbps allows both with ~30 ms DRED); (c) validate NACK/FEC
      under real delay with `netem.sh on wan` (needs sudo).
- [ ] **6 Congestion control** — **next.** Transport-wide feedback (per-packet arrival times /
      ECN), SCReAM-v2-style delay + L4S/ECN controller, encoder bitrate/resolution ladder, pacing
      only when the link is the bottleneck (burst on LAN). Video bitrate is currently fixed
      (12 Mbps default). Replace the video-only loss report with transport-wide feedback; feed the
      FEC policy from it.
- [ ] **7 Screen share** — PipeWire + xdg-desktop-portal capture (DMA-BUF), second stream
      (`kScreen`), "text" mode (full res, 5–30 fps, sharp) vs "motion" (60 fps), viewer UI.
      Negotiation already picks a codec per stream for screen.
- [ ] **8 AV1 + negotiation matrix** — SVT-AV1 RTC preset with screen-content tools, dav1d,
      hardware AV1 where present; exercise every encoder/decoder pairing end to end.
- [ ] **9 Latency work** — sub-frame slice send, zero-copy (V4L2/DMA-BUF→VA-API, VA-API→RHI),
      VA-API JPEG decode for the camera, low-latency present, native VA-API encoder (intra
      refresh, LTR) if not done earlier, Tambur-style streaming codes, glass-to-glass latency probe
      (`tools/latency_probe`) with per-stage breakdown and a WebRTC A/B.
- [ ] **10 macOS** — AVFoundation camera, ScreenCaptureKit, VideoToolbox (low-latency RC, HEVC),
      CoreAudio via miniaudio, codesign + camera/mic/screen entitlements. Platform splits exist
      (`camera_none.cpp`, `codec_probe_generic.cpp`, `audio_device_none.cpp`).

Also outstanding: active A/V sync (latencies are reported only).

## Gotchas learned
- `qt6-base-private-dev` is required for `rhi/qrhi.h`; link `Qt6::GuiPrivate` (in 6.8 it comes
  with Gui; 6.9+ needs the component).
- Mesa VA-API HEVC output is 1088 lines for 1080 input with no crop info — send display size.
- Debian's libopus lacks DRED; the local static build interposes over the copy libavcodec loads.
- Tests must never open the real camera (they use the pattern source at 1280×720@30).
- Someone else may edit files concurrently: re-read before editing and keep their changes.
