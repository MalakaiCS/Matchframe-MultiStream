// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mf {

/*
 * One serial worker thread per destination. Every blocking libobs output call for that
 * destination runs here, in FIFO order, so a slow or unreachable destination can only
 * delay its own lifecycle, never the UI or another destination. FIFO order also means a
 * new attempt is never created before the previous attempt's release has finished.
 */
class LifecycleWorker {
public:
	using Clock = std::chrono::steady_clock;

	explicit LifecycleWorker(std::string name, std::function<void()> tick = {},
				 std::chrono::milliseconds tickPeriod = std::chrono::milliseconds(1000));
	~LifecycleWorker();

	LifecycleWorker(const LifecycleWorker &) = delete;
	LifecycleWorker &operator=(const LifecycleWorker &) = delete;

	/* Thread-safe. Ignored after quit was requested. */
	void post(std::function<void()> task);
	/* Run the remaining queue, then exit. */
	void requestQuit();
	/* Waits until the queue is empty and no task is running. */
	bool waitIdle(Clock::time_point deadline);
	/* Waits for the thread to exit; joins it on success. */
	bool join(Clock::time_point deadline);
	/* Last resort after a failed bounded join: leaves the thread running. */
	void abandon();
	bool idle() const;
	bool finished() const;
	size_t pending() const;

private:
	void run();

	std::string name_;
	std::function<void()> tick_;
	std::chrono::milliseconds tickPeriod_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable idleChanged_;
	std::deque<std::function<void()>> queue_;
	bool running_ = false;
	bool quit_ = false;
	bool finished_ = false;
	std::thread thread_;
};

} // namespace mf
