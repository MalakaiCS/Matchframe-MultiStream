#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - builds the pinned OBS Studio release from source on Ubuntu 24.04
# for the Linux test suites (libobs integration tests and real-OBS UI tests).
#
# Usage: tests/scripts/build-obs-linux.sh <work-dir> <install-prefix>
# Requires the apt packages listed in docs/BUILDING.md.

set -euo pipefail

OBS_TAG="32.2.2"
OBS_COMMIT="ba2f32bdf791005443988a4955e963663e16b1ed" # peeled commit of tag 32.2.2

work="${1:?work dir}"
prefix="${2:?install prefix}"
src="${work}/obs-studio-${OBS_TAG}"

if [[ ! -d "${src}/.git" ]]; then
  git -c advice.detachedHead=false clone --depth 1 --branch "${OBS_TAG}" https://github.com/obsproject/obs-studio.git "${src}"
fi
actual="$(git -C "${src}" rev-parse HEAD)"
if [[ "${actual}" != "${OBS_COMMIT}" ]]; then
  echo "OBS ${OBS_TAG} resolved to ${actual}, expected ${OBS_COMMIT}" >&2
  exit 1
fi
# obs-browser and obs-websocket must be present even though both are disabled below.
git -C "${src}" submodule update --init --depth 1 plugins/obs-browser plugins/obs-websocket

cmake -S "${src}" -B "${src}/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="${prefix}" \
  -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF -DENABLE_SCRIPTING=OFF -DENABLE_VLC=OFF \
  -DENABLE_AJA=OFF -DENABLE_DECKLINK=OFF -DENABLE_WEBRTC=OFF -DENABLE_NVENC=OFF -DENABLE_QSV11=OFF \
  -DENABLE_VST=OFF -DENABLE_NEW_MPEGTS_OUTPUT=OFF -DENABLE_SERVICE_UPDATES=OFF -DENABLE_PIPEWIRE=OFF \
  -DENABLE_WAYLAND=OFF -DENABLE_PORTABLE_CONFIG=ON -DENABLE_V4L2=OFF -DENABLE_JACK=OFF -DENABLE_SNDIO=OFF \
  -DENABLE_CCACHE=ON
cmake --build "${src}/build" --parallel
cmake --install "${src}/build"
echo "OBS ${OBS_TAG} (${OBS_COMMIT}) installed to ${prefix}"
