// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Test "main thread": posted work runs when the test pumps the loop. Timers run on a
// virtual clock (advance()) or, with realTime, on the wall clock (integration tests).

#pragma once

#include "core/dispatcher.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace mftest {

class ManualDispatcher : public mf::Dispatcher {
public:
	using Clock = std::chrono::steady_clock;

	explicit ManualDispatcher(bool realTime = false) : realTime_(realTime), mainThread_(std::this_thread::get_id())
	{
	}

	void post(std::function<void()> fn) override
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.push_back(std::move(fn));
		}
		cv_.notify_all();
	}

	mf::TimerId postDelayed(int delayMs, std::function<void()> fn) override
	{
		mf::TimerId id = ++nextTimer_;
		timers_[id] = {now() + std::chrono::milliseconds(delayMs), std::move(fn)};
		requestedDelays.push_back(delayMs);
		return id;
	}

	void cancel(mf::TimerId id) override { timers_.erase(id); }

	bool isMainThread() const override { return std::this_thread::get_id() == mainThread_; }

	Clock::time_point now() const { return realTime_ ? Clock::now() : virtualNow_; }

	/* Runs every posted task and every due timer. Returns the number of callbacks run. */
	int runPending()
	{
		int ran = 0;
		for (;;) {
			std::deque<std::function<void()>> batch;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				batch.swap(queue_);
			}
			bool any = !batch.empty();
			for (auto &fn : batch) {
				auto begin = Clock::now();
				fn();
				recordTaskTime(begin);
				ran++;
			}
			if (fireDueTimer()) {
				ran++;
				any = true;
			}
			if (!any)
				return ran;
		}
	}

	/* Virtual time: moves the clock forward, firing timers in order. */
	void advance(std::chrono::milliseconds amount)
	{
		Clock::time_point target = virtualNow_ + amount;
		for (;;) {
			runPending();
			auto next = earliestTimer();
			if (!next || *next > target)
				break;
			virtualNow_ = *next;
			runPending();
		}
		virtualNow_ = target;
		runPending();
	}

	/* Pumps (real time) until pred() is true or the timeout passes. */
	bool runUntil(const std::function<bool()> &pred, std::chrono::milliseconds timeout)
	{
		auto deadline = Clock::now() + timeout;
		for (;;) {
			runPending();
			if (pred())
				return true;
			if (Clock::now() >= deadline)
				return false;
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait_for(lock, std::chrono::milliseconds(5), [this] { return !queue_.empty(); });
		}
	}

	size_t pendingTimers() const { return timers_.size(); }
	std::vector<int> requestedDelays;
	/* Longest single main-thread callback: proves nothing blocks the "UI" thread. */
	double maxTaskMs = 0.0;

private:
	struct Timer {
		Clock::time_point due;
		std::function<void()> fn;
	};

	std::optional<Clock::time_point> earliestTimer() const
	{
		std::optional<Clock::time_point> best;
		for (const auto &entry : timers_) {
			if (!best || entry.second.due < *best)
				best = entry.second.due;
		}
		return best;
	}

	void recordTaskTime(Clock::time_point begin)
	{
		double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
		if (ms > maxTaskMs)
			maxTaskMs = ms;
	}

	bool fireDueTimer()
	{
		auto current = now();
		for (auto it = timers_.begin(); it != timers_.end(); ++it) {
			if (it->second.due <= current) {
				auto fn = std::move(it->second.fn);
				timers_.erase(it);
				auto begin = Clock::now();
				fn();
				recordTaskTime(begin);
				return true;
			}
		}
		return false;
	}

	bool realTime_;
	std::thread::id mainThread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::function<void()>> queue_;
	std::map<mf::TimerId, Timer> timers_;
	mf::TimerId nextTimer_ = 0;
	Clock::time_point virtualNow_ = Clock::time_point{} + std::chrono::hours(1);
};

} // namespace mftest
