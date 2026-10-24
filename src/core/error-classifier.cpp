// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "error-classifier.hpp"
#include "redaction.hpp"

#include <obs-defs.h>

namespace mf {

const char *session_state_name(SessionState state)
{
	switch (state) {
	case SessionState::Disabled:
		return "Disabled";
	case SessionState::Idle:
		return "Idle";
	case SessionState::Connecting:
		return "Connecting";
	case SessionState::Sending:
		return "Sending";
	case SessionState::Reconnecting:
		return "Reconnecting";
	case SessionState::Stopping:
		return "Stopping";
	case SessionState::Error:
		return "Error";
	}
	return "Unknown";
}

const char *obs_code_name(int obsCode)
{
	switch (obsCode) {
	case OBS_OUTPUT_SUCCESS:
		return "SUCCESS";
	case OBS_OUTPUT_BAD_PATH:
		return "BAD_PATH";
	case OBS_OUTPUT_CONNECT_FAILED:
		return "CONNECT_FAILED";
	case OBS_OUTPUT_INVALID_STREAM:
		return "INVALID_STREAM";
	case OBS_OUTPUT_ERROR:
		return "ERROR";
	case OBS_OUTPUT_DISCONNECTED:
		return "DISCONNECTED";
	case OBS_OUTPUT_UNSUPPORTED:
		return "UNSUPPORTED";
	case OBS_OUTPUT_NO_SPACE:
		return "NO_SPACE";
	case OBS_OUTPUT_ENCODE_ERROR:
		return "ENCODE_ERROR";
	case OBS_OUTPUT_HDR_DISABLED:
		return "HDR_DISABLED";
	default:
		return "UNKNOWN";
	}
}

bool code_is_retryable(int obsCode, FailurePhase phase)
{
	switch (obsCode) {
	case OBS_OUTPUT_BAD_PATH:
	case OBS_OUTPUT_UNSUPPORTED:
	case OBS_OUTPUT_ENCODE_ERROR:
	case OBS_OUTPUT_HDR_DISABLED:
	case OBS_OUTPUT_NO_SPACE:
		return false;
	case OBS_OUTPUT_SUCCESS:
		/* An unrequested clean stop means OBS or another component stopped the output. */
		return false;
	default:
		break;
	}
	/* Like OBS's own streaming: a first connection failure is reported, not retried; once a
	 * stream has been established (or a reconnect cycle is running), network failures retry. */
	return phase != FailurePhase::Connecting;
}

SessionError classify_stop(int obsCode, const std::string &backendDetail, bool secureScheme, FailurePhase phase,
			   const std::vector<std::string> &secrets)
{
	SessionError error;
	error.obsCode = obsCode;
	std::string detail = sanitize_message(backendDetail, secrets);
	auto with_detail = [&](std::string message) {
		if (!detail.empty())
			message += " OBS reported: " + detail;
		return message;
	};

	switch (obsCode) {
	case OBS_OUTPUT_BAD_PATH:
		error.kind = ErrorKind::Configuration;
		error.message = with_detail("The server URL could not be used by OBS's RTMP output.");
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		if (secureScheme) {
			error.kind = ErrorKind::Tls;
			error.message = with_detail(
				"Could not establish the RTMPS connection (network, DNS or TLS certificate problem).");
		} else {
			error.kind = ErrorKind::Network;
			error.message = with_detail("Could not connect to the server.");
		}
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		error.kind = ErrorKind::Rejected;
		error.message = with_detail("The server refused the stream. This usually means the stream key is "
					    "invalid or expired, or the application path is wrong.");
		break;
	case OBS_OUTPUT_DISCONNECTED:
		error.kind = ErrorKind::Network;
		error.message = with_detail(phase == FailurePhase::Established ? "Connection to the server was lost."
									       : "The server closed the connection.");
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		error.kind = ErrorKind::Encoder;
		error.message = "The shared encoder failed. Every plugin destination using it was stopped.";
		break;
	case OBS_OUTPUT_UNSUPPORTED:
		error.kind = ErrorKind::Configuration;
		error.message = with_detail("The output does not support the selected encoder or settings.");
		break;
	case OBS_OUTPUT_HDR_DISABLED:
		error.kind = ErrorKind::Configuration;
		error.message = "HDR video is not supported for this codec over RTMP.";
		break;
	case OBS_OUTPUT_SUCCESS:
		error.kind = ErrorKind::Internal;
		error.message = with_detail("The output was stopped by OBS.");
		break;
	default:
		error.kind = ErrorKind::Internal;
		error.message = with_detail("The output stopped with an error (code " + std::to_string(obsCode) + ").");
		break;
	}
	return error;
}

} // namespace mf
