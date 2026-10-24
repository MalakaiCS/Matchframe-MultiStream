// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "integration/obs-harness.hpp"

#include <obs-nix-platform.h>
#include <util/platform.h>

#include <X11/Xlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mftest {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

double parse_double_after(const std::string &text, const std::string &key)
{
	size_t at = text.find(key);
	if (at == std::string::npos)
		return 0.0;
	return std::atof(text.c_str() + at + key.size());
}

} // namespace

ObsHarness::~ObsHarness()
{
	shutdown();
}

bool ObsHarness::loadModule(const std::string &name, std::string &error)
{
	std::string bin = paths_.obsPrefix + "/lib/obs-plugins/" + name + ".so";
	std::string data = paths_.obsPrefix + "/share/obs/obs-plugins/" + name;
	obs_module_t *module = nullptr;
	int code = obs_open_module(&module, bin.c_str(), data.c_str());
	if (code != MODULE_SUCCESS) {
		error = "obs_open_module(" + name + ") failed: " + std::to_string(code);
		return false;
	}
	if (!obs_init_module(module)) {
		error = "obs_init_module(" + name + ") failed";
		return false;
	}
	return true;
}

bool ObsHarness::makeMedia(std::string &error)
{
	std::string a = paths_.workDir + "/clip-a.mp4";
	std::string b = paths_.workDir + "/clip-b.mp4";
	std::string out;
	if (!std::filesystem::exists(a)) {
		/* 60 s loop: moving test pattern; every 2 s a white flash (3 frames at 30 fps)
		 * and a 100 ms 1 kHz beep start on the same instant. */
		std::string cmd =
			"ffmpeg -nostdin -hide_banner -loglevel error -y "
			"-f lavfi -i \"testsrc2=size=1280x720:rate=30:duration=60\" "
			"-f lavfi -i \"sine=frequency=1000:sample_rate=48000:duration=60\" "
			"-filter_complex \"[0:v]drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:"
			"enable='lt(mod(t\\,2)\\,0.1)'[v];[1:a]volume='if(lt(mod(t\\,2)\\,0.1)\\,1\\,0)':eval=frame,"
			"aformat=channel_layouts=stereo[a]\" "
			"-map \"[v]\" -map \"[a]\" -c:v libx264 -preset ultrafast -g 30 -pix_fmt yuv420p "
			"-c:a aac -b:a 192k " +
			shell_quote(a);
		if (run_capture(cmd, out) != 0) {
			error = "could not generate clip A: " + out;
			return false;
		}
	}
	if (!std::filesystem::exists(b)) {
		std::string cmd = "ffmpeg -nostdin -hide_banner -loglevel error -y "
				  "-f lavfi -i \"smptebars=size=320x180:rate=30:duration=60\" "
				  "-f lavfi -i \"sine=frequency=440:sample_rate=48000:duration=60\" "
				  "-af aformat=channel_layouts=stereo -c:v libx264 -preset ultrafast -pix_fmt yuv420p "
				  "-c:a aac -b:a 192k " +
				  shell_quote(b);
		if (run_capture(cmd, out) != 0) {
			error = "could not generate clip B: " + out;
			return false;
		}
	}
	return true;
}

bool ObsHarness::start(const HarnessPaths &paths, int width, int height, int fps, std::string &error)
{
	paths_ = paths;
	std::filesystem::create_directories(paths_.workDir);
	if (!makeMedia(error))
		return false;

	display_ = XOpenDisplay(nullptr);
	if (!display_) {
		error = "no X display (run under Xvfb)";
		return false;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display_);

	std::string config = paths_.workDir + "/obs-config";
	std::filesystem::create_directories(config);
	if (!obs_startup("en-US", config.c_str(), nullptr)) {
		error = "obs_startup failed";
		return false;
	}
	started_ = true;

	obs_audio_info2 audio = {};
	audio.samples_per_sec = 48000;
	audio.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio2(&audio)) {
		error = "obs_reset_audio2 failed";
		return false;
	}

	obs_video_info video = {};
	video.graphics_module = "libobs-opengl";
	video.fps_num = (uint32_t)fps;
	video.fps_den = 1;
	video.base_width = video.output_width = (uint32_t)width;
	video.base_height = video.output_height = (uint32_t)height;
	video.output_format = VIDEO_FORMAT_NV12;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_PARTIAL;
	video.adapter = 0;
	video.gpu_conversion = true;
	video.scale_type = OBS_SCALE_BICUBIC;
	int videoResult = obs_reset_video(&video);
	if (videoResult != OBS_VIDEO_SUCCESS) {
		error = "obs_reset_video failed: " + std::to_string(videoResult);
		return false;
	}

	for (const char *module : {"obs-outputs", "obs-x264", "obs-ffmpeg", "rtmp-services"}) {
		if (!loadModule(module, error))
			return false;
	}
	obs_post_load_modules();

	auto media_source = [&](const char *name, const std::string &file, uint32_t mixers) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", file.c_str());
		obs_data_set_bool(settings, "looping", true);
		obs_data_set_bool(settings, "hw_decode", false);
		obs_data_set_bool(settings, "close_when_inactive", false);
		obs_data_set_bool(settings, "restart_on_activate", false);
		obs_source_t *source = obs_source_create("ffmpeg_source", name, settings, nullptr);
		if (source)
			obs_source_set_audio_mixers(source, mixers);
		return source;
	};
	sourceA_ = media_source("clip A (track 1)", paths_.workDir + "/clip-a.mp4", 1u << 0);
	sourceB_ = media_source("clip B (track 2)", paths_.workDir + "/clip-b.mp4", 1u << 1);
	if (!sourceA_ || !sourceB_) {
		error = "could not create media sources";
		return false;
	}
	scene_ = obs_scene_create("Program");
	obs_sceneitem_t *itemA = obs_scene_add(scene_, sourceA_);
	/* Fill the canvas whatever its size, so a flash changes the whole frame's luma. */
	vec2 bounds = {(float)width, (float)height};
	obs_sceneitem_set_bounds_type(itemA, OBS_BOUNDS_STRETCH);
	obs_sceneitem_set_bounds(itemA, &bounds);
	obs_sceneitem_t *itemB = obs_scene_add(scene_, sourceB_);
	vec2 pos = {(float)width - 340.0f, 20.0f};
	obs_sceneitem_set_pos(itemB, &pos);
	obs_set_output_source(0, obs_scene_get_source(scene_));
	return true;
}

void ObsHarness::shutdown()
{
	if (!started_)
		return;
	obs_set_output_source(0, nullptr);
	/* The main canvas keeps its own reference to a scene until the scene is removed (libobs
	 * 32), and the scene keeps the media sources alive. Without removing it, all three were only
	 * destroyed inside obs_shutdown, sometimes while media threads still ran. */
	obs_source_remove(obs_scene_get_source(scene_));
	obs_source_remove(sourceA_);
	obs_source_remove(sourceB_);
	scene_ = nullptr;
	sourceA_ = nullptr;
	sourceB_ = nullptr;
	/* Like the OBS frontend (which clears scene data long before obs_shutdown), let the media
	 * sources' decoder threads stop before audio/video are torn down. */
	obs_wait_for_destroy_queue();
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	obs_wait_for_destroy_queue();
	obs_shutdown();
	started_ = false;
	if (display_) {
		XCloseDisplay((Display *)display_);
		display_ = nullptr;
	}
}

/* ---- receivers ---- */

RtmpReceiver::RtmpReceiver(std::string workDir, int port, std::string name)
	: workDir_(std::move(workDir)),
	  port_(port),
	  name_(std::move(name)),
	  key_("stream-" + name_)
{
}

std::string RtmpReceiver::url() const
{
	return "rtmp://127.0.0.1:" + std::to_string(port_) + "/live";
}

std::string RtmpReceiver::recording(int segment) const
{
	return workDir_ + "/recv-" + name_ + "-" + std::to_string(segment) + ".flv";
}

bool RtmpReceiver::start()
{
	segment_++;
	std::filesystem::remove(recording(segment_));
	std::vector<std::string> args = {
		"ffmpeg", "-nostdin", "-hide_banner",     "-loglevel", "warning", "-y", "-listen",
		"1",      "-i",       url() + "/" + key_, "-map",      "0",       "-c", "copy",
		"-f",     "flv",      recording(segment_)};
	if (!process_.spawn(args, workDir_ + "/recv-" + name_ + ".log"))
		return false;
	return wait_for_port(port_, 5000);
}

std::string RtmpReceiver::segmentPattern() const
{
	return workDir_ + "/recv-" + name_ + "-seg-%05d.flv";
}

bool RtmpReceiver::startSegmented(int segmentSeconds)
{
	std::vector<std::string> args = {"ffmpeg",
					 "-nostdin",
					 "-hide_banner",
					 "-loglevel",
					 "warning",
					 "-y",
					 "-listen",
					 "1",
					 "-i",
					 url() + "/" + key_,
					 "-map",
					 "0",
					 "-c",
					 "copy",
					 "-f",
					 "segment",
					 "-segment_time",
					 std::to_string(segmentSeconds),
					 "-segment_format",
					 "flv",
					 "-reset_timestamps",
					 "1",
					 segmentPattern()};
	if (!process_.spawn(args, workDir_ + "/recv-" + name_ + ".log"))
		return false;
	return wait_for_port(port_, 5000);
}

void RtmpReceiver::stop()
{
	process_.terminate(SIGINT, 3000);
}

bool AuthRtmpServer::start(const std::string &workDir, int rtmpPort, int httpPort)
{
	std::string prefix = workDir + "/nginx";
	std::filesystem::create_directories(prefix + "/logs");
	std::ofstream conf(prefix + "/nginx.conf");
	conf << "load_module /usr/lib/nginx/modules/ngx_rtmp_module.so;\n"
	     << "worker_processes 1;\nerror_log " << prefix << "/logs/error.log info;\npid " << prefix
	     << "/nginx.pid;\nevents { worker_connections 64; }\n"
	     << "rtmp { server { listen 127.0.0.1:" << rtmpPort
	     << "; application live { live on; notify_method get; on_publish http://127.0.0.1:" << httpPort
	     << "/auth; } } }\n"
	     << "http { access_log " << prefix << "/logs/access.log; client_body_temp_path " << prefix
	     << "/body; proxy_temp_path " << prefix << "/proxy; fastcgi_temp_path " << prefix
	     << "/fcgi; uwsgi_temp_path " << prefix << "/uwsgi; scgi_temp_path " << prefix << "/scgi;\n"
	     << "server { listen 127.0.0.1:" << httpPort
	     << "; location /auth { if ($arg_name ~ \"^good\") { return 200; } return 403; } } }\n";
	conf.close();
	if (!process_.spawn({"nginx", "-p", prefix, "-c", prefix + "/nginx.conf", "-g", "daemon off;"},
			    prefix + "/logs/stdout.log"))
		return false;
	return wait_for_port(rtmpPort, 5000) && wait_for_port(httpPort, 5000);
}

bool ScriptServer::start(const std::vector<std::string> &args, const std::string &logPath)
{
	std::vector<std::string> full = {"python3", "-I"};
	full.insert(full.end(), args.begin(), args.end());
	return process_.spawn(full, logPath);
}

/* Checks for a socket in LISTEN state without connecting to it: a probe connection would be
 * taken by "ffmpeg -listen 1" as its only client. */
static bool port_listening(int port)
{
	for (const char *table : {"/proc/net/tcp", "/proc/net/tcp6"}) {
		std::ifstream f(table);
		std::string line;
		std::getline(f, line); // header
		while (std::getline(f, line)) {
			std::istringstream fields(line);
			std::string slot, local, remote, state;
			fields >> slot >> local >> remote >> state;
			size_t colon = local.rfind(':');
			if (colon == std::string::npos)
				continue;
			int localPort = (int)std::strtol(local.c_str() + colon + 1, nullptr, 16);
			if (localPort == port && state == "0A")
				return true;
		}
	}
	return false;
}

bool wait_for_port(int port, int timeoutMs)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline) {
		if (port_listening(port))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	return false;
}

/* ---- verification ---- */

std::string MediaReport::summary() const
{
	char buffer[256];
	snprintf(buffer, sizeof(buffer), "%s %dx%d %d frames %.1fs | %s %d Hz %dch %.1fs | decode %s",
		 videoCodec.c_str(), width, height, videoFrames, videoSeconds, audioCodec.c_str(), sampleRate, channels,
		 audioSeconds, decodeClean ? "clean" : "ERRORS");
	return buffer;
}

MediaReport probe_recording(const std::string &path)
{
	MediaReport report;
	std::string out;
	std::string cmd = "ffprobe -v error -count_frames -show_entries "
			  "stream=codec_type,codec_name,width,height,sample_rate,channels,nb_read_frames,duration "
			  "-of default=nw=0 " +
			  shell_quote(path);
	if (run_capture(cmd, out) != 0)
		return report;
	report.probed = true;
	std::istringstream lines(out);
	std::string line, type;
	std::map<std::string, std::string> stream;
	auto flush = [&]() {
		if (stream["codec_type"] == "video") {
			report.videoCodec = stream["codec_name"];
			report.width = std::atoi(stream["width"].c_str());
			report.height = std::atoi(stream["height"].c_str());
			report.videoFrames = std::atoi(stream["nb_read_frames"].c_str());
		} else if (stream["codec_type"] == "audio") {
			report.audioCodec = stream["codec_name"];
			report.sampleRate = std::atoi(stream["sample_rate"].c_str());
			report.channels = std::atoi(stream["channels"].c_str());
			int audioFrames = std::atoi(stream["nb_read_frames"].c_str());
			report.audioSeconds = report.sampleRate ? audioFrames * 1024.0 / report.sampleRate : 0.0;
		}
		stream.clear();
	};
	while (std::getline(lines, line)) {
		if (line == "[STREAM]") {
			stream.clear();
		} else if (line == "[/STREAM]") {
			flush();
		} else {
			size_t eq = line.find('=');
			if (eq != std::string::npos)
				stream[line.substr(0, eq)] = line.substr(eq + 1);
		}
	}
	report.videoSeconds = report.videoFrames / 30.0;

	std::string errors;
	int rc = run_capture("ffmpeg -nostdin -v error -i " + shell_quote(path) + " -f null -", errors);
	report.decodeErrors = errors;
	report.decodeClean = rc == 0 && errors.find("rror") == std::string::npos;
	return report;
}

bool tone_energies(const std::string &path, double fromSec, double toSec, double &e1000, double &e440, double &rms)
{
	std::string raw = path + ".pcm";
	std::string out;
	char window[96];
	snprintf(window, sizeof(window), "-ss %.2f -t %.2f", fromSec, toSec - fromSec);
	std::string cmd = "ffmpeg -nostdin -v error -y " + std::string(window) + " -i " + shell_quote(path) +
			  " -vn -ac 1 -ar 16000 -f s16le " + shell_quote(raw);
	if (run_capture(cmd, out) != 0)
		return false;
	std::string pcm = read_file(raw);
	std::filesystem::remove(raw);
	size_t count = pcm.size() / 2;
	if (count < 1600)
		return false;
	const int16_t *samples = reinterpret_cast<const int16_t *>(pcm.data());
	auto goertzel = [&](double freq) {
		double coeff = 2.0 * std::cos(2.0 * kPi * freq / 16000.0);
		double total = 0.0;
		const size_t block = 1600; // 100 ms
		for (size_t start = 0; start + block <= count; start += block) {
			double s1 = 0, s2 = 0;
			for (size_t i = 0; i < block; i++) {
				double s0 = samples[start + i] / 32768.0 + coeff * s1 - s2;
				s2 = s1;
				s1 = s0;
			}
			total += s1 * s1 + s2 * s2 - coeff * s1 * s2;
		}
		return total;
	};
	e1000 = goertzel(1000.0);
	e440 = goertzel(440.0);
	double sum = 0;
	for (size_t i = 0; i < count; i++)
		sum += (samples[i] / 32768.0) * (samples[i] / 32768.0);
	rms = std::sqrt(sum / count);
	return true;
}

bool measure_av_sync(const std::string &path, SyncReport &report, std::string &error)
{
	/* Video: per-frame mean luma; a flash is a frame far brighter than the pattern. */
	std::string out;
	std::string luma = path + ".luma.txt";
	std::string cmd = "ffmpeg -nostdin -v error -i " + shell_quote(path) +
			  " -an -vf \"signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=" + luma +
			  "\" -f null -";
	if (run_capture(cmd, out) != 0) {
		error = "luma analysis failed: " + out;
		return false;
	}
	std::vector<double> flashes;
	{
		std::ifstream f(luma);
		std::string line;
		double pts = 0;
		bool inFlash = false;
		while (std::getline(f, line)) {
			if (line.rfind("frame:", 0) == 0) {
				pts = parse_double_after(line, "pts_time:");
			} else if (line.find("YAVG=") != std::string::npos) {
				double y = parse_double_after(line, "YAVG=");
				bool bright = y > 200.0;
				if (bright && !inFlash)
					flashes.push_back(pts);
				inFlash = bright;
			}
		}
		std::filesystem::remove(luma);
	}

	/* Audio: beep onsets = ends of silence. */
	cmd = "ffmpeg -nostdin -v info -i " + shell_quote(path) + " -vn -af silencedetect=n=-40dB:d=0.3 -f null - 2>&1";
	run_capture(cmd, out);
	std::vector<double> beeps;
	std::istringstream lines(out);
	std::string line;
	while (std::getline(lines, line)) {
		size_t at = line.find("silence_end:");
		if (at != std::string::npos)
			beeps.push_back(std::atof(line.c_str() + at + 12));
	}
	if (flashes.empty() || beeps.empty()) {
		error = "no flashes (" + std::to_string(flashes.size()) + ") or beeps (" +
			std::to_string(beeps.size()) + ") found";
		return false;
	}

	std::vector<double> offsets;
	for (double flash : flashes) {
		double best = 1e9;
		for (double beep : beeps) {
			if (std::fabs(beep - flash) < std::fabs(best))
				best = beep - flash;
		}
		if (std::fabs(best) < 0.5)
			offsets.push_back(best * 1000.0);
	}
	if (offsets.empty()) {
		error = "no flash/beep pairs within 500 ms";
		return false;
	}
	report.pairs = (int)offsets.size();
	double sum = 0;
	report.minOffsetMs = *std::min_element(offsets.begin(), offsets.end());
	report.maxOffsetMs = *std::max_element(offsets.begin(), offsets.end());
	for (double o : offsets)
		sum += o;
	report.meanOffsetMs = sum / offsets.size();
	size_t half = offsets.size() / 2;
	double a = 0, b = 0;
	for (size_t i = 0; i < offsets.size(); i++)
		(i < half ? a : b) += offsets[i];
	report.firstHalfMeanMs = half ? a / half : 0;
	report.secondHalfMeanMs = offsets.size() - half ? b / (offsets.size() - half) : 0;
	return true;
}

} // namespace mftest
