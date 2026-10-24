// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "encoder-profile.hpp"

#include <algorithm>

namespace mf {

const std::vector<int> &supported_audio_bitrates()
{
	static const std::vector<int> bitrates = {64, 96, 128, 160, 192, 256, 320};
	return bitrates;
}

Issues validate_encoder_profile(const EncoderProfile &profile)
{
	Issues issues;
	if (profile.videoBitrateKbps < kMinVideoBitrateKbps || profile.videoBitrateKbps > kMaxVideoBitrateKbps)
		issues.push_back(
			{Severity::Error, "videoBitrate", "Video bitrate must be between 500 and 51000 kbps."});
	if (profile.keyframeIntervalSec < kMinKeyframeSec || profile.keyframeIntervalSec > kMaxKeyframeSec)
		issues.push_back({Severity::Error, "keyframeInterval", "Keyframe interval must be 1-10 seconds."});
	else if (profile.keyframeIntervalSec != 2)
		issues.push_back({Severity::Warning, "keyframeInterval",
				  "YouTube, Facebook and Twitch recommend a 2 second keyframe interval."});
	const std::vector<int> &bitrates = supported_audio_bitrates();
	if (std::find(bitrates.begin(), bitrates.end(), profile.audioBitrateKbps) == bitrates.end())
		issues.push_back({Severity::Error, "audioBitrate", "Unsupported AAC bitrate."});
	if (profile.audioTrack < 1 || profile.audioTrack > kAudioTrackCount)
		issues.push_back({Severity::Error, "audioTrack", "Audio track must be 1-6."});
	if (profile.audioEncoderId.empty())
		issues.push_back({Severity::Error, "audioEncoder", "An AAC audio encoder must be selected."});
	return issues;
}

} // namespace mf
