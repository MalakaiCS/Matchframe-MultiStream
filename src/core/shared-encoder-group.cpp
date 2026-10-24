// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "shared-encoder-group.hpp"
#include "log.hpp"

#include <obs.h>

namespace mf {

SharedEncoderGroup::~SharedEncoderGroup()
{
	if (inUse())
		MF_WARN("shared encoder group destroyed with %zu holder(s)", holders_.size());
	dropEncoders();
}

bool SharedEncoderGroup::setProfile(const EncoderProfile &profile, std::string &error)
{
	if (inUse()) {
		error = "Shared encoding settings are locked while any plugin output is active.";
		return false;
	}
	Issues issues = validate_encoder_profile(profile);
	if (has_errors(issues)) {
		error = first_error(issues);
		return false;
	}
	if (!(profile == profile_)) {
		profile_ = profile;
		dropEncoders();
	}
	return true;
}

void SharedEncoderGroup::dropEncoders()
{
	video_ = nullptr;
	audio_ = nullptr;
	created_ = false;
	notes_.clear();
	recreateOnNextUse_ = false;
}

bool SharedEncoderGroup::create_obs_encoders(const EncoderProfile &profile, CreatedEncoders &out, std::string &error)
{
	if (!resolve_video_encoder(profile, out.videoChoice, error))
		return false;
	if (!resolve_audio_encoder(profile, out.audioChoice, error))
		return false;

	video_t *programVideo = obs_get_video();
	audio_t *programAudio = obs_get_audio();
	if (!programVideo || !programAudio) {
		error = "OBS video/audio output is not available.";
		return false;
	}

	OBSDataAutoRelease videoSettings = build_video_settings(out.videoChoice, profile, out.notes);
	OBSDataAutoRelease audioSettings = build_audio_settings(out.audioChoice, profile);

	OBSEncoderAutoRelease video =
		obs_video_encoder_create(out.videoChoice.id.c_str(), kVideoEncoderName, videoSettings, nullptr);
	if (!video) {
		error = "OBS could not create the " + out.videoChoice.displayName + " encoder.";
		return false;
	}
	OBSEncoderAutoRelease audio = obs_audio_encoder_create(out.audioChoice.id.c_str(), kAudioEncoderName,
							       audioSettings, profile.mixerIndex(), nullptr);
	if (!audio) {
		error = "OBS could not create the " + out.audioChoice.displayName + " encoder.";
		return false;
	}

	/* Program output: the composed main view (Studio Mode's program, never its preview). */
	obs_encoder_set_video(video, programVideo);
	obs_encoder_set_audio(audio, programAudio);

	out.video = video.Get();
	out.audio = audio.Get();
	return true;
}

bool SharedEncoderGroup::ensureEncoders(std::string &error)
{
	if (recreateOnNextUse_ && !inUse())
		dropEncoders();
	if (created_)
		return true;

	CreatedEncoders created;
	if (!factory_(profile_, created, error))
		return false;

	video_ = created.video;
	audio_ = created.audio;
	videoChoice_ = created.videoChoice;
	audioChoice_ = created.audioChoice;
	notes_ = std::move(created.notes);
	created_ = true;
	MF_INFO("shared encoders created: video=%s %d kbps CBR keyint=%ds, audio=%s %d kbps track %d",
		videoChoice_.id.c_str(), profile_.videoBitrateKbps, profile_.keyframeIntervalSec,
		audioChoice_.id.c_str(), profile_.audioBitrateKbps, profile_.audioTrack);
	return true;
}

bool SharedEncoderGroup::acquire(const std::string &destinationId, uint64_t attemptId, OBSEncoder &video,
				 OBSEncoder &audio, std::string &error)
{
	if (!inUse()) {
		/* First holder: always start from fresh encoder objects. An encoder is bound to OBS's
		 * video and audio outputs when it is created, and obs_reset_video() (OBS Settings >
		 * Video, a profile switch) frees the old video output while an idle encoder still
		 * points at it; libobs refuses obs_encoder_set_video() once an encoder was initialised.
		 * OBS's own outputs re-bind before every start for the same reason. */
		recreateOnNextUse_ = true;
		failure_.clear();
	}
	if (!ensureEncoders(error))
		return false;
	holders_.insert({destinationId, attemptId});
	video = video_;
	audio = audio_;
	return true;
}

void SharedEncoderGroup::release(const std::string &destinationId, uint64_t attemptId)
{
	holders_.erase({destinationId, attemptId});
	if (holders_.empty())
		MF_DEBUG("shared encoder group idle (video session open: %s)", videoSessionOpen() ? "yes" : "no");
}

void SharedEncoderGroup::markFailure(const std::string &message)
{
	if (failure_.empty())
		MF_ERROR("shared encoder group failure: %s", message.c_str());
	failure_ = message;
	recreateOnNextUse_ = true;
}

bool SharedEncoderGroup::releaseEncoders()
{
	if (inUse())
		return false;
	dropEncoders();
	return true;
}

namespace {

/* libobs exposes no "is initialised" query. obs_encoder_get_extra_data() only succeeds while the
 * encoder implementation instance (and with it any hardware encode session) exists, and the
 * H.264 and AAC encoders shipped with OBS all provide extra data (SPS/PPS, AudioSpecificConfig). */
bool encoder_instance_alive(obs_encoder_t *encoder)
{
	uint8_t *data = nullptr;
	size_t size = 0;
	return encoder && obs_encoder_get_extra_data(encoder, &data, &size) && size > 0;
}

} // namespace

/* While outputs hold the group an encoder may be shut down concurrently by libobs, so only the
 * atomic obs_encoder_active() is read; the instance check is used once the group is idle. */
bool SharedEncoderGroup::videoSessionOpen() const
{
	if (inUse())
		return video_ && obs_encoder_active(video_);
	return encoder_instance_alive(video_);
}

bool SharedEncoderGroup::audioSessionOpen() const
{
	if (inUse())
		return audio_ && obs_encoder_active(audio_);
	return encoder_instance_alive(audio_);
}

std::string SharedEncoderGroup::describe() const
{
	std::string out = "video encoder: ";
	out += created_ ? videoChoice_.id + " (" + videoChoice_.displayName + ")" : std::string("not created");
	out += ", audio encoder: ";
	out += created_ ? audioChoice_.id + " (" + audioChoice_.displayName + ")" : std::string("not created");
	out += ", holders: " + std::to_string(holders_.size());
	out += ", video session open: ";
	out += videoSessionOpen() ? "yes" : "no";
	out += ", audio session open: ";
	out += audioSessionOpen() ? "yes" : "no";
	return out;
}

} // namespace mf
