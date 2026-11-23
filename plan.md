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
- Browser automation (if ever needed): on the Linux machine, use the local Linux Chrome, never the macOS one.

## Dev machine
Debian 13, kernel 6.12, 12 cores, AMD RX 5700 XT (Navi 10, Mesa radeonsi). VA-API: HEVC
Main/Main10 encode + decode, JPEG decode; **no AV1 hardware** (AV1 = SVT-AV1 encode, dav1d
decode). Qt 6.8.2, FFmpeg 7.1, libsodium, miniaudio, webrtc-audio-processing-1. Camera:
Logitech StreamCam `/dev/video0` (60 fps only as MJPEG; NV12 ≤ 30 fps).

macOS (phase 10): MacBook Air M3, 16 GB, macOS 26.2, Command Line Tools only (no Xcode), Homebrew:
`qt` 6.12, `ninja`, `miniaudio`, `ffmpeg` 9.0 (x265, SVT-AV1, dav1d, VideoToolbox), `libsodium`, `opus` 1.6.
No webrtc-audio-processing formula (AEC is parked anyway). Same presets; Qt picks the Metal RHI backend.

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
      **Open:** (a) ~~intra refresh + LTR recovery need a native VA-API HEVC encoder~~ done in
      phase 9 (reference selection; intra refresh only if needed); (b) user to pick the audio
      default after a listening test: LBRR (default) vs DRED (`--audio-fec dred`; at 48 kbps /
      10 ms LBRR starves DRED's budget; 64 kbps allows both with ~30 ms DRED); (c) validate NACK/FEC
      under real delay with `netem.sh on wan` (needs sudo).
- [x] **6 Congestion control** — design above. Results (pattern 720p30, max 8 Mbps, loopback):
      no limit → stays at max, no pacing, no switches, capture→decoded ~8 ms; `--sim-rate 3000
      --sim-delay 10` → settles at the 1.9 Mbps 720p rung (probes 2.7, backs off), p95 64 ms;
      same + `--sim-ce 2` (L4S) → 1.35 Mbps, p95 30 ms; 5–10% random loss → rate untouched.
      Simulated-link unit tests: 6 Mbps link → 92% utilisation, ~5 ms mean queue; L4S ~77%.
      **Open:** (a) start-at-max into a slow link costs ~1 s of heavy loss + a few PLIs at call
      start; (b) every rung change was an IDR because FFmpeg won't retarget the encoder live (phase 9:
      the native encoder retargets same-size switches without one; following the budget
      continuously between rungs cost latency and was dropped); (c) audio and feedback bypass the pacer; (d) rung stepping on a tight link: a
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
- [x] **9 Latency work** — sub-frame slice send, zero-copy (V4L2/DMA-BUF→VA-API, VA-API→RHI),
      VA-API JPEG decode for the camera, low-latency present, native VA-API encoder (intra
      refresh, LTR) if not done earlier, Tambur-style streaming codes, glass-to-glass latency probe
      (`tools/latency_probe`) with per-stage breakdown and a WebRTC A/B.
      *Done 2026-11-23 (sub-frame slice send dropped).* Covered: per-stage breakdown (sender source/prep/encode, receiver
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
      Low-latency present: tried swap interval 0 (GNOME Wayland, Qt GL): no gain (decoded→swap
      1.8 vs 1.9 ms, probe median 7.2 vs 6.8 ms); the redraw wait is already < 2 ms with one video
      window. Dropped. The probe's absolute numbers depend on window placement (its own
      paint→capture calibration); use it for same-sitting A/B only.
      Receive-side zero-copy done: hardware-decoded frames stay VA surfaces, exported as dma-bufs
      (Y R8 + UV GR88) and imported by the renderer as EGLImages bound to GL textures QRhi wraps;
      falls back to read-back + upload (non-GL backend, import failure); `--decode-output cpu`
      for A/B. Self view uses the same import. Same sitting, camera → instance 2's screen:
      decode 8.0 → 2.9 ms, capture→swap 40.4 → 33.4 ms.
      test_call flake (burst loss, ~1 in 15 suites): all 3 NACKs went out within ~10 ms and a
      loss burst took the resends too, then the frame timed out at 20 ms. The receiver now keeps
      asking until the (unchanged) grace deadline, ≥ 3 tries, retry ≥ 3 ms: 0/20 suite failures
      (burst test lost 0 in 20/20, was 5/20 with ≥ 1). Network scenarios (MB/MD/MG600L2): resend
      volume and queueing unchanged; MD timeouts → PLI 2,2,2 → 0,0,0. Also renamed the
      packetizer's `emit` parameter: Qt's empty `emit` macro silently dropped every send once a Qt
      header got included there.
      Sub-frame slice send: dropped (user, 2026-11-23). Frame spread is 0.1 ms on loopback and
      set by the link rate on slow links; Mesa returns the coded frame only once it's whole and
      hwaccel decodes whole frames, so there is no encode/send/decode overlap to gain.
      **Native VA-API HEVC encoder — plan.** Driver check (Mesa 25.0.7 radeonsi, Navi10):
      CBR/VBR/CQP; 1 active L0 ref, no B; app-managed DPB (`reference_frames` +
      `ref_pic_list0` can point at any kept surface); packed SPS/PPS/slice headers parsed
      (so we write them); rolling intra refresh rows/columns; per-picture rate-control buffers;
      max frame size; up to 4 temporal layers; no dynamic scaling. Aims: (1) bitrate changes
      without an IDR (phase 6 open (b)); (2) recover from an unrecoverable loss with a P frame
      that references the last frame the receiver confirmed, not a PLI → IDR; (3) intra refresh
      only if (2) leaves a case it would fix. HEVC only; FFmpeg encoders stay as the fallback
      (`--encoder ffmpeg|native`) and for A/B.
      - [x] 0 Spike — **go**. Direct VA-API encode, 720p60 pattern, our own VPS/SPS/PPS/slice
            headers (packed, with emulation bytes; Mesa keeps our SPS/VUI and RPS), decoded by
            libavcodec's software HEVC decoder with 0 errors. (a) Rate-control buffers sent every
            frame retarget CBR with no IDR: 4000→1000→6000→2000 kbps land within 1–2 frames (the
            pattern's floor is ~1650 kbps at max QP, same from a fresh start). (b) A P frame
            referencing frame N−k (k = 1, 2, 4, 8, and an anchor 50 back kept in the RPS with
            used_by_curr = 0), decoded by a decoder that skipped the frames in between, is
            bit-identical to the full decode from then on. (c) Rolling intra refresh (1 CTB row
            per frame, 12-frame wave) heals after a dropped frame: identical again 19 frames later,
            12 with constrained intra pred; but at the same CBR it costs ~4 dB (27.5 vs 32 dB).
            IDR vs recovery frame at 4 Mbps: IDR 32 → 25 dB and still 29 dB six frames later, and
            1.3× the frame budget (2.6× at 2 Mbps); a P frame from 2 back holds 31.6 dB, from 8
            back 28.3 dB, ordinary size.
      - [x] 1 Native encoder, now the default: `hevc_headers.cpp` (VPS/SPS/PPS/slice headers,
            explicit RPS per slice) + `hevc_vaapi_encoder.cpp` behind `VideoEncoder`
            (`--encoder native|ffmpeg`; falls back to FFmpeg if the driver lacks packed headers or
            CBR). Mesa codes HEVC at 64-aligned width / 16-aligned height and honours our
            conformance window, so 1080p (and odd ladder sizes like 426×240) now decode at the
            picture size instead of 1088 / 442 with padding. `test_hevc_encoder`: same bytes and
            PSNR as FFmpeg's hevc_vaapi within 0.1 dB at 1080p/720p/540p/240p; zero-copy rows
            added to `test_jpeg_decode`. Calls, same sitting: 1080p60 pattern encode 5.1 vs 5.2 ms;
            camera 1080p60 capture→decoded 27.5 vs 27.8 ms; `--sim-rate 600 --sim-loss 2` same
            losses/resends/PLIs.
      - [x] 2 Live rate: rate-control buffers go to the driver when the target changes, so a rung
            switch that keeps the picture size is a retarget, not a re-open + IDR (`liveBitrate()`;
            FFmpeg encoders keep the old path). Tried first: also following the budget between
            rungs (clamped to the neighbouring rungs). 1 Mbps link: IDRs 13,13 → 6,7 and +15%
            video rate, but on a 3 Mbps link latency 29–32 → 36 ms and one run queued to 104 ms
            p95 — the rung/budget gap is the headroom. Dropped; the encoder runs at the rung rate.
      - [x] 3 Reference selection. Frame header +1 byte (`refBack`: frames back to the
            reference; header 26 → 27 bytes, old builds can't interoperate). The native encoder
            keeps the last frame + up to 3 anchors (spaced ≈ (5.5·RTT + 30 ms)/2, the loss →
            request delay) in the RPS. After a timed-out frame the receiver sends `Recover{last
            released id}` (every max(100 ms, 1.2·RTT), 3 tries, then PLI); the sender predicts the
            next frame from that frame or the newest kept frame of its chain (keyframe if none,
            or FFmpeg encoder). The assembler resumes on a frame whose reference it released, and
            such a frame also ends a gap early. Tests: assembler resync; encoder recovery decoded
            bit-identical to a full decode (3 back, anchor 8 back). Calls, native vs FFmpeg, 2 runs
            (IDRs after 5 s): MB 6,4 → 1,1, lost 6,4 → 0,3; MD 5,5 → 1,1, lost 0,14 → 0,0, latency
            100 → 89 ms; MG600L2 2,2 → 0,0 (4 recoveries), lost 10,1 → 2,1, p95 171 → 131 ms;
            bursty loss without NACK/FEC: IDRs 44,47 → 0,0 (42,36 recoveries), frozen frames and
            latency unchanged (still one round trip). MG1000/MG3000: video rate, latency equal
            (more same-size rung switches at 1 Mbps; they no longer cost an IDR). No PLIs anywhere.
      - [x] 4 Intra refresh: dropped — no gap left. After step 3, no PLIs and no loss-driven IDRs
            in any scenario; the remaining IDRs are resolution changes (a new SPS needs one
            anyway), and the spike measured ~4 dB lost at the same rate.
    - Streaming codes (Tambur-style, without its learned overhead model). `--fec window`: each
      parity packet is a random GF(256) combination of every data packet of the last ≈50 ms of
      frames (2–8 frames; `window_fec.{h,cpp}`), at the auto rate as an exact fraction of
      packets carried across frames; the receiver solves across frames and waits the window's
      span before calling a frame lost. `--fec hybrid` (now the default): per frame, the old
      per-frame RS if its cost rounded up to whole parity packets fits the spare budget above
      the rung (`fecCap_`), else the window code. Tests: lost frame rebuilt from later parity,
      fractional rate, 5% random loss (all rebuilt packets exact), resends, `planFragments`
      matches the packetizer, the hybrid rule, window FEC end to end in a call.
      A/B, 3 runs × 2 directions, per direction over t ≥ 5 s, p95 = mean of the rolling
      240-frame p95 (`t2sum.py`):
      | link                   | auto p95 | window p95 | hybrid p95 | notes |
      | 2 Mbps / 20 ms / 2%    | 45       | 77         | 45         | hybrid picks per-frame 89% |
      | 600 kbps / 20 ms / 2%  | 157      | 77         | 76         | auto: 15 frames skipped by the pacer, 18 link drops; hybrid/window 0 |
      | 800 kbps / 80 ms / 2%  | 146      | 218        | 218        | auto skips 143 frames (~10%), video 281 vs 319 kbps |
      | 4 Mbps / 80 ms / 2%    | 173      | 194        | 201        | noise (p = 0.28); lost 7.5 → 1.5 |
      | same, bursts of 3      | 271      | 245        | 273        | noise; lost 25 → 8 (p = 0.06) |
      | 3 Mbps clean           | 35       | 35         | 35         | no FEC wanted |
      600 kbps and 2 Mbps differences are complete separations (p = 0.002, permutation; the two
      directions of a run aren't fully independent). The 800 kbps trade is real: auto keeps
      its tail by dropping ~6 frames/s at the sender (those never count in latency); hybrid sends
      every frame at +72 ms p95. Also fixed: a stale call test that expected every loss to cost
      a keyframe (flaky since the native encoder recovers without one).
    - WebRTC A/B (`tools/webrtc_ab/`: Chrome loopback page — two RTCPeerConnections, host UDP,
      stock settings — sending a 1920x1080 canvas redrawn at 60 fps that flashes like
      `--video-source flash`, switch times on CLOCK_MONOTONIC via a local server, so
      `latency_probe --source-log` measures both the same way). Chrome 158, same sitting,
      45 s per row, frame created → on screen, median (p95):
      | p2pcall HEVC (native VA-API), 1080p60 | 2.2 (11.6), repeat 0.5 (8.8) |
      | p2pcall, 720p60                       | −3.1 (5.9) |
      | Chrome VP8 (default), 1080p60         | 61.1 (83.4), repeat 55.8 (85.5) |
      | Chrome H264, 1080p60                  | 53.4 (68.8) |
      | Chrome AV1, 1080p60                   | 53.0 (71.4) |
      | Chrome, canvas → <video>, no WebRTC   | 30.9 (47.1), repeat 24.4 (41.1) |
      The probe's absolute numbers run low (p2pcall's own capture→swap is 8.8 ms at 1080p), so
      read differences only: p2pcall is ~50–60 ms ahead end to end; of that, ~25–30 ms is
      Chrome's canvas capture + video element display (the control) and ~25–30 ms WebRTC
      itself (getStats: encode 7.7 ms, jitter buffer 9–11 ms, decode 0.6–5 ms; Chrome chose
      1080p60, no quality limit, no loss). Caveats: loopback only, no impairment, trivially
      compressible content, one browser; a camera-sourced call has its own capture path in
      place of the canvas.
- [~] **10 macOS** — AVFoundation camera, ScreenCaptureKit, VideoToolbox (low-latency RC, HEVC),
      CoreAudio via miniaudio, codesign + camera/mic/screen entitlements.
      Steps: (1) builds and calls with what exists; (2) VideoToolbox HEVC encode + decode (native
      VTCompressionSession, low-latency RC, live retarget, recovery refs if VT allows; decode to
      IOSurface → Metal without a copy); (3) AVFoundation camera; (4) ScreenCaptureKit; (5) app
      bundle, Info.plist usage strings, ad-hoc codesign, TCC permissions.
      - [x] 1 Builds and runs (2026-11-23). EGL dma-buf import is Linux-only (`video_view.cpp`
            stub elsewhere: upload path); `codec_probe_macos.cpp` advertises HEVC hw encode when VT
            lists a hardware HEVC encoder (FFmpeg `hevc_videotoolbox` for now), decoders software
            only. Without it the software-only probe picked AV1 and hit the SVT-AV1 bug below.
            Headless pair, pattern 1080p60, Release: 60 fps both ways, capture→decoded 7.7 ms avg,
            software HEVC decode 1.4 ms; control RTT 0.1–0.3 ms. GUI: Metal backend, both views draw.
            Debug build: pattern prep ~17 ms/frame caps the pattern at ~30 fps at 1080p (−O0).
            Tests 9/10 suites; `test_call` fails only `congestionControlAdaptsToBottleneck`:
            `hevc_videotoolbox` runs well under its target (4 Mbps set, ~600–700 kbps out on the
            pattern) so the 2 Mbps link never congests. VT rejects `constant_bit_rate` for HEVC on
            M3 ("not supported by the encoder"). Expected to be fixed by step 2. Seen once each,
            not reproduced in 5 later runs: `windowFecRepairsWithoutResends`,
            `congestionControlStaysOutOfTheWayOnLan` (first probably load from GUI instances).
      - [x] 2 VideoToolbox encoder + decoder (2026-11-23).
            Spike (M3, HEVC, hardware): `EnableLowLatencyRateControl` works for HEVC;
            `ConstantBitRate`, `MaxFrameDelayCount`, `PrioritizeEncodingSpeedOverQuality` unsupported;
            `EnableLTR`, `DataRateLimits`, Min/MaxAllowedFrameQP supported. 720p30 with detail,
            4000→1000→6000→2000 kbps: low-latency RC follows within ~1 s, no keyframes; default RC
            ignores downward changes (1000 asked → ~2100 out); `DataRateLimits` no effect under
            low-latency RC. VT never pads: the plain test pattern codes at ~620 kbps whatever the
            target. Low-latency RC drops whole frames when content can't fit (flag FrameDropped).
            Encode ~3 ms median at 720p, one frame in flight. LTR: VT marks its own LTRs (~every 2 s
            when quiet, often around losses, every refresh frame); `ForceLTRRefresh` with
            `AcknowledgedLTRTokens` gives a P frame (1.15–1.3× size) that decodes bit-identically to
            a full decode; with no usable acked LTR it sends a keyframe. Acking only the newest LTR
            fails on back-to-back losses (VT then marks most frames LTR, every loss → keyframe);
            acking every LTR the receiver decoded works (`ltr3` spike: 0 keyframes).
            `hevc_vt_encoder.cpp` (default; `--encoder ffmpeg` = FFmpeg's hevc_videotoolbox):
            low-latency RC, live `AverageBitRate`, LTR recovery (`recoverFrom(good)`: drop LTRs after
            good, ack the rest, refresh; `refBack` = to the newest acked; cleared on keyframes), VUI
            colours (BT.709 limited / BT.601 full via the pixel format), Annex B with parameter sets
            on keyframes, encoder drops use no frame id. `setAnchorInterval` is a no-op (VT picks LTRs).
            `test_vt_encoder`: ladder sizes incl. 426×240 exact, keyframes on demand, live rate within
            4%, recovery bit-identical, back-to-back losses (fails on the newest-only logic), colours.
            `CaptureConfig::patternDetail` (test pattern noise, default 0) so `test_call`'s two CC tests
            send a real 4 Mbps; bottleneck test now passes (2 Mbps link → 672–960 kbps 540p rungs, 4–6
            switches; Linux held 1.9 Mbps 720p on a 3 Mbps link — possibly VT's ~1 s settling; open).
            Calls, Release, 720p30, native vs FFmpeg: clean 4.9/7.7 vs 6.1/9.8 ms avg/p95; MG600L2
            keyframes 3 vs 4, p95 72 vs 99 ms; 3% loss in bursts of 4, no NACK/FEC, 3 runs: keyframes
            1,1,1 vs 17,17,27, recoveries 19,18,14 vs 0, video 562–622 vs 896–999 kbps. 1080p60 clean,
            same sitting: capture→decoded 10.1 vs 9.7 ms avg, p95 12.6 vs 13.6. 11/11 suites; test_call
            21/21 × 5. Not checked on Linux: `patternDetail` in the two CC tests there.
            Decoder: FFmpeg's hevc decoder with the VideoToolbox hwaccel (synchronous); frames stay
            CVPixelBuffers (`GpuImage::cvPixelBuffer()`, CPU readers lock and copy); probe now
            advertises HEVC hw decode (AV1 stays software). Bit-identical to the software decoder
            (720p, 426×240, BT.601 full). Renderer on Metal: `metal_nv12_import.mm` wraps Y/UV as
            R8/RG8 MTLTextures via CVMetalTextureCache (4 imports kept alive), `createFrom` per frame;
            falls back to upload. `test_video_view` (Apple, opens a window): grab of the imported frame
            == grab of the same picture uploaded (0.000 mean diff; another frame differs by 20–32), no
            read-back, no fallback warning. Render-thread cost per new remote frame, 1080p60: 0.05 ms
            zero-copy vs 0.76–0.82 ms read-back + upload (`--decode-output cpu`), decode stage 1.9 vs
            2.2 ms. End-to-end display wait can't show it: 60 fps source vs 60 Hz display is
            unsynchronised, so decoded→swap is set by the phase (2–20 ms, random per run, either mode);
            two GUI windows at the same spot also throttle the hidden one (use `-qwindowgeometry`).
            Clean-call LTR cadence at 60 fps: every 241 frames.
            Fixed on the way: (a) macOS lets a dual-stack listen succeed on a port another program
            holds on 127.0.0.1 (QTcpServer sets SO_REUSEADDR); loopback calls then went to that
            program ("frame too large", seen once in test_call: another local tool listened on the
            port). `net/port_check` probe; CallController retries port 0 / refuses a fixed port.
            The UDP media socket has no SO_REUSEADDR, so macOS already refuses there (test added).
            (b) test_call's `runImpaired` read null stats after a failed call wait and crashed the
            suite (any platform); it now returns early. 12/12 suites, test_call 22/22 × 3.

Also outstanding: active A/V sync (latencies are reported only); the AV1 software encoder bug below.

## Gotchas learned
- `qt6-base-private-dev` is required for `rhi/qrhi.h`; link `Qt6::GuiPrivate` (in 6.8 it comes
  with Gui; 6.9+ needs the component).
- FFmpeg's hevc_vaapi on Mesa outputs 1088 lines for 1080 input with no crop info — send display size.
  Mesa codes HEVC at 64-aligned width / 16-aligned height but honours a conformance window in the
  packed SPS (the native encoder writes one).
- Mesa radeonsi rewrites the HEVC slice header from the fields it parses out of the packed one
  (RPS included) and applies VA rate-control buffers on any picture, not just IDRs.
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
- macOS: the filesystem is case-insensitive, so the QML module's default output dir `build/dev/P2PCall`
  collided with the `p2pcall` executable (ld: "Is a directory"); the module now builds into `qml/P2PCall`.
- macOS: `QTRY_*` / `qWait` sleep ~10 ms between event passes; with both peers in one thread each
  control-channel leg waits out a sleep (~12.7 ms on macOS) and the legs differ by up to ~10 ms, so
  the clock-offset check now runs the ping burst under a real `QEventLoop`.
