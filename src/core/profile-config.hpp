// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "destination-config.hpp"
#include "encoder-profile.hpp"

#include <string>
#include <vector>

namespace mf {

inline constexpr int kConfigSchemaVersion = 1;
inline constexpr const char *kConfigSchemaId = "matchframe-multistream.config";
inline constexpr const char *kExportSchemaId = "matchframe-multistream.export";
/* Saved destinations per profile; at most kMaxSimultaneousOutputs of them may run at once. */
inline constexpr size_t kMaxDestinations = 32;
inline constexpr int kMaxSimultaneousOutputs = 8;

struct ProfileConfig {
	int schemaVersion = kConfigSchemaVersion;
	/* Random UUID created with the configuration. It scopes credential names and survives an
	 * OBS profile rename (the file is moved), so a duplicated OBS profile never shares keys. */
	std::string profileId;
	EncoderProfile encoder;
	std::vector<DestinationConfig> destinations;

	const DestinationConfig *find(const std::string &id) const;
	DestinationConfig *find(const std::string &id);
};

struct ParseResult {
	bool ok = false;
	ProfileConfig config;
	bool newerSchema = false; // written by a newer plugin; must not be overwritten
	std::vector<std::string> warnings;
	std::string error;
};

std::string new_uuid();

std::string serialize_config(const ProfileConfig &config);
ParseResult parse_config(const std::string &json);

/* Export contains metadata only: no credential references, no usernames, no ids. */
std::string serialize_export(const ProfileConfig &config);

struct ImportResult {
	bool ok = false;
	EncoderProfile encoder;
	bool hasEncoder = false;
	std::vector<DestinationConfig> destinations; // fresh ids, no credentials
	std::vector<std::string> problems;           // reasons the file was rejected
	std::vector<std::string> warnings;
};

/* All-or-nothing: any schema, URL or field error rejects the whole file. */
ImportResult parse_import(const std::string &json);

} // namespace mf
