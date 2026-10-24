// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <functional>

namespace mf {

/*
 * Plugin reconnect schedule: 2, 4, 8, 16, then 30 seconds for every further attempt.
 *
 * OBS 32.2.2's built-in output reconnect (obs_output_set_reconnect_settings) multiplies the
 * delay by ~1.5 (+random) per attempt and caps it at 15 minutes, so it cannot express this
 * policy. Plugin outputs therefore disable the built-in reconnect (retry count 0) and this
 * schedule, driven by DestinationSession, is the single reconnect authority.
 */
inline int reconnect_delay_ms(int attempt)
{
	switch (attempt) {
	case 1:
		return 2000;
	case 2:
		return 4000;
	case 3:
		return 8000;
	case 4:
		return 16000;
	default:
		return attempt < 1 ? 2000 : 30000;
	}
}

/* Injectable for tests that must not wait real seconds. */
using ReconnectSchedule = std::function<int(int attempt)>;

} // namespace mf
