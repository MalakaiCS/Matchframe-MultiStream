// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Scriptable stand-in for OBS's RTMP output, for deterministic state-machine tests.

#pragma once

#include "core/transport.hpp"

#include <obs-defs.h>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mftest {

struct Behaviour {
	enum Kind {
		Connect,         // emits "start" right away
		ConnectFail,     // emits "stop" with code right away
		StartFail,       // obs_output_start() returns false
		Hang,            // connect never completes; stop() emits "stop"
		LateStartOnStop, // like Hang, but a "start" sneaks in while stopping
	};
	Kind kind = Connect;
	int code = OBS_OUTPUT_CONNECT_FAILED;
	std::string detail;
	bool encoderFailure = false;
	int stopDelayMs = 0; // simulates a slow obs_output_stop/release
};

class FakeNetwork {
public:
	void script(const std::string &destinationId, std::vector<Behaviour> behaviours)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto &queue = scripts_[destinationId];
		for (auto &b : behaviours)
			queue.push_back(std::move(b));
	}

	Behaviour next(const std::string &destinationId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto &queue = scripts_[destinationId];
		if (queue.empty())
			return Behaviour{};
		Behaviour b = queue.front();
		queue.pop_front();
		return b;
	}

	struct Live {
		uint64_t attemptId;
		std::shared_ptr<mf::TransportEvents> events;
		bool active;
	};

	void added(const std::string &id, uint64_t attemptId, std::shared_ptr<mf::TransportEvents> events)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		live_[id].push_back({attemptId, std::move(events), false});
		created_[id]++;
		size_t count = live_[id].size();
		if (count > maxConcurrent_[id])
			maxConcurrent_[id] = count;
		secretsSeen_.push_back(id);
	}

	void setActive(const std::string &id, uint64_t attemptId, bool active)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (Live &live : live_[id]) {
			if (live.attemptId == attemptId)
				live.active = active;
		}
	}

	void removed(const std::string &id, uint64_t attemptId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto &list = live_[id];
		for (auto it = list.begin(); it != list.end(); ++it) {
			if (it->attemptId == attemptId) {
				list.erase(it);
				break;
			}
		}
	}

	/* Simulates the network dropping an established connection (OBS output thread). */
	bool drop(const std::string &id, int code = OBS_OUTPUT_DISCONNECTED)
	{
		std::shared_ptr<mf::TransportEvents> events;
		uint64_t attemptId = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (Live &live : live_[id]) {
				if (live.active) {
					live.active = false;
					events = live.events;
					attemptId = live.attemptId;
				}
			}
		}
		if (!events)
			return false;
		std::thread([events, attemptId, code] {
			events->transportStopped(attemptId, code, "Connection reset");
		}).join();
		return true;
	}

	size_t live(const std::string &id)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return live_[id].size();
	}
	size_t maxConcurrent(const std::string &id)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return maxConcurrent_[id];
	}
	int created(const std::string &id)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return created_[id];
	}

	std::mutex mutex_;
	std::map<std::string, std::deque<Behaviour>> scripts_;
	std::map<std::string, std::vector<Live>> live_;
	std::map<std::string, size_t> maxConcurrent_;
	std::map<std::string, int> created_;
	std::vector<std::string> secretsSeen_;
	std::vector<std::string> keysSeen;
};

class FakeTransport final : public mf::Transport {
public:
	FakeTransport(FakeNetwork &network, const mf::AttemptSpec &spec, std::shared_ptr<mf::TransportEvents> events)
		: network_(network),
		  id_(spec.destinationId),
		  attemptId_(spec.attemptId),
		  events_(std::move(events)),
		  behaviour_(network.next(spec.destinationId))
	{
		network_.added(id_, attemptId_, events_);
		std::lock_guard<std::mutex> lock(network_.mutex_);
		network_.keysSeen.push_back(spec.secrets.streamKey);
	}

	~FakeTransport() override
	{
		if (behaviour_.stopDelayMs)
			std::this_thread::sleep_for(std::chrono::milliseconds(behaviour_.stopDelayMs));
		network_.removed(id_, attemptId_);
	}

	bool start(mf::StartFailure &failure) override
	{
		switch (behaviour_.kind) {
		case Behaviour::StartFail:
			failure.obsCode = behaviour_.code;
			failure.detail = behaviour_.detail;
			failure.encoderFailure = behaviour_.encoderFailure;
			return false;
		case Behaviour::Connect:
			active_ = true;
			network_.setActive(id_, attemptId_, true);
			events_->transportStarted(attemptId_);
			return true;
		case Behaviour::ConnectFail:
			events_->transportStopped(attemptId_, behaviour_.code, behaviour_.detail);
			return true;
		case Behaviour::Hang:
		case Behaviour::LateStartOnStop:
			return true;
		}
		return true;
	}

	void stop(bool) override
	{
		if (behaviour_.stopDelayMs)
			std::this_thread::sleep_for(std::chrono::milliseconds(behaviour_.stopDelayMs));
		if (behaviour_.kind == Behaviour::LateStartOnStop && !stopped_)
			events_->transportStarted(attemptId_);
		if (!stopped_) {
			stopped_ = true;
			active_ = false;
			network_.setActive(id_, attemptId_, false);
			events_->transportStopped(attemptId_, OBS_OUTPUT_SUCCESS, "");
		}
	}

	bool active() const override { return active_; }

	mf::TransportStats sample() const override
	{
		mf::TransportStats stats;
		stats.active = active_;
		bytes_ += 750000; // 6 Mbps worth per one-second tick
		stats.totalBytes = bytes_;
		stats.totalFrames = 30;
		return stats;
	}

	bool attachedTo(obs_encoder_t *, obs_encoder_t *) const override { return true; }

private:
	FakeNetwork &network_;
	std::string id_;
	uint64_t attemptId_;
	std::shared_ptr<mf::TransportEvents> events_;
	Behaviour behaviour_;
	bool active_ = false;
	bool stopped_ = false;
	mutable uint64_t bytes_ = 0;
};

} // namespace mftest
