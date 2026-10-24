// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "encoder-profile.hpp"

#include <obs.hpp>

#include <string>
#include <vector>

namespace mf {

/*
 * Encoders discovered at runtime from the running OBS (no hard-coded vendor lists). The
 * shared profile only sets the cross-vendor keys an encoder actually exposes in its
 * properties ("bitrate", "rate_control" = "CBR", "keyint_sec"); all other settings keep
 * that encoder's own defaults.
 */
struct EncoderChoice {
	std::string id;
	std::string displayName;
	bool gpuTexture = false; // OBS_ENCODER_CAP_PASS_TEXTURE: hardware encoder fed from the GPU
	bool hasBitrate = false;
	bool hasCbr = false;
	bool hasKeyint = false;
	bool usable() const { return hasBitrate; }
};

std::vector<EncoderChoice> list_h264_encoders();
std::vector<EncoderChoice> list_aac_encoders();

/* Hardware (GPU texture) H.264 first, then x264, then anything usable. */
std::string choose_default_video_encoder(const std::vector<EncoderChoice> &choices);

/* Resolves the profile's encoder (empty = automatic) against what OBS has registered. */
bool resolve_video_encoder(const EncoderProfile &profile, EncoderChoice &out, std::string &error);
bool resolve_audio_encoder(const EncoderProfile &profile, EncoderChoice &out, std::string &error);

OBSDataAutoRelease build_video_settings(const EncoderChoice &encoder, const EncoderProfile &profile,
					std::vector<std::string> &notes);
OBSDataAutoRelease build_audio_settings(const EncoderChoice &encoder, const EncoderProfile &profile);

} // namespace mf
