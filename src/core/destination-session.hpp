// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "destination-config.hpp"
#include "dispatcher.hpp"
#include "lifecycle-worker.hpp"
#include "session-types.hpp"
#include "transport.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace mf {

class SharedEncoderGroup;

/* Services a session needs from its owner (OutputManager). Main thread unless noted. */
class SessionEnvironment {
public:
	virtual ~SessionEnvironment() = default;
	virtual Dispatcher &dispatcher() = 0;
	virtual SharedEncoderGroup &encoderGroup() = 0;
	/* Called on lifecycle worker threads; must be thread-safe. */
	virtual std::unique_ptr<Transport>
	createTransport(const AttemptSpec &spec, std::shared_ptr<TransportEvents> events, std::string &error) = 0;
	virtual int reconnectDelayMs(int attempt) = 0;
	virtual int maxShutdownSec() = 0;
	virtual void sessionChanged(const std::string &destinationId) = 0;
	virtual void sessionReleased(const std::string &destinationId) = 0;
	virtual void groupFailure(const std::string &destinationId, const std::string &message) = 0;
};

struct StartRequest {
	std::string displayName;
	std::string serverUrl;
	DestinationSecrets secrets;
	bool useAuth = false;
	bool autoReconnect = true;
	int maxReconnectAttempts = kDefaultMaxReconnectAttempts;
};

/*
 * One destination's transport lifecycle.
 *
 * Threading: all public methods and all state live on the main thread. Each connection
 * attempt gets a unique attempt id; OBS signals and worker results are posted back with it
 * and anything carrying a stale id is ignored, so a stopped attempt can never resurrect the
 * session. Each attempt owns a fresh obs_output created and released on this destination's
 * LifecycleWorker; FIFO order on that worker guarantees at most one live output per
 * destination. Reconnects are driven only by this class (OBS's built-in reconnect is off).
 */
class DestinationSession {
public:
	DestinationSession(std::string destinationId, SessionEnvironment &environment, bool enabled);
	~DestinationSession();

	DestinationSession(const DestinationSession &) = delete;
	DestinationSession &operator=(const DestinationSession &) = delete;

	const std::string &id() const { return id_; }
	SessionState state() const { return state_; }
	/* Holds or is about to hold an output, or waits to reconnect. Locks URL/credentials. */
	bool busy() const;
	bool holdsOutput() const { return liveAttempt_ != 0; }

	void setEnabled(bool enabled);
	bool start(StartRequest request, std::string &error);
	void stop(StopReason reason);
	/* Shows a pre-start validation failure on the destination. */
	void failBeforeStart(const SessionError &error);
	SessionSnapshot snapshot() const;

	/* Synchronous teardown support (profile change, OBS exit). */
	bool waitWorkerIdle(LifecycleWorker::Clock::time_point deadline);
	/* After waitWorkerIdle() succeeded: settle state without waiting for queued events. */
	void finalizeAfterSyncStop();
	void beginShutdown();
	bool waitShutdown(LifecycleWorker::Clock::time_point deadline);
	void abandonWorker();

private:
	enum class AfterRelease { None, Retry, Error };
	struct Link {
		DestinationSession *session;
	};
	class EventSink;

	using SteadyClock = std::chrono::steady_clock;

	bool launch(SessionState launchState, std::string &error);
	void finishStop();
	void handleFailure(uint64_t attemptId, int obsCode, const std::string &detail, bool encoderFailure,
			   bool needsRelease);
	void scheduleRetry();
	void cancelRetry();
	void notify();
	LifecycleWorker &worker();

	/* Main-thread event handlers (posted). */
	void onStarted(uint64_t attemptId);
	void onStopped(uint64_t attemptId, int obsCode, const std::string &detail);
	void onStartFailed(uint64_t attemptId, int obsCode, const std::string &detail, bool encoderFailure);
	void onReleased(uint64_t attemptId);
	void onStats(uint64_t attemptId, const TransportStats &stats, SteadyClock::time_point when, bool attached);
	void onRetryTimer(uint64_t generation);

	/* Worker-thread tasks. */
	void workerStart(AttemptSpec spec);
	void workerStop(uint64_t attemptId, bool force, int graceMs);
	void workerRelease(uint64_t attemptId);
	void workerTick();

	std::string id_;
	SessionEnvironment &env_;
	std::shared_ptr<Link> link_;
	std::shared_ptr<EventSink> sink_;
	std::unique_ptr<LifecycleWorker> worker_;

	/* Main-thread state. */
	bool enabled_;
	SessionState state_;
	StartRequest request_;
	uint64_t generation_ = 0;
	uint64_t liveAttempt_ = 0;
	bool stopRequested_ = false;
	AfterRelease afterRelease_ = AfterRelease::None;
	int retryAttempt_ = 0;
	int reconnectCount_ = 0;
	TimerId retryTimer_ = 0;
	SteadyClock::time_point retryDue_{};
	SessionError lastError_;
	bool lastErrorRecovered_ = false;
	SteadyClock::time_point sendingSince_{};
	TransportStats stats_;
	uint64_t statsAttempt_ = 0;
	uint64_t prevBytes_ = 0;
	SteadyClock::time_point prevSampleAt_{};
	double bitrateKbps_ = 0.0;
	bool attachedToShared_ = false;

	/* Worker-thread state. */
	struct LiveTransport {
		std::unique_ptr<Transport> transport;
		obs_encoder_t *video = nullptr; // identity only, for the attachment check
		obs_encoder_t *audio = nullptr;
	};
	std::map<uint64_t, LiveTransport> transports_;
};

} // namespace mf
