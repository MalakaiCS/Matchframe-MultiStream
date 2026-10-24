// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

namespace mf {

inline constexpr double kUploadAllowance = 1.30;

/*
 * Planning estimate for the plugin's own outputs only (not a speed test, not a guarantee):
 *   payload_upload_mbps     = N x (video_kbps + audio_kbps) / 1000
 *   recommended_upload_mbps = payload_upload_mbps x 1.30
 * Native OBS streaming/recording and other traffic on the link need additional capacity.
 */
struct BandwidthEstimate {
	int destinations = 0;
	double payloadMbps = 0.0;
	double recommendedMbps = 0.0;
};

inline BandwidthEstimate estimate_bandwidth(int destinations, int videoKbps, int audioKbps)
{
	BandwidthEstimate estimate;
	estimate.destinations = destinations < 0 ? 0 : destinations;
	estimate.payloadMbps = estimate.destinations * (double)(videoKbps + audioKbps) / 1000.0;
	estimate.recommendedMbps = estimate.payloadMbps * kUploadAllowance;
	return estimate;
}

} // namespace mf
