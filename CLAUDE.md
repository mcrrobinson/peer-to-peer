# p2pcall: notes for Claude

`plan.md` is the source of truth: ground rules, dev machine, build/run/test commands,
architecture, phase status, gotchas. Read it first and follow its ground rules (stay in scope,
no sudo, commit only when asked, tests never open the real camera).

## Handoff: branch `feat/audio-opus-1.6` (written on the Mac, 2026-11-22)

Written and committed on macOS, which cannot build the app (no Qt 6, miniaudio or
webrtc-audio-processing installed, and the macOS port hasn't started). **Nothing beyond the
two self-contained test suites has run.** The job on the Debian box is to build it, run the
full suite, and fix whatever breaks.

### What changed
- **libopus 1.6.1** via `tools/build-opus-dred.sh`, with four upstream fixes from libopus `main`
  in `tools/patches/opus-1.6.1/`: a DRED encoder off-by-one, `opus_dred_parse` offsets, a bound
  on DRED latents from untrusted packets (security), and BWE state reset. The script writes
  `build/third_party/opus/include/opus/p2p_dred_rev.h` (`P2P_OPUS_DRED_REV`, 12 for 1.6.x).
  Built without `--enable-osce` and `--enable-qext` on purpose: OSCE only acts on 20 ms SILK-WB
  frames, and QEXT is unused extra parsing code exposed to the peer.
- **Decoder complexity 5** (`OpusDecoderWrapper::kComplexity`): enables neural (FARGAN) PLC.
  The default was 0. Higher values only select LACE/NoLACE, which never run on 10 ms frames.
- **One loss-protection scheme at a time** (`audio::LossProtection`, `--audio-fec lbrr|dred`,
  default lbrr). Previously LBRR mode also requested DRED, which got no bits but still ran the
  DRED encoder network. Now LBRR mode sends no DRED, and DRED mode turns LBRR off.
- **`MediaCaps::audioDredRev`** (JSON `dredRev`) in the caps exchange. `CallController` fills it
  from `audio::dredRevision()` when it is 0. `MediaSession::setPeerDredCompatible()` falls back to
  LBRR unless both revisions match and are non-zero. 1.5 (rev 10) and 1.6 (rev 12) DRED don't
  interoperate: a mismatched decoder ignores it silently.
- **Audio bitrate follows congestion control** (`src/audio/audio_rate.h`):
  allowance = CC target − minimum rates of the sending video streams − ~57.6 kbps of per-packet
  header overhead. Steps run from 12 to 48 kbps: down at once, up one step per 2 s. The value is
  checked every 10 frames on the audio thread, and only when `--cc on`. `--audio-bitrate` is now
  the maximum. On a normal link it stays at 48. At the CC floor (video min + 80 kbps) it drops to
  about 20.
- **AEC reference fix**: `processRender` is now fed `out` after `playout_.pull()`, i.e. after the
  drift resampler, in 480-sample chunks. It used to be fed decoded audio before the ±2 %
  resampler. AEC is still **off by default**. This is the first build where the change compiles
  for real, because the Mac has no webrtc-audio-processing.
- `plan.md` is updated, including a correction: "64 kbps allows LBRR + DRED" was wrong.

### Verified on the Mac
- The build script runs on macOS: checksum OK, all 4 patches apply, libopus builds, DRED rev 12.
- `test_audio_rate` (new, 9 cases) and `test_negotiation` (with a new DRED-rev case) pass
  against Qt 5 headers.
- A harness of the Opus wrapper against the built 1.6.1, on a synthetic voiced signal rather
  than real speech:
  - LBRR at 48 kbps: ~44.5 kbps payload, recovers single losses only.
  - DRED at 48 kbps: ~51 kbps, recovers bursts up to 70 ms.
  - DRED at 64 kbps: ~69 kbps, recovers every burst tested (up to 100 ms).
- All changed `.cpp` files pass `-fsyntax-only` against Qt 5 headers.

### To do on Debian
```sh
tools/build-opus-dred.sh
cmake --preset dev -U 'OPUS*' -U '__pkg_config_checked_OPUS'
cmake --build --preset dev && ctest --preset dev     # now 10 suites
```
- New tests in `test_call` that have never run:
  - `audioDredFallsBackOnRevisionMismatch`
  - `audioLbrrModeSendsNoDred`
  - `audioBitrateFollowsTightLink` (400 kbps sim link; expects Opus < 48 kbps within 15 s)
  - `audioBitrateStaysAtMaximumOnLan`

  The tight-link one is the most likely to need tuning. Also confirm `audioDredConcealsBursts`
  still passes with 1.6.1.
- Check that the static 1.6.1 still interposes over the system libopus that libavcodec loads.
- Listening test with `--aec on` (camera + mic on one side) to see if the AEC fix helped.
  Check alignment via APM stats and the tone source before blaming AEC3.

### Deliberately not done
- Noise suppression: the user found it poor; ignore it for now.
- Feeding `setExpectedLoss()` from the CC loss rate: not requested. It mainly sets DRED's share of
  the bitrate.
- Switching to 20 ms frames on tight links. It would halve the ~58 kbps header overhead, but
  480-sample frames are hardcoded in `audio_pipeline.h`, `jitter_buffer.h` and the DRED offset.
- The webrtc-audio-processing 2.x upgrade, and macOS VoiceProcessingIO.
