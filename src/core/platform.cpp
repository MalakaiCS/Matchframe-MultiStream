// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "platform.hpp"

namespace mf {

const std::vector<PlatformInfo> &all_platforms()
{
	/* Help links are the official references listed in the product brief. Dashboard links are
	 * the stream_key_link values from OBS 32.2.2's plugins/rtmp-services/data/services.json
	 * (Kick has no entry there, so its home page is used and no constraint data exists). */
	static const std::vector<PlatformInfo> platforms = {
		{Platform::YouTube,
		 "youtube",
		 "YouTube",
		 "https://support.google.com/youtube/answer/10364924",
		 "https://www.youtube.com/live_dashboard",
		 "YouTube - RTMPS",
		 "rtmps://<ingest host>:443/<application>",
		 "YouTube Studio may still require you to press Go Live (or to enable auto-start) before viewers can see the stream.",
		 true,
		 false,
		 {"youtube.com"}},
		{Platform::Facebook,
		 "facebook",
		 "Facebook",
		 "https://www.facebook.com/business/help/165076674943644",
		 "https://www.facebook.com/live/producer",
		 "Facebook Live",
		 "rtmps://<ingest host>:443/rtmp/",
		 "Facebook Live Producer normally shows a preview first; press Go Live there to publish.",
		 true,
		 true,
		 {"facebook.com", "fbcdn.net"}},
		{Platform::Twitch,
		 "twitch",
		 "Twitch",
		 "https://dev.twitch.tv/docs/video-broadcast/",
		 "https://dashboard.twitch.tv/settings/stream",
		 "Twitch",
		 "rtmps://<ingest host>:443/app",
		 "Confirm in the Twitch creator dashboard that the channel shows as live.",
		 false,
		 false,
		 {"twitch.tv", "live-video.net"}},
		{Platform::Kick,
		 "kick",
		 "Kick",
		 "https://help.kick.com/en/articles/7066931-how-to-stream-on-kick-com",
		 "https://kick.com/",
		 "",
		 "rtmps://<ingest host>:443/app/",
		 "Confirm in the Kick creator dashboard that the stream is live.",
		 true,
		 false,
		 {"live-video.net", "kick.com"}},
		{Platform::Custom,
		 "custom",
		 "Custom RTMP/RTMPS",
		 "",
		 "",
		 "",
		 "rtmp(s)://<host>[:port]/<application>",
		 "A connected server does not prove that any viewer-facing platform has published the stream.",
		 false,
		 false,
		 {}},
	};
	return platforms;
}

const PlatformInfo &platform_info(Platform platform)
{
	for (const PlatformInfo &info : all_platforms()) {
		if (info.platform == platform)
			return info;
	}
	return all_platforms().back();
}

bool platform_from_id(const std::string &id, Platform &out)
{
	for (const PlatformInfo &info : all_platforms()) {
		if (id == info.id) {
			out = info.platform;
			return true;
		}
	}
	return false;
}

const char *platform_id(Platform platform)
{
	return platform_info(platform).id;
}

} // namespace mf
