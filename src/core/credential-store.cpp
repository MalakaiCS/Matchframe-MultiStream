// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "credential-store.hpp"
#include "redaction.hpp"

namespace mf {

MemoryCredentialStore::~MemoryCredentialStore()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &entry : values_)
		secure_clear(entry.second);
}

CredResult MemoryCredentialStore::write(const std::string &target, const std::string &secret)
{
	if (failWrites_)
		return {CredStatus::Failed, "Simulated secure storage failure."};
	std::lock_guard<std::mutex> lock(mutex_);
	std::string &slot = values_[target];
	secure_clear(slot);
	slot = secret;
	return {CredStatus::Ok, {}};
}

CredResult MemoryCredentialStore::read(const std::string &target, std::string &secret)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = values_.find(target);
	if (it == values_.end())
		return {CredStatus::NotFound, "Not stored."};
	secret = it->second;
	return {CredStatus::Ok, {}};
}

CredResult MemoryCredentialStore::remove(const std::string &target)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = values_.find(target);
	if (it == values_.end())
		return {CredStatus::NotFound, "Not stored."};
	secure_clear(it->second);
	values_.erase(it);
	return {CredStatus::Ok, {}};
}

size_t MemoryCredentialStore::size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return values_.size();
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::unique_ptr<CredentialStore> create_platform_credential_store()
{
	return std::make_unique<UnavailableCredentialStore>(
		"Secure credential storage is not implemented on this platform in this release (Windows Credential "
		"Manager and the macOS Keychain are).");
}
#endif

std::string credential_target(const std::string &profileId, const std::string &destinationId, SecretField field)
{
	return std::string(kCredentialTargetPrefix) + profileId + "/" + destinationId + "/" + secret_field_id(field);
}

SecretVault::SecretVault(CredentialStore *persistent) : persistent_(persistent) {}

bool SecretVault::persistentAvailable() const
{
	return persistent_ && persistent_->available();
}

std::string SecretVault::persistentName() const
{
	return persistent_ ? persistent_->backendName() : "none";
}

std::string SecretVault::persistentUnavailableReason() const
{
	if (!persistent_)
		return "No secure credential store is configured.";
	return persistent_->unavailableReason();
}

SecretVault::StoreResult SecretVault::store(const std::string &profileId, const std::string &destinationId,
					    SecretField field, const std::string &value, bool allowSessionFallback)
{
	StoreResult result;
	std::string sessionKey = credential_target(profileId, destinationId, field);

	if (persistentAvailable()) {
		CredResult written = persistent_->write(sessionKey, value);
		if (written.ok()) {
			session_.remove(sessionKey);
			result.location = SecretLocation::Persistent;
			result.target = sessionKey;
			return result;
		}
		result.error = written.message.empty() ? "Secure storage write failed." : written.message;
	} else {
		result.error = persistentUnavailableReason();
	}

	if (allowSessionFallback) {
		session_.write(sessionKey, value);
		result.location = SecretLocation::SessionOnly;
	}
	return result;
}

void SecretVault::storeSessionOnly(const std::string &profileId, const std::string &destinationId, SecretField field,
				   const std::string &value)
{
	session_.write(credential_target(profileId, destinationId, field), value);
}

CredResult SecretVault::load(const std::string &profileId, const std::string &destinationId, SecretField field,
			     const std::string &persistentRef, std::string &value, SecretLocation *where)
{
	if (where)
		*where = SecretLocation::None;
	if (session_.read(credential_target(profileId, destinationId, field), value).ok()) {
		if (where)
			*where = SecretLocation::SessionOnly;
		return {CredStatus::Ok, {}};
	}
	if (persistentRef.empty())
		return {CredStatus::NotFound, "Not stored."};
	if (!persistentAvailable())
		return {CredStatus::Unavailable, persistentUnavailableReason()};
	CredResult read = persistent_->read(persistentRef, value);
	if (read.ok() && where)
		*where = SecretLocation::Persistent;
	return read;
}

SecretLocation SecretVault::locate(const std::string &profileId, const std::string &destinationId, SecretField field,
				   const std::string &persistentRef) const
{
	std::string ignored;
	if (session_.read(credential_target(profileId, destinationId, field), ignored).ok()) {
		secure_clear(ignored);
		return SecretLocation::SessionOnly;
	}
	return persistentRef.empty() ? SecretLocation::None : SecretLocation::Persistent;
}

void SecretVault::remove(const std::string &profileId, const std::string &destinationId, SecretField field,
			 const std::string &persistentRef)
{
	session_.remove(credential_target(profileId, destinationId, field));
	if (!persistentRef.empty() && persistentAvailable())
		persistent_->remove(persistentRef);
}

void SecretVault::removeAll(const std::string &profileId, const DestinationConfig &config)
{
	for (SecretField field : {SecretField::StreamKey, SecretField::Username, SecretField::Password})
		remove(profileId, config.id, field, config.credentials.ref(field));
}

CredResult SecretVault::loadSecrets(const std::string &profileId, const DestinationConfig &config,
				    DestinationSecrets &out)
{
	out.clear();
	CredResult key =
		load(profileId, config.id, SecretField::StreamKey, config.credentials.streamKey, out.streamKey);
	if (!key.ok()) {
		if (key.status == CredStatus::NotFound)
			key.message = "No stream key is stored for this destination. Edit it and enter the key.";
		else
			key.message = "The stream key could not be read from " + persistentName() + ": " + key.message;
		return key;
	}
	if (config.useAuth) {
		CredResult user =
			load(profileId, config.id, SecretField::Username, config.credentials.username, out.username);
		if (!user.ok()) {
			user.message = "The server username could not be loaded. Edit the destination and re-enter it.";
			return user;
		}
		CredResult pass =
			load(profileId, config.id, SecretField::Password, config.credentials.password, out.password);
		if (!pass.ok() && pass.status != CredStatus::NotFound) {
			pass.message = "The server password could not be loaded. Edit the destination and re-enter it.";
			return pass;
		}
	}
	return {CredStatus::Ok, {}};
}

} // namespace mf
