// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "common/fake-transport.hpp"
#include "common/manual-dispatcher.hpp"
#include "common/test-framework.hpp"

#include "core/output-manager.hpp"

#include <obs-defs.h>

#include <chrono>
#include <set>

using namespace mf;
using namespace std::chrono_literals;
using mftest::Behaviour;

namespace {

MediaInfo good_media()
{
	MediaInfo m;
	m.videoAvailable = true;
	m.baseWidth = m.outputWidth = 1920;
	m.baseHeight = m.outputHeight = 1080;
	m.fpsNum = 30;
	m.fpsDen = 1;
	m.format = VIDEO_FORMAT_NV12;
	m.colorspace = VIDEO_CS_709;
	m.audioAvailable = true;
	m.sampleRate = 48000;
	m.speakers = SPEAKERS_STEREO;
	return m;
}

/* Wires an OutputManager to the fake network, a fake encoder factory and a manual loop. */
struct Harness {
	mftest::ManualDispatcher loop;
	mftest::FakeNetwork network;
	int encoderCreations = 0;
	bool encoderFactoryFails = false;
	std::unique_ptr<OutputManager> manager;
	std::set<std::string> missingKeys;

	Harness()
	{
		OutputManager::Options options;
		options.transportFactory = [this](const AttemptSpec &spec, std::shared_ptr<TransportEvents> events,
						  std::string &) -> std::unique_ptr<Transport> {
			return std::make_unique<mftest::FakeTransport>(network, spec, std::move(events));
		};
		options.mediaProvider = good_media;
		options.checkPlatformConstraints = false;
		options.encoderFactory = [this](const EncoderProfile &, SharedEncoderGroup::CreatedEncoders &out,
						std::string &error) {
			if (encoderFactoryFails) {
				error = "fake encoder failure";
				return false;
			}
			encoderCreations++;
			out.videoChoice.id = "fake_h264";
			out.audioChoice.id = "fake_aac";
			return true;
		};
		manager = std::make_unique<OutputManager>(loop, std::move(options));
		manager->setSecretsProvider(
			[this](const DestinationConfig &config, DestinationSecrets &secrets, std::string &error) {
				if (missingKeys.count(config.id)) {
					error = "No stream key is stored for this destination.";
					return false;
				}
				secrets.streamKey = "key-for-" + config.id;
				return true;
			});
		ProfileConfig config;
		config.profileId = "profile";
		std::string error;
		manager->loadProfile(config, error);
	}

	~Harness() { manager->shutdown(2000ms, 5000ms); }

	std::string add(const std::string &name, bool enabled = true, int maxAttempts = kDefaultMaxReconnectAttempts)
	{
		DestinationConfig d;
		d.id = name + "-id";
		d.name = name;
		d.platform = Platform::Custom;
		d.serverUrl = "rtmp://127.0.0.1:1935/live";
		d.enabled = enabled;
		d.maxReconnectAttempts = maxAttempts;
		std::string error;
		bool ok = manager->addDestination(d, error);
		if (!ok)
			std::fprintf(stderr, "add failed: %s\n", error.c_str());
		return d.id;
	}

	SessionState state(const std::string &id) { return manager->snapshot(id).state; }

	bool waitFor(const std::string &id, SessionState wanted, std::chrono::milliseconds timeout = 2000ms)
	{
		return loop.runUntil([&] { return state(id) == wanted; }, timeout);
	}

	/* Lets worker threads finish queued work and the loop process their events. */
	void settle()
	{
		loop.runUntil([] { return false; }, 60ms);
	}
};

} // namespace

MF_TEST(session_start_sending_stop_idle)
{
	Harness h;
	std::string id = h.add("A");
	std::string error;
	REQUIRE(h.manager->startDestination(id, error));
	CHECK(h.state(id) == SessionState::Connecting);
	CHECK(h.manager->encoderLocked());
	REQUIRE(h.waitFor(id, SessionState::Sending));
	CHECK_EQ(h.network.keysSeen.back(), std::string("key-for-A-id"));

	h.loop.runUntil([&] { return h.manager->snapshot(id).bitrateKbps > 0; }, 3000ms);
	CHECK(h.manager->snapshot(id).bitrateKbps > 1000);

	h.manager->stopDestination(id);
	CHECK(h.state(id) == SessionState::Stopping);
	REQUIRE(h.waitFor(id, SessionState::Idle));
	CHECK_EQ(h.network.live(id), (size_t)0);
	CHECK(!h.manager->sharedEncoders().inUse());
	CHECK(!h.manager->encoderLocked());
}

MF_TEST(reconnect_follows_policy_and_recovers)
{
	Harness h;
	std::string id = h.add("A");
	h.network.script(id, {Behaviour{Behaviour::Connect}});
	std::string error;
	REQUIRE(h.manager->startDestination(id, error));
	REQUIRE(h.waitFor(id, SessionState::Sending));

	/* Five failed reconnect attempts, then success. */
	for (int i = 0; i < 5; i++)
		h.network.script(id, {Behaviour{Behaviour::ConnectFail, OBS_OUTPUT_CONNECT_FAILED}});
	h.network.script(id, {Behaviour{Behaviour::Connect}});

	h.loop.requestedDelays.clear();
	REQUIRE(h.network.drop(id));
	REQUIRE(h.waitFor(id, SessionState::Reconnecting));
	CHECK_EQ(h.manager->snapshot(id).reconnectCount, 1);

	std::vector<int> expected = {2000, 4000, 8000, 16000, 30000, 30000};
	for (size_t i = 0; i < expected.size(); i++) {
		h.loop.runUntil([&] { return h.loop.requestedDelays.size() > i; }, 2000ms);
		REQUIRE(h.loop.requestedDelays.size() > i);
		CHECK_EQ(h.loop.requestedDelays[i], expected[i]);
		CHECK(h.state(id) == SessionState::Reconnecting);
		h.loop.advance(std::chrono::milliseconds(expected[i]));
		h.settle();
	}
	REQUIRE(h.waitFor(id, SessionState::Sending));
	SessionSnapshot snap = h.manager->snapshot(id);
	CHECK(snap.lastErrorRecovered);
	CHECK_EQ(snap.reconnectCount, 1);
	CHECK(h.network.maxConcurrent(id) == 1);
}

MF_TEST(reconnect_exhaustion_reaches_error_then_manual_restart)
{
	Harness h;
	std::string id = h.add("A", true, 3);
	std::string error;
	REQUIRE(h.manager->startDestination(id, error));
	REQUIRE(h.waitFor(id, SessionState::Sending));
	for (int i = 0; i < 3; i++)
		h.network.script(id, {Behaviour{Behaviour::ConnectFail, OBS_OUTPUT_CONNECT_FAILED}});
	REQUIRE(h.network.drop(id));
	for (int i = 0; i < 4 && h.state(id) != SessionState::Error; i++) {
		h.settle();
		h.loop.advance(30000ms);
		h.settle();
	}
	REQUIRE(h.waitFor(id, SessionState::Error));
	SessionSnapshot snap = h.manager->snapshot(id);
	CHECK(snap.lastError.kind == ErrorKind::Exhausted);
	CHECK(snap.lastError.message.find("Gave up after 3") != std::string::npos);
	CHECK(h.manager->summary().degraded);
	CHECK_EQ(h.network.live(id), (size_t)0);

	REQUIRE(h.manager->startDestination(id, error));
	REQUIRE(h.waitFor(id, SessionState::Sending));
}

MF_TEST(manual_stop_cancels_pending_reconnect_immediately)
{
	Harness h;
	std::string id = h.add("A");
	std::string error;
	REQUIRE(h.manager->startDestination(id, error));
	REQUIRE(h.waitFor(id, SessionState::Sending));
	REQUIRE(h.network.drop(id));
	REQUIRE(h.waitFor(id, SessionState::Reconnecting));
	h.loop.runUntil([&] { return h.loop.pendingTimers() > 0; }, 2000ms);
	int createdBefore = h.network.created(id);

	h.manager->stopDestination(id);
	CHECK(h.state(id) == SessionState::Idle);
	CHECK_EQ(h.loop.pendingTimers(), (size_t)0);
	h.loop.advance(120000ms);
	h.settle();
	CHECK_EQ(h.network.created(id), createdBefore); // no attempt after the stop
	CHECK(h.state(id) == SessionState::Idle);
}

MF_TEST(stop_while_connecting_cannot_resurrect)
{
	Harness h;
	std::string id = h.add("A");
	h.network.script(id, {Behaviour{Behaviour::LateStartOnStop}});
	std::string error;
	REQUIRE(h.manager->startDestination(id, error));
	h.settle();
	CHECK(h.state(id) == SessionState::Connecting);
	h.manager->stopDestination(id);
	REQUIRE(h.waitFor(id, SessionState::Idle));
	h.settle();
	CHECK(h.state(id) == SessionState::Idle); // the late "start" was ignored
	CHECK_EQ(h.network.live(id), (size_t)0);
}

MF_TEST(rapid_start_stop_start_never_duplicates)
{
	Harness h;
	std::string id = h.add("A");
	for (int round = 0; round < 25; round++) {
		h.network.script(id, {Behaviour{round % 3 == 0 ? Behaviour::Hang : Behaviour::Connect}});
		std::string error;
		h.manager->startDestination(id, error);
		h.manager->stopDestination(id);
		/* Start while stopping is refused rather than queued twice. */
		CHECK(!h.manager->startDestination(id, error) || h.state(id) == SessionState::Connecting);
		h.loop.runPending();
	}
	h.manager->stopDestination(id);
	REQUIRE(h.waitFor(id, SessionState::Idle, 5000ms));
	h.settle();
	CHECK_EQ(h.network.maxConcurrent(id), (size_t)1);
	CHECK_EQ(h.network.live(id), (size_t)0);
	CHECK(!h.manager->sharedEncoders().inUse());
}

MF_TEST(start_enabled_staggers_skips_disabled_and_isolates_failures)
{
	Harness h;
	std::string a = h.add("A");
	std::string b = h.add("B");
	std::string c = h.add("C", false);
	std::string d = h.add("D");
	h.missingKeys.insert(b);
	h.network.script(d, {Behaviour{Behaviour::ConnectFail, OBS_OUTPUT_INVALID_STREAM}});

	auto report = h.manager->startEnabled();
	CHECK_EQ(report.queued.size(), (size_t)3);
	for (int i = 0; i < 10; i++) {
		h.loop.advance(200ms);
		h.settle();
	}
	CHECK(h.waitFor(a, SessionState::Sending));
	CHECK(h.state(b) == SessionState::Error); // missing key, others unaffected
	CHECK(h.manager->snapshot(b).lastError.message.find("No stream key") != std::string::npos);
	CHECK(h.state(c) == SessionState::Disabled);
	CHECK(h.waitFor(d, SessionState::Error));
	CHECK(h.manager->snapshot(d).lastError.kind == ErrorKind::Rejected); // first failure is not retried
	CHECK(h.manager->summary().degraded);

	/* Start Enabled again does not restart the destination that is already sending. */
	int createdA = h.network.created(a);
	h.manager->startEnabled();
	for (int i = 0; i < 5; i++) {
		h.loop.advance(200ms);
		h.settle();
	}
	CHECK_EQ(h.network.created(a), createdA);
	std::string error;
	CHECK(!h.manager->startDestination(c, error)); // disabled is not startable
}

MF_TEST(stop_all_cancels_queued_starts_and_reconnects)
{
	Harness h;
	std::vector<std::string> ids;
	for (int i = 0; i < 5; i++)
		ids.push_back(h.add("D" + std::to_string(i)));
	h.manager->startEnabled();
	h.settle();
	CHECK(h.manager->summary().queued > 0);
	REQUIRE(h.waitFor(ids[0], SessionState::Sending));
	auto targets = h.manager->stopAllTargets();
	CHECK_EQ(targets.size(), (size_t)5);
	h.manager->stopAll();
	CHECK_EQ(h.manager->summary().queued, 0);
	h.loop.advance(5000ms);
	h.settle();
	for (const std::string &id : ids) {
		h.loop.runUntil([&] { return h.state(id) == SessionState::Idle; }, 2000ms);
		CHECK(h.state(id) == SessionState::Idle);
	}
	CHECK_EQ(h.network.created(ids[4]), 0); // was still queued: never started
}

MF_TEST(simultaneous_output_limit_is_eight)
{
	Harness h;
	std::vector<std::string> ids;
	for (int i = 0; i < 9; i++)
		ids.push_back(h.add("D" + std::to_string(i)));
	std::string error;
	for (int i = 0; i < 8; i++)
		REQUIRE(h.manager->startDestination(ids[i], error));
	CHECK(!h.manager->startDestination(ids[8], error));
	CHECK(error.find("At most 8") != std::string::npos);
	for (int i = 0; i < 8; i++)
		CHECK(h.waitFor(ids[i], SessionState::Sending));
	CHECK_EQ(h.encoderCreations, 1); // one shared encoder pair for all eight
	CHECK_EQ(h.manager->sharedEncoders().holderCount(), (size_t)8);

	/* Stopping the first-started destination leaves the others running on the same encoders. */
	h.manager->stopDestination(ids[0]);
	REQUIRE(h.waitFor(ids[0], SessionState::Idle));
	for (int i = 1; i < 8; i++)
		CHECK(h.state(ids[i]) == SessionState::Sending);
	CHECK_EQ(h.manager->sharedEncoders().holderCount(), (size_t)7);
	CHECK_EQ(h.encoderCreations, 1);
	REQUIRE(h.manager->startDestination(ids[8], error));
	CHECK(h.waitFor(ids[8], SessionState::Sending));
}

MF_TEST(locks_while_active)
{
	Harness h;
	std::string id = h.add("A");
	std::string error;
	REQUIRE(h.manager->startDestination(id, error));
	REQUIRE(h.waitFor(id, SessionState::Sending));

	DestinationConfig cfg = *h.manager->config().find(id);
	cfg.serverUrl = "rtmp://127.0.0.1:1936/live";
	CHECK(!h.manager->updateDestination(cfg, error));
	cfg = *h.manager->config().find(id);
	cfg.name = "Renamed live";
	CHECK(h.manager->updateDestination(cfg, error)); // display name is editable
	CHECK(!h.manager->removeDestination(id, error));
	CHECK(!h.manager->setEnabled(id, false, error));
	EncoderProfile profile = h.manager->config().encoder;
	profile.videoBitrateKbps = 4000;
	CHECK(!h.manager->setEncoderProfile(profile, error));

	/* Other destinations may be added and edited while one is live. */
	std::string other = h.add("B");
	DestinationConfig otherCfg = *h.manager->config().find(other);
	otherCfg.serverUrl = "rtmp://127.0.0.1:1937/live";
	CHECK(h.manager->updateDestination(otherCfg, error));

	h.manager->stopDestination(id);
	REQUIRE(h.waitFor(id, SessionState::Idle));
	CHECK(h.manager->setEncoderProfile(profile, error));
	CHECK(h.manager->removeDestination(id, error));
}

MF_TEST(encoder_failure_is_a_group_error_without_retry)
{
	Harness h;
	std::string a = h.add("A");
	h.network.script(a, {Behaviour{Behaviour::StartFail, OBS_OUTPUT_ENCODE_ERROR, "NVENC init failed", true}});
	std::string error;
	REQUIRE(h.manager->startDestination(a, error));
	REQUIRE(h.waitFor(a, SessionState::Error));
	SessionSnapshot snap = h.manager->snapshot(a);
	CHECK(snap.lastError.kind == ErrorKind::Encoder);
	CHECK(snap.lastError.message.find("Every plugin destination") != std::string::npos);
	CHECK(!h.manager->summary().groupFailure.empty());
	CHECK_EQ(h.loop.pendingTimers(), (size_t)0);

	/* Runtime encode error while sending: every attached destination stops, none retries. */
	std::string b = h.add("B");
	std::string c = h.add("C");
	REQUIRE(h.manager->startDestination(b, error));
	REQUIRE(h.manager->startDestination(c, error));
	REQUIRE(h.waitFor(b, SessionState::Sending));
	REQUIRE(h.waitFor(c, SessionState::Sending));
	CHECK(h.manager->summary().groupFailure.empty()); // cleared by the fresh start
	h.network.drop(b, OBS_OUTPUT_ENCODE_ERROR);
	h.network.drop(c, OBS_OUTPUT_ENCODE_ERROR);
	REQUIRE(h.waitFor(b, SessionState::Error));
	REQUIRE(h.waitFor(c, SessionState::Error));
	CHECK(h.manager->snapshot(b).lastError.kind == ErrorKind::Encoder);
	CHECK_EQ(h.loop.pendingTimers(), (size_t)0);
}

MF_TEST(encoder_creation_failure_reports_group_error)
{
	Harness h;
	h.encoderFactoryFails = true;
	std::string a = h.add("A");
	std::string error;
	CHECK(!h.manager->startDestination(a, error));
	CHECK(h.state(a) == SessionState::Error);
	CHECK(h.manager->snapshot(a).lastError.kind == ErrorKind::Encoder);
	CHECK(!h.manager->summary().groupFailure.empty());
}

MF_TEST(auto_reconnect_disabled_goes_to_error)
{
	Harness h;
	std::string id = h.add("A");
	DestinationConfig cfg = *h.manager->config().find(id);
	cfg.autoReconnect = false;
	std::string error;
	REQUIRE(h.manager->updateDestination(cfg, error));
	REQUIRE(h.manager->startDestination(id, error));
	REQUIRE(h.waitFor(id, SessionState::Sending));
	h.network.drop(id);
	REQUIRE(h.waitFor(id, SessionState::Error));
	CHECK_EQ(h.loop.pendingTimers(), (size_t)0);
}

MF_TEST(sync_stop_and_shutdown_are_bounded)
{
	Harness h;
	std::string a = h.add("A");
	std::string b = h.add("B");
	h.network.script(a, {Behaviour{Behaviour::Connect, 0, "", false, 300}});
	std::string error;
	REQUIRE(h.manager->startDestination(a, error));
	REQUIRE(h.manager->startDestination(b, error));
	REQUIRE(h.waitFor(a, SessionState::Sending));
	REQUIRE(h.waitFor(b, SessionState::Sending));

	auto begin = std::chrono::steady_clock::now();
	CHECK(h.manager->stopAllAndWait(StopReason::ProfileChange, 3000ms));
	auto elapsed = std::chrono::steady_clock::now() - begin;
	CHECK(elapsed < 3000ms);
	CHECK(!h.manager->anyBusy());
	CHECK(h.state(a) == SessionState::Idle);
	CHECK(!h.manager->sharedEncoders().inUse());

	/* A late "released" event for the finalized attempt is harmless. */
	h.settle();
	CHECK(h.state(a) == SessionState::Idle);
	CHECK(!h.manager->sharedEncoders().inUse());
}

MF_TEST(profile_load_requires_idle)
{
	Harness h;
	std::string id = h.add("A");
	std::string error;
	REQUIRE(h.manager->startDestination(id, error));
	ProfileConfig other;
	other.profileId = "other";
	CHECK(!h.manager->loadProfile(other, error));
	h.manager->stopDestination(id);
	REQUIRE(h.waitFor(id, SessionState::Idle));
	CHECK(h.manager->loadProfile(other, error));
	CHECK(h.manager->config().destinations.empty());
}
