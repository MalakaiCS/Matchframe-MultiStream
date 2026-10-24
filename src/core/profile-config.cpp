// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "profile-config.hpp"

#include <obs.hpp>
#include <util/platform.h>

#include <functional>
#include <set>

namespace mf {

namespace {

/*
 * Migration chain: entry i upgrades a document from schema version i+1 to i+2 in place.
 * Version 1 is the first released schema, so the chain is empty; add a step here (and a
 * unit test with a fixture of the old format) whenever kConfigSchemaVersion is raised.
 */
using Migration = std::function<bool(obs_data_t *root, std::string &error)>;
const std::vector<Migration> &migrations()
{
	static const std::vector<Migration> chain;
	return chain;
}

bool migrate(obs_data_t *root, int fromVersion, std::string &error)
{
	for (int version = fromVersion; version < kConfigSchemaVersion; version++) {
		size_t index = (size_t)(version - 1);
		if (index >= migrations().size() || !migrations()[index](root, error)) {
			if (error.empty())
				error = "No migration from schema version " + std::to_string(version);
			return false;
		}
		obs_data_set_int(root, "schemaVersion", version + 1);
	}
	return true;
}

std::string get_string(obs_data_t *data, const char *name)
{
	const char *value = obs_data_get_string(data, name);
	return value ? value : "";
}

void write_encoder(obs_data_t *root, const EncoderProfile &encoder)
{
	OBSDataAutoRelease enc = obs_data_create();
	obs_data_set_string(enc, "videoEncoder", encoder.videoEncoderId.c_str());
	obs_data_set_int(enc, "videoBitrateKbps", encoder.videoBitrateKbps);
	obs_data_set_int(enc, "keyframeIntervalSec", encoder.keyframeIntervalSec);
	obs_data_set_string(enc, "rateControl", "CBR");
	obs_data_set_string(enc, "audioEncoder", encoder.audioEncoderId.c_str());
	obs_data_set_int(enc, "audioBitrateKbps", encoder.audioBitrateKbps);
	obs_data_set_int(enc, "audioTrack", encoder.audioTrack);
	obs_data_set_obj(root, "encoding", enc);
}

EncoderProfile read_encoder(obs_data_t *root, bool &present)
{
	EncoderProfile encoder;
	OBSDataAutoRelease enc = obs_data_get_obj(root, "encoding");
	present = enc != nullptr;
	if (!enc)
		return encoder;
	encoder.videoEncoderId = get_string(enc, "videoEncoder");
	if (obs_data_has_user_value(enc, "videoBitrateKbps"))
		encoder.videoBitrateKbps = (int)obs_data_get_int(enc, "videoBitrateKbps");
	if (obs_data_has_user_value(enc, "keyframeIntervalSec"))
		encoder.keyframeIntervalSec = (int)obs_data_get_int(enc, "keyframeIntervalSec");
	if (obs_data_has_user_value(enc, "audioEncoder"))
		encoder.audioEncoderId = get_string(enc, "audioEncoder");
	if (obs_data_has_user_value(enc, "audioBitrateKbps"))
		encoder.audioBitrateKbps = (int)obs_data_get_int(enc, "audioBitrateKbps");
	if (obs_data_has_user_value(enc, "audioTrack"))
		encoder.audioTrack = (int)obs_data_get_int(enc, "audioTrack");
	return encoder;
}

OBSDataAutoRelease write_destination(const DestinationConfig &dest, bool includePrivate)
{
	OBSDataAutoRelease item = obs_data_create();
	if (includePrivate)
		obs_data_set_string(item, "id", dest.id.c_str());
	obs_data_set_string(item, "name", dest.name.c_str());
	obs_data_set_string(item, "platform", platform_id(dest.platform));
	obs_data_set_string(item, "serverUrl", dest.serverUrl.c_str());
	obs_data_set_bool(item, "enabled", dest.enabled);
	obs_data_set_bool(item, "autoReconnect", dest.autoReconnect);
	obs_data_set_int(item, "maxReconnectAttempts", dest.maxReconnectAttempts);
	obs_data_set_bool(item, "useAuth", dest.useAuth);
	if (includePrivate) {
		OBSDataAutoRelease creds = obs_data_create();
		for (SecretField field : {SecretField::StreamKey, SecretField::Username, SecretField::Password})
			obs_data_set_string(creds, secret_field_id(field), dest.credentials.ref(field).c_str());
		obs_data_set_obj(item, "credentialRefs", creds);
	}
	return item;
}

DestinationConfig read_destination(obs_data_t *item, bool readPrivate, std::vector<std::string> &warnings)
{
	DestinationConfig dest;
	if (readPrivate)
		dest.id = get_string(item, "id");
	dest.name = get_string(item, "name");
	std::string platform = get_string(item, "platform");
	if (!platform_from_id(platform, dest.platform)) {
		dest.platform = Platform::Custom;
		warnings.push_back("Unknown platform '" + platform + "' for '" + dest.name + "'; treated as custom.");
	}
	dest.serverUrl = get_string(item, "serverUrl");
	dest.enabled = obs_data_has_user_value(item, "enabled") ? obs_data_get_bool(item, "enabled") : true;
	dest.autoReconnect = obs_data_has_user_value(item, "autoReconnect") ? obs_data_get_bool(item, "autoReconnect")
									    : true;
	dest.maxReconnectAttempts = obs_data_has_user_value(item, "maxReconnectAttempts")
					    ? (int)obs_data_get_int(item, "maxReconnectAttempts")
					    : kDefaultMaxReconnectAttempts;
	dest.useAuth = obs_data_get_bool(item, "useAuth");
	if (readPrivate) {
		OBSDataAutoRelease creds = obs_data_get_obj(item, "credentialRefs");
		if (creds) {
			for (SecretField field : {SecretField::StreamKey, SecretField::Username, SecretField::Password})
				dest.credentials.ref(field) = get_string(creds, secret_field_id(field));
		}
	}
	return dest;
}

} // namespace

const DestinationConfig *ProfileConfig::find(const std::string &id) const
{
	for (const DestinationConfig &dest : destinations) {
		if (dest.id == id)
			return &dest;
	}
	return nullptr;
}

DestinationConfig *ProfileConfig::find(const std::string &id)
{
	return const_cast<DestinationConfig *>(static_cast<const ProfileConfig *>(this)->find(id));
}

std::string new_uuid()
{
	char *uuid = os_generate_uuid();
	std::string out = uuid ? uuid : "";
	bfree(uuid);
	return out;
}

std::string serialize_config(const ProfileConfig &config)
{
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, "schema", kConfigSchemaId);
	obs_data_set_int(root, "schemaVersion", kConfigSchemaVersion);
	obs_data_set_string(root, "profileId", config.profileId.c_str());
	write_encoder(root, config.encoder);

	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const DestinationConfig &dest : config.destinations) {
		OBSDataAutoRelease item = write_destination(dest, true);
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(root, "destinations", array);

	const char *json = obs_data_get_json_pretty(root);
	return json ? json : "";
}

ParseResult parse_config(const std::string &json)
{
	ParseResult result;
	OBSDataAutoRelease root = obs_data_create_from_json(json.c_str());
	if (!root) {
		result.error = "Configuration is not valid JSON.";
		return result;
	}
	if (get_string(root, "schema") != kConfigSchemaId) {
		result.error = "Not a MatchFrame Multistream configuration file.";
		return result;
	}
	int version = (int)obs_data_get_int(root, "schemaVersion");
	if (version < 1) {
		result.error = "Configuration has an invalid schema version.";
		return result;
	}
	if (version > kConfigSchemaVersion) {
		result.newerSchema = true;
		result.error = "Configuration was written by a newer version of MatchFrame Multistream (schema " +
			       std::to_string(version) + ").";
		return result;
	}
	if (!migrate(root, version, result.error))
		return result;

	ProfileConfig &config = result.config;
	config.profileId = get_string(root, "profileId");
	if (config.profileId.empty())
		config.profileId = new_uuid();
	bool encoderPresent = false;
	config.encoder = read_encoder(root, encoderPresent);

	std::set<std::string> seen;
	OBSDataArrayAutoRelease array = obs_data_get_array(root, "destinations");
	size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		DestinationConfig dest = read_destination(item, true, result.warnings);
		if (dest.id.empty() || seen.count(dest.id)) {
			/* A new identity cannot inherit stored credentials. */
			dest.id = new_uuid();
			dest.credentials = {};
			result.warnings.push_back(
				"Destination '" + dest.name +
				"' had a missing or duplicate id; its credentials must be re-entered.");
		}
		seen.insert(dest.id);
		config.destinations.push_back(std::move(dest));
	}
	result.ok = true;
	return result;
}

std::string serialize_export(const ProfileConfig &config)
{
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, "schema", kExportSchemaId);
	obs_data_set_int(root, "schemaVersion", kConfigSchemaVersion);
	obs_data_set_string(
		root, "note",
		"Metadata only. Stream keys and server credentials are never exported and must be re-entered.");
	write_encoder(root, config.encoder);

	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const DestinationConfig &dest : config.destinations) {
		OBSDataAutoRelease item = write_destination(dest, false);
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(root, "destinations", array);

	const char *json = obs_data_get_json_pretty(root);
	return json ? json : "";
}

ImportResult parse_import(const std::string &json)
{
	ImportResult result;
	OBSDataAutoRelease root = obs_data_create_from_json(json.c_str());
	if (!root) {
		result.problems.push_back("The file is not valid JSON.");
		return result;
	}
	std::string schema = get_string(root, "schema");
	if (schema != kExportSchemaId && schema != kConfigSchemaId) {
		result.problems.push_back("The file is not a MatchFrame Multistream export.");
		return result;
	}
	int version = (int)obs_data_get_int(root, "schemaVersion");
	if (version < 1 || version > kConfigSchemaVersion) {
		result.problems.push_back("Unsupported export schema version " + std::to_string(version) + ".");
		return result;
	}
	std::string migrateError;
	if (!migrate(root, version, migrateError)) {
		result.problems.push_back(migrateError);
		return result;
	}

	result.encoder = read_encoder(root, result.hasEncoder);
	if (result.hasEncoder) {
		for (const Issue &issue : validate_encoder_profile(result.encoder)) {
			if (issue.severity == Severity::Error)
				result.problems.push_back("Encoding settings: " + issue.message);
		}
	}

	OBSDataArrayAutoRelease array = obs_data_get_array(root, "destinations");
	if (!array) {
		result.problems.push_back("The file has no destination list.");
		return result;
	}
	size_t count = obs_data_array_count(array);
	if (count > kMaxDestinations) {
		result.problems.push_back("The file has more than 32 destinations.");
		return result;
	}

	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		std::string platform = get_string(item, "platform");
		Platform parsedPlatform;
		DestinationConfig dest = read_destination(item, false, result.warnings);
		std::string label = dest.name.empty() ? "Destination " + std::to_string(i + 1) : "'" + dest.name + "'";
		if (!platform_from_id(platform, parsedPlatform))
			result.problems.push_back(label + ": unknown platform '" + platform + "'.");
		dest.id = new_uuid();
		dest.credentials = {};
		dest.name = trim(dest.name);
		for (const Issue &issue : validate_destination(dest)) {
			if (issue.severity == Severity::Error)
				result.problems.push_back(label + ": " + issue.message);
		}
		result.destinations.push_back(std::move(dest));
	}

	result.ok = result.problems.empty();
	if (!result.ok)
		result.destinations.clear();
	return result;
}

} // namespace mf
