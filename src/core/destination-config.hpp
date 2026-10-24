// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "platform.hpp"
#include "validation.hpp"

#include <string>

namespace mf {

inline constexpr int kDefaultMaxReconnectAttempts = 20;
inline constexpr int kMaxReconnectAttemptsLimit = 100;
inline constexpr size_t kMaxNameLength = 80;
inline constexpr size_t kMaxUrlLength = 1024;
inline constexpr size_t kMaxSecretLength = 1024;

enum class SecretField { StreamKey, Username, Password };
const char *secret_field_id(SecretField field);

/*
 * Persistent credential references. Each is the secure store's entry name holding the secret
 * (Windows Credential Manager target, macOS Keychain account), or empty when nothing is
 * persisted for that field.
 * The secret values themselves never appear in configuration files.
 */
struct CredentialRefs {
	std::string streamKey;
	std::string username;
	std::string password;

	std::string &ref(SecretField field);
	const std::string &ref(SecretField field) const;
	bool operator==(const CredentialRefs &other) const = default;
};

struct DestinationConfig {
	std::string id; // stable UUID
	std::string name;
	Platform platform = Platform::Custom;
	std::string serverUrl;
	bool enabled = true;
	bool autoReconnect = true;
	int maxReconnectAttempts = kDefaultMaxReconnectAttempts;
	bool useAuth = false; // custom destinations only: server username/password
	CredentialRefs credentials;

	bool operator==(const DestinationConfig &other) const = default;
	/* Fields that are locked while the destination is live (everything except the display name). */
	bool sameTransportSettings(const DestinationConfig &other) const;
};

/* Secret values held in memory only while needed. Cleared on destruction. */
struct DestinationSecrets {
	std::string streamKey;
	std::string username;
	std::string password;

	DestinationSecrets() = default;
	DestinationSecrets(const DestinationSecrets &) = default;
	DestinationSecrets &operator=(const DestinationSecrets &) = default;
	~DestinationSecrets();
	void clear();
	std::vector<std::string> values() const; // for scrubbing backend messages
};

std::string trim(const std::string &text);

/* Field-level validation of everything stored in configuration (no secrets needed). */
Issues validate_destination(const DestinationConfig &config);
Issues validate_server_url(const std::string &url, Platform platform);
/* Secret validation; streamKey is also checked against the server URL so it is not pasted twice. */
Issues validate_secrets(const DestinationConfig &config, const DestinationSecrets &secrets);

} // namespace mf
