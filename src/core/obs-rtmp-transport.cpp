// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "transport.hpp"
#include "log.hpp"
#include "redaction.hpp"

#include <obs.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace mf {

namespace {

constexpr const char *kOutputType = "rtmp_output";
constexpr const char *kServiceType = "rtmp_custom";
constexpr auto kStopSignalWait = std::chrono::milliseconds(5000);
constexpr auto kSendThreadGrace = std::chrono::milliseconds(250);

class ObsRtmpTransport final : public Transport {
public:
	ObsRtmpTransport(const AttemptSpec &spec, std::shared_ptr<TransportEvents> events)
		: context_{std::move(events), spec.attemptId},
		  videoEncoder_(spec.videoEncoder),
		  audioEncoder_(spec.audioEncoder),
		  name_(spec.outputName)
	{
	}

	~ObsRtmpTransport() override
	{
		if (output_ && obs_output_active(output_))
			obs_output_force_stop(output_);
		if (output_ && context_.started)
			waitForSendThreadExit();
		/* Disconnect first: after signal_handler_disconnect returns no callback is running or
		 * will run with &context_, so the context can be destroyed with this object. */
		startSignal_.Disconnect();
		stopSignal_.Disconnect();
		reconnectSignal_.Disconnect();
		output_ = nullptr; // obs_output_release: may join OBS's connect/send threads
		service_ = nullptr;
	}

	bool create(const AttemptSpec &spec, std::string &error)
	{
		if (!obs_get_output_flags(kOutputType)) {
			error = "OBS's RTMP output (obs-outputs) is not loaded.";
			return false;
		}

		OBSDataAutoRelease serviceSettings = obs_data_create();
		obs_data_set_string(serviceSettings, "server", spec.serverUrl.c_str());
		obs_data_set_string(serviceSettings, "key", spec.secrets.streamKey.c_str());
		obs_data_set_bool(serviceSettings, "use_auth", spec.useAuth);
		if (spec.useAuth) {
			obs_data_set_string(serviceSettings, "username", spec.secrets.username.c_str());
			obs_data_set_string(serviceSettings, "password", spec.secrets.password.c_str());
		}
		obs_data_set_bool(serviceSettings, "bwtest", false);
		service_ = obs_service_create_private(kServiceType, spec.outputName.c_str(), serviceSettings);
		if (!service_) {
			error = "OBS's custom RTMP service (rtmp-services) is not available.";
			return false;
		}

		OBSDataAutoRelease outputSettings = obs_data_create();
		/* Dynamic bitrate would call obs_encoder_update() on the shared encoder and change the
		 * bitrate of every destination, so it is always off for plugin outputs. */
		obs_data_set_bool(outputSettings, "dyn_bitrate", false);
		obs_data_set_int(outputSettings, "max_shutdown_time_sec", spec.maxShutdownSec);
		output_ = obs_output_create(kOutputType, spec.outputName.c_str(), outputSettings, nullptr);
		if (!output_) {
			error = "Could not create an RTMP output.";
			return false;
		}

		/* Built-in reconnect off: DestinationSession is the only reconnect authority. */
		obs_output_set_reconnect_settings(output_, 0, 0);
		obs_output_set_video_encoder(output_, videoEncoder_);
		obs_output_set_audio_encoder(output_, audioEncoder_, 0);
		obs_output_set_service(output_, service_);

		signal_handler_t *handler = obs_output_get_signal_handler(output_);
		startSignal_.Connect(handler, "start", on_start, &context_);
		stopSignal_.Connect(handler, "stop", on_stop, &context_);
		reconnectSignal_.Connect(handler, "reconnect", on_unexpected_reconnect, &context_);
		return true;
	}

	bool start(StartFailure &failure) override
	{
		if (!obs_service_can_try_to_connect(service_)) {
			failure.obsCode = OBS_OUTPUT_BAD_PATH;
			failure.detail = "The service has no server URL.";
			return false;
		}
		if (obs_output_start(output_))
			return true;

		/* With a valid service, rtmp_stream_start() only fails in
		 * obs_output_initialize_encoders(), i.e. the shared encoder did not initialise. */
		const char *outputError = obs_output_get_last_error(output_);
		const char *videoError = obs_encoder_get_last_error(videoEncoder_);
		const char *audioError = obs_encoder_get_last_error(audioEncoder_);
		failure.encoderFailure = true;
		failure.obsCode = OBS_OUTPUT_ENCODE_ERROR;
		failure.detail = outputError ? outputError : videoError ? videoError : audioError ? audioError : "";
		return false;
	}

	void stop(bool force) override
	{
		if (!output_)
			return;
		/* obs_output_stop() is a no-op while the RTMP connect thread is still running
		 * (the output is not "active" yet), so a connecting output is force-stopped,
		 * which joins the connect thread and then stops whatever it established. */
		if (!force && obs_output_active(output_))
			obs_output_stop(output_);
		else
			obs_output_force_stop(output_);
	}

	bool active() const override { return output_ && obs_output_active(output_); }

	TransportStats sample() const override
	{
		TransportStats stats;
		if (!output_)
			return stats;
		stats.active = obs_output_active(output_);
		stats.totalBytes = obs_output_get_total_bytes(output_);
		stats.framesDropped = obs_output_get_frames_dropped(output_);
		stats.totalFrames = obs_output_get_total_frames(output_);
		stats.congestion = obs_output_get_congestion(output_);
		stats.connectTimeMs = obs_output_get_connect_time_ms(output_);
		return stats;
	}

	bool attachedTo(obs_encoder_t *video, obs_encoder_t *audio) const override
	{
		return output_ && obs_output_get_video_encoder(output_) == video &&
		       obs_output_get_audio_encoder(output_, 0) == audio;
	}

private:
	struct SignalContext {
		std::shared_ptr<TransportEvents> events;
		uint64_t attemptId;
		std::atomic<bool> started{false};
		std::atomic<bool> stopped{false};
	};

	/*
	 * When a stream ends, OBS's RTMP send thread asks libobs to end data capture and then keeps
	 * using its stream object for a few more statements (rtmp-stream.c, end of send_thread).
	 * obs_output_active() turns false in between, and rtmp_stream_destroy() joins the send
	 * thread only while the stream still looks stopping or active, so releasing the output the
	 * moment it reports inactive can free the stream under that thread. Wait for libobs's "stop"
	 * signal and a short grace period first; OBS's own outputs are released much later.
	 */
	void waitForSendThreadExit()
	{
		auto deadline = std::chrono::steady_clock::now() + kStopSignalWait;
		while (!context_.stopped && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		std::this_thread::sleep_for(kSendThreadGrace);
	}

	static void on_start(void *param, calldata_t *)
	{
		auto *context = static_cast<SignalContext *>(param);
		context->started = true;
		context->events->transportStarted(context->attemptId);
	}

	static void on_stop(void *param, calldata_t *data)
	{
		auto *context = static_cast<SignalContext *>(param);
		context->stopped = true;
		int code = (int)calldata_int(data, "code");
		const char *lastError = calldata_string(data, "last_error");
		context->events->transportStopped(context->attemptId, code, lastError ? lastError : "");
	}

	static void on_unexpected_reconnect(void *param, calldata_t *)
	{
		auto *context = static_cast<SignalContext *>(param);
		MF_WARN("attempt %llu: OBS built-in reconnect fired although it is disabled",
			(unsigned long long)context->attemptId);
	}

	SignalContext context_;
	OBSEncoder videoEncoder_;
	OBSEncoder audioEncoder_;
	std::string name_;
	OBSServiceAutoRelease service_;
	OBSOutputAutoRelease output_;
	OBSSignal startSignal_;
	OBSSignal stopSignal_;
	OBSSignal reconnectSignal_;
};

} // namespace

std::unique_ptr<Transport> create_obs_rtmp_transport(const AttemptSpec &spec, std::shared_ptr<TransportEvents> events,
						     std::string &error)
{
	auto transport = std::make_unique<ObsRtmpTransport>(spec, std::move(events));
	if (!transport->create(spec, error))
		return nullptr;
	return transport;
}

} // namespace mf
