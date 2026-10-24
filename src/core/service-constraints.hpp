// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "encoder-profile.hpp"
#include "media-info.hpp"
#include "platform.hpp"
#include "validation.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mf {

/*
 * Recommendations for a platform taken from the running OBS's own service database
 * (rtmp-services "rtmp_common", queried through a temporary private service). They are
 * OBS's maintained recommendations, not a guarantee of what a given account accepts.
 */
struct PlatformConstraints {
	bool known = false;
	std::string serviceName;
	int maxVideoKbps = 0;
	int maxAudioKbps = 0;
	int maxFps = 0;
	std::vector<std::pair<int, int>> resolutions;
	std::vector<std::string> videoCodecs;
};

PlatformConstraints query_platform_constraints(Platform platform);

/* Warnings only: the operator must choose compatible shared settings (one shared encoder). */
Issues check_platform_constraints(Platform platform, const PlatformConstraints &constraints,
				  const EncoderProfile &profile, const MediaInfo &media);

} // namespace mf
