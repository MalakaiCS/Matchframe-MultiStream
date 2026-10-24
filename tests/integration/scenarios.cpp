// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Integration scenarios against the real OBS 32.2.2 RTMP output and local receivers.
// Each scenario lists the acceptance tests (numbers from the product brief) it covers.

#include "integration/scenarios.hpp"

#include "common/manual-dispatcher.hpp"
#include "core/credential-store.hpp"
#include "core/output-manager.hpp"
#include "core/redaction.hpp"

#include <obs-defs.h>
#include <obs.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>

using namespace std::chrono_literals;

namespace mftest {

namespace {

using Clock = std::chrono::steady_clock;

int g_nextPort = 19350;
int next_port()
{
	return g_nextPort++;
}

double seconds_since(Clock::time_point t)
{
	return std::chrono::duration<double>(Clock::now() - t).count();
}

std::string fmt(const char *format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

/* Counts live plugin obs_output objects, optionally for one destination. */
int plugin_outputs(const std::string &destinationId = "")
{
	struct Ctx {
		std::string prefix;
		int count = 0;
	} ctx{"matchframe-" + destinationId.substr(0, 8), 0};
	obs_enum_outputs(
		[](void *param, obs_output_t *output) {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_output_get_name(output);
			if (name && strncmp(name, c->prefix.c_str(), c->prefix.size()) == 0)
				c->count++;
			return true;
		},
		&ctx);
	return ctx.count;
}

struct EncoderCount {
	int video = 0;
	int audio = 0;
};

EncoderCount plugin_encoders()
{
	EncoderCount count;
	obs_enum_encoders(
		[](void *param, obs_encoder_t *encoder) {
			auto *c = static_cast<EncoderCount *>(param);
			const char *name = obs_encoder_get_name(encoder);
			if (name && strcmp(name, mf::SharedEncoderGroup::kVideoEncoderName) == 0)
				c->video++;
			if (name && strcmp(name, mf::SharedEncoderGroup::kAudioEncoderName) == 0)
				c->audio++;
			return true;
		},
		&count);
	return count;
}

/* One OutputManager wired exactly as the plugin wires it, minus the Qt dock. */
class Fixture {
public:
	Fixture(ObsHarness &obs, ScenarioResult &result) : obs_(obs), result_(result), vault_(&store_)
	{
		mf::OutputManager::Options options; // real OBS transport, encoders and media info
		manager_ = std::make_unique<mf::OutputManager>(loop_, std::move(options));
		manager_->setSecretsProvider([this](const mf::DestinationConfig &config,
						    mf::DestinationSecrets &secrets, std::string &error) {
			mf::CredResult r = vault_.loadSecrets(profile_, config, secrets);
			error = r.message;
			return r.ok();
		});
		mf::ProfileConfig config;
		config.profileId = profile_;
		config.encoder.videoEncoderId = "obs_x264";
		std::string error;
		manager_->loadProfile(config, error);
	}

	~Fixture()
	{
		auto begin = Clock::now();
		manager_->shutdown(5000ms, 45000ms);
		note(fmt("teardown (manager shutdown) took %.2f s", seconds_since(begin)));
		note(fmt("longest main-thread task: %.1f ms", loop_.maxTaskMs));
	}

	mf::OutputManager &manager() { return *manager_; }
	ManualDispatcher &loop() { return loop_; }

	std::string add(const std::string &name, const std::string &url, const std::string &key, int maxAttempts = 20,
			bool autoReconnect = true)
	{
		mf::DestinationConfig d;
		d.id = mf::new_uuid();
		d.name = name;
		d.platform = mf::Platform::Custom;
		d.serverUrl = url;
		d.maxReconnectAttempts = maxAttempts;
		d.autoReconnect = autoReconnect;
		auto stored = vault_.store(profile_, d.id, mf::SecretField::StreamKey, key, false);
		d.credentials.streamKey = stored.target;
		std::string error;
		if (!manager_->addDestination(d, error))
			check(false, "add destination " + name + ": " + error);
		return d.id;
	}

	void setKey(const std::string &id, const std::string &key)
	{
		vault_.store(profile_, id, mf::SecretField::StreamKey, key, false);
	}

	mf::SessionSnapshot snap(const std::string &id) { return manager_->snapshot(id); }
	mf::SessionState state(const std::string &id) { return snap(id).state; }

	bool waitState(const std::string &id, mf::SessionState wanted, std::chrono::milliseconds timeout)
	{
		return loop_.runUntil([&] { return state(id) == wanted; }, timeout);
	}

	void pump(std::chrono::milliseconds duration)
	{
		loop_.runUntil([] { return false; }, duration);
	}

	bool start(const std::string &id)
	{
		std::string error;
		auto begin = Clock::now();
		bool ok = manager_->startDestination(id, error);
		mainCallMs_ =
			std::max(mainCallMs_, std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
		if (!ok)
			note("start refused: " + error);
		return ok;
	}

	void check(bool condition, const std::string &what)
	{
		std::printf("    [%s] %s\n", condition ? " ok " : "FAIL", what.c_str());
		std::fflush(stdout);
		if (!condition)
			result_.passed = false;
	}

	void note(const std::string &text)
	{
		std::printf("    [note] %s\n", text.c_str());
		std::fflush(stdout);
		result_.notes.push_back(text);
	}

	double mainCallMs() const { return mainCallMs_; }
	const std::string &work() const { return obs_.paths().workDir; }
	ObsHarness &obs() { return obs_; }

private:
	ObsHarness &obs_;
	ScenarioResult &result_;
	ManualDispatcher loop_{true};
	mf::MemoryCredentialStore store_; // stands in for Windows Credential Manager on Linux
	mf::SecretVault vault_;
	std::string profile_ = "integration";
	std::unique_ptr<mf::OutputManager> manager_;
	double mainCallMs_ = 0.0;
};

void verify_recording(Fixture &f, const std::string &label, const std::string &path, double minSeconds,
		      int width = 1280, int height = 720)
{
	MediaReport r = probe_recording(path);
	f.note(label + ": " + r.summary());
	f.check(r.probed, label + " recording exists and is readable");
	f.check(r.videoCodec == "h264" && r.width == width && r.height == height,
		label + " has H.264 " + std::to_string(width) + "x" + std::to_string(height));
	f.check(r.audioCodec == "aac" && r.sampleRate == 48000 && r.channels == 2, label + " has AAC 48 kHz stereo");
	f.check(r.videoSeconds >= minSeconds,
		label + fmt(" has >= %.1f s of video (%.1f s)", minSeconds, r.videoSeconds));
	f.check(r.decodeClean, label + " decodes without errors" +
				       (r.decodeClean ? std::string() : ": " + r.decodeErrors.substr(0, 200)));
}

/* ---------------- scenarios ---------------- */

void two_outputs_shared(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	RtmpReceiver r1(f.work(), next_port(), "shared-1"), r2(f.work(), next_port(), "shared-2");
	f.check(r1.start() && r2.start(), "two local receivers listening");
	std::string a = f.add("Shared A", r1.url(), r1.key());
	std::string b = f.add("Shared B", r2.url(), r2.key());

	f.check(!f.manager().sharedEncoders().videoSessionOpen(), "no encode session before any start");
	f.manager().startEnabled();
	f.check(f.waitState(a, mf::SessionState::Sending, 15s), "A reaches Sending");
	f.check(f.waitState(b, mf::SessionState::Sending, 15s), "B reaches Sending");
	f.pump(8s);

	EncoderCount count = plugin_encoders();
	f.check(count.video == 1 && count.audio == 1,
		fmt("exactly one plugin video and one plugin audio encoder exist (video=%d audio=%d)", count.video,
		    count.audio));
	f.check(f.snap(a).attachedToSharedEncoders && f.snap(b).attachedToSharedEncoders,
		"both outputs are attached to the group's encoder objects");
	f.check(f.manager().sharedEncoders().videoSessionOpen(), "shared video encode session open while sending");
	f.note(fmt("measured bitrate A=%.0f kbps B=%.0f kbps", f.snap(a).bitrateKbps, f.snap(b).bitrateKbps));
	f.check(f.snap(a).bitrateKbps > 2000 && f.snap(b).bitrateKbps > 2000, "both report measured throughput");
	f.note(f.manager().diagnostics());

	f.manager().stopAll();
	f.check(f.waitState(a, mf::SessionState::Idle, 15s) && f.waitState(b, mf::SessionState::Idle, 15s),
		"both return to Idle after Stop All");
	f.check(!f.manager().sharedEncoders().videoSessionOpen() && !f.manager().sharedEncoders().audioSessionOpen(),
		"stopping the last output released the encoder sessions (encoder type data freed)");
	f.check(plugin_outputs() == 0, "no plugin obs_output objects remain");
	r1.waitExit(5000);
	r2.waitExit(5000);
	verify_recording(f, "receiver 1", r1.recording(1), 6.0);
	verify_recording(f, "receiver 2", r2.recording(1), 6.0);
	double e1000 = 0, e440 = 0, rms = 0;
	f.check(tone_energies(r1.recording(1), 1.0, 6.0, e1000, e440, rms) && e1000 > 10 * e440,
		fmt("track 1 carries clip A's 1 kHz beeps, not clip B's 440 Hz (e1000=%.3g e440=%.3g)", e1000, e440));
}

void eight_outputs(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	std::vector<std::unique_ptr<RtmpReceiver>> receivers;
	std::vector<std::string> ids;
	bool listening = true;
	for (int i = 0; i < 8; i++) {
		receivers.push_back(
			std::make_unique<RtmpReceiver>(f.work(), next_port(), "eight-" + std::to_string(i)));
		listening = listening && receivers.back()->start();
		ids.push_back(f.add("Eight " + std::to_string(i), receivers.back()->url(), receivers.back()->key()));
	}
	f.check(listening, "eight local receivers listening");
	auto startedAt = Clock::now();
	f.manager().startEnabled();
	bool all = true;
	for (const std::string &id : ids)
		all = f.waitState(id, mf::SessionState::Sending, 20s) && all;
	f.check(all, fmt("all eight reach Sending (%.1f s after Start Enabled)", seconds_since(startedAt)));
	f.pump(6s);
	EncoderCount count = plugin_encoders();
	f.check(count.video == 1 && count.audio == 1, "eight destinations share one video and one audio encoder");
	bool attached = true;
	for (const std::string &id : ids)
		attached = attached && f.snap(id).attachedToSharedEncoders;
	f.check(attached, "every output reports the shared encoder objects");
	mf::ManagerSummary sum = f.manager().summary();
	f.note(fmt("aggregate measured %.0f kbps; estimate %.2f Mbps payload, %.1f Mbps recommended", sum.measuredKbps,
		   sum.estimate.payloadMbps, sum.estimate.recommendedMbps));
	f.check(sum.sending == 8 && !sum.degraded, "summary: 8 sending, not degraded");

	/* Stop the first-started destination; the other seven keep their encoders and feed. */
	f.manager().stopDestination(ids[0]);
	f.check(f.waitState(ids[0], mf::SessionState::Idle, 15s), "first-started destination stopped");
	f.check(f.manager().sharedEncoders().videoSessionOpen(), "shared encoder session still open for the others");
	f.pump(7s);
	bool othersSending = true;
	for (size_t i = 1; i < ids.size(); i++)
		othersSending = othersSending && f.state(ids[i]) == mf::SessionState::Sending &&
				f.snap(ids[i]).bitrateKbps > 1000;
	f.check(othersSending, "the other seven are still sending with measured throughput");
	f.manager().stopAll();
	for (const std::string &id : ids)
		f.waitState(id, mf::SessionState::Idle, 15s);
	for (auto &r : receivers)
		r->waitExit(5000);
	verify_recording(f, "receiver 0 (stopped early)", receivers[0]->recording(1), 4.0);
	for (size_t i = 1; i < receivers.size(); i++)
		verify_recording(f, "receiver " + std::to_string(i), receivers[i]->recording(1), 11.0);
	MediaReport first = probe_recording(receivers[0]->recording(1));
	MediaReport other = probe_recording(receivers[1]->recording(1));
	f.check(other.videoSeconds > first.videoSeconds + 5.0,
		"later destinations kept streaming after the first stopped");
}

void receiver_restart(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	std::vector<std::unique_ptr<RtmpReceiver>> receivers;
	std::vector<std::string> ids;
	for (int i = 0; i < 3; i++) {
		receivers.push_back(
			std::make_unique<RtmpReceiver>(f.work(), next_port(), "restart-" + std::to_string(i)));
		receivers.back()->start();
		ids.push_back(f.add("Restart " + std::to_string(i), receivers.back()->url(), receivers.back()->key()));
	}
	f.manager().startEnabled();
	for (const std::string &id : ids)
		f.waitState(id, mf::SessionState::Sending, 20s);
	f.pump(4s);

	auto killedAt = Clock::now();
	receivers[1]->stop(); // receiver goes away mid-stream
	bool reconnecting = f.waitState(ids[1], mf::SessionState::Reconnecting, 20s);
	f.check(reconnecting, fmt("destination 1 detects the loss and reconnects (%.1f s)", seconds_since(killedAt)));
	f.check(f.state(ids[0]) == mf::SessionState::Sending && f.state(ids[2]) == mf::SessionState::Sending,
		"destinations 0 and 2 keep sending");
	f.pump(3s);
	receivers[1]->start();
	bool back = f.waitState(ids[1], mf::SessionState::Sending, 30s);
	f.check(back, fmt("destination 1 back to Sending after the receiver restarted (%.1f s after loss)",
			  seconds_since(killedAt)));
	f.check(f.snap(ids[1]).reconnectCount == 1 && f.snap(ids[1]).lastErrorRecovered,
		"reconnect counted once and the last error marked recovered");
	f.pump(5s);
	f.check(f.state(ids[0]) == mf::SessionState::Sending && f.state(ids[2]) == mf::SessionState::Sending,
		"others unaffected throughout");
	f.manager().stopAll();
	for (const std::string &id : ids)
		f.waitState(id, mf::SessionState::Idle, 15s);
	for (auto &r : receivers)
		r->waitExit(5000);
	verify_recording(f, "receiver 0", receivers[0]->recording(1), 10.0);
	verify_recording(f, "receiver 2", receivers[2]->recording(1), 10.0);
	verify_recording(f, "receiver 1 after restart", receivers[1]->recording(2), 3.0);
}

void failure_modes(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	std::string work = f.work();
	std::string scripts = obs.paths().scriptsDir;

	/* Healthy reference output running throughout. */
	RtmpReceiver healthy(work, next_port(), "healthy");
	healthy.start();
	std::string ok = f.add("Healthy", healthy.url(), healthy.key());
	f.start(ok);
	f.check(f.waitState(ok, mf::SessionState::Sending, 15s), "healthy reference destination sending");

	/* Invalid stream key: nginx-rtmp refuses the publish (on_publish -> 403). */
	int nginxRtmp = next_port(), nginxHttp = next_port();
	AuthRtmpServer nginx;
	f.check(nginx.start(work, nginxRtmp, nginxHttp), "nginx-rtmp auth server running");
	std::string nginxUrl = "rtmp://127.0.0.1:" + std::to_string(nginxRtmp) + "/live";
	std::string bad = f.add("Invalid key", nginxUrl, "wrong-key-123");
	f.start(bad);
	f.check(f.waitState(bad, mf::SessionState::Error, 20s), "invalid key ends in Error");
	f.check(f.snap(bad).lastError.kind == mf::ErrorKind::Rejected,
		"invalid key classified as a server rejection: " + f.snap(bad).lastError.message);
	f.check(f.snap(bad).lastError.message.find("wrong-key-123") == std::string::npos,
		"error text does not echo the key");
	std::string good = f.add("Valid key", nginxUrl, "good-key-1");
	f.start(good);
	f.check(f.waitState(good, mf::SessionState::Sending, 15s), "same server accepts the valid key");
	f.manager().stopDestination(good);

	/* Refused connection. */
	std::string refused = f.add("Refused", "rtmp://127.0.0.1:" + std::to_string(next_port()) + "/live", "key-x");
	f.start(refused);
	f.check(f.waitState(refused, mf::SessionState::Error, 20s), "refused connection ends in Error");
	f.check(f.snap(refused).lastError.kind == mf::ErrorKind::Network,
		"refused connection classified as network: " + f.snap(refused).lastError.message);

	/* TLS: self-signed certificate must fail verification (no insecure bypass exists). */
	std::string out;
	run_capture("openssl req -x509 -newkey rsa:2048 -nodes -keyout " + work + "/selfsigned.key -out " + work +
			    "/selfsigned.pem -days 2 -subj /CN=localhost -addext subjectAltName=DNS:localhost",
		    out);
	int tlsPort = next_port();
	ScriptServer tls;
	tls.start({scripts + "/tls_proxy.py", "--listen", std::to_string(tlsPort), "--target",
		   std::to_string(healthy.port()), "--cert", work + "/selfsigned.pem", "--key",
		   work + "/selfsigned.key"},
		  work + "/tls-selfsigned.log");
	wait_for_port(tlsPort, 5000);
	std::string tlsBad =
		f.add("Self-signed RTMPS", "rtmps://localhost:" + std::to_string(tlsPort) + "/live", "key-y");
	f.start(tlsBad);
	f.check(f.waitState(tlsBad, mf::SessionState::Error, 30s), "self-signed RTMPS ends in Error");
	f.check(f.snap(tlsBad).lastError.kind == mf::ErrorKind::Tls,
		"classified as an RTMPS/TLS failure: " + f.snap(tlsBad).lastError.message);
	tls.stop();

	/* Stalled receiver: accepts TCP, never answers the RTMP handshake. */
	int stallPort = next_port();
	ScriptServer stall;
	stall.start({scripts + "/stall_server.py", "--listen", std::to_string(stallPort)}, work + "/stall.log");
	wait_for_port(stallPort, 5000);
	std::string stalled = f.add("Stalled", "rtmp://127.0.0.1:" + std::to_string(stallPort) + "/live", "key-z");
	double before = f.loop().maxTaskMs;
	f.start(stalled);
	f.pump(6s);
	f.check(f.state(stalled) == mf::SessionState::Connecting, "stalled destination shows Connecting, not Sending");
	f.check(f.state(ok) == mf::SessionState::Sending && f.snap(ok).bitrateKbps > 1000,
		fmt("healthy output unaffected while another stalls (%.0f kbps)", f.snap(ok).bitrateKbps));
	auto stopAt = Clock::now();
	f.manager().stopDestination(stalled);
	f.check(f.state(stalled) == mf::SessionState::Stopping,
		"stop of a stalled connect returns immediately (Stopping)");
	bool idle = f.waitState(stalled, mf::SessionState::Idle, 60s);
	f.check(idle, fmt("stalled destination reaches Idle %.1f s after Stop (bounded by OBS's RTMP receive timeout)",
			  seconds_since(stopAt)));
	f.check(f.loop().maxTaskMs < 100.0,
		fmt("main thread never blocked: longest task %.1f ms (before stall test %.1f ms)", f.loop().maxTaskMs,
		    before));
	f.check(f.mainCallMs() < 200.0, fmt("longest start call on the main thread %.1f ms", f.mainCallMs()));
	f.check(f.state(ok) == mf::SessionState::Sending, "healthy output still sending after all failure cases");
	stall.stop();
	nginx.stop();
	f.manager().stopAll();
	f.waitState(ok, mf::SessionState::Idle, 15s);
	healthy.waitExit(5000);
	verify_recording(f, "healthy receiver", healthy.recording(1), 20.0);
}

void reconnect_policy(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	RtmpReceiver r(f.work(), next_port(), "policy");
	r.start();
	std::string id = f.add("Policy", r.url(), r.key(), 4);
	f.start(id);
	f.check(f.waitState(id, mf::SessionState::Sending, 15s), "sending");
	f.pump(2s);

	auto dropAt = Clock::now();
	r.stop(); // receiver gone for good: every reconnect is refused
	std::vector<double> attemptTimes;
	uint64_t lastAttempt = f.snap(id).attemptId;
	f.loop().runUntil(
		[&] {
			mf::SessionSnapshot s = f.snap(id);
			if (s.attemptId != 0 && s.attemptId != lastAttempt) {
				attemptTimes.push_back(seconds_since(dropAt));
				lastAttempt = s.attemptId;
			}
			return s.state == mf::SessionState::Error;
		},
		90s);
	std::string times;
	for (double t : attemptTimes)
		times += fmt("%.2f ", t);
	f.note("reconnect attempt start times after the loss (s): " + times);
	f.check(attemptTimes.size() == 4, fmt("exactly 4 attempts (max attempts = 4), got %zu", attemptTimes.size()));
	std::vector<double> expectedGaps = {2, 4, 8, 16};
	bool gapsOk = attemptTimes.size() == 4;
	for (size_t i = 1; gapsOk && i < attemptTimes.size(); i++) {
		double gap = attemptTimes[i] - attemptTimes[i - 1];
		gapsOk = std::fabs(gap - expectedGaps[i]) < 0.75;
	}
	gapsOk = gapsOk && std::fabs(attemptTimes[0] - expectedGaps[0]) < 1.5;
	f.check(gapsOk, "attempt spacing follows 2, 4, 8, 16 s");
	f.check(f.state(id) == mf::SessionState::Error && f.snap(id).lastError.kind == mf::ErrorKind::Exhausted,
		"exhausted retries end in Error: " + f.snap(id).lastError.message);

	/* Manual restart after exhaustion (once the failed attempt's output has been released). */
	f.loop().runUntil([&] { return !f.manager().destinationLocked(id); }, 10s);
	r.start();
	f.start(id);
	f.check(f.waitState(id, mf::SessionState::Sending, 15s), "manual restart after exhaustion works");

	/* Manual stop cancels a pending retry immediately; nothing reconnects afterwards. */
	r.stop();
	f.check(f.waitState(id, mf::SessionState::Reconnecting, 20s), "reconnecting after second loss");
	r.start(); // a receiver is available again, so any stray retry would connect
	auto stopAt = Clock::now();
	f.manager().stopDestination(id);
	f.check(f.waitState(id, mf::SessionState::Idle, 5s),
		fmt("manual stop during the retry wait reaches Idle in %.2f s", seconds_since(stopAt)));
	f.pump(10s);
	f.check(f.state(id) == mf::SessionState::Idle && plugin_outputs(id) == 0, "no reconnect after manual stop");
	r.stop();
}

void rapid_cycles(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	int rtmp = next_port(), http = next_port();
	AuthRtmpServer nginx;
	f.check(nginx.start(f.work(), rtmp, http), "nginx-rtmp running (accepts repeated publishes)");
	std::string url = "rtmp://127.0.0.1:" + std::to_string(rtmp) + "/live";
	std::string a = f.add("Rapid A", url, "good-a");
	std::string b = f.add("Rapid B", url, "good-b");

	std::atomic<bool> sampling{true};
	std::atomic<int> maxA{0}, maxB{0};
	std::thread sampler([&] {
		while (sampling) {
			int ca = plugin_outputs(a), cb = plugin_outputs(b);
			if (ca > maxA)
				maxA = ca;
			if (cb > maxB)
				maxB = cb;
			std::this_thread::sleep_for(2ms);
		}
	});

	std::mt19937 rng(20261024);
	std::uniform_int_distribution<int> action(0, 5);
	std::uniform_int_distribution<int> pause(0, 250);
	for (int i = 0; i < 60; i++) {
		switch (action(rng)) {
		case 0:
			f.start(a);
			break;
		case 1:
			f.manager().stopDestination(a);
			break;
		case 2:
			f.start(b);
			break;
		case 3:
			f.manager().stopDestination(b);
			break;
		case 4:
			f.manager().startEnabled();
			break;
		case 5:
			f.manager().stopAll();
			break;
		}
		f.pump(std::chrono::milliseconds(pause(rng)));
	}

	/* Stop All while both are in a reconnect wait. */
	f.manager().stopAll();
	f.waitState(a, mf::SessionState::Idle, 30s);
	f.waitState(b, mf::SessionState::Idle, 30s);
	f.start(a);
	f.start(b);
	f.waitState(a, mf::SessionState::Sending, 15s);
	f.waitState(b, mf::SessionState::Sending, 15s);
	nginx.stop();
	bool bothReconnecting = f.waitState(a, mf::SessionState::Reconnecting, 20s) &&
				f.waitState(b, mf::SessionState::Reconnecting, 20s);
	f.check(bothReconnecting, "both reconnecting after the server went away");
	f.manager().stopAll();
	f.check(f.state(a) == mf::SessionState::Idle || f.state(a) == mf::SessionState::Stopping,
		"Stop All during retry acts immediately");
	f.waitState(a, mf::SessionState::Idle, 30s);
	f.waitState(b, mf::SessionState::Idle, 30s);
	nginx.start(f.work(), rtmp, http);
	f.pump(12s);
	f.check(f.state(a) == mf::SessionState::Idle && f.state(b) == mf::SessionState::Idle,
		"nothing resurrected after Stop All, even with the server back");
	sampling = false;
	sampler.join();
	f.check(maxA <= 1 && maxB <= 1,
		fmt("never more than one live output per destination (max A=%d, B=%d)", maxA.load(), maxB.load()));
	f.check(plugin_outputs() == 0 && !f.manager().sharedEncoders().inUse(), "no outputs or encoder holds remain");
	nginx.stop();
}

void audio_track_and_mute(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	RtmpReceiver r(f.work(), next_port(), "audio");
	r.start();
	std::string id = f.add("Audio", r.url(), r.key());

	mf::EncoderProfile profile = f.manager().config().encoder;
	profile.audioTrack = 2;
	std::string error;
	f.check(f.manager().setEncoderProfile(profile, error), "select OBS audio Track 2 (mixer index 1)");
	f.start(id);
	f.waitState(id, mf::SessionState::Sending, 15s);
	f.pump(5s);
	obs_source_set_muted(obs.sourceB(), true); // the only source routed to track 2
	f.pump(5s);
	obs_source_set_muted(obs.sourceB(), false);
	f.manager().stopDestination(id);
	f.waitState(id, mf::SessionState::Idle, 15s);
	r.waitExit(5000);

	double e1000 = 0, e440 = 0, rms = 0;
	bool okTone = tone_energies(r.recording(1), 1.0, 4.0, e1000, e440, rms);
	f.check(okTone && e440 > 10 * e1000,
		fmt("Track 2 carries clip B's 440 Hz tone only (e440=%.3g e1000=%.3g rms=%.3f)", e440, e1000, rms));
	double m1000 = 0, m440 = 0, mrms = 0;
	bool okMute = tone_energies(r.recording(1), 6.5, 9.5, m1000, m440, mrms);
	f.check(okMute && mrms < 0.005, fmt("muting the source silences the plugin output (rms %.4f)", mrms));

	profile.audioTrack = 1;
	f.manager().setEncoderProfile(profile, error);
}

void independent_native_outputs(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	RtmpReceiver p1(f.work(), next_port(), "indep-plugin-1"), p2(f.work(), next_port(), "indep-plugin-2");
	RtmpReceiver native(f.work(), next_port(), "indep-native");
	p1.start();
	p2.start();
	native.start();
	std::string a = f.add("Plugin 1", p1.url(), p1.key());
	std::string b = f.add("Plugin 2", p2.url(), p2.key());

	/* A stand-in for OBS's own streaming and recording outputs, each with its own encoders. */
	OBSDataAutoRelease venc = obs_data_create();
	obs_data_set_int(venc, "bitrate", 2500);
	OBSEncoderAutoRelease nativeVideo = obs_video_encoder_create("obs_x264", "native stream video", venc, nullptr);
	OBSEncoderAutoRelease nativeAudio =
		obs_audio_encoder_create("ffmpeg_aac", "native stream audio", nullptr, 0, nullptr);
	obs_encoder_set_video(nativeVideo, obs_get_video());
	obs_encoder_set_audio(nativeAudio, obs_get_audio());
	OBSDataAutoRelease svc = obs_data_create();
	obs_data_set_string(svc, "server", native.url().c_str());
	obs_data_set_string(svc, "key", native.key().c_str());
	OBSServiceAutoRelease nativeService = obs_service_create_private("rtmp_custom", "native service", svc);
	OBSOutputAutoRelease nativeStream = obs_output_create("rtmp_output", "native stream", nullptr, nullptr);
	obs_output_set_video_encoder(nativeStream, nativeVideo);
	obs_output_set_audio_encoder(nativeStream, nativeAudio, 0);
	obs_output_set_service(nativeStream, nativeService);

	/* flv_output (obs-outputs) records in-process; ffmpeg_muxer would need OBS's obs-ffmpeg-mux helper
	 * binary next to this test executable. */
	OBSDataAutoRelease recSettings = obs_data_create();
	std::string recPath = f.work() + "/native-recording.flv";
	std::filesystem::remove(recPath);
	obs_data_set_string(recSettings, "path", recPath.c_str());
	OBSEncoderAutoRelease recVideo = obs_video_encoder_create("obs_x264", "native rec video", venc, nullptr);
	OBSEncoderAutoRelease recAudio =
		obs_audio_encoder_create("ffmpeg_aac", "native rec audio", nullptr, 0, nullptr);
	obs_encoder_set_video(recVideo, obs_get_video());
	obs_encoder_set_audio(recAudio, obs_get_audio());
	OBSOutputAutoRelease recording = obs_output_create("flv_output", "native recording", recSettings, nullptr);
	obs_output_set_video_encoder(recording, recVideo);
	obs_output_set_audio_encoder(recording, recAudio, 0);

	auto wait_active = [&](obs_output_t *o, bool active) {
		return f.loop().runUntil([&] { return obs_output_active(o) == active; }, 15s);
	};

	/* Plugin outputs start before native streaming exists. */
	f.manager().startEnabled();
	f.check(f.waitState(a, mf::SessionState::Sending, 15s) && f.waitState(b, mf::SessionState::Sending, 15s),
		"plugin outputs start without native streaming");
	f.check(obs_output_start(nativeStream) && wait_active(nativeStream, true), "native-like stream starts");
	f.check(obs_output_start(recording) && wait_active(recording, true), "native-like recording starts");
	f.pump(4s);
	EncoderCount count = plugin_encoders();
	f.check(count.video == 1 && count.audio == 1,
		"plugin still owns exactly one encoder pair alongside native ones");
	obs_output_stop(nativeStream);
	f.check(wait_active(nativeStream, false), "native-like stream stops");
	f.pump(3s);
	f.check(f.state(a) == mf::SessionState::Sending && f.state(b) == mf::SessionState::Sending,
		"plugin outputs unaffected by native stream stop");
	obs_output_stop(recording);
	f.check(wait_active(recording, false), "native-like recording stops");
	f.pump(2s);
	f.check(f.state(a) == mf::SessionState::Sending && f.state(b) == mf::SessionState::Sending,
		"plugin outputs unaffected by recording stop");

	/* Stopping all plugin outputs does not touch native outputs. */
	native.stop();
	native.start();
	f.check(obs_output_start(nativeStream) && wait_active(nativeStream, true), "native-like stream restarted");
	f.manager().stopAll();
	f.waitState(a, mf::SessionState::Idle, 15s);
	f.waitState(b, mf::SessionState::Idle, 15s);
	f.pump(2s);
	f.check(obs_output_active(nativeStream), "native-like stream still active after plugin Stop All");
	obs_output_stop(nativeStream);
	wait_active(nativeStream, false);
	p1.waitExit(5000);
	p2.waitExit(5000);
	verify_recording(f, "plugin receiver 1", p1.recording(1), 8.0);
	MediaReport rec = probe_recording(recPath);
	f.note("native-like recording: " + rec.summary());
	f.check(rec.decodeClean && rec.videoFrames > 60, "native-like recording decodes");
}

void rtmps_success(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	std::string w = f.work() + "/pki";
	std::filesystem::create_directories(w);
	std::string out;
	std::string ext = w + "/server.ext";
	{
		std::ofstream e(ext);
		e << "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n"
		     "keyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n";
	}
	bool pki =
		run_capture(
			"openssl req -x509 -newkey rsa:2048 -nodes -keyout " + w + "/ca.key -out " + w +
				"/ca.pem -days 2 -subj '/CN=MatchFrame Integration Test CA' "
				"-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign",
			out) == 0 &&
		run_capture("openssl req -newkey rsa:2048 -nodes -keyout " + w + "/server.key -out " + w +
				    "/server.csr -subj /CN=localhost",
			    out) == 0 &&
		run_capture("openssl x509 -req -in " + w + "/server.csr -CA " + w + "/ca.pem -CAkey " + w +
				    "/ca.key -CAcreateserial -out " + w + "/server.pem -days 2 -extfile " + ext,
			    out) == 0;
	f.check(pki, "test CA and localhost server certificate created");

	/* librtmp (mbedTLS) on Linux trusts /etc/ssl/certs; the test CA is added only for this test. */
	if (access("/etc/ssl/certs", W_OK) != 0) {
		f.note("SKIPPED: /etc/ssl/certs is not writable (run as root to test a verified RTMPS connection)");
		return;
	}
	std::string installed = "/etc/ssl/certs/matchframe-integration-test-ca.pem";
	std::filesystem::copy_file(w + "/ca.pem", installed, std::filesystem::copy_options::overwrite_existing);
	struct Cleanup {
		std::string path;
		~Cleanup() { std::filesystem::remove(path); }
	} cleanup{installed};

	RtmpReceiver r(f.work(), next_port(), "rtmps");
	r.start();
	int tlsPort = next_port();
	ScriptServer proxy;
	proxy.start({obs.paths().scriptsDir + "/tls_proxy.py", "--listen", std::to_string(tlsPort), "--target",
		     std::to_string(r.port()), "--cert", w + "/server.pem", "--key", w + "/server.key"},
		    f.work() + "/tls-good.log");
	wait_for_port(tlsPort, 5000);
	std::string id = f.add("RTMPS verified", "rtmps://localhost:" + std::to_string(tlsPort) + "/live", r.key());
	f.start(id);
	f.check(f.waitState(id, mf::SessionState::Sending, 20s),
		"RTMPS with a properly verified certificate reaches Sending: " + f.snap(id).lastError.message);
	f.pump(5s);
	f.manager().stopDestination(id);
	f.waitState(id, mf::SessionState::Idle, 15s);
	r.waitExit(5000);
	proxy.stop();
	verify_recording(f, "RTMPS receiver", r.recording(1), 3.0);
}

/*
 * OBS frees its video output on every obs_reset_video() (Settings > Video, a profile switch). The
 * shared encoders must follow the new one: start, stop, change the output resolution, start again.
 */
void video_reset_between_starts(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	obs_video_info original = {};
	obs_get_video_info(&original);
	RtmpReceiver r(f.work(), next_port(), "video-reset");
	f.check(r.start(), "local receiver listening");
	std::string id = f.add("Video reset", r.url(), r.key());

	f.manager().startEnabled();
	f.check(f.waitState(id, mf::SessionState::Sending, 15s), "first session reaches Sending");
	f.pump(4s);
	f.manager().stopAll();
	f.check(f.waitState(id, mf::SessionState::Idle, 15s), "first session stopped");
	r.waitExit(5000);

	obs_video_info smaller = original;
	smaller.output_width = (original.output_width / 2) & ~3u;
	smaller.output_height = (original.output_height / 2) & ~1u;
	int reset = obs_reset_video(&smaller);
	f.check(reset == OBS_VIDEO_SUCCESS, fmt("OBS video reset to %ux%u while the plugin is idle (result %d)",
						smaller.output_width, smaller.output_height, reset));

	f.check(r.start(), "receiver listening again");
	f.manager().startEnabled();
	f.check(f.waitState(id, mf::SessionState::Sending, 15s), "second session reaches Sending after the reset");
	f.pump(5s);
	f.manager().stopAll();
	f.check(f.waitState(id, mf::SessionState::Idle, 15s), "second session stopped");
	r.waitExit(5000);
	verify_recording(f, "first session", r.recording(1), 3.0, (int)original.output_width,
			 (int)original.output_height);
	verify_recording(f, "second session (new video output)", r.recording(2), 4.0, (int)smaller.output_width,
			 (int)smaller.output_height);

	f.check(obs_reset_video(&original) == OBS_VIDEO_SUCCESS, "OBS video restored for the next scenario");
}

void av_sync_short(ObsHarness &obs, ScenarioResult &result)
{
	Fixture f(obs, result);
	RtmpReceiver r(f.work(), next_port(), "sync");
	r.start();
	std::string id = f.add("Sync", r.url(), r.key());

	/* Reference: OBS's own recording output with its own encoders, fed by the same Program at the
	 * same time. The absolute offset depends on how libobs times the test's media source on this
	 * machine (it has varied between -84 and +39 ms across CI runs); the plugin must not add to it. */
	OBSDataAutoRelease venc = obs_data_create();
	obs_data_set_int(venc, "bitrate", 2500);
	OBSDataAutoRelease recSettings = obs_data_create();
	std::string refPath = f.work() + "/sync-reference.flv";
	std::filesystem::remove(refPath);
	obs_data_set_string(recSettings, "path", refPath.c_str());
	OBSEncoderAutoRelease refVideo = obs_video_encoder_create("obs_x264", "sync reference video", venc, nullptr);
	OBSEncoderAutoRelease refAudio =
		obs_audio_encoder_create("ffmpeg_aac", "sync reference audio", nullptr, 0, nullptr);
	obs_encoder_set_video(refVideo, obs_get_video());
	obs_encoder_set_audio(refAudio, obs_get_audio());
	OBSOutputAutoRelease reference = obs_output_create("flv_output", "sync reference", recSettings, nullptr);
	obs_output_set_video_encoder(reference, refVideo);
	obs_output_set_audio_encoder(reference, refAudio, 0);
	f.check(obs_output_start(reference), "reference recording (OBS's own output) starts");

	f.start(id);
	f.waitState(id, mf::SessionState::Sending, 15s);
	f.pump(30s);
	f.manager().stopDestination(id);
	obs_output_stop(reference);
	f.waitState(id, mf::SessionState::Idle, 15s);
	f.loop().runUntil([&] { return !obs_output_active(reference); }, 15s);
	r.waitExit(5000);

	SyncReport sync, ref;
	std::string error, refError;
	bool measured = measure_av_sync(r.recording(1), sync, error);
	bool refMeasured = measure_av_sync(refPath, ref, refError);
	f.check(measured, "flash/beep pairs found in the plugin output: " + error);
	f.check(refMeasured, "flash/beep pairs found in the reference recording: " + refError);
	f.note(fmt("plugin output A/V offset over %d pairs: mean %.1f ms, min %.1f, max %.1f", sync.pairs,
		   sync.meanOffsetMs, sync.minOffsetMs, sync.maxOffsetMs));
	f.note(fmt("OBS's own recording, same time: mean %.1f ms, min %.1f, max %.1f", ref.meanOffsetMs,
		   ref.minOffsetMs, ref.maxOffsetMs));
	f.check(measured && sync.maxOffsetMs - sync.minOffsetMs < 80.0, "A/V offset is stable over the run");
	f.check(measured && refMeasured && std::fabs(sync.meanOffsetMs - ref.meanOffsetMs) < 50.0,
		fmt("plugin output keeps the same A/V alignment as OBS's own recording (difference %.1f ms)",
		    sync.meanOffsetMs - ref.meanOffsetMs));
}

} // namespace

const std::vector<Scenario> &all_scenarios()
{
	static const std::vector<Scenario> scenarios = {
		{"two_outputs_shared", "2,3", two_outputs_shared},
		{"eight_outputs", "2,3,4", eight_outputs},
		{"receiver_restart", "4,6", receiver_restart},
		{"failure_modes", "5", failure_modes},
		{"reconnect_policy", "6", reconnect_policy},
		{"rapid_cycles", "7", rapid_cycles},
		{"audio_track_and_mute", "8 (audio track, muting)", audio_track_and_mute},
		{"independent_native_outputs", "9 (libobs level)", independent_native_outputs},
		{"rtmps_success", "RTMPS with verification", rtmps_success},
		{"video_reset_between_starts", "OBS video reset between starts", video_reset_between_starts},
		{"av_sync_short", "11 (A/V sync against OBS's own recording)", av_sync_short},
	};
	return scenarios;
}

void run_soak(ObsHarness &obs, int minutes, int destinations, int segmentSeconds, ScenarioResult &result)
{
	Fixture f(obs, result);
	std::vector<std::unique_ptr<RtmpReceiver>> receivers;
	std::vector<std::string> ids;
	for (int i = 0; i < destinations; i++) {
		receivers.push_back(std::make_unique<RtmpReceiver>(f.work(), next_port(), "soak-" + std::to_string(i)));
		receivers.back()->startSegmented(segmentSeconds);
		ids.push_back(f.add("Soak " + std::to_string(i), receivers.back()->url(), receivers.back()->key()));
	}
	f.manager().startEnabled();
	for (const std::string &id : ids)
		f.waitState(id, mf::SessionState::Sending, 20s);

	std::string csvPath = f.work() + "/soak-metrics.csv";
	std::ofstream csv(csvPath);
	csv << "elapsed_s,cpu_percent,rss_mb,measured_kbps,sending,dropped_frames,max_congestion,reconnects,"
	       "render_lagged,encoder_skipped\n";

	auto read_cpu = []() {
		std::ifstream stat("/proc/self/stat");
		std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
		size_t close = content.rfind(')');
		std::istringstream rest(content.substr(close + 2));
		std::string field;
		unsigned long long utime = 0, stime = 0;
		for (int i = 3; i <= 15 && rest >> field; i++) {
			if (i == 14)
				utime = std::stoull(field);
			if (i == 15)
				stime = std::stoull(field);
		}
		return (double)(utime + stime) / (double)sysconf(_SC_CLK_TCK);
	};
	auto read_rss = []() {
		std::ifstream status("/proc/self/status");
		std::string line;
		while (std::getline(status, line)) {
			if (line.rfind("VmRSS:", 0) == 0)
				return std::atof(line.c_str() + 6) / 1024.0;
		}
		return 0.0;
	};

	auto begin = Clock::now();
	double lastCpu = read_cpu();
	auto lastSample = Clock::now();
	std::vector<double> rssSamples, timeSamples;
	double maxCongestion = 0;
	int totalDropped = 0;
	int reconnects = 0;
	while (seconds_since(begin) < minutes * 60.0) {
		f.pump(10s);
		double cpu = read_cpu();
		double dt = std::chrono::duration<double>(Clock::now() - lastSample).count();
		double cpuPct = 100.0 * (cpu - lastCpu) / dt;
		lastCpu = cpu;
		lastSample = Clock::now();
		mf::ManagerSummary sum = f.manager().summary();
		int dropped = 0;
		float congestion = 0;
		int rc = 0;
		for (const std::string &id : ids) {
			mf::SessionSnapshot s = f.snap(id);
			dropped += s.framesDropped;
			congestion = std::max(congestion, s.congestion);
			rc += s.reconnectCount;
		}
		double rss = read_rss();
		double elapsed = seconds_since(begin);
		rssSamples.push_back(rss);
		timeSamples.push_back(elapsed);
		maxCongestion = std::max(maxCongestion, (double)congestion);
		totalDropped = dropped;
		reconnects = rc;
		csv << fmt("%.0f,%.1f,%.1f,%.0f,%d,%d,%.3f,%d,%u,%u\n", elapsed, cpuPct, rss, sum.measuredKbps,
			   sum.sending, dropped, congestion, rc, obs_get_lagged_frames(),
			   video_output_get_skipped_frames(obs_get_video()));
		csv.flush();

		/* Keep disk use bounded: delete finished segments except every 6th (30 min apart). */
		for (auto &r : receivers) {
			std::string pattern = r->segmentPattern();
			std::string stem = pattern.substr(0, pattern.find("%05d"));
			for (int seg = 0; seg < 2000; seg++) {
				std::string path = stem + fmt("%05d", seg) + ".flv";
				std::string nextPath = stem + fmt("%05d", seg + 1) + ".flv";
				if (!std::filesystem::exists(path))
					continue; // already removed, or not written yet
				bool finished = std::filesystem::exists(nextPath);
				if (finished && seg % 6 != 0)
					std::filesystem::remove(path);
			}
		}
	}

	bool allSending = true;
	for (const std::string &id : ids)
		allSending = allSending && f.state(id) == mf::SessionState::Sending;
	f.check(allSending, "every destination still sending at the end of the soak");

	/* Memory trend: least-squares slope of RSS over time, after a 5 minute warm-up. */
	double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (size_t i = 0; i < rssSamples.size(); i++) {
		if (timeSamples[i] < std::min(300.0, minutes * 60.0 / 4))
			continue;
		double x = timeSamples[i] / 3600.0, y = rssSamples[i];
		n++;
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	double slope = n > 2 ? (n * sxy - sx * sy) / (n * sxx - sx * sx) : 0.0;
	f.note(fmt("RSS start %.1f MB, end %.1f MB, trend %.2f MB/hour", rssSamples.front(), rssSamples.back(), slope));
	f.note(fmt("network dropped frames total %d, max congestion %.3f, reconnects %d", totalDropped, maxCongestion,
		   reconnects));
	if (minutes >= 30)
		f.check(slope < 20.0, "no material memory growth");
	else
		f.note("run too short to judge a memory trend (needs at least 30 minutes); not checked");
	f.check(maxCongestion < 0.5, "no growing network backlog (RTMP congestion stays low)");

	f.manager().stopAll();
	for (const std::string &id : ids)
		f.waitState(id, mf::SessionState::Idle, 30s);
	for (auto &r : receivers)
		r->waitExit(10000);

	if (receivers.empty())
		return; // baseline run without destinations: resource metrics only

	/* A/V sync on retained segments of receiver 0: first, middle and last. */
	std::vector<std::string> retained;
	std::string pattern = receivers[0]->segmentPattern();
	for (int seg = 0; seg < 10000; seg++) {
		char name[64];
		snprintf(name, sizeof(name), "%05d", seg);
		std::string path = pattern.substr(0, pattern.find("%05d")) + name + ".flv";
		if (std::filesystem::exists(path))
			retained.push_back(path);
	}
	std::vector<double> means;
	for (const std::string &path : retained) {
		SyncReport sync;
		std::string error;
		if (measure_av_sync(path, sync, error)) {
			means.push_back(sync.meanOffsetMs);
			f.note(fmt("%s: %d pairs, mean %.1f ms, range %.1f..%.1f ms",
				   std::filesystem::path(path).filename().c_str(), sync.pairs, sync.meanOffsetMs,
				   sync.minOffsetMs, sync.maxOffsetMs));
		} else {
			f.note(std::filesystem::path(path).filename().string() + ": " + error);
		}
	}
	f.check(means.size() >= 2, "A/V sync measured on retained segments across the run");
	if (means.size() >= 2) {
		double drift = means.back() - means.front();
		f.note(fmt("A/V offset drift from first to last retained segment: %.1f ms", drift));
		f.check(std::fabs(drift) < 50.0, "no progressive A/V sync drift");
	}
	f.note("metrics CSV: " + csvPath);
}

} // namespace mftest
