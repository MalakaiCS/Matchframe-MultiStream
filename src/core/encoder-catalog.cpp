// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "encoder-catalog.hpp"

#include <obs.h>

#include <cstring>

namespace mf {

namespace {

constexpr const char *kX264Id = "obs_x264";

bool list_contains(obs_property_t *property, const char *value)
{
	if (!property || obs_property_get_type(property) != OBS_PROPERTY_LIST)
		return false;
	if (obs_property_list_format(property) != OBS_COMBO_FORMAT_STRING)
		return false;
	size_t count = obs_property_list_item_count(property);
	for (size_t i = 0; i < count; i++) {
		const char *item = obs_property_list_item_string(property, i);
		if (item && strcmp(item, value) == 0)
			return true;
	}
	return false;
}

bool is_int_property(obs_property_t *property)
{
	return property && obs_property_get_type(property) == OBS_PROPERTY_INT;
}

EncoderChoice inspect(const char *id)
{
	EncoderChoice choice;
	choice.id = id;
	const char *name = obs_encoder_get_display_name(id);
	choice.displayName = name ? name : id;
	choice.gpuTexture = (obs_get_encoder_caps(id) & OBS_ENCODER_CAP_PASS_TEXTURE) != 0;

	obs_properties_t *props = obs_get_encoder_properties(id);
	if (props) {
		choice.hasBitrate = is_int_property(obs_properties_get(props, "bitrate"));
		choice.hasCbr = list_contains(obs_properties_get(props, "rate_control"), "CBR");
		choice.hasKeyint = is_int_property(obs_properties_get(props, "keyint_sec"));
		obs_properties_destroy(props);
	}
	return choice;
}

std::vector<EncoderChoice> list_encoders(obs_encoder_type type, const char *codec)
{
	std::vector<EncoderChoice> out;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != type)
			continue;
		const char *encoderCodec = obs_get_encoder_codec(id);
		if (!encoderCodec || strcmp(encoderCodec, codec) != 0)
			continue;
		uint32_t caps = obs_get_encoder_caps(id);
		if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		out.push_back(inspect(id));
	}
	return out;
}

bool resolve(const std::vector<EncoderChoice> &choices, const std::string &id, EncoderChoice &out)
{
	for (const EncoderChoice &choice : choices) {
		if (choice.id == id) {
			out = choice;
			return true;
		}
	}
	return false;
}

} // namespace

std::vector<EncoderChoice> list_h264_encoders()
{
	return list_encoders(OBS_ENCODER_VIDEO, "h264");
}

std::vector<EncoderChoice> list_aac_encoders()
{
	return list_encoders(OBS_ENCODER_AUDIO, "aac");
}

std::string choose_default_video_encoder(const std::vector<EncoderChoice> &choices)
{
	for (const EncoderChoice &choice : choices) {
		if (choice.gpuTexture && choice.usable())
			return choice.id;
	}
	for (const EncoderChoice &choice : choices) {
		if (choice.id == kX264Id && choice.usable())
			return choice.id;
	}
	for (const EncoderChoice &choice : choices) {
		if (choice.usable())
			return choice.id;
	}
	return {};
}

bool resolve_video_encoder(const EncoderProfile &profile, EncoderChoice &out, std::string &error)
{
	std::vector<EncoderChoice> choices = list_h264_encoders();
	std::string id = profile.videoEncoderId.empty() ? choose_default_video_encoder(choices)
							: profile.videoEncoderId;
	if (id.empty()) {
		error = "OBS has no usable H.264 encoder registered.";
		return false;
	}
	if (!resolve(choices, id, out)) {
		error = "The selected H.264 encoder (" + id + ") is not available in this OBS installation.";
		return false;
	}
	if (!out.usable()) {
		error = "The selected encoder does not expose a bitrate setting and cannot be shared.";
		return false;
	}
	return true;
}

bool resolve_audio_encoder(const EncoderProfile &profile, EncoderChoice &out, std::string &error)
{
	std::vector<EncoderChoice> choices = list_aac_encoders();
	if (!resolve(choices, profile.audioEncoderId, out)) {
		error = "The selected AAC encoder (" + profile.audioEncoderId +
			") is not available in this OBS installation.";
		return false;
	}
	if (!out.usable()) {
		error = "The selected AAC encoder does not expose a bitrate setting.";
		return false;
	}
	return true;
}

OBSDataAutoRelease build_video_settings(const EncoderChoice &encoder, const EncoderProfile &profile,
					std::vector<std::string> &notes)
{
	OBSDataAutoRelease settings = obs_encoder_defaults(encoder.id.c_str());
	if (!settings)
		settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", profile.videoBitrateKbps);
	if (encoder.hasCbr)
		obs_data_set_string(settings, "rate_control", "CBR");
	else
		notes.push_back("This encoder does not offer a CBR rate-control option; its default is used.");
	if (encoder.hasKeyint)
		obs_data_set_int(settings, "keyint_sec", profile.keyframeIntervalSec);
	else
		notes.push_back("This encoder does not expose a keyframe interval; its default is used.");
	return settings;
}

OBSDataAutoRelease build_audio_settings(const EncoderChoice &, const EncoderProfile &profile)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", profile.audioBitrateKbps);
	return settings;
}

} // namespace mf
