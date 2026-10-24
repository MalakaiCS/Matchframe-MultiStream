// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "destination-config.hpp"
#include "redaction.hpp"
#include "rtmp-url.hpp"

#include <algorithm>
#include <cctype>

namespace mf {

namespace {

bool has_space_or_control(const std::string &text)
{
	return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7F; });
}

/* Spaces, control characters and every non-ASCII byte, e.g. a no-break or zero-width space pasted
 * along with a URL or key from a web page; servers refuse those and the cause is invisible. */
bool has_non_printable_ascii(const std::string &text)
{
	return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7F; });
}

bool ends_with(const std::string &value, const std::string &suffix)
{
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool host_matches_hint(const std::string &host, const std::string &hint)
{
	return host == hint || ends_with(host, "." + hint);
}

} // namespace

const char *secret_field_id(SecretField field)
{
	switch (field) {
	case SecretField::StreamKey:
		return "streamKey";
	case SecretField::Username:
		return "username";
	case SecretField::Password:
		return "password";
	}
	return "unknown";
}

std::string &CredentialRefs::ref(SecretField field)
{
	switch (field) {
	case SecretField::Username:
		return username;
	case SecretField::Password:
		return password;
	case SecretField::StreamKey:
	default:
		return streamKey;
	}
}

const std::string &CredentialRefs::ref(SecretField field) const
{
	return const_cast<CredentialRefs *>(this)->ref(field);
}

bool DestinationConfig::sameTransportSettings(const DestinationConfig &other) const
{
	return id == other.id && platform == other.platform && serverUrl == other.serverUrl &&
	       enabled == other.enabled && autoReconnect == other.autoReconnect &&
	       maxReconnectAttempts == other.maxReconnectAttempts && useAuth == other.useAuth &&
	       credentials == other.credentials;
}

DestinationSecrets::~DestinationSecrets()
{
	clear();
}

void DestinationSecrets::clear()
{
	secure_clear(streamKey);
	secure_clear(username);
	secure_clear(password);
}

std::vector<std::string> DestinationSecrets::values() const
{
	std::vector<std::string> out;
	for (const std::string *value : {&streamKey, &username, &password}) {
		if (!value->empty())
			out.push_back(*value);
	}
	return out;
}

std::string trim(const std::string &text)
{
	size_t begin = 0;
	size_t end = text.size();
	while (begin < end && std::isspace((unsigned char)text[begin]))
		begin++;
	while (end > begin && std::isspace((unsigned char)text[end - 1]))
		end--;
	return text.substr(begin, end - begin);
}

Issues validate_server_url(const std::string &url, Platform platform)
{
	Issues issues;
	auto error = [&](const std::string &message) {
		issues.push_back({Severity::Error, "serverUrl", message});
	};

	if (url.empty()) {
		error("Server URL is required.");
		return issues;
	}
	if (url.size() > kMaxUrlLength) {
		error("Server URL is too long.");
		return issues;
	}
	if (has_non_printable_ascii(url)) {
		error("Server URL must not contain spaces or invisible or non-ASCII characters; copy it again from "
		      "the platform's dashboard.");
		return issues;
	}
	if (url.find_first_of("{}") != std::string::npos) {
		error("Remove the {stream_key} placeholder from the server URL and enter the key in the Stream key "
		      "field.");
		return issues;
	}

	RtmpUrl parsed = parse_rtmp_url(url);
	if (!parsed.parsed) {
		error("Server URL must start with rtmp:// or rtmps://.");
		return issues;
	}
	if (!parsed.isRtmpFamily()) {
		error("Only rtmp:// and rtmps:// server URLs are supported (found " + parsed.scheme + "://).");
		return issues;
	}
	if (!parsed.userInfo.empty()) {
		error("Do not put a username or password in the server URL; use the server authentication fields.");
		return issues;
	}
	if (!is_valid_host(parsed.host, parsed.hostIsIpv6)) {
		error("Server URL has a missing or invalid host name.");
		return issues;
	}
	if (parsed.portPresent && !parsed.portValid) {
		error("Server URL has an invalid port (use 1-65535).");
		return issues;
	}
	if (!parsed.fragment.empty()) {
		error("Server URL must not contain '#'.");
		return issues;
	}

	const PlatformInfo &info = platform_info(platform);
	if (parsed.application().empty()) {
		/* Every named platform uses an application path; some custom servers do not need one. */
		if (platform != Platform::Custom) {
			error("Server URL must include the RTMP application path, for example rtmps://host:443/app.");
			return issues;
		}
		issues.push_back({Severity::Warning, "serverUrl",
				  "The server URL has no RTMP application path (such as /app or /live2). Most servers "
				  "need one; check the URL your service gives you."});
	}
	if (!parsed.secure() && info.requiresRtmps) {
		error(std::string(info.label) +
		      " accepts only RTMPS. Use the rtmps:// server URL from its dashboard (port 443).");
		return issues;
	}
	if (!parsed.secure() && info.suggestRtmps) {
		issues.push_back(
			{Severity::Warning, "serverUrl",
			 std::string(info.label) +
				 " supports RTMPS. Prefer the rtmps:// ingest URL from your dashboard so the "
				 "stream key is encrypted in transit. The plugin never changes the scheme for you."});
	}
	if (public_query(parsed.query) != parsed.query) {
		issues.push_back({Severity::Warning, "serverUrl",
				  "The URL has query parameters. The plugin hides them in its own messages and "
				  "diagnostics and leaves them out of exports, but OBS's RTMP output writes the full "
				  "server URL to the OBS log, so do not put secrets in it."});
	}
	if (!info.hostHints.empty()) {
		bool matches = std::any_of(info.hostHints.begin(), info.hostHints.end(), [&](const std::string &hint) {
			return host_matches_hint(parsed.host, hint);
		});
		if (!matches) {
			issues.push_back(
				{Severity::Info, "serverUrl",
				 std::string("This host does not look like a ") + info.label +
					 " ingest server. That is fine for a test receiver; otherwise check the URL."});
		}
	}
	return issues;
}

Issues validate_destination(const DestinationConfig &config)
{
	Issues issues;
	std::string name = trim(config.name);
	if (name.empty())
		issues.push_back({Severity::Error, "name", "Display name is required."});
	else if (name.size() > kMaxNameLength)
		issues.push_back({Severity::Error, "name", "Display name is too long (80 characters maximum)."});

	append(issues, validate_server_url(config.serverUrl, config.platform));

	if (config.maxReconnectAttempts < 1 || config.maxReconnectAttempts > kMaxReconnectAttemptsLimit)
		issues.push_back(
			{Severity::Error, "maxReconnectAttempts", "Reconnect attempts must be between 1 and 100."});

	if (config.useAuth && config.platform != Platform::Custom)
		issues.push_back({Severity::Error, "useAuth",
				  "Server username/password is only available for custom destinations."});
	return issues;
}

Issues validate_secrets(const DestinationConfig &config, const DestinationSecrets &secrets)
{
	Issues issues;
	auto check_secret = [&](const std::string &value, const char *field, const char *label, bool required) {
		if (value.empty()) {
			if (required)
				issues.push_back({Severity::Error, field, std::string(label) + " is required."});
			return;
		}
		if (value.size() > kMaxSecretLength)
			issues.push_back({Severity::Error, field, std::string(label) + " is too long."});
		else if (has_space_or_control(value))
			issues.push_back({Severity::Error, field,
					  std::string(label) + " must not contain spaces or control characters."});
		else if (std::string(field) == "streamKey" && has_non_printable_ascii(value))
			issues.push_back({Severity::Error, field,
					  "Stream key contains an invisible or non-ASCII character; copy it again from "
					  "the platform's dashboard."});
	};

	check_secret(secrets.streamKey, "streamKey", "Stream key", true);
	if (config.useAuth) {
		check_secret(secrets.username, "username", "Server username", true);
		check_secret(secrets.password, "password", "Server password", false);
	}

	if (secrets.streamKey.size() >= 6 && config.serverUrl.find(secrets.streamKey) != std::string::npos) {
		issues.push_back({Severity::Error, "serverUrl",
				  "The server URL contains the stream key. Enter the key only in the Stream key field; "
				  "OBS's RTMP output writes the server URL to the OBS log."});
	}
	return issues;
}

} // namespace mf
