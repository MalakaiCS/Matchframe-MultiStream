// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Headless libobs for integration tests: real OBS 32.2.2 modules, a program scene with
// test media, and local receivers (ffmpeg RTMP listeners, nginx-rtmp with publish auth,
// a TLS-terminating proxy and a stalling TCP server).

#pragma once

#include "integration/process.hpp"

#include <obs.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mftest {

struct HarnessPaths {
	std::string obsPrefix;  // OBS install prefix (lib/obs-plugins, share/obs)
	std::string workDir;    // scratch directory for media, recordings and logs
	std::string scriptsDir; // tests/scripts
};

class ObsHarness {
public:
	~ObsHarness();
	bool start(const HarnessPaths &paths, int width, int height, int fps, std::string &error);
	void shutdown();

	/* Program scene: clip A (moving pattern, 1 kHz beeps on flashes) on audio track 1 only,
	 * clip B (440 Hz tone) on track 2 only. */
	obs_source_t *sourceA() const { return sourceA_; }
	obs_source_t *sourceB() const { return sourceB_; }
	obs_scene_t *scene() const { return scene_; }
	const HarnessPaths &paths() const { return paths_; }

private:
	bool loadModule(const std::string &name, std::string &error);
	bool makeMedia(std::string &error);

	HarnessPaths paths_;
	void *display_ = nullptr;
	bool started_ = false;
	OBSSourceAutoRelease sourceA_;
	OBSSourceAutoRelease sourceB_;
	OBSSceneAutoRelease scene_;
};

/* One ffmpeg process listening as an RTMP server and copying the stream to an FLV file. */
class RtmpReceiver {
public:
	RtmpReceiver(std::string workDir, int port, std::string name);
	bool start();
	/* Long runs: FLV segments of segmentSeconds each (recv-<name>-seg-%05d.flv). */
	bool startSegmented(int segmentSeconds);
	std::string segmentPattern() const;
	void stop();
	bool running() { return process_.running(); }
	bool waitExit(int ms) { return process_.waitExit(ms); }
	std::string url() const; // rtmp://127.0.0.1:<port>/live
	const std::string &key() const { return key_; }
	std::string recording(int segment) const;
	int segments() const { return segment_; }
	int port() const { return port_; }

private:
	std::string workDir_;
	int port_;
	std::string name_;
	std::string key_;
	int segment_ = 0;
	Process process_;
};

/* nginx-rtmp that only accepts stream names starting with "good" (on_publish -> HTTP 403 otherwise). */
class AuthRtmpServer {
public:
	bool start(const std::string &workDir, int rtmpPort, int httpPort);
	void stop() { process_.terminate(SIGTERM, 3000); } // fast shutdown drops live RTMP sessions

private:
	Process process_;
};

/* Python helpers from tests/scripts. */
class ScriptServer {
public:
	bool start(const std::vector<std::string> &args, const std::string &logPath);
	void stop() { process_.terminate(SIGTERM, 2000); }

private:
	Process process_;
};

bool wait_for_port(int port, int timeoutMs);

struct MediaReport {
	bool probed = false;
	std::string videoCodec;
	int width = 0;
	int height = 0;
	int videoFrames = 0;
	double videoSeconds = 0.0;
	std::string audioCodec;
	int sampleRate = 0;
	int channels = 0;
	double audioSeconds = 0.0;
	bool decodeClean = false;
	std::string decodeErrors;
	std::string summary() const;
};

MediaReport probe_recording(const std::string &path);
/* Energy ratio of the 1000 Hz vs 440 Hz test tones in the decoded audio (Goertzel). */
bool tone_energies(const std::string &path, double fromSec, double toSec, double &e1000, double &e440, double &rms);

struct SyncReport {
	int pairs = 0;
	double meanOffsetMs = 0.0;
	double minOffsetMs = 0.0;
	double maxOffsetMs = 0.0;
	double firstHalfMeanMs = 0.0;
	double secondHalfMeanMs = 0.0;
};
/* Matches each video flash (luma spike) with the nearest 1 kHz beep onset. */
bool measure_av_sync(const std::string &path, SyncReport &report, std::string &error);

} // namespace mftest
