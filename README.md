# MatchFrame Multistream

An OBS Studio plugin that sends the **same finished OBS Program feed** (scene cuts, transitions,
graphics, replays and the selected audio track) to up to **eight RTMP/RTMPS destinations at once**
from one computer, without a cloud relay. Several destinations may be on the same platform.

* Native OBS module (C++20, libobs, obs-frontend-api, Qt 6), built from the official
  [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
* Target: **OBS Studio 32.2.2, Windows 10/11 x64**. Linux builds are used for the test suites.
* One plugin-owned H.264 encoder and one AAC encoder shared by every destination, each destination
  with its own RTMP output, state, statistics and reconnect policy.
* Stream keys and server passwords live in Windows Credential Manager, never in plugin files, logs,
  exports or diagnostics.
* No accounts, platform APIs, telemetry or cloud services. Platform choices (YouTube, Facebook,
  Twitch, Kick, Custom) only provide setup help and dashboard links; the operator pastes the server
  URL and stream key from each dashboard.

**"Sending to server" means the ingest server accepted the stream, not that a platform published
it.** Confirm every broadcast in the platform's own dashboard.

## Documentation

| Document | For |
| --- | --- |
| [docs/INSTALL.md](docs/INSTALL.md) | Installing, uninstalling and removing stored keys on Windows |
| [docs/OPERATOR_GUIDE.md](docs/OPERATOR_GUIDE.md) | Running a multi-destination broadcast from the dock |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Components, threading, lifecycle and OBS integration |
| [docs/BUILDING.md](docs/BUILDING.md) | Pinned toolchain and reproducible build commands |
| [docs/TESTING.md](docs/TESTING.md) | The test suites, what was actually run, and what is still unverified |

## Status

Version 0.1.0 (MVP). See [docs/TESTING.md](docs/TESTING.md) for exactly which checks have been
run and which acceptance items remain open (notably the Windows GUI run on real hardware and live
platform accounts).

## Licence

GPL-2.0-or-later (see [LICENSE](LICENSE)), compatible with OBS Studio.
