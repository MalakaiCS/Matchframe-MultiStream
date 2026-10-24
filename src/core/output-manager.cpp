// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "output-manager.hpp"
#include "log.hpp"
#include "redaction.hpp"

#include <obs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mf {

namespace {

std::string format_kbps(double kbps)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%.0f", kbps);
	return buffer;
}

} // namespace

OutputManager::OutputManager(Dispatcher &dispatcher, Options options)
	: dispatcher_(dispatcher),
	  options_(std::move(options)),
	  group_(options_.encoderFactory)
{
}

OutputManager::~OutputManager()
{
	if (!shutDown_)
		shutdown(std::chrono::milliseconds(5000), std::chrono::milliseconds(30000));
}

std::unique_ptr<Transport> OutputManager::createTransport(const AttemptSpec &spec,
							  std::shared_ptr<TransportEvents> events, std::string &error)
{
	return options_.transportFactory(spec, std::move(events), error);
}

DestinationSession *OutputManager::session(const std::string &id) const
{
	auto it = sessions_.find(id);
	return it == sessions_.end() ? nullptr : it->second.get();
}

void OutputManager::createSession(const DestinationConfig &config)
{
	SessionEnvironment &environment = *this;
	sessions_[config.id] = std::make_unique<DestinationSession>(config.id, environment, config.enabled);
}

void OutputManager::notifyManager()
{
	if (listener_)
		listener_->managerChanged();
}

void OutputManager::sessionChanged(const std::string &destinationId)
{
	if (listener_)
		listener_->destinationChanged(destinationId);
}

void OutputManager::sessionReleased(const std::string &)
{
	/* Never destroy a session from inside its own event handler: reap on the next turn. */
	if (!draining_.empty()) {
		std::weak_ptr<int> alive = alive_;
		dispatcher_.post([this, alive] {
			if (alive.lock())
				reapDraining();
		});
	}
	if (pendingProfile_ && !group_.inUse()) {
		std::string error;
		if (group_.setProfile(*pendingProfile_, error))
			pendingProfile_.reset();
	}
	notifyManager();
}

void OutputManager::groupFailure(const std::string &, const std::string &message)
{
	group_.markFailure(message);
	notifyManager();
}

void OutputManager::reapDraining()
{
	draining_.erase(std::remove_if(draining_.begin(), draining_.end(),
				       [](const std::unique_ptr<DestinationSession> &s) { return !s->busy(); }),
			draining_.end());
}

bool OutputManager::anyBusy() const
{
	for (const auto &entry : sessions_) {
		if (entry.second->busy())
			return true;
	}
	return !startQueue_.empty();
}

bool OutputManager::destinationLocked(const std::string &id) const
{
	DestinationSession *s = session(id);
	return s && s->busy();
}

bool OutputManager::encoderLocked() const
{
	if (group_.inUse())
		return true;
	for (const auto &entry : sessions_) {
		if (entry.second->busy())
			return true;
	}
	return false;
}

bool OutputManager::isQueued(const std::string &id) const
{
	return std::find(startQueue_.begin(), startQueue_.end(), id) != startQueue_.end();
}

int OutputManager::outputsInUse() const
{
	int count = 0;
	for (const auto &entry : sessions_) {
		if (entry.second->busy())
			count++;
	}
	return count + (int)draining_.size();
}

bool OutputManager::loadProfile(const ProfileConfig &config, std::string &error)
{
	if (anyBusy()) {
		error = "Plugin outputs must be stopped before another profile's configuration is loaded.";
		return false;
	}
	cancelStartQueue();
	sessions_.clear();
	config_ = config;
	if (group_.inUse()) {
		/* Draining sessions from the previous profile still hold encoders. */
		pendingProfile_ = config_.encoder;
	} else if (!group_.setProfile(config_.encoder, error)) {
		MF_WARN("stored encoding settings rejected (%s); defaults used", error.c_str());
		config_.encoder = EncoderProfile{};
		std::string ignored;
		group_.setProfile(config_.encoder, ignored);
		error.clear();
	}
	for (const DestinationConfig &dest : config_.destinations)
		createSession(dest);
	constraintCache_.clear();
	notifyManager();
	return true;
}

bool OutputManager::addDestination(const DestinationConfig &config, std::string &error)
{
	if (config.id.empty() || config_.find(config.id)) {
		error = "Destination id is missing or already used.";
		return false;
	}
	if (config_.destinations.size() >= kMaxDestinations) {
		error = "At most 32 destinations can be saved per profile.";
		return false;
	}
	Issues issues = validate_destination(config);
	if (has_errors(issues)) {
		error = first_error(issues);
		return false;
	}
	config_.destinations.push_back(config);
	createSession(config);
	notifyManager();
	return true;
}

bool OutputManager::updateDestination(const DestinationConfig &config, std::string &error)
{
	DestinationConfig *existing = config_.find(config.id);
	if (!existing) {
		error = "Destination not found.";
		return false;
	}
	if (destinationLocked(config.id) && !existing->sameTransportSettings(config)) {
		error = "Stop this destination before changing its server, credentials, platform or reconnect settings.";
		return false;
	}
	Issues issues = validate_destination(config);
	if (has_errors(issues)) {
		error = first_error(issues);
		return false;
	}
	bool enabledChanged = existing->enabled != config.enabled;
	*existing = config;
	if (enabledChanged) {
		session(config.id)->setEnabled(config.enabled);
		if (!config.enabled)
			startQueue_.erase(std::remove(startQueue_.begin(), startQueue_.end(), config.id),
					  startQueue_.end());
	}
	sessionChanged(config.id);
	notifyManager();
	return true;
}

bool OutputManager::removeDestination(const std::string &id, std::string &error)
{
	if (destinationLocked(id)) {
		error = "Stop this destination before deleting it.";
		return false;
	}
	auto it = std::find_if(config_.destinations.begin(), config_.destinations.end(),
			       [&](const DestinationConfig &d) { return d.id == id; });
	if (it == config_.destinations.end()) {
		error = "Destination not found.";
		return false;
	}
	config_.destinations.erase(it);
	startQueue_.erase(std::remove(startQueue_.begin(), startQueue_.end(), id), startQueue_.end());
	sessions_.erase(id);
	notifyManager();
	return true;
}

bool OutputManager::setEnabled(const std::string &id, bool enabled, std::string &error)
{
	DestinationConfig *existing = config_.find(id);
	if (!existing) {
		error = "Destination not found.";
		return false;
	}
	if (existing->enabled == enabled)
		return true;
	if (destinationLocked(id)) {
		error = "Stop this destination before disabling it.";
		return false;
	}
	DestinationConfig updated = *existing;
	updated.enabled = enabled;
	return updateDestination(updated, error);
}

bool OutputManager::setEncoderProfile(const EncoderProfile &profile, std::string &error)
{
	if (encoderLocked()) {
		error = "Shared encoding settings are locked while any plugin output is active.";
		return false;
	}
	if (!group_.setProfile(profile, error))
		return false;
	config_.encoder = profile;
	pendingProfile_.reset();
	constraintCache_.clear();
	notifyManager();
	return true;
}

bool OutputManager::replaceAll(std::vector<DestinationConfig> destinations, const EncoderProfile *profile,
			       std::string &error)
{
	if (anyBusy()) {
		error = "Stop all plugin outputs before replacing the destination list.";
		return false;
	}
	if (destinations.size() > kMaxDestinations) {
		error = "At most 32 destinations can be saved per profile.";
		return false;
	}
	if (profile && !group_.setProfile(*profile, error))
		return false;
	if (profile)
		config_.encoder = *profile;
	cancelStartQueue();
	sessions_.clear();
	config_.destinations = std::move(destinations);
	for (const DestinationConfig &dest : config_.destinations)
		createSession(dest);
	notifyManager();
	return true;
}

bool OutputManager::startDestination(const std::string &id, std::string &error)
{
	if (shutDown_) {
		error = "The plugin is shutting down.";
		return false;
	}
	const DestinationConfig *config = config_.find(id);
	DestinationSession *s = session(id);
	if (!config || !s) {
		error = "Destination not found.";
		return false;
	}
	if (!config->enabled) {
		error = "This destination is disabled. Enable it first.";
		return false;
	}
	if (s->busy()) {
		error = "This destination is already active.";
		return false;
	}

	auto fail = [&](ErrorKind kind, const std::string &message) {
		error = message;
		s->failBeforeStart({kind, sanitize_message(message), 0});
		notifyManager();
		return false;
	};

	if (outputsInUse() >= kMaxSimultaneousOutputs)
		return fail(ErrorKind::Configuration, "At most 8 plugin outputs can run at the same time.");
	if (pendingProfile_)
		return fail(ErrorKind::Configuration,
			    "Outputs from the previous profile are still stopping. Try again in a moment.");

	Issues issues = validate_destination(*config);
	if (has_errors(issues))
		return fail(ErrorKind::Configuration, first_error(issues));
	Issues runtime = runtimeIssues();
	if (has_errors(runtime))
		return fail(ErrorKind::Configuration, first_error(runtime));

	StartRequest request;
	request.displayName = config->name;
	request.serverUrl = config->serverUrl;
	request.useAuth = config->useAuth;
	request.autoReconnect = config->autoReconnect;
	request.maxReconnectAttempts = config->maxReconnectAttempts;
	std::string secretError;
	if (!secretsProvider_ || !secretsProvider_(*config, request.secrets, secretError))
		return fail(ErrorKind::Configuration,
			    secretError.empty() ? std::string("No credentials are available.") : secretError);
	Issues secretIssues = validate_secrets(*config, request.secrets);
	if (has_errors(secretIssues))
		return fail(ErrorKind::Configuration, first_error(secretIssues));

	bool started = s->start(std::move(request), error);
	notifyManager();
	return started;
}

void OutputManager::stopDestination(const std::string &id)
{
	startQueue_.erase(std::remove(startQueue_.begin(), startQueue_.end(), id), startQueue_.end());
	if (DestinationSession *s = session(id))
		s->stop(StopReason::Manual);
	notifyManager();
}

OutputManager::StartAllReport OutputManager::startEnabled()
{
	StartAllReport report;
	for (const DestinationConfig &dest : config_.destinations) {
		DestinationSession *s = session(dest.id);
		if (!dest.enabled) {
			report.skipped.push_back({dest.id, "disabled"});
		} else if (!s || s->busy()) {
			report.skipped.push_back({dest.id, "already active"});
		} else if (isQueued(dest.id)) {
			report.skipped.push_back({dest.id, "already queued"});
		} else {
			startQueue_.push_back(dest.id);
			report.queued.push_back(dest.id);
		}
	}
	if (!startTimer_)
		processStartQueue();
	notifyManager();
	return report;
}

void OutputManager::processStartQueue()
{
	startTimer_ = 0;
	while (!startQueue_.empty()) {
		std::string id = startQueue_.front();
		startQueue_.pop_front();
		DestinationSession *s = session(id);
		const DestinationConfig *config = config_.find(id);
		if (!s || !config || !config->enabled || s->busy())
			continue;
		std::string error;
		if (startDestination(id, error))
			break;
		/* A failing destination records its own error and never blocks the rest. */
		MF_WARN("destination %s: not started: %s", id.substr(0, 8).c_str(), sanitize_message(error).c_str());
	}
	if (!startQueue_.empty())
		startTimer_ =
			dispatcher_.postDelayed(options_.startStaggerMs, [this, alive = std::weak_ptr<int>(alive_)] {
				if (alive.lock())
					processStartQueue();
			});
	notifyManager();
}

void OutputManager::cancelStartQueue()
{
	startQueue_.clear();
	if (startTimer_) {
		dispatcher_.cancel(startTimer_);
		startTimer_ = 0;
	}
}

std::vector<std::string> OutputManager::stopAllTargets() const
{
	std::vector<std::string> targets;
	for (const DestinationConfig &dest : config_.destinations) {
		DestinationSession *s = session(dest.id);
		if ((s && s->busy()) || isQueued(dest.id))
			targets.push_back(dest.id);
	}
	return targets;
}

void OutputManager::stopAll(StopReason reason)
{
	cancelStartQueue();
	for (auto &entry : sessions_) {
		if (entry.second->busy())
			entry.second->stop(reason);
	}
	MF_INFO("stop all plugin outputs requested");
	notifyManager();
}

bool OutputManager::stopAllAndWait(StopReason reason, std::chrono::milliseconds budget)
{
	stopAll(reason);
	auto deadline = LifecycleWorker::Clock::now() + budget;
	bool allIdle = true;
	for (auto &entry : sessions_) {
		if (!entry.second->holdsOutput() && !entry.second->busy())
			continue;
		if (entry.second->waitWorkerIdle(deadline))
			entry.second->finalizeAfterSyncStop();
		else
			allIdle = false;
	}
	if (!allIdle) {
		/* Keep the stragglers alive (they still own an output) but out of the active set. */
		for (auto it = sessions_.begin(); it != sessions_.end();) {
			if (it->second->busy()) {
				MF_WARN("destination %s: still stopping after %lld ms; continuing in background",
					it->first.substr(0, 8).c_str(), (long long)budget.count());
				draining_.push_back(std::move(it->second));
				std::string id = it->first;
				it = sessions_.erase(it);
				const DestinationConfig *config = config_.find(id);
				if (config)
					createSession(*config);
			} else {
				++it;
			}
		}
	}
	notifyManager();
	return allIdle;
}

void OutputManager::shutdown(std::chrono::milliseconds budget, std::chrono::milliseconds hardCap)
{
	if (shutDown_)
		return;
	shutDown_ = true;
	cancelStartQueue();
	MF_INFO("shutting down plugin outputs");

	std::vector<std::unique_ptr<DestinationSession>> all;
	for (auto &entry : sessions_)
		all.push_back(std::move(entry.second));
	sessions_.clear();
	for (auto &s : draining_)
		all.push_back(std::move(s));
	draining_.clear();

	for (auto &s : all)
		s->beginShutdown();

	auto start = LifecycleWorker::Clock::now();
	auto soft = start + budget;
	auto hard = start + std::max(budget, hardCap);
	for (auto &s : all) {
		if (!s->waitShutdown(soft)) {
			MF_WARN("destination %s: teardown exceeded %lld ms, waiting up to %lld ms more",
				s->id().substr(0, 8).c_str(), (long long)budget.count(),
				(long long)(hardCap - budget).count());
			if (!s->waitShutdown(hard)) {
				MF_ERROR("destination %s: teardown did not finish; abandoning its worker",
					 s->id().substr(0, 8).c_str());
				s->abandonWorker();
				(void)s.release(); // its worker may still reference it
				continue;
			}
		}
		/* The worker has exited, so its output is released; drop the encoder hold now
		 * because the queued "released" event will never be processed. */
		s->finalizeAfterSyncStop();
	}
	all.clear();
	if (!group_.releaseEncoders())
		MF_WARN("shared encoders still referenced by an abandoned destination");
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(LifecycleWorker::Clock::now() - start);
	MF_INFO("plugin outputs shut down in %lld ms", (long long)elapsed.count());
}

SessionSnapshot OutputManager::snapshot(const std::string &id) const
{
	DestinationSession *s = session(id);
	if (!s) {
		SessionSnapshot empty;
		empty.id = id;
		return empty;
	}
	return s->snapshot();
}

ManagerSummary OutputManager::summary() const
{
	ManagerSummary summary;
	summary.configured = (int)config_.destinations.size();
	for (const DestinationConfig &dest : config_.destinations) {
		if (dest.enabled)
			summary.enabled++;
		DestinationSession *s = session(dest.id);
		if (!s)
			continue;
		SessionSnapshot snap = s->snapshot();
		switch (snap.state) {
		case SessionState::Sending:
			summary.sending++;
			summary.measuredKbps += snap.bitrateKbps;
			break;
		case SessionState::Connecting:
			summary.connecting++;
			break;
		case SessionState::Reconnecting:
			summary.reconnecting++;
			if (dest.enabled)
				summary.degraded = true;
			break;
		case SessionState::Stopping:
			summary.stopping++;
			break;
		case SessionState::Error:
			summary.errors++;
			if (dest.enabled)
				summary.degraded = true;
			break;
		default:
			break;
		}
	}
	summary.queued = (int)startQueue_.size();
	summary.estimate =
		estimate_bandwidth(summary.enabled, config_.encoder.videoBitrateKbps, config_.encoder.audioBitrateKbps);
	summary.groupFailure = group_.failure();
	if (!summary.groupFailure.empty())
		summary.degraded = true;
	summary.encoderLocked = encoderLocked();
	return summary;
}

Issues OutputManager::runtimeIssues() const
{
	Issues issues = validate_media(options_.mediaProvider());
	append(issues, validate_encoder_profile(config_.encoder));
	return issues;
}

const PlatformConstraints &OutputManager::constraintsFor(Platform platform) const
{
	MediaInfo media = options_.mediaProvider();
	ConstraintCacheEntry &entry = constraintCache_[platform];
	if (entry.width != media.outputWidth || entry.height != media.outputHeight || entry.fpsNum != media.fpsNum ||
	    entry.fpsDen != media.fpsDen || entry.fpsDen == 0) {
		entry.width = media.outputWidth;
		entry.height = media.outputHeight;
		entry.fpsNum = media.fpsNum;
		entry.fpsDen = media.fpsDen ? media.fpsDen : 1;
		entry.constraints = options_.checkPlatformConstraints ? query_platform_constraints(platform)
								      : PlatformConstraints{};
	}
	return entry.constraints;
}

Issues OutputManager::destinationIssues(const std::string &id) const
{
	Issues issues;
	const DestinationConfig *config = config_.find(id);
	if (!config)
		return issues;
	append(issues, validate_destination(*config));
	if (options_.checkPlatformConstraints)
		append(issues, check_platform_constraints(config->platform, constraintsFor(config->platform),
							  config_.encoder, options_.mediaProvider()));
	return issues;
}

std::string OutputManager::diagnostics() const
{
	std::string out;
	MediaInfo media = options_.mediaProvider();
	out += "OBS version: " + std::string(obs_get_version_string()) + "\n";
	out += "Program video: " + media.describeVideo() + " (" + video_format_name(media.format) + ")\n";
	out += "Program audio: " + media.describeAudio() + "\n";
	const EncoderProfile &p = config_.encoder;
	out += "Shared profile: H.264 " + std::to_string(p.videoBitrateKbps) + " kbps CBR, keyframe " +
	       std::to_string(p.keyframeIntervalSec) + " s; AAC " + std::to_string(p.audioBitrateKbps) +
	       " kbps; audio track " + std::to_string(p.audioTrack) + " (mixer index " +
	       std::to_string(p.mixerIndex()) + ")\n";
	out += "Encoder group: " + group_.describe() + "\n";
	if (!group_.failure().empty())
		out += "Encoder group failure: " + group_.failure() + "\n";

	struct Count {
		int video = 0;
		int audio = 0;
	} count;
	obs_enum_encoders(
		[](void *param, obs_encoder_t *encoder) {
			auto *c = static_cast<Count *>(param);
			const char *name = obs_encoder_get_name(encoder);
			if (name && strcmp(name, SharedEncoderGroup::kVideoEncoderName) == 0)
				c->video++;
			if (name && strcmp(name, SharedEncoderGroup::kAudioEncoderName) == 0)
				c->audio++;
			return true;
		},
		&count);
	out += "Plugin encoder objects in OBS: video=" + std::to_string(count.video) +
	       ", audio=" + std::to_string(count.audio) + "\n";

	ManagerSummary sum = summary();
	char line[256];
	snprintf(line, sizeof(line),
		 "Destinations: %d configured, %d enabled, %d sending, %d reconnecting, %d error; measured %s kbps; "
		 "estimate %.2f Mbps payload / %.1f Mbps recommended\n",
		 sum.configured, sum.enabled, sum.sending, sum.reconnecting, sum.errors,
		 format_kbps(sum.measuredKbps).c_str(), sum.estimate.payloadMbps, sum.estimate.recommendedMbps);
	out += line;

	for (const DestinationConfig &dest : config_.destinations) {
		SessionSnapshot snap = snapshot(dest.id);
		out += "- " + dest.id.substr(0, 8) + " \"" + sanitize_message(dest.name) + "\" [" +
		       platform_info(dest.platform).label + "] " + redact_url(dest.serverUrl) + "\n";
		snprintf(line, sizeof(line),
			 "    state=%s enabled=%s attempt=%llu bitrate=%s kbps dropped=%d/%d reconnects=%d "
			 "shared-encoders=%s\n",
			 session_state_name(snap.state), dest.enabled ? "yes" : "no",
			 (unsigned long long)snap.attemptId, format_kbps(snap.bitrateKbps).c_str(), snap.framesDropped,
			 snap.totalFrames, snap.reconnectCount, snap.attachedToSharedEncoders ? "yes" : "n/a");
		out += line;
		if (!snap.lastError.empty())
			out += "    last error: " + snap.lastError.message + "\n";
	}
	return out;
}

} // namespace mf
