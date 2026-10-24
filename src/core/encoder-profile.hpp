// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "validation.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace mf {

inline constexpr int kMinVideoBitrateKbps = 500;
inline constexpr int kMaxVideoBitrateKbps = 51000;
inline constexpr int kMinKeyframeSec = 1;
inline constexpr int kMaxKeyframeSec = 10;
inline constexpr int kAudioTrackCount = 6;

const std::vector<int> &supported_audio_bitrates();

/*
 * The one shared profile used by every plugin destination (MVP). Resolution and frame
 * rate are not part of it: they are inherited, read-only, from OBS's output settings.
 */
struct EncoderProfile {
	std::string videoEncoderId;  // empty: pick a hardware H.264 encoder, else x264, at runtime
	int videoBitrateKbps = 6000; // CBR
	int keyframeIntervalSec = 2;
	std::string audioEncoderId = "ffmpeg_aac";
	int audioBitrateKbps = 160;
	int audioTrack = 1; // as labelled in OBS (1-6)

	/* libobs mixer index for the selected track (Track 1 -> 0 ... Track 6 -> 5). */
	size_t mixerIndex() const { return (size_t)(audioTrack - 1); }
	bool operator==(const EncoderProfile &other) const = default;
};

Issues validate_encoder_profile(const EncoderProfile &profile);

} // namespace mf
