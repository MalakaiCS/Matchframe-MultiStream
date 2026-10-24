// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <cstdint>
#include <functional>

namespace mf {

using TimerId = uint64_t;

/*
 * The "main thread" abstraction. In OBS it is the Qt UI thread; in tests it is a
 * manual event loop. All session state machines live on it. OBS signal callbacks and
 * lifecycle workers never touch session state directly: they post().
 */
class Dispatcher {
public:
	virtual ~Dispatcher() = default;
	/* Thread-safe. Runs fn on the main thread later. Never runs fn synchronously. */
	virtual void post(std::function<void()> fn) = 0;
	/* Main thread only. */
	virtual TimerId postDelayed(int delayMs, std::function<void()> fn) = 0;
	/* Main thread only. Cancelling an already-fired or unknown timer is a no-op. */
	virtual void cancel(TimerId id) = 0;
	virtual bool isMainThread() const = 0;
};

} // namespace mf
