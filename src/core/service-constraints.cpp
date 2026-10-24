// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "service-constraints.hpp"

#include <obs.hpp>

#include <cstring>

namespace mf {

namespace {

bool service_type_registered(const char *wanted)
{
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_service_types(i, &id); i++) {
		if (id && strcmp(id, wanted) == 0)
			return true;
	}
	return false;
}

} // namespace

PlatformConstraints query_platform_constraints(Platform platform)
{
	PlatformConstraints constraints;
	const PlatformInfo &info = platform_info(platform);
	if (!info.obsServiceName || !*info.obsServiceName || !service_type_registered("rtmp_common"))
		return constraints;
	constraints.serviceName = info.obsServiceName;

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "service", info.obsServiceName);
	OBSServiceAutoRelease service =
		obs_service_create_private("rtmp_common", "matchframe-constraints-probe", settings);
	if (!service)
		return constraints;

	int video = 0;
	int audio = 0;
	obs_service_get_max_bitrate(service, &video, &audio);
	constraints.maxVideoKbps = video;
	constraints.maxAudioKbps = audio;

	int fps = 0;
	obs_service_get_max_fps(service, &fps);
	constraints.maxFps = fps;

	obs_service_resolution *resolutions = nullptr;
	size_t count = 0;
	obs_service_get_supported_resolutions(service, &resolutions, &count);
	for (size_t i = 0; i < count; i++)
		constraints.resolutions.emplace_back(resolutions[i].cx, resolutions[i].cy);
	bfree(resolutions);

	const char **codecs = obs_service_get_supported_video_codecs(service);
	for (size_t i = 0; codecs && codecs[i]; i++)
		constraints.videoCodecs.emplace_back(codecs[i]);

	constraints.known = video > 0 || audio > 0 || fps > 0 || count > 0 || !constraints.videoCodecs.empty();
	return constraints;
}

Issues check_platform_constraints(Platform platform, const PlatformConstraints &constraints,
				  const EncoderProfile &profile, const MediaInfo &media)
{
	Issues issues;
	const char *label = platform_info(platform).label;
	if (!constraints.known)
		return issues;

	auto warn = [&](const std::string &message) {
		issues.push_back({Severity::Warning, "constraints",
				  std::string(label) + ": " + message +
					  " (OBS service recommendation; choose compatible shared settings)."});
	};

	if (!constraints.videoCodecs.empty()) {
		bool h264 = false;
		for (const std::string &codec : constraints.videoCodecs)
			h264 = h264 || codec == "h264";
		if (!h264)
			issues.push_back({Severity::Error, "constraints",
					  std::string(label) + " does not list H.264 as a supported codec."});
	}
	if (constraints.maxVideoKbps > 0 && profile.videoBitrateKbps > constraints.maxVideoKbps)
		warn("video bitrate " + std::to_string(profile.videoBitrateKbps) + " kbps is above the recommended " +
		     std::to_string(constraints.maxVideoKbps) + " kbps for " + media.describeVideo());
	if (constraints.maxAudioKbps > 0 && profile.audioBitrateKbps > constraints.maxAudioKbps)
		warn("audio bitrate " + std::to_string(profile.audioBitrateKbps) + " kbps is above the recommended " +
		     std::to_string(constraints.maxAudioKbps) + " kbps");
	if (constraints.maxFps > 0 && media.videoAvailable && media.fps() > constraints.maxFps + 0.01)
		warn("frame rate is above the recommended " + std::to_string(constraints.maxFps) + " fps");
	if (!constraints.resolutions.empty() && media.videoAvailable) {
		bool listed = false;
		for (const auto &resolution : constraints.resolutions)
			listed = listed || (resolution.first == (int)media.outputWidth &&
					    resolution.second == (int)media.outputHeight);
		if (!listed)
			warn("output resolution " + std::to_string(media.outputWidth) + "x" +
			     std::to_string(media.outputHeight) + " is not in the supported list");
	}
	return issues;
}

} // namespace mf
