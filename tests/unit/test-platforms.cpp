// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Server URLs and stream keys as the platforms hand them out, checked against the plugin's
// validation. URLs come from OBS 32.2.2's own services.json (tests/data), never from memory.

#include "common/test-framework.hpp"

#include "core/destination-config.hpp"
#include "core/profile-config.hpp"
#include "core/redaction.hpp"
#include "core/rtmp-url.hpp"

#include <obs.hpp>

#include <cstdio>
#include <string>

using namespace mf;

namespace {

bool has_issue(const Issues &issues, Severity severity, const std::string &needle)
{
	for (const Issue &issue : issues) {
		if (issue.severity == severity && issue.message.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

Platform platform_for_service(const std::string &service)
{
	if (service == "YouTube - RTMPS")
		return Platform::YouTube;
	if (service == "Facebook Live")
		return Platform::Facebook;
	if (service == "Twitch")
		return Platform::Twitch;
	return Platform::Custom;
}

DestinationConfig destination(Platform platform, const std::string &url)
{
	DestinationConfig d;
	d.id = new_uuid();
	d.name = "Test";
	d.platform = platform;
	d.serverUrl = url;
	return d;
}

} // namespace

MF_TEST(every_rtmp_server_in_obs_services_json_is_accepted)
{
	std::string path = std::string(MF_TEST_DATA_DIR) + "/obs-32.2.2-services.json";
	OBSDataAutoRelease root = obs_data_create_from_json_file(path.c_str());
	REQUIRE(root != nullptr);
	OBSDataArrayAutoRelease services = obs_data_get_array(root, "services");
	REQUIRE(services != nullptr);

	size_t checked = 0;
	for (size_t i = 0; i < obs_data_array_count(services); i++) {
		OBSDataAutoRelease service = obs_data_array_item(services, i);
		std::string name = obs_data_get_string(service, "name");
		OBSDataArrayAutoRelease servers = obs_data_get_array(service, "servers");
		for (size_t j = 0; servers && j < obs_data_array_count(servers); j++) {
			OBSDataAutoRelease server = obs_data_array_item(servers, j);
			std::string url = obs_data_get_string(server, "url");
			RtmpUrl parsed = parse_rtmp_url(url);
			if (!parsed.parsed || !parsed.isRtmpFamily())
				continue; // https:// (HLS) and the like are not RTMP ingest
			Platform platform = platform_for_service(name);
			Issues issues = validate_server_url(url, platform);
			if (has_errors(issues))
				std::printf("      rejected %s: %s (%s)\n", name.c_str(), url.c_str(),
					    first_error(issues).c_str());
			CHECK(!has_errors(issues));
			/* The host and application stay readable; nothing after them leaks into messages. */
			CHECK(redact_url(url).find(parsed.host) != std::string::npos);
			checked++;
		}
	}
	CHECK(checked >= 500); // 536 RTMP/RTMPS servers across 84 services in OBS 32.2.2
}

MF_TEST(platform_rules_for_youtube_facebook_twitch_kick)
{
	/* Facebook refuses plain RTMP; OBS's own Facebook service forces RTMPS as well. */
	CHECK(has_issue(validate_server_url("rtmp://live-api-s.facebook.com:80/rtmp/", Platform::Facebook),
			Severity::Error, "accepts only RTMPS"));
	CHECK(!has_errors(validate_server_url("rtmps://live-api-s.facebook.com:443/rtmp/", Platform::Facebook)));
	CHECK(!has_errors(validate_server_url("rtmps://rtmp-api.facebook.com:443/rtmp/", Platform::Facebook)));

	/* YouTube and Kick dashboards offer rtmps:// URLs: plain RTMP works but is warned about. */
	Issues youtube = validate_server_url("rtmp://a.rtmp.youtube.com/live2", Platform::YouTube);
	CHECK(!has_errors(youtube));
	CHECK(has_issue(youtube, Severity::Warning, "supports RTMPS"));
	Issues kick = validate_server_url("rtmp://example.global-contribute.live-video.net/app", Platform::Kick);
	CHECK(!has_errors(kick));
	CHECK(has_issue(kick, Severity::Warning, "supports RTMPS"));

	/* Twitch's dashboard shows no ingest URL, so there is nothing to "prefer". */
	Issues twitch = validate_server_url("rtmp://live-hkg.twitch.tv/app", Platform::Twitch);
	CHECK(!has_errors(twitch));
	CHECK(!has_issue(twitch, Severity::Warning, "RTMPS"));

	/* Named platforms always use an application path; a custom server may not. */
	CHECK(has_issue(validate_server_url("rtmps://a.rtmps.youtube.com:443", Platform::YouTube), Severity::Error,
			"application path"));
	Issues bare = validate_server_url("rtmp://stream.example.net", Platform::Custom);
	CHECK(!has_errors(bare));
	CHECK(has_issue(bare, Severity::Warning, "no RTMP application path"));
}

MF_TEST(pasted_urls_with_placeholders_or_invisible_characters_are_rejected)
{
	CHECK(has_issue(validate_server_url("rtmp://jfk50.contribute.live-video.net/app/{stream_key}",
					    Platform::Twitch),
			Severity::Error, "{stream_key}"));
	/* A no-break space after the URL and a zero-width space inside it, as copied from web pages. */
	CHECK(has_issue(validate_server_url("rtmp://a.rtmp.youtube.com/live2\xC2\xA0", Platform::YouTube),
			Severity::Error, "invisible"));
	CHECK(has_issue(validate_server_url("rtmp://a.rtmp.youtube.com/\xE2\x80\x8Blive2", Platform::YouTube),
			Severity::Error, "invisible"));
}

MF_TEST(query_parameters_are_warned_about_except_youtube_backup)
{
	Issues token = validate_server_url("rtmps://ingest.example.com/app?token=abc", Platform::Custom);
	CHECK(!has_errors(token));
	CHECK(has_issue(token, Severity::Warning, "writes the full server URL to the OBS log"));
	Issues backup = validate_server_url("rtmps://b.rtmps.youtube.com:443/live2?backup=1", Platform::YouTube);
	CHECK(!has_errors(backup));
	CHECK(!has_issue(backup, Severity::Warning, "query"));

	CHECK_EQ(public_query("backup=1"), std::string("backup=1"));
	CHECK_EQ(public_query("backup=1&token=abc"), std::string("backup=1"));
	CHECK_EQ(public_query("token=abc&backup=1"), std::string("backup=1"));
	CHECK_EQ(public_query("token=abc"), std::string());
	CHECK_EQ(public_query("backupx=1&xbackup=2"), std::string());
	CHECK_EQ(public_query(""), std::string());
}

MF_TEST(stream_keys_as_platforms_issue_them_are_accepted)
{
	DestinationConfig d = destination(Platform::Facebook, "rtmps://live-api-s.facebook.com:443/rtmp/");
	const char *keys[] = {
		"abcd-efgh-ijkl-mnop-qrst",                                    // YouTube
		"FB-1234567890123456-0-AbCdEfGhIjKlMnOp",                      // Facebook persistent key
		"1234567890123456?s_bl=1&s_psm=1&s_sc=1234567890&s_sw=0&a=Ab", // older Facebook key
		"live_123456789_AbCdEfGhIjKlMnOpQrStUvWxYz",                   // Twitch
		"sk_us-west-2_AbCdEf123456",                                   // Kick (IVS)
		"key/with:colon+plus%25",
	};
	for (const char *key : keys) {
		DestinationSecrets secrets;
		secrets.streamKey = key;
		CHECK(!has_errors(validate_secrets(d, secrets)));
	}
	DestinationSecrets invisible;
	invisible.streamKey = std::string("abcd-efgh") + "\xE2\x80\x8B" + "-ijkl"; // zero-width space
	CHECK(has_issue(validate_secrets(d, invisible), Severity::Error, "invisible"));
}

MF_TEST(export_keeps_youtube_backup_flag_and_drops_other_query_parameters)
{
	ProfileConfig config;
	config.profileId = new_uuid();
	config.destinations.push_back(destination(Platform::YouTube, "rtmps://b.rtmps.youtube.com:443/live2?backup=1"));
	config.destinations.push_back(
		destination(Platform::Custom, "rtmps://ingest.example.com/app?backup=1&t=SeCrEt"));
	config.destinations.push_back(destination(Platform::YouTube, "rtmps://a.rtmps.youtube.com:443/live2"));

	int removed = -1;
	std::string exported = serialize_export(config, &removed);
	CHECK_EQ(removed, 1);
	CHECK(exported.find("SeCrEt") == std::string::npos);
	ImportResult imported = parse_import(exported);
	REQUIRE(imported.ok);
	REQUIRE(imported.destinations.size() == 3);
	CHECK_EQ(imported.destinations[0].serverUrl, std::string("rtmps://b.rtmps.youtube.com:443/live2?backup=1"));
	CHECK_EQ(imported.destinations[1].serverUrl, std::string("rtmps://ingest.example.com/app?backup=1"));
	CHECK_EQ(imported.destinations[2].serverUrl, std::string("rtmps://a.rtmps.youtube.com:443/live2"));
}
