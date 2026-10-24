// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "lifecycle-worker.hpp"
#include "log.hpp"

#include <util/threading.h>

namespace mf {

LifecycleWorker::LifecycleWorker(std::string name, std::function<void()> tick, std::chrono::milliseconds tickPeriod)
	: name_(std::move(name)),
	  tick_(std::move(tick)),
	  tickPeriod_(tickPeriod)
{
	thread_ = std::thread([this] { run(); });
}

LifecycleWorker::~LifecycleWorker()
{
	requestQuit();
	if (thread_.joinable())
		thread_.join();
}

void LifecycleWorker::post(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (quit_)
			return;
		queue_.push_back(std::move(task));
	}
	wake_.notify_one();
}

void LifecycleWorker::requestQuit()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = true;
	}
	wake_.notify_one();
}

bool LifecycleWorker::waitIdle(Clock::time_point deadline)
{
	std::unique_lock<std::mutex> lock(mutex_);
	return idleChanged_.wait_until(lock, deadline, [this] { return finished_ || (queue_.empty() && !running_); });
}

bool LifecycleWorker::join(Clock::time_point deadline)
{
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (!idleChanged_.wait_until(lock, deadline, [this] { return finished_; }))
			return false;
	}
	if (thread_.joinable())
		thread_.join();
	return true;
}

void LifecycleWorker::abandon()
{
	if (thread_.joinable())
		thread_.detach();
}

bool LifecycleWorker::idle() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.empty() && !running_;
}

bool LifecycleWorker::finished() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return finished_;
}

size_t LifecycleWorker::pending() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return queue_.size() + (running_ ? 1 : 0);
}

void LifecycleWorker::run()
{
	os_set_thread_name(("mf-" + name_).c_str());
	auto nextTick = Clock::now() + tickPeriod_;

	for (;;) {
		std::function<void()> task;
		bool doTick = false;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait_until(lock, nextTick, [this] { return quit_ || !queue_.empty(); });
			if (!queue_.empty()) {
				task = std::move(queue_.front());
				queue_.pop_front();
				running_ = true;
			} else if (quit_) {
				break;
			} else if (Clock::now() >= nextTick) {
				doTick = (bool)tick_;
				running_ = doTick;
			}
		}

		if (task) {
			task();
		} else if (doTick) {
			tick_();
		}
		if (Clock::now() >= nextTick)
			nextTick = Clock::now() + tickPeriod_;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			running_ = false;
		}
		idleChanged_.notify_all();
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		finished_ = true;
	}
	idleChanged_.notify_all();
}

} // namespace mf
