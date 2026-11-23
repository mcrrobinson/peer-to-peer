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
cmake --preset dev && cmake --build --preset dev && ctest --preset dev   # 9 suites
tools/build-opus-dred.sh        # optional: libopus 1.5.2 with DRED into build/third_party/opus
                                # (CMake picks it up; clear cache with -U 'OPUS*' -U '__pkg_config_checked_OPUS')
./build/dev/p2pcall --instance 1 --video-source camera
./build/dev/p2pcall --instance 2 --video-source pattern
```
Headless pair: `--headless --no-discovery --auto-accept --exit-after-call N` on one side,
`--connect 127.0.0.1:4710N` on the other; `--audio-io null --audio-source tone|silence`.
Impairment: `--sim-loss <pct> --sim-burst <n>` (in-app), `--sim-rate <kbps> --sim-delay <ms> --sim-queue <ms>
--sim-ce <ms>` (in-app bottleneck on outgoing media, optional L4S CE marking),
`sudo tests/net/netem.sh on lan|wifi|wan|lossy|bottleneck` / `off` (real delay/jitter on loopback; RTT = 2× delay).
Other flags: `--cc on|off`, `--min-video-bitrate`, `--fec auto|off|<pct>`, `--no-nack`, `--audio-fec lbrr|dred`, `--video-size`,
`--video-fps`, `--video-bitrate`, `--camera-format auto|mjpeg|nv12`, `--prefer-av1 camera|screen|both`,
`--aec/--ns/--agc` (all off by default — buggy, deliberately parked).

## Architecture (as built)
- `src/net`: `crypto` (X25519 commit/reveal handshake, 6-digit SAS, ChaCha20-Poly1305, subkeys),
  `control_channel` (TCP, length-prefixed encrypted JSON, ping/pong RTT + clock offset with an
  initial 5-ping burst), `discovery` (multicast beacons 239.255.77.77:47999),
  `media_transport` (UDP, 12-byte header = AEAD AD, packet number = nonce, replay window,
  per-stream media + feedback handlers, rx loss accounting, Gilbert-Elliott sim loss, ECT(1) on
  send / ECN read via `IP_RECVTOS`, transport-wide feedback every ~10 ms, pacer thread, link-sim
  thread), `fec` (GF(256) Cauchy Reed-Solomon erasure code),
  `cc/transport_feedback` (RFC 8888-like reports, overlapping ranges), `cc/congestion_controller`.
- `src/audio`: miniaudio device I/O, Opus 10 ms VOIP 48 kbps + LBRR (or DRED), adaptive jitter
  buffer (p95 jitter target, DRED→FEC→PLC concealment), drift resampler, optional APM.
- `src/video`: V4L2 camera (MJPEG→NV12 via swscale, BT.601→709), test pattern with a 32-bit frame
  code readable after decode, VA-API/x265/SVT-AV1 encoder (CBR, VBV ≈ 1 frame, P-only, IDR on
  demand), VA-API/dav1d decoder, packetizer + `FrameAssembler`, `rate_ladder` (rungs ×0.7, resolution
  drops below 0.04 bits/pixel/frame, hysteresis), `video_stream` (send/receive; swscale downscale).
- `src/app`: `CallController` (state machine, caps/invite/accept), `MediaSession`, `VideoView`
  (`QQuickRhiItem`, NV12 shader in `shaders/`), `main.cpp` CLI. UI in `qml/Main.qml`.

Video fragment header (24 B, inside the encrypted payload): frame id, index, data-fragment count,
capture µs, flags (key / test-pattern / retransmit), codec, display w/h (Mesa pads 1080→1088 with
no conformance window, so the receiver crops), FEC parity-per-block, FEC block size. Video
feedback kinds: PLI, NACK (frame id + index, 0xffff = whole frame). Transport feedback (wire type 3):
report µs, base pn, count, u16 per packet {received, ECN, arrival offset in 64 µs units}.

Congestion control (phase 6), in `MediaTransport` because it sees every packet:
- Queue delay = OWD − 30 s windowed min OWD; control signal = min over 50 ms (standing queue, not
  per-frame burst). Target 10 ms. Over target: cut ∝ excess; queue > 2× target: cut to below the
  delivery rate so it drains in ~400 ms, then hold further delay cuts until it could have drained.
- L4S: CE fraction → EWMA α (g = 1/16), cut α/2 at most once per RTT + 50 ms.
- Loss counts as congestion only with queueing (q > target/2) or > 10% loss; random loss is left to
  NACK/FEC. Loss is only declared for packets a report lists as missing (lost reports ≠ lost media).
- Increase +50%/s, +12%/s within ±15% of the last congestion rate; held when the queue is above
  target/2 or when app-limited (target > 1.6× delivered).
- Starts at max (LAN). Range: video max × 1.25 + 150 kbps down to video min + 80 kbps.
- Pacing (1.5× target) only within 10 s of a congestion signal; otherwise bursts as before.
- Video budget = (target − audio send rate) / (1 + FEC rate) → `RungSelector`: down immediately
  below 80% of the rung, else after 300 ms; up one rung after 2 s with 10% headroom. Each switch
  re-opens the encoder (≈10–15 ms) and sends an IDR: FFmpeg's hevc_vaapi and libx265 ignore
  mid-stream bitrate changes (verified). Frames are skipped while the pacer holds > 1.5 frame times.
- FEC rate comes from the controller's loss estimate (the video-only loss report is gone).
- The receiver's NACK quiet time is max(2 ms, 3× EWMA fragment gap), so paced frames aren't NACKed.

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
- [x] **6 Congestion control** — design above. Results (pattern 720p30, max 8 Mbps, loopback):
      no limit → stays at max, no pacing, no switches, capture→decoded ~8 ms; `--sim-rate 3000
      --sim-delay 10` → settles at the 1.9 Mbps 720p rung (probes 2.7, backs off), p95 64 ms;
      same + `--sim-ce 2` (L4S) → 1.35 Mbps, p95 30 ms; 5–10% random loss → rate untouched.
      Simulated-link unit tests: 6 Mbps link → 92% utilisation, ~5 ms mean queue; L4S ~77%.
      **Open:** (a) start-at-max into a slow link costs ~1 s of heavy loss + a few PLIs at call
      start; (b) every rung change is an IDR because FFmpeg won't retarget the encoder live — a
      native VA-API encoder (phase 9) would allow continuous rate changes; (c) audio and feedback bypass the pacer; (d) rung stepping on a tight link: a
      failed step up now returns exactly one rung and that rung isn't retried for 10 s (doubling to
      80 s); for 3 s after, only a budget < 70% of the rung drops further. Most remaining switches
      are the start-up ramp (start-at-max, see (a)). (e) Fixed: the pacer had no age limit, so after
      a start-up overload pre-congestion frames + NACK resends could keep it seconds behind while
      every new frame was skipped (seen ~1 in 8 runs). Now paced packets older than 200 ms are
      dropped; a NACK item is resent only if it fits under the frame-skip backlog (1.5 frame
      intervals) and its frame is < 500 ms old. A second cause on the receiver: after a loss the
      assembler accepted (and could re-show / NACK) frames it had already released; it now keeps a
      floor at the lost frame (test `neverResyncsOnOldFramesAfterLoss`). 10/10 runs clean.
      (f) Fixed: the receiver's NACK retry spacing and grace used only the control-channel RTT
      (~0.25 ms on loopback), so frames were declared lost (20 ms) before any resend could arrive;
      now max(control RTT, the receiver's own media srtt). 2 Mbps / 20 ms / 2% loss, FEC off:
      lost 46–59 → 0–9 frames per 21 s, resends useful 3% → ~97%, 44 → 60 fps; capture→decoded
      43 → 52 ms (frames now wait for their resend instead of a PLI + keyframe). (g) Fixed: every
      FEC block gets ≥ 1 parity fragment, so on small frames any FEC doubled the stream; at the
      floor rung on a 600 kbps link that pinned the call at ~48% loss (audio ~2/3 concealed). Auto
      FEC is now capped to the budget left above the current rung: 600 kbps → 0 drops, 42–48 ms,
      no audio concealed; 1000 kbps, 2 Mbps and LAN unchanged. **Open:** at mid rungs the
      ≥ 1-parity rounding still makes real FEC overhead larger than the displayed rate (e.g.
      "fec 9%" ≈ 50% real at 941 kbps). A prototype spreading parity across small frames fixed
      fixed-rate FEC but hurt 2%-loss runs under the old NACK timing; not retried since (f).
      Verified with real netem `bottleneck` + GUI (camera 1080p60 one way, pattern back):
      `--cc off` → 87–90% loss, video ~1 frame / 30 s, 0.4–1 s latency, ~40% audio concealed;
      `--cc on` → 59 fps both ways, p95 40–75 ms, audio <1% concealed, ~58 ms.
- [x] **7 Screen share** — xdg-desktop-portal ScreenCast over QtDBus (monitor or window, embedded
      cursor, restore token per instance so later shares skip the picker) → PipeWire stream
      (BGRx/RGBx SHM) → swscale to NV12 BT.709 (`screen_capture_pipewire.cpp`). Second send/recv
      stream on `kScreen` (hub `remote-screen`), started/stopped mid-call with the control message
      `screen {on, mode}`. Idle screens repeat the last frame every 1 s, or at once on PLI. Text
      mode: up to 4K, ≤30 fps, ladder sized for 10 fps so it keeps resolution; motion: ≤1080p60.
      While camera and screen both send, the screen gets 65% of the video budget; the CC max
      includes `--screen-bitrate` (default 10 Mbps). UI: share button + mode, remote screen on
      stage with the remote camera as a tile. Ends cleanly on picker cancel, GNOME's stop control,
      or hangup. Test `screenShareStartsStopsAndRestarts` (1080p pattern source). Verified in the
      GUI on GNOME Wayland (1920x1080 via the portal, both directions).
      **Deferred:** DMA-BUF zero-copy capture → phase 9 (SHM + swscale for now).
- [ ] **8 AV1 + negotiation matrix** — *skipped for now (user, 2026-11-22); phase 9 next.* SVT-AV1 RTC preset with screen-content tools, dav1d,
      hardware AV1 where present; exercise every encoder/decoder pairing end to end.
- [ ] **9 Latency work** — sub-frame slice send, zero-copy (V4L2/DMA-BUF→VA-API, VA-API→RHI),
      VA-API JPEG decode for the camera, low-latency present, native VA-API encoder (intra
      refresh, LTR) if not done earlier, Tambur-style streaming codes, glass-to-glass latency probe
      (`tools/latency_probe`) with per-stage breakdown and a WebRTC A/B.
      *In progress.* Done: per-stage breakdown (sender source/prep/encode, receiver
      encode/send/spread/wait/decode, display wait + capture→swap via `frameSwapped`),
      `latency_probe` (screen-capture glass-to-glass; `--source-log` with `--video-source flash`
      needs no camera). Measured 1080p60 loopback, flash source: created→on screen 26 ms median
      remote, 8 ms self view; after-swap cost ≈ one compositor frame. Camera: source stage 40 ms
      (25.5 ms driver timestamp→dequeue, 14.6 ms software MJPEG decode).
      VA-API JPEG decode done (`jpeg_decode.cpp`, `--camera-decode auto|software`, falls back to
      software): decode 1 ms → VPP repack 4:2:2→NV12 4.5 ms → download 2.7 ms → CPU 601-full→709-
      limited pass 2.4 ms (Mesa VPP ignores colour properties for YUV→YUV). Convert 14.5 → 11 ms,
      CPU ~13 → ~3 ms/frame, so the capture thread keeps up: timestamp→dequeue 25 → 20 ms, camera
      drops ~250 → ~5 per 25 s; sender capture→encoded 45 → 36–41 ms.
      Zero-copy camera done: JPEG decoded on the process-wide VA device (`vaapi_device.cpp`), frame
      stays a VA surface (`VideoFrame::gpu`); the encoder takes an NV12 surface repacked/scaled by
      VPP; self view downloads on the render thread only when drawn. Camera MJPEG is sent in its
      own colours (BT.601 full, signalled in the VUI; `VideoFrame::colour`, shader matrix per frame),
      so there is no CSC anywhere. Mesa's EFC shortcut (unscaled VPP → encoder reads the YUYV source
      and garbles it) is switched off at start-up. Camera thread 10 → 1.1 ms/frame; capture→encoded
      at the receiver 31.8 → 25.8 ms (same session, software decode vs zero-copy).
- [ ] **10 macOS** — AVFoundation camera, ScreenCaptureKit, VideoToolbox (low-latency RC, HEVC),
      CoreAudio via miniaudio, codesign + camera/mic/screen entitlements. Platform splits exist
      (`camera_none.cpp`, `codec_probe_generic.cpp`, `audio_device_none.cpp`).

Also outstanding: active A/V sync (latencies are reported only); the AV1 software encoder bug below.

## Gotchas learned
- `qt6-base-private-dev` is required for `rhi/qrhi.h`; link `Qt6::GuiPrivate` (in 6.8 it comes
  with Gui; 6.9+ needs the component).
- Mesa VA-API HEVC output is 1088 lines for 1080 input with no crop info — send display size.
- Debian's libopus lacks DRED; the local static build interposes over the copy libavcodec loads.
- Tests must never open the real camera (they use the pattern source at 1280×720@30).
- Someone else may edit files concurrently: re-read before editing and keep their changes.
- FFmpeg hevc_vaapi sends rate-control params only at IDR and libx265 doesn't reconfigure: changing
  `bit_rate` mid-stream does nothing (measured). Mesa CBR pads every frame, IDRs included, to ≈ the
  per-frame budget, so keyframes are not bursts.
- ECN on loopback works without root: socket TOS 0xB9 (EF + ECT(1)) arrives intact, a per-packet
  `IP_TOS` cmsg can set CE, and `IP_RECVTOS` on the dual-stack socket reports it for v4-mapped peers.
- **Bug for phase 8:** `libsvtav1` (2.3) refuses to open with `rc_max_rate` set ("Max Bitrate only
  supported with CRF mode"), so AV1 software encode (`--prefer-av1`) currently fails. Pre-existing.
