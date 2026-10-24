// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <string>
#include <vector>

namespace mf {

/*
 * Platform labels are setup guidance only. They never imply that an account is
 * eligible, that an event exists, or that the platform publishes the broadcast:
 * every destination streams to the server URL the operator typed in.
 */
enum class Platform { YouTube, Facebook, Twitch, Kick, Custom };

struct PlatformInfo {
	Platform platform;
	const char *id;             // stable JSON identifier
	const char *label;          // UI label
	const char *helpUrl;        // official ingest/encoder setup article
	const char *dashboardUrl;   // where the operator confirms publication
	const char *obsServiceName; // entry in OBS's rtmp-services database used for constraint checks, or ""
	const char *serverExample;  // shape of the URL only, never used as a default
	const char *goLiveNote;     // reminder that transport != publication
	bool supportsRtmps;
	std::vector<std::string> hostHints; // informational host sanity check
};

const std::vector<PlatformInfo> &all_platforms();
const PlatformInfo &platform_info(Platform platform);
bool platform_from_id(const std::string &id, Platform &out);
const char *platform_id(Platform platform);

} // namespace mf
