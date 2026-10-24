// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "destination-config.hpp"
#include "session-types.hpp"

#include <obs.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mf {

/* Everything one connection attempt needs. Built on the main thread, consumed on the worker. */
struct AttemptSpec {
	uint64_t attemptId = 0;
	std::string destinationId;
	std::string outputName;
	std::string serverUrl;
	DestinationSecrets secrets;
	bool useAuth = false;
	OBSEncoder videoEncoder;
	OBSEncoder audioEncoder;
	int maxShutdownSec = 5;
};

/* Thread-safe event sink. Called from OBS output threads and from the lifecycle worker. */
class TransportEvents {
public:
	virtual ~TransportEvents() = default;
	virtual void transportStarted(uint64_t attemptId) = 0;
	virtual void transportStopped(uint64_t attemptId, int obsCode, const std::string &lastError) = 0;
};

struct StartFailure {
	int obsCode = 0;
	std::string detail;
	bool encoderFailure = false; // the shared encoder could not initialise: a group error
};

/*
 * One obs_output + obs_service pair for one attempt. Created, driven and destroyed only on
 * the destination's lifecycle worker thread, because obs_output_stop/release can block
 * (OBS 32.2.2's rtmp_stream_stop joins a connect thread that may be mid-connect).
 */
class Transport {
public:
	virtual ~Transport() = default;
	/* Asynchronous connect. true only means the connect thread was started. */
	virtual bool start(StartFailure &failure) = 0;
	virtual void stop(bool force) = 0;
	virtual bool active() const = 0;
	virtual TransportStats sample() const = 0;
	/* Diagnostics: confirms the output is attached to the given shared encoders. */
	virtual bool attachedTo(obs_encoder_t *video, obs_encoder_t *audio) const = 0;
};

using TransportFactory = std::function<std::unique_ptr<Transport>(
	const AttemptSpec &spec, std::shared_ptr<TransportEvents> events, std::string &error)>;

/* Uses OBS's installed "rtmp_output" (obs-outputs) and "rtmp_custom" (rtmp-services). */
std::unique_ptr<Transport> create_obs_rtmp_transport(const AttemptSpec &spec, std::shared_ptr<TransportEvents> events,
						     std::string &error);

} // namespace mf
