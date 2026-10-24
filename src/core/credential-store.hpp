// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "destination-config.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace mf {

enum class CredStatus { Ok, NotFound, Unavailable, Failed };

struct CredResult {
	CredStatus status = CredStatus::Failed;
	std::string message; // sanitized, operator-facing
	bool ok() const { return status == CredStatus::Ok; }
};

/* Secure persistent secret storage (Windows Credential Manager, macOS Keychain). */
class CredentialStore {
public:
	virtual ~CredentialStore() = default;
	virtual bool available() const = 0;
	virtual std::string backendName() const = 0;
	virtual std::string unavailableReason() const { return {}; }
	virtual CredResult write(const std::string &target, const std::string &secret) = 0;
	virtual CredResult read(const std::string &target, std::string &secret) = 0;
	virtual CredResult remove(const std::string &target) = 0;
};

/* In-memory store: used for session-only secrets and as a test double. Never touches disk. */
class MemoryCredentialStore : public CredentialStore {
public:
	~MemoryCredentialStore() override;
	bool available() const override { return true; }
	std::string backendName() const override { return "session memory"; }
	CredResult write(const std::string &target, const std::string &secret) override;
	CredResult read(const std::string &target, std::string &secret) override;
	CredResult remove(const std::string &target) override;
	size_t size() const;
	void setFailWrites(bool fail) { failWrites_ = fail; } // test hook: simulate a failing secure store

private:
	mutable std::mutex mutex_;
	std::map<std::string, std::string> values_;
	bool failWrites_ = false;
};

/* Placeholder on platforms without a secure store implementation (phase two). */
class UnavailableCredentialStore : public CredentialStore {
public:
	explicit UnavailableCredentialStore(std::string reason) : reason_(std::move(reason)) {}
	bool available() const override { return false; }
	std::string backendName() const override { return "none"; }
	std::string unavailableReason() const override { return reason_; }
	CredResult write(const std::string &, const std::string &) override
	{
		return {CredStatus::Unavailable, reason_};
	}
	CredResult read(const std::string &, std::string &) override { return {CredStatus::Unavailable, reason_}; }
	CredResult remove(const std::string &) override { return {CredStatus::Unavailable, reason_}; }

private:
	std::string reason_;
};

/* Windows: Credential Manager. macOS: login Keychain. Elsewhere: UnavailableCredentialStore. */
std::unique_ptr<CredentialStore> create_platform_credential_store();

/* "MatchFrameMultistream/v1/<profileId>/<destinationId>/<field>" */
std::string credential_target(const std::string &profileId, const std::string &destinationId, SecretField field);
inline constexpr const char *kCredentialTargetPrefix = "MatchFrameMultistream/v1/";

enum class SecretLocation { None, Persistent, SessionOnly };

/*
 * Where secrets live: the secure persistent store when it works, otherwise (only with the
 * operator's consent) process memory for this OBS session. Plaintext is never written.
 */
class SecretVault {
public:
	explicit SecretVault(CredentialStore *persistent);

	bool persistentAvailable() const;
	std::string persistentName() const;
	std::string persistentUnavailableReason() const;

	struct StoreResult {
		SecretLocation location = SecretLocation::None;
		std::string target; // persistent target name (empty when session-only)
		std::string error;  // why persistent storage failed
	};

	/* Persists the value; on failure stores it session-only only when allowSessionFallback is set. */
	StoreResult store(const std::string &profileId, const std::string &destinationId, SecretField field,
			  const std::string &value, bool allowSessionFallback);
	void storeSessionOnly(const std::string &profileId, const std::string &destinationId, SecretField field,
			      const std::string &value);

	/* Looks in session memory first, then in the persistent store under persistentRef. */
	CredResult load(const std::string &profileId, const std::string &destinationId, SecretField field,
			const std::string &persistentRef, std::string &value, SecretLocation *where = nullptr);
	SecretLocation locate(const std::string &profileId, const std::string &destinationId, SecretField field,
			      const std::string &persistentRef) const;

	void remove(const std::string &profileId, const std::string &destinationId, SecretField field,
		    const std::string &persistentRef);
	void removeAll(const std::string &profileId, const DestinationConfig &config);

	/* Loads everything a destination needs to connect. */
	CredResult loadSecrets(const std::string &profileId, const DestinationConfig &config, DestinationSecrets &out);

private:
	CredentialStore *persistent_;
	mutable MemoryCredentialStore session_;
};

} // namespace mf
