// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "session-types.hpp"

#include <string>
#include <vector>

namespace mf {

enum class FailurePhase { Connecting, Established, Reconnecting };

/*
 * Converts an obs_output stop code plus the backend's last_error text into a sanitized,
 * truthful operator message. Authentication problems are only named as such when the
 * backend says the server refused the stream (OBS_OUTPUT_INVALID_STREAM); otherwise the
 * message stays generic and quotes OBS's own detail.
 */
SessionError classify_stop(int obsCode, const std::string &backendDetail, bool secureScheme, FailurePhase phase,
			   const std::vector<std::string> &secrets);

/* Whether the single reconnect controller may retry after this failure. */
bool code_is_retryable(int obsCode, FailurePhase phase);

const char *obs_code_name(int obsCode);

} // namespace mf
