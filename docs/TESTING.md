# Testing

This file lists the test suites, how to run them, **what was actually run**, and what has **not**
been verified. Nothing here is a claim about a run that did not happen.

## Suites

| Suite | Where | What it exercises |
| --- | --- | --- |
| Unit tests (`mf-unit-tests`) | `tests/unit/` | Core logic with a fake transport and virtual time: URL parsing/validation (every RTMP/RTMPS server in OBS 32.2.2's own `services.json`, pinned in `tests/data/`, must be accepted), platform rules (Facebook RTMPS only, placeholders and invisible characters rejected), redaction, bandwidth, reconnect schedule, error classification, config schema/migration/backup/corruption, export/import, credential vault (session-only fallback), session state machine, reconnect policy, stop/start races, Start Enabled/Stop All, 8-output limit, locks, encoder group errors, bounded shutdown. On Windows also real **Windows Credential Manager** round trips (`test-wincred.cpp`), on macOS real **Keychain** round trips (`test-keychain.cpp`). |
| libobs integration (`mf-integration-tests`) | `tests/integration/` | Headless libobs **32.2.2** with the real OBS modules (`obs-outputs` `rtmp_output`, `rtmp-services`, `obs-x264`, `obs-ffmpeg`), streaming generated A/V test clips to local receivers (`ffmpeg -listen`, nginx-rtmp with on_publish key check, a Python TLS proxy, a TCP server that never answers the RTMP handshake). An OBS video reset between two starts (`video_reset_between_starts`). Recordings are probed and fully decoded; audio is classified with Goertzel filters; A/V sync is measured from flash/beep pairs. |
| Real-OBS UI tests | `tests/ui-driver/`, `tests/ui/run_ui_tests.py` | The real OBS Studio 32.2.2 frontend with the plugin installed: on Linux built from source and run under Xvfb, on Windows and macOS the official OBS 32.2.2 release builds on CI runners. Where a secure store exists the run also requires keys to be saved to it, restored after an OBS restart and removed when a destination is deleted. A test-only module (`mf-ui-driver`, never packaged) clicks through the dock's widgets by object name (Add, editor fields, Save, Start/Stop, Start Enabled, Stop All confirmation, Duplicate, Delete, Export/Import dialogs, Encoding dialog), drives OBS (scenes, Studio Mode, transitions, native streaming/recording/replay buffer, profiles, exit), and the orchestrator verifies what the receivers got and scans every OBS log/config/profile/export file for the test stream keys. |
| Soak (`mf-integration-tests --soak`) | `tests/integration/scenarios.cpp` (`run_soak`) | Long run with N destinations; samples CPU, RSS, measured throughput, network drops, RTMP congestion, reconnects, render lag and encoder skips every 10 s into `soak-metrics.csv`; keeps segmented recordings and measures A/V sync drift across the run. |
| Windows and macOS CI | `.github/workflows/ci.yaml` | Template-script builds with warnings as errors (MSVC; Xcode universal), unit tests including the secure store, packaging and package inspection (no test binaries), install with the shipped installer/script, the real-OBS UI tests, then uninstall and stored-key removal with the shipped scripts. |

## Running them

Build as in [BUILDING.md](BUILDING.md) (`ubuntu-ci-x86_64` preset), then:

```bash
# Unit tests
LD_LIBRARY_PATH="$HOME/obs-install/lib" ctest --test-dir build_x86_64 --output-on-failure

# libobs integration tests (needs an X display for libobs's GL context; root only for the RTMPS
# scenario, which installs a temporary test CA into /etc/ssl/certs and removes it afterwards)
Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp &
sudo env "PATH=$PATH" DISPLAY=:99 LD_LIBRARY_PATH="$HOME/obs-install/lib" \
  build_x86_64/tests/mf-integration-tests --obs-prefix "$HOME/obs-install" --work /tmp/itest \
  --scripts tests/scripts all

# Real OBS UI tests (uses the clips the integration run generated)
DISPLAY=:99 python3 -I tests/ui/run_ui_tests.py --obs-prefix "$HOME/obs-install" --work /tmp/ui --clips /tmp/itest

# Soak: 120 minutes, 4 destinations, 720p30, recordings segmented every 300 s
DISPLAY=:99 LD_LIBRARY_PATH="$HOME/obs-install/lib" build_x86_64/tests/mf-integration-tests \
  --obs-prefix "$HOME/obs-install" --work /tmp/soak --scripts tests/scripts \
  --size 1280x720 --soak 120 --soak-destinations 4 --soak-segment 300
```

Single scenarios can be named instead of `all` (integration) or appended (UI: `crud`, `eight`,
`program`, `profile_exit`). Reports: `integration-report.txt`, `ui-report.txt`, `soak-metrics.csv`.
When a receiver check fails, the UI orchestrator prints the recording size, the end of the
receiver's ffmpeg log and the OBS log lines about the outputs (test keys masked).

Diagnostic switches for the UI tests (never set in normal runs):

* `MF_OBS_EXTRA_ENV="NAME=value;NAME=value"` adds variables to OBS's environment, e.g.
  `DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib` for Guard Malloc on macOS (only on an ad-hoc
  re-signed copy of OBS: the official app's hardened runtime ignores `DYLD_` variables).
* `MF_UITEST_DOCK=docked|floating` overrides where the driver puts the dock (floating on macOS,
  docked elsewhere), and `MF_UITEST_PREVIEW=off` turns OBS's preview off. Both are passed to OBS
  through `MF_OBS_EXTRA_ENV`.

## Acceptance coverage

| # | Acceptance item | Covered by | Status |
| --- | --- | --- | --- |
| 1 | Create/edit/duplicate/delete; restart without exposing keys | UI `crud` + `restore`; unit config/vault tests; Credential Manager and Keychain unit tests | Verified on Windows and macOS in the official OBS 32.2.2 builds (keys restored from the secure store after a restart) and on Linux (keys session-only there) |
| 2 | 8 concurrent decodable outputs | integration `eight_outputs`; UI `eight` | Verified (local receivers, 720p30) |
| 3 | One shared encoder pair | integration `two_outputs_shared`, `eight_outputs` (counts plugin encoder objects in OBS, checks every output reports the shared encoders) | Verified |
| 4 | Stop one, others continue | integration `eight_outputs`, `receiver_restart`; UI `eight` | Verified |
| 5 | Invalid key, refused connection, TLS failure, stall | integration `failure_modes` (nginx-rtmp key check, closed port, self-signed RTMPS, handshake stall), UI stays responsive (main-thread task < 100 ms) | Verified |
| 6 | Reconnect policy, manual stop, exhaustion | integration `reconnect_policy` (measured attempt spacing 2/4/8/16 s), `receiver_restart`; unit tests | Verified |
| 7 | Rapid start/stop races | integration `rapid_cycles` (nginx-rtmp, repeated cycles, never more than one connection per destination); unit race tests | Verified |
| 8 | Scene cuts, transitions, graphics, replay, audio track, muting, Studio Mode preview | UI `program` (audio identifies the on-Program scene; pixel check for a graphic added on Program); integration `audio_track_and_mute` (Track 2 selection, muting) | Verified (graphics with a colour source; browser sources not built in the Linux test OBS) |
| 9 | Native streaming/recording/replay independent | UI `program` (real OBS Start Streaming, recording, replay buffer); integration `independent_native_outputs` | Verified |
| 10 | Hide dock, profile change, exit, no auto-broadcast | UI `crud` (dock hidden while live), `profile_exit`, `no_autostart`, `restore` | Verified |
| 11 | 2-hour soak | soak runs below | 120 min verified at 720p30 on a CPU-only VM at commit 4b9070d, before two streaming-path changes. A 30-minute soak on the 1.0.0 code passed every check except the memory trend (see below) |
| 12 | Real platform accounts | — | **Not run**: no platform accounts or stream keys were available. Pending. |

## Results for 1.0.0

### CI on the release candidate

Runs [40077447542](https://github.com/MalakaiCS/Matchframe-MultiStream/actions/runs/40077447542)
(push) and [40077450493](https://github.com/MalakaiCS/Matchframe-MultiStream/actions/runs/40077450493)
(pull request), commit `464e52d`, every job green on fresh GitHub-hosted runners. The plugin,
packaging and build files there are the 1.0.0 ones except for the version number; the release run
on the 1.0.0 commit repeats every job and publishes only if all of them pass, and the release's
`SHA256SUMS` cover the published files. The first commit with version 1.0.0, `273f977`, passed
the same jobs in runs
[40079328759](https://github.com/MalakaiCS/Matchframe-MultiStream/actions/runs/40079328759) and
[40079331528](https://github.com/MalakaiCS/Matchframe-MultiStream/actions/runs/40079331528).

* **Windows** — image `windows-2025-vs2026` 20261021.278.1 (Windows Server 2025, 10.0.26100),
  Visual Studio Enterprise 2026 18.11, MSVC 19.51.36260, SDK 10.0.26100.0, CMake 4.4.4: build with
  warnings as errors, 0 warnings, 0 errors; unit tests **48 run, 0 failed** (Credential Manager
  included); package content check; official OBS 32.2.2 installed (SHA-256 pinned) and the plugin
  installed with its script; real-OBS UI tests **155 checks, 0 failed**; uninstall and stored-key
  removal verified.
* **macOS** — image `macos-26-arm64` 20261021.0445.8 (macOS 26.6.2, 25G83), Xcode 26.5 (17F42),
  macOS SDK 26.5, AppleClang 21.0.0: universal (`x86_64 arm64`) build with warnings as errors;
  unit tests **47 run, 0 failed** (Keychain included); unsigned `.pkg` inspected and installed
  with `installer`; official OBS 32.2.2 Apple silicon build; real-OBS UI tests **155 checks,
  0 failed**; uninstall and stored-key removal verified.
* **Linux** — image `ubuntu-24.04` 20261012.334.1 (24.04.5), GCC 13.3.0, OBS 32.2.2 built from the
  pinned commit: format check; build with warnings as errors; unit tests passed; libobs
  integration **11 scenarios, 0 failed** (new: `video_reset_between_starts`; plugin output A/V
  offset mean 29.5 ms against 29.2 ms for OBS's own recording made at the same time, a 0.4 ms
  difference); real-OBS UI tests **148 checks, 0 failed**.

### macOS CI: OBS's software OpenGL renderer

The macOS UI job crashed OBS intermittently, mostly in the `crud` scenario and at scattered places
(OBS's RTMP logging, libobs's audio thread, x264, the AAC encoder's destructor), with pointers such
as `0xff000000ff000000`: pixel data written over the heap. A
temporary bisection step repeated the scenario with parts left out and under Guard Malloc. Under
Guard Malloc every run faulted on OBS's graphics thread inside Apple's software OpenGL renderer
(`GLRendererFloat`; the CI virtual machines have no GPU for OpenGL), in `glBlitFramebuffer` called
from libobs-opengl's `device_present`. When a display grows, `device_present` blits at the new size
before the display's framebuffer has been resized, since that resize is queued to the main thread,
and this renderer then writes past its buffers. The `crud` scenario resized the preview most,
because the dock changes size as destinations are added.

The UI driver therefore floats the dock in its own window on macOS, so the preview keeps its size.
The plugin itself is unchanged; on real Macs OpenGL is hardware-accelerated. On run 40077447542
the comparison step gave:

| macOS CI setting | `crud` under Guard Malloc | `program` under Guard Malloc | `program` + `profile_exit` |
| --- | --- | --- | --- |
| Dock floating, preview on (the default now) | 2 runs, 58 checks each, 0 failed | 30 checks, 0 failed | 53 checks, 0 failed |
| Dock docked, preview off | — | — | 4 of 53 failed |
| Dock floating, preview off | — | — | 3 of 53 failed |

With the preview off, OBS on those runners drew about one frame in six. OBS's own native stream,
recording and replay buffer reported 79–81 % of frames lagged, and the plugin's output 84 %. The
programme receiver then got no data after Studio Mode was left, and three of the four profile
sessions recorded 0 frames. With the preview off on Linux (Mesa llvmpipe) the same scenarios passed (53 checks,
0 failed).

### Local runs (4 vCPU Linux VM described below)

* Real-OBS UI tests on the 1.0.0 code: **148 checks, 0 failed**; with the dock floating as on
  macOS (`crud`, `program`, `profile_exit`) **105 checks, 0 failed**; with the preview off
  (`program`, `profile_exit`) **53 checks, 0 failed**.

### 30-minute soak on the 1.0.0 code

Commit `464e52d` (the 1.0.0 plugin sources), 2026-10-24 08:43–09:14 UTC, the same VM and settings
as the 0.1.0 soak except AAC at 128 kbps: 4 destinations to 4 local receivers, 1280x720 @ 30 fps,
6000 kbps CBR, 300 s segments.

| Measure | Result |
| --- | --- |
| Destinations sending | 4 of 4 in all 180 samples and at the end |
| Network | 0 frames dropped, 0 reconnects, RTMP congestion 0.000 throughout |
| Measured outbound | mean 24.0 Mbps (22.2–26.0) |
| Rendering / encoding | 51 lagged frames, 0 encoder skips |
| Process CPU | mean 211 % of 400 %, max 228 % |
| A/V | first segment mean -3.1 ms; drift to the last (partial) segment 38.0 ms, limit 50 ms |
| Memory (RSS) | 366.6 MB at start, 382.5 MB at the end; trend after warm-up **+26.3 MB/hour, limit 20: check failed** |

The memory check failed. Over the same 5–30 minute window the 0.1.0 soak had a trend of
-3.8 MB/hour. RSS did not climb steadily: it was flat from about 4.5 to 17 minutes, then moved in
steps of +4.0, -3.8, +4.4 and +2.6 MB in the last 10 minutes. Steps like these are typical of the
allocator, and the 0.1.0 soak showed smaller ones, but a 30-minute run cannot tell them from a
slow leak. The plugin code that runs while streaming is the same as in the 0.1.0 soak: the
changes since then run at start and stop.

## Results recorded for 0.1.0

These are the runs recorded for the 0.1.0 release; the 2-hour soak below has not been repeated
since.

Environment: Linux VM, 4 vCPU, no GPU (Mesa llvmpipe software rendering), Ubuntu 24.04 container,
OBS Studio 32.2.2 built from the pinned commit, x264 encoder. Test video 1280x720 @ 30 fps because
1080p software rendering plus x264 saturates this CPU-only VM; the plugin defaults (inherit OBS
video, 6000 kbps CBR, 2 s keyframes, AAC 160 kbps 48 kHz stereo) were otherwise unchanged.

### CI on the release candidate

Run [40064797687](https://github.com/MalakaiCS/Matchframe-MultiStream/actions/runs/40064797687),
commit `c0b5d6e`, all three jobs green on fresh GitHub-hosted runners. (The release itself is built
again by the run for the `v0.1.0` tag, which must pass the same jobs before it publishes; the
release's own `SHA256SUMS` cover the published files.)

**Windows** — runner `windows-2025-vs2026`, Visual Studio Enterprise 2026 18.11, MSVC 19.51.36260,
SDK 10.0.26100.0, CMake 4.4.4:

* OBS 32.2.2 sources cloned and verified at commit `ba2f32bd…`; obs-deps and Qt6 2026-07-15
  hash-verified; libobs and obs-frontend-api built from them.
* Build with warnings as errors: 0 warnings, 0 errors (core, plugin, unit tests, UI driver).
* Unit tests: 41 run, 0 failed (including the Windows Credential Manager tests).
* Package `matchframe-multistream-0.1.0-windows-x64.zip` (SHA-256 `7f17f07b…e949` in that run);
  content check passed, no test binaries; `Remove-MatchFrameStoredKeys.ps1` deleted a real
  credential created with `cmdkey`.
* Official **OBS Studio 32.2.2** Windows x64 release downloaded, SHA-256 pinned and matching
  GitHub's published asset digest; plugin installed with `Install-MatchFrameMultistream.ps1`.
* Real-OBS UI tests: **155 checks, 0 failed** — including keys saved to Windows Credential Manager
  with no session-only fallback, both keys restored after an OBS restart and one destination
  streaming with only the restored key, and deleting destinations removing their entries.
* `Uninstall-MatchFrameMultistream.ps1` and `Remove-MatchFrameStoredKeys.ps1 -RemoveSettings`
  left no plugin folder, credential or settings behind.

**macOS** — runner `macos-26` (macOS 26.6.2, arm64), Xcode 26.5 (17F42), macOS SDK 26.5:

* Universal (`x86_64 arm64`) build with warnings as errors, deployment target 13.0.
* Unit tests: 40 run, 0 failed (including the two macOS Keychain tests).
* Installer `matchframe-multistream-0.1.0-macos-universal.pkg` (SHA-256 `2900f5a7…83db` in that
  run, unsigned): expanded and inspected (universal plugin binary, locale data, no test
  binaries), then installed with `installer -target CurrentUserHomeDirectory`.
* Official **OBS Studio 32.2.2** Apple silicon release (`.dmg`), SHA-256 pinned and matching
  GitHub's published asset digest.
* Real-OBS UI tests: **155 checks, 0 failed** — the same scenarios, with keys in the login Keychain.
* `uninstall-matchframe-multistream.sh` and `remove-matchframe-stored-keys.sh --remove-settings`
  left no plugin, Keychain entry or settings behind.

**Linux** — `ubuntu-24.04`, OBS 32.2.2 built from the pinned commit: format check, build with
warnings as errors, unit tests, libobs integration **10 scenarios, 0 failed** (A/V alignment of the
plugin output within 0.7 ms of OBS's own recording made at the same time), real-OBS UI tests
**148 checks, 0 failed** (no secure store on Linux, so the Keychain/Credential Manager checks do
not apply).

### Local runs (4 vCPU VM described above)

* Unit tests: 38/38 passed (repeated three times; also clean under ThreadSanitizer).
* libobs integration: all 10 scenarios passed. Measured examples: two outputs at 6433/6549 kbps from
  one encoder pair (one video and one audio encoder object in OBS); eight outputs, aggregate
  49.5 Mbps against a 49.28 Mbps payload estimate; reconnect attempts after a receiver loss started
  at 2.05, 6.28, 14.50, 30.72 s (policy 2/4/8/16 s); a destination stalled in the RTMP handshake
  reached Idle within OBS's receive timeout while the UI thread's longest task stayed under 1 ms;
  short A/V sync check mean offset 38.9 ms (range 31–47 ms).
* Real-OBS UI tests: crud/restore, eight, program, profile_exit/no_autostart all passed.
* Final local reruns on the release candidate code: libobs integration 10 scenarios, 0 failed,
  clean process exit (no remaining sources at `obs_shutdown`), plugin output A/V alignment 3.8 ms
  from OBS's own recording; a 3-minute soak passed and exited cleanly; real-OBS UI tests
  148 checks, 0 failed.

### Soak (acceptance 11)

120 minutes (7,209 s), 4 destinations to 4 local receivers, 1280x720 @ 30 fps, plugin defaults
6000 kbps CBR + AAC 160 kbps, recordings cut into 300 s segments (every sixth kept). Run on the
4 vCPU VM above, 2026-10-24 01:50–03:51 UTC, on commit 4b9070d. Plugin changes after that
commit (secret scrubbing in encoder error messages, query parameters left out of exports, the
macOS Keychain backend) do not touch the streaming path; the soak was not repeated on the
release commit. The raw samples (every 10 s) are in
[`test-results/0.1.0/soak-metrics.csv`](test-results/0.1.0/soak-metrics.csv) and the run log in
[`test-results/0.1.0/soak-run.log`](test-results/0.1.0/soak-run.log).

| Measure | Result |
| --- | --- |
| Destinations sending | 4 of 4 in all 719 samples and at the end |
| Network | 0 frames dropped, 0 reconnects, RTMP congestion 0.000 throughout |
| Measured outbound | mean 24.1 Mbps (22.4–26.2), i.e. 6.03 Mbps per destination |
| Rendering / encoding | 216,261 frames drawn, 65 lagged (0.03 %), 0 encoder skips |
| Process CPU (OBS core + x264 + 4 outputs, software rendering) | mean 199 % of 400 %, max 238 % |
| Memory (RSS) | 355.1 MB at start, 375.2 MB at the end; trend after warm-up +1.76 MB/hour |
| A/V offset per kept segment | -18.2 ms (0 min), +50.7 (30 min), +41.8 (60 min), +33.2 (90 min); the last, partial segment had only 4 flash/beep pairs (+18.3) |

All soak checks passed: every destination still sending, no material memory growth (limit
20 MB/hour), no growing backlog, and no progressive A/V drift (first vs last kept segment 36.5 ms,
limit 50 ms). Stated plainly, the offset moved within a -18 to +51 ms band during the run rather
than drifting one way.

Caveats: 720p rather than 1080p (software rendering plus x264 on this CPU-only VM cannot sustain
1080p30). The 65 lagged render frames were spread across the run (6 in the first five minutes);
short local builds also ran on the same VM during the soak. That soak binary predated the harness teardown fix described below and crashed inside
`obs_shutdown` after all checks had been printed, so its summary file was not written; the plugin
had already destroyed all of its outputs and both shared encoders cleanly (see the end of the log).

### Known log noise (fixed)

Until commit `075e58f` the headless harness released its scene without removing it, so libobs 32's
main canvas kept the scene and both media sources alive until `obs_shutdown` ("2 source(s) were
remaining", "Double destroy just occurred"), occasionally crashing there. The harness now calls
`obs_source_remove` first. The plugin creates no sources, and the messages never appeared in the
real-OBS UI test logs.

## Not verified

* **Operator hardware.** Windows and macOS were verified on GitHub-hosted CI virtual machines (no
  GPU, no audio devices, nobody at the keyboard), not on an operator's own Windows or Mac.
* **Hardware encoders** (NVENC, Quick Sync, AMF, Apple VideoToolbox). The runtime selection logic
  prefers them, but no test machine had a GPU encoder; everything above used x264.
* **1080p30 on target hardware**, and GPU load during the soak (no GPU available). The 2-hour
  soak predates two streaming-path changes (fresh encoders for each start, stops that wait for
  OBS's send thread). On the 1.0.0 code only a 30-minute soak has been run, and its memory check
  failed (+26 MB/hour, in steps). Whether memory keeps growing over a 2-hour run is not yet known.
* **Plugin output under severe render lag.** On the macOS CI machines with OBS's preview off
  (software OpenGL, about one frame in six drawn), the plugin's output sometimes delivered no
  video after Studio Mode was left or the profile changed. OBS's own outputs were not running at
  those moments, so whether they would have fared the same is not known.
* **Real platforms** (acceptance 12): YouTube, Facebook, Twitch and Kick ingest, Go Live flows,
  platform-side keyframe/bitrate warnings, and RTMPS against public ingest certificates.
* **Browser sources** and capture devices (not used by the tests; the Linux test OBS is built
  without CEF).
* **Signed macOS package**: the `.pkg` is unsigned and not notarized, so Gatekeeper's handling of
  it was not tested beyond `installer(8)`.
* **Linux release and secure storage on Linux** (phase two; keys are session-only there).
