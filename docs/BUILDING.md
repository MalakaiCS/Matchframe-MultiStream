# Building and compatibility

## Pinned target

| Item | Pinned value | Source of truth |
| --- | --- | --- |
| OBS Studio | **32.2.2**, tag commit `ba2f32bdf791005443988a4955e963663e16b1ed` (tagged 2026-08-14) | `buildspec.json` (`commit`), `tests/scripts/build-obs-linux.sh` |
| Windows dependencies | obs-deps **2026-07-15** (`windows-deps-2026-07-15-x64.zip`, SHA-256 `6f90e959…2ba9`) | `buildspec.json`, copied from OBS 32.2.2 `CMakePresets.json` |
| Qt (Windows) | obs-deps Qt6 **2026-07-15** (`windows-deps-qt6-2026-07-15-x64.zip`, SHA-256 `7c7f9857…5579`) — the Qt build OBS 32.2.2 ships with | as above |
| Windows compiler | Visual Studio 2026 (generator "Visual Studio 18 2026"), Windows SDK **10.0.26100.0**. Verified in CI on runner `windows-2025-vs2026`: Visual Studio Enterprise 2026 18.11.12315.225, MSVC **19.51.36260** (toolset 14.51.36231), x64 | `CMakePresets.json`, CI log ("Record toolchain", "Configuring") |
| C/C++ standard | C17 / C++20 | `cmake/common/compiler_common.cmake` |
| CMake | ≥ 3.28 (policy range 3.28...3.30). Windows CI used CMake 4.4.4; the local Ubuntu 24.04 test machine used 3.28.3 | `CMakeLists.txt` |
| Plugin template | obsproject/obs-plugintemplate commit `3e7d7ac` (GPL-2.0) | this file |
| Linux test platform | Ubuntu 24.04, GCC 13.3, Qt 6.4.2 (Ubuntu packages), FFmpeg 6.1, mbedTLS 2.28 | `.github/workflows/ci.yaml` |

OBS 33.0.0 was tagged on 2026-10-23, one day before this work, and was deliberately not chosen as the
production pin. The online OBS documentation tracks the newest release; the API calls used here were
checked against the 32.2.2 headers and sources (`libobs/obs.h`, `libobs/obs-output.c`,
`libobs/obs-encoder.c`, `plugins/obs-outputs/rtmp-stream.c`, `frontend/api/obs-frontend-api.h`).

**Why the OBS sources are pinned by commit:** GitHub's generated source archives are not
byte-reproducible (the archive for tag 32.2.2 could not be reproduced locally with `git archive`),
so the template's dependency step was extended to shallow-clone the tag and fail unless `HEAD`
equals the pinned commit.

## Windows x64 (release build)

Requirements: Windows 10/11 x64, Visual Studio 2026 with "Desktop development with C++" and the
Windows 10.0.26100 SDK, CMake ≥ 3.28 that knows the VS 2026 generator, Git, PowerShell 7.

```powershell
git clone https://github.com/MalakaiCS/Matchframe-MultiStream.git
cd Matchframe-MultiStream
$env:CI = 'true'                       # the template scripts require it
.github/scripts/Build-Windows.ps1 -Target x64 -Configuration RelWithDebInfo
.github/scripts/Package-Windows.ps1 -Target x64 -Configuration RelWithDebInfo
# -> release/matchframe-multistream-0.1.0-windows-x64.zip
```

The first configure downloads obs-deps and Qt6 (hash-verified), clones OBS 32.2.2 (commit-verified)
into `.deps/`, and builds libobs and obs-frontend-api from those sources (Debug and Release), as the
template does. `windows-ci-x64` treats warnings as errors and also builds the unit tests; run them
with the runtime DLLs on `PATH` as in `.github/workflows/ci.yaml` ("Unit tests" step).

## Linux (development and the test suites)

Linux builds exist to run the tests; secure credential storage on Linux is phase two (keys are
session-only there).

```bash
sudo apt-get install -y --no-install-recommends build-essential cmake ninja-build ccache git \
  extra-cmake-modules libglib2.0-dev libcurl4-openssl-dev libavcodec-dev libavdevice-dev libavfilter-dev \
  libavformat-dev libavutil-dev libswresample-dev libswscale-dev libjansson-dev libx11-xcb-dev \
  libgles2-mesa-dev libwayland-dev libpulse-dev libx264-dev libmbedtls-dev libgl1-mesa-dev uthash-dev \
  libsimde-dev libx11-dev libxcb-randr0-dev libxcb-shm0-dev libxcb-xinerama0-dev libxcb-composite0-dev \
  libxinerama-dev libxcb1-dev libxcb-xfixes0-dev libxss-dev libglvnd-dev libxkbcommon-dev \
  libxcomposite-dev libxdamage-dev libasound2-dev libfontconfig-dev libfreetype6-dev libspeexdsp-dev \
  libudev-dev libva-dev libpci-dev libdrm-dev nlohmann-json3-dev qt6-base-dev libqt6svg6-dev \
  qt6-base-private-dev uuid-dev xvfb xauth ffmpeg nginx libnginx-mod-rtmp openssl python3

# 1. OBS Studio 32.2.2 from the pinned commit (about 15 minutes on 4 cores)
tests/scripts/build-obs-linux.sh "$HOME/obs-src" "$HOME/obs-install"

# 2. Plugin, tests and the test-only UI driver
cmake --preset ubuntu-ci-x86_64 -DCMAKE_PREFIX_PATH="$HOME/obs-install" -DCMAKE_INSTALL_PREFIX="$HOME/obs-install"
cmake --build build_x86_64
cmake --install build_x86_64
cp build_x86_64/tests/ui-driver/mf-ui-driver.so "$HOME/obs-install/lib/obs-plugins/"
mkdir -p "$HOME/obs-install/share/obs/obs-plugins/mf-ui-driver"
```

See [TESTING.md](TESTING.md) for running the suites.

## Formatting

`clang-format` with the template's `.clang-format` (CI uses clang-format 18):

```bash
find src tests \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-format --style=file -i
```

## Licence

GPL-2.0-or-later, the same licence family as OBS Studio and the plugin template (see `LICENSE`).
The CMake build system and the Windows build/package scripts come from obs-plugintemplate (GPL-2.0)
with the changes noted in this file; no other third-party code is copied into this repository.
