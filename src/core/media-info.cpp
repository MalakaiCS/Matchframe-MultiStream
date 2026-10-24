// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "media-info.hpp"

#include <cmath>
#include <cstdio>

namespace mf {

const char *video_format_name(video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
		return "NV12";
	case VIDEO_FORMAT_I420:
		return "I420";
	case VIDEO_FORMAT_I444:
		return "I444";
	case VIDEO_FORMAT_P010:
		return "P010";
	case VIDEO_FORMAT_I010:
		return "I010";
	case VIDEO_FORMAT_P216:
		return "P216";
	case VIDEO_FORMAT_P416:
		return "P416";
	case VIDEO_FORMAT_BGRA:
		return "BGRA";
	case VIDEO_FORMAT_RGBA:
		return "RGBA";
	default:
		return "other";
	}
}

const char *speaker_layout_name(speaker_layout speakers)
{
	switch (speakers) {
	case SPEAKERS_MONO:
		return "Mono";
	case SPEAKERS_STEREO:
		return "Stereo";
	case SPEAKERS_2POINT1:
		return "2.1";
	case SPEAKERS_4POINT0:
		return "4.0";
	case SPEAKERS_4POINT1:
		return "4.1";
	case SPEAKERS_5POINT1:
		return "5.1";
	case SPEAKERS_7POINT1:
		return "7.1";
	default:
		return "Unknown";
	}
}

std::string MediaInfo::describeVideo() const
{
	if (!videoAvailable)
		return "OBS video is not initialised";
	char buffer[96];
	double rate = fps();
	if (std::fabs(rate - std::round(rate)) < 0.005)
		snprintf(buffer, sizeof(buffer), "%ux%u @ %.0f fps", outputWidth, outputHeight, rate);
	else
		snprintf(buffer, sizeof(buffer), "%ux%u @ %.2f fps", outputWidth, outputHeight, rate);
	return buffer;
}

std::string MediaInfo::describeAudio() const
{
	if (!audioAvailable)
		return "OBS audio is not initialised";
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.1f kHz %s", sampleRate / 1000.0, speaker_layout_name(speakers));
	return buffer;
}

MediaInfo read_media_info()
{
	MediaInfo info;
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi)) {
		info.videoAvailable = true;
		info.baseWidth = ovi.base_width;
		info.baseHeight = ovi.base_height;
		info.outputWidth = ovi.output_width;
		info.outputHeight = ovi.output_height;
		info.fpsNum = ovi.fps_num;
		info.fpsDen = ovi.fps_den ? ovi.fps_den : 1;
		info.format = ovi.output_format;
		info.colorspace = ovi.colorspace;
	}
	obs_audio_info oai = {};
	if (obs_get_audio_info(&oai)) {
		info.audioAvailable = true;
		info.sampleRate = oai.samples_per_sec;
		info.speakers = oai.speakers;
	}
	return info;
}

Issues validate_media(const MediaInfo &info)
{
	Issues issues;
	if (!info.videoAvailable) {
		issues.push_back({Severity::Error, "video", "OBS video is not initialised."});
	} else {
		if (info.format != VIDEO_FORMAT_NV12 && info.format != VIDEO_FORMAT_I420) {
			issues.push_back(
				{Severity::Error, "video",
				 std::string("OBS color format is ") + video_format_name(info.format) +
					 ". The shared H.264 profile needs 8-bit 4:2:0 video: set OBS Settings > "
					 "Advanced > Color Format to NV12."});
		}
		if (info.colorspace == VIDEO_CS_2100_PQ || info.colorspace == VIDEO_CS_2100_HLG) {
			issues.push_back(
				{Severity::Error, "video",
				 "OBS is set to an HDR color space (Rec. 2100). The shared H.264 profile is SDR "
				 "only: choose Rec. 709 in OBS Settings > Advanced."});
		}
		if (info.fps() > 60.01) {
			issues.push_back({Severity::Warning, "video",
					  "OBS runs above 60 fps; most platforms accept at most 60 fps."});
		}
	}

	if (!info.audioAvailable) {
		issues.push_back({Severity::Error, "audio", "OBS audio is not initialised."});
	} else {
		int channels = (int)get_audio_channels(info.speakers);
		if (channels > 2) {
			issues.push_back({Severity::Error, "audio",
					  std::string("OBS audio is ") + speaker_layout_name(info.speakers) +
						  ". Plugin destinations need stereo AAC: set OBS Settings > Audio > "
						  "Channels to Stereo (requires an OBS restart)."});
		} else if (channels == 1) {
			issues.push_back(
				{Severity::Warning, "audio", "OBS audio is mono; destinations will receive mono AAC."});
		}
		if (info.sampleRate != 48000) {
			issues.push_back(
				{Severity::Warning, "audio",
				 "OBS audio runs at " + std::to_string(info.sampleRate) +
					 " Hz. The shared AAC profile uses OBS's sample rate; 48 kHz is "
					 "recommended (OBS Settings > Audio > Sample Rate, requires a restart)."});
		}
	}
	return issues;
}

} // namespace mf
