// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "common/test-framework.hpp"

#include "core/bandwidth.hpp"
#include "core/config-repository.hpp"
#include "core/credential-store.hpp"
#include "core/destination-config.hpp"
#include "core/encoder-profile.hpp"
#include "core/error-classifier.hpp"
#include "core/profile-config.hpp"
#include "core/reconnect-policy.hpp"
#include "core/redaction.hpp"
#include "core/rtmp-url.hpp"

#include <obs-defs.h>
#include <util/platform.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace mf;

namespace {

bool has_issue(const Issues &issues, Severity severity, const std::string &needle)
{
	for (const Issue &issue : issues) {
		if (issue.severity == severity && issue.message.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

std::string temp_dir(const std::string &name)
{
	auto dir = std::filesystem::temp_directory_path() / ("mf-unit-" + name + "-" + new_uuid().substr(0, 8));
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	return dir.string();
}

DestinationConfig sample_destination(const std::string &name, Platform platform, const std::string &url)
{
	DestinationConfig d;
	d.id = new_uuid();
	d.name = name;
	d.platform = platform;
	d.serverUrl = url;
	return d;
}

} // namespace

/* ---- URL parsing and validation ---- */

MF_TEST(url_parses_rtmps_with_port_and_app)
{
	RtmpUrl u = parse_rtmp_url("rtmps://a.rtmps.youtube.com:443/live2");
	CHECK(u.parsed);
	CHECK_EQ(u.scheme, std::string("rtmps"));
	CHECK_EQ(u.host, std::string("a.rtmps.youtube.com"));
	CHECK_EQ(u.port, 443);
	CHECK_EQ(u.application(), std::string("live2"));
	CHECK(u.secure());
}

MF_TEST(url_parses_ipv6_and_default_ports)
{
	RtmpUrl u = parse_rtmp_url("rtmp://[::1]/live");
	CHECK(u.parsed);
	CHECK(u.hostIsIpv6);
	CHECK_EQ(u.host, std::string("::1"));
	CHECK_EQ(u.effectivePort(), 1935);
	CHECK_EQ(parse_rtmp_url("rtmps://host/app").effectivePort(), 443);
}

MF_TEST(url_validation_accepts_platform_and_local_urls)
{
	CHECK(!has_errors(validate_server_url("rtmps://a.rtmps.youtube.com:443/live2", Platform::YouTube)));
	CHECK(!has_errors(validate_server_url("rtmps://live-api-s.facebook.com:443/rtmp/", Platform::Facebook)));
	CHECK(!has_errors(validate_server_url("rtmp://127.0.0.1:19350/live", Platform::Custom)));
	CHECK(!has_errors(validate_server_url("rtmp://192.168.1.20/app", Platform::Custom)));
	CHECK(!has_errors(validate_server_url("rtmp://localhost:1935/live", Platform::Custom)));
	/* A local test receiver labelled as a platform is allowed, with an informational note. */
	Issues local = validate_server_url("rtmp://10.0.0.5:1935/live2", Platform::YouTube);
	CHECK(!has_errors(local));
	CHECK(has_issue(local, Severity::Info, "does not look like a YouTube"));
}

MF_TEST(url_validation_rejects_bad_input)
{
	CHECK(has_issue(validate_server_url("", Platform::Custom), Severity::Error, "required"));
	CHECK(has_issue(validate_server_url("srt://host:9000", Platform::Custom), Severity::Error, "Only rtmp"));
	CHECK(has_issue(validate_server_url("https://a.upload.youtube.com/x", Platform::YouTube), Severity::Error,
			"Only rtmp"));
	CHECK(has_issue(validate_server_url("rtmp://user:pass@host/app", Platform::Custom), Severity::Error,
			"username or password"));
	CHECK(has_issue(validate_server_url("rtmp://host:99999/app", Platform::Custom), Severity::Error, "port"));
	CHECK(has_issue(validate_server_url("rtmp://host:abc/app", Platform::Custom), Severity::Error, "port"));
	CHECK(has_issue(validate_server_url("rtmp://host", Platform::Custom), Severity::Error, "application path"));
	CHECK(has_issue(validate_server_url("rtmp://host/", Platform::Custom), Severity::Error, "application path"));
	CHECK(has_issue(validate_server_url("rtmp://ho st/app", Platform::Custom), Severity::Error, "spaces"));
	CHECK(has_issue(validate_server_url("rtmp://-bad-.com/app", Platform::Custom), Severity::Error, "host"));
	CHECK(has_issue(validate_server_url("rtmp://host/app#x", Platform::Custom), Severity::Error, "#"));
	CHECK(has_issue(validate_server_url("rtmp:/host/app", Platform::Custom), Severity::Error, "rtmp"));
}

MF_TEST(url_validation_never_downgrades_but_warns_on_plain_rtmp)
{
	Issues issues = validate_server_url("rtmp://a.rtmp.youtube.com/live2", Platform::YouTube);
	CHECK(!has_errors(issues));
	CHECK(has_issue(issues, Severity::Warning, "supports RTMPS"));
	Issues custom = validate_server_url("rtmp://a.example.com/live", Platform::Custom);
	CHECK(!has_issue(custom, Severity::Warning, "RTMPS"));
}

MF_TEST(secrets_validation_detects_key_in_url_and_whitespace)
{
	DestinationConfig d = sample_destination("A", Platform::Custom, "rtmp://host/live/abcd-1234-efgh");
	DestinationSecrets s;
	s.streamKey = "abcd-1234-efgh";
	CHECK(has_issue(validate_secrets(d, s), Severity::Error, "contains the stream key"));
	s.streamKey = "has space";
	d.serverUrl = "rtmp://host/live";
	CHECK(has_issue(validate_secrets(d, s), Severity::Error, "spaces"));
	s.streamKey = "";
	CHECK(has_issue(validate_secrets(d, s), Severity::Error, "Stream key is required"));
	s.streamKey = "good-key";
	CHECK(!has_errors(validate_secrets(d, s)));
	d.useAuth = true;
	CHECK(has_issue(validate_secrets(d, s), Severity::Error, "username"));
	s.username = "operator";
	CHECK(!has_errors(validate_secrets(d, s)));
}

MF_TEST(destination_validation_rules)
{
	DestinationConfig d =
		sample_destination("  ", Platform::Twitch, "rtmps://ingest.global-contribute.live-video.net/app");
	CHECK(has_issue(validate_destination(d), Severity::Error, "name is required"));
	d.name = "Club Twitch";
	CHECK(!has_errors(validate_destination(d)));
	d.useAuth = true;
	CHECK(has_issue(validate_destination(d), Severity::Error, "only available for custom"));
	d.useAuth = false;
	d.maxReconnectAttempts = 0;
	CHECK(has_errors(validate_destination(d)));
}

/* ---- Redaction ---- */

MF_TEST(redact_url_keeps_host_and_app_only)
{
	CHECK_EQ(redact_url("rtmps://a.rtmps.youtube.com:443/live2"),
		 std::string("rtmps://a.rtmps.youtube.com:443/live2"));
	CHECK_EQ(redact_url("rtmp://host/live/secretkey123"), std::string("rtmp://host/live/[redacted]"));
	CHECK_EQ(redact_url("rtmp://host/app?token=abc"), std::string("rtmp://host/app?[redacted]"));
	CHECK_EQ(redact_url("rtmp://u:p@host/app"), std::string("rtmp://[redacted]@host/app"));
	CHECK_EQ(redact_url("rtmps://host:443/rtmp/"), std::string("rtmps://host:443/rtmp/"));
	CHECK_EQ(redact_url("rtmp://host/a-very-long-application-segment-xyz"), std::string("rtmp://host/[redacted]"));
	CHECK_EQ(redact_url("garbage"), std::string("[unrecognised URL]"));
}

MF_TEST(sanitize_message_scrubs_secrets_and_urls)
{
	std::string key = "live_123456_SECRETKEY";
	std::string text = "Failed to publish rtmp://host/app/" + key + " with key " + key + " and pass hunter22\n";
	std::string out = sanitize_message(text, {key, "hunter22"});
	CHECK(out.find(key) == std::string::npos);
	CHECK(out.find("hunter22") == std::string::npos);
	CHECK(out.find("rtmp://host/app/[redacted]") != std::string::npos);
	CHECK(out.find('\n') == std::string::npos);
	std::string encoded = sanitize_message("k=a%2Fb%2Bc", {"a/b+c"});
	CHECK(encoded.find("a%2Fb%2Bc") == std::string::npos);
	std::string longText(1000, 'x');
	CHECK(sanitize_message(longText).size() < 320);
}

MF_TEST(secure_clear_empties_string)
{
	std::string secret = "abcdef";
	secure_clear(secret);
	CHECK(secret.empty());
}

/* ---- Bandwidth and reconnect policy ---- */

MF_TEST(bandwidth_matches_brief_example)
{
	BandwidthEstimate e = estimate_bandwidth(4, 6000, 160);
	CHECK_NEAR(e.payloadMbps, 24.64, 1e-9);
	CHECK_NEAR(e.recommendedMbps, 32.032, 1e-9);
	CHECK_EQ(estimate_bandwidth(0, 6000, 160).payloadMbps, 0.0);
	CHECK_NEAR(estimate_bandwidth(8, 6000, 160).recommendedMbps, 64.064, 1e-9);
}

MF_TEST(reconnect_schedule_is_bounded_exponential)
{
	CHECK_EQ(reconnect_delay_ms(1), 2000);
	CHECK_EQ(reconnect_delay_ms(2), 4000);
	CHECK_EQ(reconnect_delay_ms(3), 8000);
	CHECK_EQ(reconnect_delay_ms(4), 16000);
	CHECK_EQ(reconnect_delay_ms(5), 30000);
	CHECK_EQ(reconnect_delay_ms(20), 30000);
	for (int i = 1; i <= 100; i++)
		CHECK(reconnect_delay_ms(i) <= 30000);
}

MF_TEST(error_classification_is_specific_and_sanitized)
{
	std::vector<std::string> secrets = {"SECRETKEY99"};
	SessionError rejected = classify_stop(OBS_OUTPUT_INVALID_STREAM, "", false, FailurePhase::Connecting, secrets);
	CHECK(rejected.kind == ErrorKind::Rejected);
	CHECK(rejected.message.find("stream key") != std::string::npos);
	SessionError tls = classify_stop(OBS_OUTPUT_CONNECT_FAILED, "The RTMP server sent an invalid SSL certificate.",
					 true, FailurePhase::Connecting, secrets);
	CHECK(tls.kind == ErrorKind::Tls);
	CHECK(tls.message.find("invalid SSL certificate") != std::string::npos);
	SessionError net = classify_stop(OBS_OUTPUT_CONNECT_FAILED, "bad SECRETKEY99 rtmp://h/app/SECRETKEY99", false,
					 FailurePhase::Connecting, secrets);
	CHECK(net.kind == ErrorKind::Network);
	CHECK(net.message.find("SECRETKEY99") == std::string::npos);
	CHECK(classify_stop(OBS_OUTPUT_ENCODE_ERROR, "", false, FailurePhase::Established, {}).kind ==
	      ErrorKind::Encoder);
	CHECK(!code_is_retryable(OBS_OUTPUT_CONNECT_FAILED, FailurePhase::Connecting));
	CHECK(code_is_retryable(OBS_OUTPUT_DISCONNECTED, FailurePhase::Established));
	CHECK(code_is_retryable(OBS_OUTPUT_CONNECT_FAILED, FailurePhase::Reconnecting));
	CHECK(!code_is_retryable(OBS_OUTPUT_ENCODE_ERROR, FailurePhase::Established));
	CHECK(!code_is_retryable(OBS_OUTPUT_BAD_PATH, FailurePhase::Reconnecting));
}

/* ---- Configuration ---- */

MF_TEST(config_roundtrip_contains_no_secrets)
{
	ProfileConfig config;
	config.profileId = new_uuid();
	config.encoder.videoBitrateKbps = 4500;
	config.encoder.audioTrack = 3;
	DestinationConfig d =
		sample_destination("KGB YouTube", Platform::YouTube, "rtmps://a.rtmps.youtube.com:443/live2");
	d.credentials.streamKey = credential_target(config.profileId, d.id, SecretField::StreamKey);
	config.destinations.push_back(d);
	config.destinations.push_back(
		sample_destination("Club FB", Platform::Facebook, "rtmps://live-api-s.facebook.com:443/rtmp/"));

	std::string json = serialize_config(config);
	CHECK(json.find("schemaVersion") != std::string::npos);
	CHECK(json.find("MatchFrameMultistream/v1/") != std::string::npos); // a reference, not a secret

	ParseResult parsed = parse_config(json);
	REQUIRE(parsed.ok);
	CHECK_EQ(parsed.config.profileId, config.profileId);
	CHECK(parsed.config.encoder == config.encoder);
	REQUIRE(parsed.config.destinations.size() == 2);
	CHECK(parsed.config.destinations[0] == config.destinations[0]);
	CHECK(parsed.config.destinations[1].platform == Platform::Facebook);
}

MF_TEST(config_rejects_newer_and_invalid_schema)
{
	CHECK(!parse_config("not json").ok);
	CHECK(!parse_config("{\"schema\":\"other\",\"schemaVersion\":1}").ok);
	ParseResult newer = parse_config(std::string("{\"schema\":\"") + kConfigSchemaId + "\",\"schemaVersion\":99}");
	CHECK(!newer.ok);
	CHECK(newer.newerSchema);
	CHECK(!parse_config(std::string("{\"schema\":\"") + kConfigSchemaId + "\",\"schemaVersion\":0}").ok);
}

MF_TEST(config_duplicate_ids_get_new_identity_without_credentials)
{
	ProfileConfig config;
	config.profileId = new_uuid();
	DestinationConfig d = sample_destination("A", Platform::Custom, "rtmp://h/app");
	d.credentials.streamKey = "ref";
	config.destinations.push_back(d);
	config.destinations.push_back(d);
	ParseResult parsed = parse_config(serialize_config(config));
	REQUIRE(parsed.ok);
	REQUIRE(parsed.config.destinations.size() == 2);
	CHECK(parsed.config.destinations[0].id != parsed.config.destinations[1].id);
	CHECK(parsed.config.destinations[1].credentials.streamKey.empty());
	CHECK(!parsed.warnings.empty());
}

MF_TEST(repository_save_load_backup_and_corruption)
{
	std::string dir = temp_dir("repo");
	ConfigRepository repo(dir + "/profiles");
	auto first = repo.load("Untitled");
	CHECK(!first.existed);
	CHECK(!first.config.profileId.empty());

	ProfileConfig config = first.config;
	config.destinations.push_back(sample_destination("A", Platform::Custom, "rtmp://h/app"));
	std::string error;
	REQUIRE(repo.save("Untitled", config, error));
	config.destinations.push_back(sample_destination("B", Platform::Custom, "rtmp://h/app"));
	REQUIRE(repo.save("Untitled", config, error));

	auto loaded = repo.load("Untitled");
	CHECK(loaded.existed);
	CHECK_EQ(loaded.config.destinations.size(), (size_t)2);
	CHECK(std::filesystem::exists(repo.pathFor("Untitled") + ".bak"));

	/* Corrupt the main file: the backup (one destination) is restored, with a warning. */
	{
		std::ofstream f(repo.pathFor("Untitled"), std::ios::trunc);
		f << "{ broken";
	}
	auto restored = repo.load("Untitled");
	CHECK_EQ(restored.config.destinations.size(), (size_t)1);
	CHECK(!restored.warnings.empty());

	/* Corrupt both: the unreadable file is moved aside, not overwritten. */
	{
		std::ofstream f(repo.pathFor("Untitled"), std::ios::trunc);
		f << "{ broken";
		std::ofstream b(repo.pathFor("Untitled") + ".bak", std::ios::trunc);
		b << "also broken";
	}
	auto empty = repo.load("Untitled");
	CHECK(empty.config.destinations.empty());
	CHECK(!empty.readOnly);
	bool asideFound = false;
	for (auto &entry : std::filesystem::directory_iterator(dir + "/profiles"))
		asideFound = asideFound || entry.path().string().find(".unreadable-") != std::string::npos;
	CHECK(asideFound);

	/* A newer schema is loaded read-only and never overwritten. */
	{
		std::ofstream f(repo.pathFor("Newer"), std::ios::trunc);
		f << "{\"schema\":\"" << kConfigSchemaId << "\",\"schemaVersion\":42}";
	}
	auto newer = repo.load("Newer");
	CHECK(newer.readOnly);

	std::filesystem::remove_all(dir);
}

MF_TEST(repository_move_for_profile_rename)
{
	std::string dir = temp_dir("move");
	ConfigRepository repo(dir);
	ProfileConfig config;
	config.profileId = new_uuid();
	std::string error;
	REQUIRE(repo.save("Old", config, error));
	REQUIRE(repo.move("Old", "New", error));
	CHECK(!repo.exists("Old"));
	CHECK(repo.exists("New"));
	CHECK_EQ(repo.load("New").config.profileId, config.profileId);
	REQUIRE(repo.save("Old", config, error));
	CHECK(!repo.move("Old", "New", error));                                               // never overwrites
	CHECK_EQ(ConfigRepository::sanitize_key("../evil/..\\x"), std::string("_evil_.._x")); // no separators survive
	std::filesystem::remove_all(dir);
}

MF_TEST(export_import_metadata_only_and_all_or_nothing)
{
	ProfileConfig config;
	config.profileId = new_uuid();
	DestinationConfig d =
		sample_destination("Home club", Platform::YouTube, "rtmps://a.rtmps.youtube.com:443/live2");
	d.useAuth = false;
	d.credentials.streamKey = credential_target(config.profileId, d.id, SecretField::StreamKey);
	config.destinations.push_back(d);
	DestinationConfig c = sample_destination("Custom", Platform::Custom, "rtmp://10.1.1.1/live");
	c.useAuth = true;
	c.credentials.username = "ref-user";
	config.destinations.push_back(c);

	std::string exported = serialize_export(config);
	CHECK(exported.find("MatchFrameMultistream/v1/") == std::string::npos);
	CHECK(exported.find("credentialRefs") == std::string::npos);
	CHECK(exported.find(d.id) == std::string::npos);
	CHECK(exported.find(config.profileId) == std::string::npos);

	ImportResult imported = parse_import(exported);
	REQUIRE(imported.ok);
	REQUIRE(imported.destinations.size() == 2);
	CHECK(imported.destinations[0].id != d.id);
	CHECK(imported.destinations[0].credentials == CredentialRefs{});
	CHECK(imported.destinations[1].useAuth);
	CHECK(imported.hasEncoder);

	std::string bad = exported;
	size_t at = bad.find("rtmp://10.1.1.1/live");
	REQUIRE(at != std::string::npos);
	bad.replace(at, std::strlen("rtmp://10.1.1.1/live"), "srt://10.1.1.1:9000");
	ImportResult rejected = parse_import(bad);
	CHECK(!rejected.ok);
	CHECK(rejected.destinations.empty());
	CHECK(!rejected.problems.empty());
	CHECK(!parse_import("[]").ok);
	CHECK(!parse_import("{\"schema\":\"matchframe-multistream.export\",\"schemaVersion\":7,\"destinations\":[]}")
		       .ok);
}

MF_TEST(encoder_profile_validation)
{
	EncoderProfile p;
	CHECK(!has_errors(validate_encoder_profile(p)));
	CHECK_EQ(p.mixerIndex(), (size_t)0);
	p.audioTrack = 6;
	CHECK_EQ(p.mixerIndex(), (size_t)5);
	p.audioTrack = 7;
	CHECK(has_errors(validate_encoder_profile(p)));
	p.audioTrack = 1;
	p.videoBitrateKbps = 100;
	CHECK(has_errors(validate_encoder_profile(p)));
	p.videoBitrateKbps = 6000;
	p.audioBitrateKbps = 150;
	CHECK(has_errors(validate_encoder_profile(p)));
	p.audioBitrateKbps = 160;
	p.keyframeIntervalSec = 4;
	CHECK(has_issue(validate_encoder_profile(p), Severity::Warning, "2 second"));
}

/* ---- Credentials ---- */

MF_TEST(vault_persists_in_secure_store_and_never_in_config)
{
	MemoryCredentialStore persistent;
	SecretVault vault(&persistent);
	auto result = vault.store("P", "D", SecretField::StreamKey, "key-1", false);
	CHECK(result.location == SecretLocation::Persistent);
	CHECK_EQ(result.target, credential_target("P", "D", SecretField::StreamKey));
	std::string value;
	CHECK(vault.load("P", "D", SecretField::StreamKey, result.target, value).ok());
	CHECK_EQ(value, std::string("key-1"));
	vault.remove("P", "D", SecretField::StreamKey, result.target);
	CHECK(!vault.load("P", "D", SecretField::StreamKey, result.target, value).ok());
	CHECK_EQ(persistent.size(), (size_t)0);
}

MF_TEST(vault_falls_back_to_session_only_with_consent)
{
	MemoryCredentialStore persistent;
	persistent.setFailWrites(true);
	SecretVault vault(&persistent);
	auto refused = vault.store("P", "D", SecretField::StreamKey, "key-2", false);
	CHECK(refused.location == SecretLocation::None);
	CHECK(!refused.error.empty());
	std::string value;
	CHECK(!vault.load("P", "D", SecretField::StreamKey, "", value).ok());

	auto session = vault.store("P", "D", SecretField::StreamKey, "key-2", true);
	CHECK(session.location == SecretLocation::SessionOnly);
	CHECK(session.target.empty()); // nothing to persist in the config
	SecretLocation where;
	CHECK(vault.load("P", "D", SecretField::StreamKey, "", value, &where).ok());
	CHECK(where == SecretLocation::SessionOnly);
	CHECK_EQ(value, std::string("key-2"));
	CHECK_EQ(persistent.size(), (size_t)0);
}

MF_TEST(vault_reports_unavailable_store)
{
	UnavailableCredentialStore none("not here");
	SecretVault vault(&none);
	CHECK(!vault.persistentAvailable());
	auto result = vault.store("P", "D", SecretField::Password, "pw", true);
	CHECK(result.location == SecretLocation::SessionOnly);
	CHECK_EQ(result.error, std::string("not here"));

	DestinationConfig config;
	config.id = "D";
	DestinationSecrets secrets;
	CredResult missing = vault.loadSecrets("P", config, secrets);
	CHECK(!missing.ok());
	CHECK(missing.message.find("No stream key") != std::string::npos);
}
