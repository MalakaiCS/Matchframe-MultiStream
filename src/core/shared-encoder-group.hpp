// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "encoder-catalog.hpp"
#include "encoder-profile.hpp"

#include <obs.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mf {

/*
 * One plugin-owned H.264 video encoder and one AAC audio encoder, fed from OBS's program
 * output (obs_get_video() / obs_get_audio() mixer track), shared by every plugin
 * destination. Never the native OBS streaming encoders, so plugin outputs work before
 * native streaming starts and survive it stopping.
 *
 * Every connection attempt acquires the group before its output is created and releases
 * it only after that output has been fully released, so the encoder objects stay retained
 * until the last output is gone. libobs itself shuts an encoder down (releasing a hardware
 * session) when its last output detaches.
 *
 * Main thread only.
 */
class SharedEncoderGroup {
public:
	/* Creates the two encoder objects for a profile. Replaceable for unit tests only. */
	struct CreatedEncoders {
		OBSEncoder video;
		OBSEncoder audio;
		EncoderChoice videoChoice;
		EncoderChoice audioChoice;
		std::vector<std::string> notes;
	};
	using EncoderFactory = std::function<bool(const EncoderProfile &, CreatedEncoders &, std::string &error)>;
	static bool create_obs_encoders(const EncoderProfile &profile, CreatedEncoders &out, std::string &error);

	explicit SharedEncoderGroup(EncoderFactory factory = create_obs_encoders) : factory_(std::move(factory)) {}
	~SharedEncoderGroup();

	const EncoderProfile &profile() const { return profile_; }
	/* Refused while any attempt holds the group: a running shared encoder is never mutated. */
	bool setProfile(const EncoderProfile &profile, std::string &error);

	bool acquire(const std::string &destinationId, uint64_t attemptId, OBSEncoder &video, OBSEncoder &audio,
		     std::string &error);
	void release(const std::string &destinationId, uint64_t attemptId);
	bool inUse() const { return !holders_.empty(); }
	size_t holderCount() const { return holders_.size(); }

	/* Group-level failure (encoder init or encode error): reported once for all destinations. */
	void markFailure(const std::string &message);
	const std::string &failure() const { return failure_; }
	void clearFailure() { failure_.clear(); }

	obs_encoder_t *videoEncoder() const { return video_; }
	obs_encoder_t *audioEncoder() const { return audio_; }
	const EncoderChoice &videoChoice() const { return videoChoice_; }
	const EncoderChoice &audioChoice() const { return audioChoice_; }
	/* True while libobs holds an initialised encoder instance (i.e. an encode session); see .cpp. */
	bool videoSessionOpen() const;
	bool audioSessionOpen() const;
	const std::vector<std::string> &notes() const { return notes_; }
	std::string describe() const;
	/* Drops the encoder objects (OBS exit). Refused while held. */
	bool releaseEncoders();

	static constexpr const char *kVideoEncoderName = "MatchFrame Multistream shared H.264";
	static constexpr const char *kAudioEncoderName = "MatchFrame Multistream shared AAC";

private:
	bool ensureEncoders(std::string &error);
	void dropEncoders();

	EncoderFactory factory_;
	EncoderProfile profile_;
	OBSEncoder video_;
	OBSEncoder audio_;
	bool created_ = false;
	EncoderChoice videoChoice_;
	EncoderChoice audioChoice_;
	std::vector<std::string> notes_;
	std::set<std::pair<std::string, uint64_t>> holders_;
	std::string failure_;
	bool recreateOnNextUse_ = false;
};

} // namespace mf
