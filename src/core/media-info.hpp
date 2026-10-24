// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "validation.hpp"

#include <obs.h>

#include <cstdint>
#include <string>

namespace mf {

/*
 * The OBS video/audio configuration the shared encoders inherit. Read-only: the plugin
 * never calls obs_reset_video()/obs_reset_audio() and never edits OBS settings.
 */
struct MediaInfo {
	bool videoAvailable = false;
	uint32_t baseWidth = 0, baseHeight = 0;
	uint32_t outputWidth = 0, outputHeight = 0;
	uint32_t fpsNum = 0, fpsDen = 1;
	video_format format = VIDEO_FORMAT_NONE;
	video_colorspace colorspace = VIDEO_CS_DEFAULT;
	bool audioAvailable = false;
	uint32_t sampleRate = 0;
	speaker_layout speakers = SPEAKERS_UNKNOWN;

	double fps() const { return fpsDen ? (double)fpsNum / (double)fpsDen : 0.0; }
	std::string describeVideo() const;
	std::string describeAudio() const;
	bool operator==(const MediaInfo &other) const = default;
};

MediaInfo read_media_info();
/* Errors block plugin starts; warnings are shown in the dock. */
Issues validate_media(const MediaInfo &info);

const char *video_format_name(video_format format);
const char *speaker_layout_name(speaker_layout speakers);

} // namespace mf
