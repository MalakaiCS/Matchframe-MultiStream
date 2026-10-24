// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <cstdint>
#include <string>

namespace mf {

/*
 * Transport states. "Sending" means the RTMP server accepted the connection and the plugin
 * is delivering packets; it is never proof that a platform has published the broadcast.
 */
enum class SessionState { Disabled, Idle, Connecting, Sending, Reconnecting, Stopping, Error };

const char *session_state_name(SessionState state);
inline bool state_is_active(SessionState state)
{
	return state == SessionState::Connecting || state == SessionState::Sending ||
	       state == SessionState::Reconnecting || state == SessionState::Stopping;
}

enum class StopReason { Manual, StopAll, ProfileChange, Shutdown, Removed };

enum class ErrorKind {
	None,
	Configuration, // bad URL, missing key, invalid local settings
	Network,       // could not connect / connection lost
	Rejected,      // server refused the publish (often key / application path)
	Tls,           // RTMPS connection could not be established
	Encoder,       // shared encoder failed (group error)
	Exhausted,     // reconnect attempts used up
	Internal,
};

struct SessionError {
	ErrorKind kind = ErrorKind::None;
	std::string message; // sanitized
	int obsCode = 0;
	bool empty() const { return kind == ErrorKind::None; }
};

/* Values sampled from the obs_output on the lifecycle worker once per second. */
struct TransportStats {
	bool active = false;
	uint64_t totalBytes = 0;
	int framesDropped = 0;
	int totalFrames = 0;
	float congestion = 0.0f;
	int connectTimeMs = -1;
};

/* Main-thread view of a session for the UI and diagnostics. */
struct SessionSnapshot {
	std::string id;
	SessionState state = SessionState::Idle;
	double bitrateKbps = 0.0;
	int64_t sendingSeconds = 0; // since the current connection was established
	int framesDropped = 0;
	int totalFrames = 0;
	double dropPercent = 0.0;
	bool dropStatsAvailable = false;
	float congestion = 0.0f;
	int reconnectCount = 0; // connection losses since the operator started it
	int retryAttempt = 0;   // current attempt while Reconnecting
	int retryMax = 0;
	int retryInSeconds = -1; // countdown while waiting for the next attempt
	SessionError lastError;
	bool lastErrorRecovered = false;       // the connection came back after this error
	bool attachedToSharedEncoders = false; // live output verified to use the group's encoders
	uint64_t attemptId = 0;
};

} // namespace mf
