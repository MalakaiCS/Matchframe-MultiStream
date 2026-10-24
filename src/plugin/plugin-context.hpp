// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "core/config-repository.hpp"
#include "core/credential-store.hpp"
#include "core/output-manager.hpp"

#include <obs-frontend-api.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mf {

class QtDispatcher;

/* A requested change to one secret in the destination editor. */
struct SecretEdit {
	bool changed = false;
	std::string value;             // empty with changed=true clears the secret
	bool allowSessionOnly = false; // operator accepted session-only storage
};

struct SecretEdits {
	SecretEdit streamKey;
	SecretEdit username;
	SecretEdit password;
	SecretEdit &of(SecretField field)
	{
		return field == SecretField::StreamKey  ? streamKey
		       : field == SecretField::Username ? username
							: password;
	}
	~SecretEdits();
};

struct SaveOutcome {
	bool ok = false;
	std::string error;
	/* Secure storage refused this field; the editor may offer session-only storage. */
	bool needsSessionConsent = false;
	SecretField consentField = SecretField::StreamKey;
	std::string storageError;
	std::string savedId;
};

/*
 * Plugin-wide controller on the UI thread: owns persistence, secrets and the OutputManager,
 * and maps OBS frontend lifecycle events onto them. The dock only talks to this class.
 */
class PluginContext final : public OutputManager::Listener {
public:
	static PluginContext &instance();

	void moduleLoad();
	void moduleUnload();

	/* Dock API (UI thread). */
	OutputManager &manager() { return *manager_; }
	SecretVault &vault() { return *vault_; }
	const std::string &profileId() const { return manager_->config().profileId; }
	const std::string &profileKey() const { return profileKey_; }
	bool loaded() const { return loaded_; }
	bool readOnly() const { return readOnly_; }
	const std::vector<std::string> &notices() const { return notices_; }
	void dismissNotices();

	SaveOutcome saveDestination(const DestinationConfig &config, SecretEdits &edits);
	bool duplicateDestination(const std::string &id, std::string &newId, std::string &error);
	bool deleteDestination(const std::string &id, std::string &error);
	bool setEnabled(const std::string &id, bool enabled, std::string &error);
	bool setEncoderProfile(const EncoderProfile &profile, std::string &error);
	bool exportTo(const std::string &path, std::string &error, int *queriesRemoved = nullptr);
	ImportResult readImport(const std::string &path);
	bool applyImport(const ImportResult &import, bool replace, std::string &error);
	SecretLocation secretLocation(const DestinationConfig &config, SecretField field) const;
	bool loadSecret(const DestinationConfig &config, SecretField field, std::string &value);
	std::string diagnostics() const;

	using ChangeHandler = std::function<void(const std::string &destinationId)>;
	void setChangeHandler(ChangeHandler handler) { changeHandler_ = std::move(handler); }

	/* OutputManager::Listener */
	void destinationChanged(const std::string &id) override;
	void managerChanged() override;

private:
	PluginContext() = default;
	static void frontendEvent(obs_frontend_event event, void *data);
	void onFrontendEvent(obs_frontend_event event);
	void loadProfile(const std::string &key);
	void reconcileProfile();
	void persist();
	void shutdownOutputs();
	std::string currentProfileKey() const;

	std::unique_ptr<QtDispatcher> dispatcher_;
	std::unique_ptr<CredentialStore> credentialStore_;
	std::unique_ptr<SecretVault> vault_;
	std::unique_ptr<ConfigRepository> repository_;
	std::unique_ptr<OutputManager> manager_;
	std::string profileKey_;
	bool loaded_ = false;
	bool readOnly_ = false;
	bool renamePending_ = false;
	bool reconcileScheduled_ = false;
	bool exiting_ = false;
	bool outputsShutDown_ = false;
	std::vector<std::string> notices_;
	ChangeHandler changeHandler_;
};

} // namespace mf
