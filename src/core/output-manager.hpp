// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "bandwidth.hpp"
#include "destination-session.hpp"
#include "dispatcher.hpp"
#include "media-info.hpp"
#include "profile-config.hpp"
#include "reconnect-policy.hpp"
#include "service-constraints.hpp"
#include "shared-encoder-group.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mf {

struct ManagerSummary {
	int configured = 0;
	int enabled = 0;
	int sending = 0;
	int connecting = 0;
	int reconnecting = 0;
	int stopping = 0;
	int errors = 0;
	int queued = 0;
	double measuredKbps = 0.0;  // sum of measured outbound bitrate of sending destinations
	BandwidthEstimate estimate; // for the enabled destinations, plugin outputs only
	bool degraded = false;      // an enabled destination is failing or reconnecting
	std::string groupFailure;
	bool encoderLocked = false;
};

/*
 * Owns every DestinationSession and the SharedEncoderGroup for the loaded OBS profile.
 * Main thread only. Policy enforced here (not just in the UI):
 *  - URL, credentials, enabled flag and deletion are locked while a destination is busy;
 *  - shared encoding settings are locked while any plugin output exists;
 *  - at most kMaxSimultaneousOutputs destinations hold an output at once;
 *  - Start Enabled only starts idle/errored enabled destinations, one every startStaggerMs;
 *  - Stop All cancels queued starts and pending reconnects and stops plugin outputs only.
 */
class OutputManager : private SessionEnvironment {
public:
	struct Options {
		std::function<std::unique_ptr<Transport>(const AttemptSpec &, std::shared_ptr<TransportEvents>,
							 std::string &)>
			transportFactory = create_obs_rtmp_transport;
		ReconnectSchedule reconnectSchedule = reconnect_delay_ms;
		int startStaggerMs = 200;
		int maxShutdownSec = 5;
		std::function<MediaInfo()> mediaProvider = read_media_info;
		bool checkPlatformConstraints = true;
		SharedEncoderGroup::EncoderFactory encoderFactory = SharedEncoderGroup::create_obs_encoders;
	};

	using SecretsProvider =
		std::function<bool(const DestinationConfig &config, DestinationSecrets &secrets, std::string &error)>;

	class Listener {
	public:
		virtual ~Listener() = default;
		virtual void destinationChanged(const std::string &id) = 0;
		virtual void managerChanged() = 0;
	};

	OutputManager(Dispatcher &dispatcher, Options options);
	~OutputManager() override;

	void setListener(Listener *listener) { listener_ = listener; }
	void setSecretsProvider(SecretsProvider provider) { secretsProvider_ = std::move(provider); }

	/* Configuration. */
	bool loadProfile(const ProfileConfig &config, std::string &error);
	const ProfileConfig &config() const { return config_; }
	bool addDestination(const DestinationConfig &config, std::string &error);
	bool updateDestination(const DestinationConfig &config, std::string &error);
	bool removeDestination(const std::string &id, std::string &error);
	bool setEnabled(const std::string &id, bool enabled, std::string &error);
	bool setEncoderProfile(const EncoderProfile &profile, std::string &error);
	bool replaceAll(std::vector<DestinationConfig> destinations, const EncoderProfile *profile, std::string &error);

	/* Control. */
	bool startDestination(const std::string &id, std::string &error);
	void stopDestination(const std::string &id);
	struct StartAllReport {
		std::vector<std::string> queued;
		std::vector<std::pair<std::string, std::string>> skipped; // id, reason
	};
	StartAllReport startEnabled();
	/* Destinations a Stop All would affect: busy or queued. */
	std::vector<std::string> stopAllTargets() const;
	void stopAll(StopReason reason = StopReason::StopAll);
	/* Blocks the main thread up to budget; returns false if some destination is still stopping. */
	bool stopAllAndWait(StopReason reason, std::chrono::milliseconds budget);
	/* OBS exit / module unload. Blocks up to budget, then up to hardCap, then abandons. */
	void shutdown(std::chrono::milliseconds budget, std::chrono::milliseconds hardCap);
	/* A teardown exceeded the hard cap and its worker thread was left running. */
	bool abandonedWorkers() const { return abandonedWorkers_; }

	/* Queries. */
	bool destinationLocked(const std::string &id) const;
	bool encoderLocked() const;
	bool anyBusy() const;
	bool isQueued(const std::string &id) const;
	SessionSnapshot snapshot(const std::string &id) const;
	ManagerSummary summary() const;
	const SharedEncoderGroup &sharedEncoders() const { return group_; }
	MediaInfo mediaInfo() const { return options_.mediaProvider(); }
	Issues runtimeIssues() const;
	Issues destinationIssues(const std::string &id) const;
	std::string diagnostics() const;

private:
	/* SessionEnvironment */
	Dispatcher &dispatcher() override { return dispatcher_; }
	SharedEncoderGroup &encoderGroup() override { return group_; }
	std::unique_ptr<Transport> createTransport(const AttemptSpec &spec, std::shared_ptr<TransportEvents> events,
						   std::string &error) override;
	int reconnectDelayMs(int attempt) override { return options_.reconnectSchedule(attempt); }
	int maxShutdownSec() override { return options_.maxShutdownSec; }
	void sessionChanged(const std::string &destinationId) override;
	void sessionReleased(const std::string &destinationId) override;
	void groupFailure(const std::string &destinationId, const std::string &message) override;

	DestinationSession *session(const std::string &id) const;
	void createSession(const DestinationConfig &config);
	void processStartQueue();
	void cancelStartQueue();
	int outputsInUse() const;
	void notifyManager();
	void reapDraining();
	const PlatformConstraints &constraintsFor(Platform platform) const;

	Dispatcher &dispatcher_;
	Options options_;
	Listener *listener_ = nullptr;
	SecretsProvider secretsProvider_;
	ProfileConfig config_;
	SharedEncoderGroup group_;
	std::optional<EncoderProfile> pendingProfile_;
	std::map<std::string, std::unique_ptr<DestinationSession>> sessions_;
	/* Sessions from a previous profile still finishing a stop that exceeded its budget. */
	std::vector<std::unique_ptr<DestinationSession>> draining_;
	std::deque<std::string> startQueue_;
	TimerId startTimer_ = 0;
	bool shutDown_ = false;
	bool abandonedWorkers_ = false;
	/* Guards callbacks posted to the dispatcher against running after destruction. */
	std::shared_ptr<int> alive_ = std::make_shared<int>(0);

	struct ConstraintCacheEntry {
		uint32_t width = 0, height = 0, fpsNum = 0, fpsDen = 0;
		PlatformConstraints constraints;
	};
	mutable std::map<Platform, ConstraintCacheEntry> constraintCache_;
};

} // namespace mf
