// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "destination-session.hpp"
#include "error-classifier.hpp"
#include "log.hpp"
#include "redaction.hpp"
#include "rtmp-url.hpp"
#include "shared-encoder-group.hpp"

#include <obs-defs.h>

#include <atomic>
#include <cmath>
#include <thread>

namespace mf {

namespace {

std::atomic<uint64_t> g_attemptCounter{0};

std::string short_id(const std::string &id)
{
	return id.substr(0, 8);
}

} // namespace

/* Bridges OBS/worker threads to the main thread. Holds only a weak link to the session. */
class DestinationSession::EventSink final : public TransportEvents {
public:
	EventSink(Dispatcher &dispatcher, std::weak_ptr<Link> link) : dispatcher_(dispatcher), link_(std::move(link)) {}

	void transportStarted(uint64_t attemptId) override
	{
		post([attemptId](DestinationSession &s) { s.onStarted(attemptId); });
	}

	void transportStopped(uint64_t attemptId, int obsCode, const std::string &lastError) override
	{
		post([attemptId, obsCode, lastError](DestinationSession &s) {
			s.onStopped(attemptId, obsCode, lastError);
		});
	}

	void startFailed(uint64_t attemptId, int obsCode, std::string detail, bool encoderFailure)
	{
		post([=](DestinationSession &s) { s.onStartFailed(attemptId, obsCode, detail, encoderFailure); });
	}

	void released(uint64_t attemptId)
	{
		post([attemptId](DestinationSession &s) { s.onReleased(attemptId); });
	}

	void stats(uint64_t attemptId, TransportStats stats, bool attached)
	{
		auto when = std::chrono::steady_clock::now();
		post([=](DestinationSession &s) { s.onStats(attemptId, stats, when, attached); });
	}

private:
	template<typename F> void post(F &&fn)
	{
		std::weak_ptr<Link> link = link_;
		dispatcher_.post([link, fn = std::forward<F>(fn)]() {
			if (auto locked = link.lock())
				fn(*locked->session);
		});
	}

	Dispatcher &dispatcher_;
	std::weak_ptr<Link> link_;
};

DestinationSession::DestinationSession(std::string destinationId, SessionEnvironment &environment, bool enabled)
	: id_(std::move(destinationId)),
	  env_(environment),
	  link_(std::make_shared<Link>(Link{this})),
	  enabled_(enabled),
	  state_(enabled ? SessionState::Idle : SessionState::Disabled)
{
	sink_ = std::make_shared<EventSink>(env_.dispatcher(), link_);
}

DestinationSession::~DestinationSession()
{
	cancelRetry();
	link_.reset();
	/* The owner has already stopped the session; this joins an idle worker. */
	worker_.reset();
}

LifecycleWorker &DestinationSession::worker()
{
	if (!worker_)
		worker_ = std::make_unique<LifecycleWorker>(short_id(id_), [this] { workerTick(); });
	return *worker_;
}

bool DestinationSession::busy() const
{
	return state_is_active(state_) || liveAttempt_ != 0;
}

void DestinationSession::setEnabled(bool enabled)
{
	enabled_ = enabled;
	if (!busy() && state_ != SessionState::Error)
		state_ = enabled ? SessionState::Idle : SessionState::Disabled;
	notify();
}

void DestinationSession::notify()
{
	env_.sessionChanged(id_);
}

bool DestinationSession::start(StartRequest request, std::string &error)
{
	if (busy()) {
		error = "This destination is already active.";
		return false;
	}
	if (!enabled_) {
		error = "This destination is disabled. Enable it first.";
		return false;
	}
	cancelRetry();
	request_ = std::move(request);
	generation_++;
	retryAttempt_ = 0;
	reconnectCount_ = 0;
	lastError_ = {};
	lastErrorRecovered_ = false;
	if (!launch(SessionState::Connecting, error)) {
		request_.secrets.clear();
		return false;
	}
	MF_INFO("destination %s: start requested (%s)", short_id(id_).c_str(), redact_url(request_.serverUrl).c_str());
	return true;
}

bool DestinationSession::launch(SessionState launchState, std::string &error)
{
	uint64_t attemptId = ++g_attemptCounter;
	OBSEncoder video;
	OBSEncoder audio;
	if (!env_.encoderGroup().acquire(id_, attemptId, video, audio, error)) {
		lastError_ = {ErrorKind::Encoder, sanitize_message(error), 0};
		state_ = SessionState::Error;
		env_.groupFailure(id_, lastError_.message);
		notify();
		return false;
	}

	liveAttempt_ = attemptId;
	stopRequested_ = false;
	afterRelease_ = AfterRelease::None;
	state_ = launchState;
	statsAttempt_ = attemptId;
	stats_ = {};
	prevBytes_ = 0;
	prevSampleAt_ = {};
	bitrateKbps_ = 0.0;
	attachedToShared_ = false;

	AttemptSpec spec;
	spec.attemptId = attemptId;
	spec.destinationId = id_;
	spec.outputName = "matchframe-" + short_id(id_) + "-" + std::to_string(attemptId);
	spec.serverUrl = request_.serverUrl;
	spec.secrets = request_.secrets;
	spec.useAuth = request_.useAuth;
	spec.videoEncoder = video;
	spec.audioEncoder = audio;
	spec.maxShutdownSec = env_.maxShutdownSec();

	worker().post([this, spec = std::move(spec)]() mutable { workerStart(std::move(spec)); });
	notify();
	return true;
}

void DestinationSession::stop(StopReason reason)
{
	generation_++;
	cancelRetry();
	if (liveAttempt_ != 0) {
		if (!stopRequested_) {
			stopRequested_ = true;
			state_ = SessionState::Stopping;
			bool force = reason == StopReason::Shutdown || reason == StopReason::ProfileChange;
			/* If a failure already queued a release for this attempt, that release completes the stop. */
			if (afterRelease_ == AfterRelease::None) {
				uint64_t attemptId = liveAttempt_;
				int graceMs = (env_.maxShutdownSec() + 3) * 1000;
				worker().post(
					[this, attemptId, force, graceMs] { workerStop(attemptId, force, graceMs); });
			}
			afterRelease_ = AfterRelease::None;
			MF_INFO("destination %s: stop requested", short_id(id_).c_str());
		}
	} else {
		finishStop();
	}
	notify();
}

void DestinationSession::finishStop()
{
	stopRequested_ = false;
	afterRelease_ = AfterRelease::None;
	request_.secrets.clear();
	retryAttempt_ = 0;
	lastError_ = {};
	lastErrorRecovered_ = false;
	bitrateKbps_ = 0.0;
	attachedToShared_ = false;
	state_ = enabled_ ? SessionState::Idle : SessionState::Disabled;
}

void DestinationSession::failBeforeStart(const SessionError &error)
{
	if (busy())
		return;
	lastError_ = error;
	lastErrorRecovered_ = false;
	state_ = SessionState::Error;
	notify();
}

void DestinationSession::cancelRetry()
{
	if (retryTimer_) {
		env_.dispatcher().cancel(retryTimer_);
		retryTimer_ = 0;
	}
}

void DestinationSession::scheduleRetry()
{
	cancelRetry();
	int delay = env_.reconnectDelayMs(retryAttempt_);
	retryDue_ = SteadyClock::now() + std::chrono::milliseconds(delay);
	uint64_t generation = generation_;
	std::weak_ptr<Link> link = link_;
	retryTimer_ = env_.dispatcher().postDelayed(delay, [link, generation] {
		if (auto locked = link.lock())
			locked->session->onRetryTimer(generation);
	});
	MF_INFO("destination %s: reconnect attempt %d/%d in %d ms", short_id(id_).c_str(), retryAttempt_,
		request_.maxReconnectAttempts, delay);
}

void DestinationSession::onRetryTimer(uint64_t generation)
{
	retryTimer_ = 0;
	if (generation != generation_ || state_ != SessionState::Reconnecting || liveAttempt_ != 0)
		return;
	std::string error;
	if (!launch(SessionState::Reconnecting, error))
		request_.secrets.clear();
}

void DestinationSession::onStarted(uint64_t attemptId)
{
	if (attemptId != liveAttempt_ || stopRequested_ || afterRelease_ != AfterRelease::None)
		return;
	bool reconnected = state_ == SessionState::Reconnecting;
	state_ = SessionState::Sending;
	sendingSince_ = SteadyClock::now();
	retryAttempt_ = 0;
	if (!lastError_.empty())
		lastErrorRecovered_ = true;
	MF_INFO("destination %s: %s server (attempt %llu)", short_id(id_).c_str(),
		reconnected ? "reconnected to" : "sending to", (unsigned long long)attemptId);
	notify();
}

void DestinationSession::onStopped(uint64_t attemptId, int obsCode, const std::string &detail)
{
	if (attemptId != liveAttempt_)
		return; // stale attempt
	if (stopRequested_ || afterRelease_ != AfterRelease::None)
		return; // expected stop, or failure already being handled
	handleFailure(attemptId, obsCode, detail, obsCode == OBS_OUTPUT_ENCODE_ERROR, true);
}

void DestinationSession::onStartFailed(uint64_t attemptId, int obsCode, const std::string &detail, bool encoderFailure)
{
	if (attemptId != liveAttempt_)
		return;
	/* The worker already released this attempt's output. */
	env_.encoderGroup().release(id_, attemptId);
	liveAttempt_ = 0;
	if (stopRequested_) {
		finishStop();
		notify();
		env_.sessionReleased(id_);
		return;
	}
	handleFailure(attemptId, obsCode, detail, encoderFailure, false);
	env_.sessionReleased(id_);
}

void DestinationSession::handleFailure(uint64_t attemptId, int obsCode, const std::string &detail, bool encoderFailure,
				       bool needsRelease)
{
	FailurePhase phase = state_ == SessionState::Sending        ? FailurePhase::Established
			     : state_ == SessionState::Reconnecting ? FailurePhase::Reconnecting
								    : FailurePhase::Connecting;
	bool secure = parse_rtmp_url(request_.serverUrl).secure();
	SessionError error = classify_stop(obsCode, detail, secure, phase, request_.secrets.values());

	AfterRelease next = AfterRelease::Error;
	if (encoderFailure) {
		error.kind = ErrorKind::Encoder;
		std::string reason = sanitize_message(detail);
		if (!reason.empty() && reason.back() != '.')
			reason += '.';
		error.message = "The shared encoder could not run" +
				(reason.empty() ? std::string(".") : ": " + reason) +
				" Every plugin destination using it is affected. Check Encoding settings "
				"(try x264 if a hardware encoder is unavailable).";
		env_.groupFailure(id_, error.message);
	} else if (request_.autoReconnect && code_is_retryable(obsCode, phase)) {
		if (phase == FailurePhase::Established) {
			reconnectCount_++;
			retryAttempt_ = 1;
		} else {
			retryAttempt_++;
		}
		if (retryAttempt_ > request_.maxReconnectAttempts) {
			error.kind = ErrorKind::Exhausted;
			error.message = "Gave up after " + std::to_string(request_.maxReconnectAttempts) +
					" reconnect attempts. Last error: " + error.message;
		} else {
			next = AfterRelease::Retry;
		}
	}

	lastError_ = error;
	lastErrorRecovered_ = false;
	bitrateKbps_ = 0.0;
	state_ = next == AfterRelease::Retry ? SessionState::Reconnecting : SessionState::Error;
	MF_WARN("destination %s: attempt %llu ended (%s): %s", short_id(id_).c_str(), (unsigned long long)attemptId,
		obs_code_name(obsCode), error.message.c_str());

	if (needsRelease) {
		afterRelease_ = next;
		worker().post([this, attemptId] { workerRelease(attemptId); });
	} else if (next == AfterRelease::Retry) {
		scheduleRetry();
	} else {
		request_.secrets.clear();
	}
	notify();
}

void DestinationSession::onReleased(uint64_t attemptId)
{
	env_.encoderGroup().release(id_, attemptId);
	if (attemptId != liveAttempt_) {
		env_.sessionReleased(id_);
		return;
	}
	liveAttempt_ = 0;
	AfterRelease next = afterRelease_;
	afterRelease_ = AfterRelease::None;
	if (stopRequested_) {
		finishStop();
		MF_INFO("destination %s: stopped", short_id(id_).c_str());
	} else if (next == AfterRelease::Retry && state_ == SessionState::Reconnecting) {
		scheduleRetry();
	} else {
		request_.secrets.clear();
	}
	notify();
	env_.sessionReleased(id_);
}

void DestinationSession::onStats(uint64_t attemptId, const TransportStats &stats, SteadyClock::time_point when,
				 bool attached)
{
	if (attemptId != liveAttempt_ || attemptId != statsAttempt_)
		return;
	if (prevSampleAt_ != SteadyClock::time_point{} && stats.totalBytes >= prevBytes_) {
		double seconds = std::chrono::duration<double>(when - prevSampleAt_).count();
		if (seconds > 0.2)
			bitrateKbps_ = (double)(stats.totalBytes - prevBytes_) * 8.0 / 1000.0 / seconds;
	}
	prevBytes_ = stats.totalBytes;
	prevSampleAt_ = when;
	stats_ = stats;
	attachedToShared_ = attached;
	if (state_ != SessionState::Sending)
		bitrateKbps_ = 0.0;
	notify();
}

SessionSnapshot DestinationSession::snapshot() const
{
	SessionSnapshot snap;
	snap.id = id_;
	snap.state = state_;
	snap.attemptId = liveAttempt_;
	snap.lastError = lastError_;
	snap.lastErrorRecovered = lastErrorRecovered_;
	snap.reconnectCount = reconnectCount_;
	snap.retryMax = request_.maxReconnectAttempts;
	if (state_ == SessionState::Sending) {
		snap.sendingSeconds =
			std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - sendingSince_).count();
		snap.bitrateKbps = bitrateKbps_;
		snap.framesDropped = stats_.framesDropped;
		snap.totalFrames = stats_.totalFrames;
		snap.dropStatsAvailable = stats_.totalFrames > 0;
		snap.dropPercent = stats_.totalFrames > 0 ? 100.0 * stats_.framesDropped / stats_.totalFrames : 0.0;
		snap.congestion = stats_.congestion;
		snap.attachedToSharedEncoders = attachedToShared_;
	}
	if (state_ == SessionState::Reconnecting) {
		snap.retryAttempt = retryAttempt_;
		if (retryTimer_) {
			auto remaining = std::chrono::duration<double>(retryDue_ - SteadyClock::now()).count();
			snap.retryInSeconds = remaining > 0 ? (int)std::ceil(remaining) : 0;
		}
	}
	return snap;
}

bool DestinationSession::waitWorkerIdle(LifecycleWorker::Clock::time_point deadline)
{
	return !worker_ || worker_->waitIdle(deadline);
}

void DestinationSession::finalizeAfterSyncStop()
{
	cancelRetry();
	if (liveAttempt_ != 0) {
		/* The worker is idle, so the attempt's output is already released; its queued
		 * Released event will arrive later with a stale id and only drop the hold again. */
		env_.encoderGroup().release(id_, liveAttempt_);
		liveAttempt_ = 0;
	}
	finishStop();
	notify();
}

void DestinationSession::beginShutdown()
{
	generation_++;
	cancelRetry();
	if (!worker_)
		return;
	if (liveAttempt_ != 0 && !stopRequested_) {
		stopRequested_ = true;
		state_ = SessionState::Stopping;
		uint64_t attemptId = liveAttempt_;
		worker_->post([this, attemptId] { workerStop(attemptId, true, 2000); });
	}
	/* Releases any attempt still known to the worker, then exits. */
	worker_->post([this] {
		while (!transports_.empty())
			workerRelease(transports_.begin()->first);
	});
	worker_->requestQuit();
}

bool DestinationSession::waitShutdown(LifecycleWorker::Clock::time_point deadline)
{
	if (!worker_)
		return true;
	return worker_->join(deadline);
}

void DestinationSession::abandonWorker()
{
	if (worker_) {
		worker_->abandon();
		(void)worker_.release(); // intentionally leaked: its thread may still reference it
	}
}

/* ---- worker thread ---- */

void DestinationSession::workerStart(AttemptSpec spec)
{
	uint64_t attemptId = spec.attemptId;
	std::string error;
	std::unique_ptr<Transport> transport = env_.createTransport(spec, sink_, error);
	if (!transport) {
		spec = {};
		sink_->startFailed(attemptId, OBS_OUTPUT_ERROR, error, false);
		return;
	}
	StartFailure failure;
	if (!transport->start(failure)) {
		transport.reset();
		spec = {};
		sink_->startFailed(attemptId, failure.obsCode, failure.detail, failure.encoderFailure);
		return;
	}
	LiveTransport live;
	live.video = spec.videoEncoder;
	live.audio = spec.audioEncoder;
	live.transport = std::move(transport);
	transports_[attemptId] = std::move(live);
	/* spec (with its copy of the secrets) is destroyed here. */
}

void DestinationSession::workerStop(uint64_t attemptId, bool force, int graceMs)
{
	auto it = transports_.find(attemptId);
	if (it != transports_.end()) {
		Transport &transport = *it->second.transport;
		transport.stop(force);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
		while (transport.active() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (transport.active()) {
			MF_WARN("attempt %llu did not stop within %d ms; forcing", (unsigned long long)attemptId,
				graceMs);
			transport.stop(true);
		}
	}
	workerRelease(attemptId);
}

void DestinationSession::workerRelease(uint64_t attemptId)
{
	auto it = transports_.find(attemptId);
	if (it != transports_.end()) {
		std::unique_ptr<Transport> transport = std::move(it->second.transport);
		transports_.erase(it);
		transport.reset(); // releases the obs_output (bounded by OBS's own network timeouts)
	}
	sink_->released(attemptId);
}

void DestinationSession::workerTick()
{
	for (auto &entry : transports_) {
		LiveTransport &live = entry.second;
		sink_->stats(entry.first, live.transport->sample(), live.transport->attachedTo(live.video, live.audio));
	}
}

} // namespace mf
