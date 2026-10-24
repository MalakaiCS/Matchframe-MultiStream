// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "plugin/plugin-context.hpp"
#include "core/log.hpp"
#include "core/redaction.hpp"
#include "ui/qt-dispatcher.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QTimer>

#include <chrono>

using namespace std::chrono_literals;

namespace mf {

namespace {

/* OBS resets its video right after PROFILE_CHANGING returns. A destination still in the RTMP
 * handshake keeps encoders bound to the old video output, and if that handshake succeeded
 * after the reset it would start them on freed memory. librtmp gives up a stalled handshake
 * after its 30 s receive timeout, so waiting longer than that leaves no such straggler. */
constexpr auto kProfileChangeBudget = 35000ms;
/* A read-only configuration is still referenced by its file, so nothing may change it or the
 * secure-store entries it points to (deleting a destination there would delete its keys). */
constexpr const char *kReadOnlyError =
	"This profile's plugin configuration is read-only (see the notice in the dock), so it cannot be "
	"changed now.";
constexpr auto kExitSoftBudget = 5000ms;
/* OBS 32.2.2's RTMP output can block a stop for up to its 30 s receive timeout while a
 * connect is stuck in the handshake; teardown waits for that, then gives up. */
constexpr auto kExitHardCap = 40000ms;

std::string last_path_component(std::string path)
{
	while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
		path.pop_back();
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

SecretEdits::~SecretEdits()
{
	secure_clear(streamKey.value);
	secure_clear(username.value);
	secure_clear(password.value);
}

PluginContext &PluginContext::instance()
{
	static PluginContext context;
	return context;
}

void PluginContext::moduleLoad()
{
	dispatcher_ = std::make_unique<QtDispatcher>();
	credentialStore_ = create_platform_credential_store();
	vault_ = std::make_unique<SecretVault>(credentialStore_.get());

	char *configDir = obs_module_config_path("profiles");
	repository_ = std::make_unique<ConfigRepository>(configDir ? configDir : "");
	bfree(configDir);

	manager_ = std::make_unique<OutputManager>(*dispatcher_, OutputManager::Options{});
	manager_->setListener(this);
	manager_->setSecretsProvider(
		[this](const DestinationConfig &config, DestinationSecrets &secrets, std::string &error) {
			CredResult result = vault_->loadSecrets(profileId(), config, secrets);
			error = result.message;
			return result.ok();
		});

	if (!vault_->persistentAvailable())
		notices_.push_back("Secure credential storage is unavailable (" +
				   vault_->persistentUnavailableReason() +
				   "). Stream keys can be kept for this OBS session only.");

	obs_frontend_add_event_callback(frontendEvent, this);
	MF_INFO("loaded; credential store: %s", vault_->persistentName().c_str());
}

void PluginContext::moduleUnload()
{
	obs_frontend_remove_event_callback(frontendEvent, this);
	shutdownOutputs();
	/* An abandoned worker can still post its last event through the dispatcher (the event
	 * itself is discarded), so the dispatcher must outlive it. */
	bool abandoned = manager_ && manager_->abandonedWorkers();
	changeHandler_ = nullptr;
	if (manager_)
		manager_->setListener(nullptr);
	manager_.reset();
	vault_.reset();
	credentialStore_.reset();
	repository_.reset();
	if (abandoned)
		(void)dispatcher_.release();
	else
		dispatcher_.reset();
}

void PluginContext::shutdownOutputs()
{
	if (outputsShutDown_ || !manager_)
		return;
	outputsShutDown_ = true;
	manager_->shutdown(kExitSoftBudget, kExitHardCap);
}

void PluginContext::frontendEvent(obs_frontend_event event, void *data)
{
	static_cast<PluginContext *>(data)->onFrontendEvent(event);
}

std::string PluginContext::currentProfileKey() const
{
	char *path = obs_frontend_get_current_profile_path();
	std::string key = path ? last_path_component(path) : std::string();
	bfree(path);
	if (key.empty()) {
		char *name = obs_frontend_get_current_profile();
		key = name ? name : "default";
		bfree(name);
	}
	return key;
}

void PluginContext::onFrontendEvent(obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		loadProfile(currentProfileKey());
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		/* OBS resets video right after this event; plugin outputs must be gone first. It cannot
		 * be vetoed, so active outputs are stopped (bounded) before the next profile loads. */
		if (manager_->anyBusy()) {
			bool clean = manager_->stopAllAndWait(StopReason::ProfileChange, kProfileChangeBudget);
			notices_.push_back(clean ? "Plugin outputs were stopped because the OBS profile is changing."
						 : "Plugin outputs were stopped for a profile change; some were still "
						   "closing their connection.");
		}
		break;

	case OBS_FRONTEND_EVENT_PROFILE_RENAMED:
		renamePending_ = true;
		[[fallthrough]];
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		/* Rename, duplicate and "new profile" emit PROFILE_CHANGED without PROFILE_CHANGING, and a
		 * rename's PROFILE_RENAMED follows in the same call stack, so decide on the next turn. */
		if (!reconcileScheduled_) {
			reconcileScheduled_ = true;
			QTimer::singleShot(0, [this] { reconcileProfile(); });
		}
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		exiting_ = true;
		if (loaded_)
			persist();
		shutdownOutputs();
		break;

	default:
		break;
	}
}

void PluginContext::reconcileProfile()
{
	reconcileScheduled_ = false;
	if (exiting_ || !manager_)
		return;
	std::string key = currentProfileKey();
	if (key == profileKey_) {
		renamePending_ = false;
		return;
	}
	if (renamePending_) {
		/* Same profile, new directory name: move the file; live outputs keep running and keep
		 * their credentials, which are scoped by the configuration's own profileId. */
		renamePending_ = false;
		std::string error;
		if (!repository_->move(profileKey_, key, error))
			notices_.push_back("Profile renamed, but the plugin configuration could not be moved: " +
					   error);
		profileKey_ = key;
		persist();
		managerChanged();
		return;
	}
	if (manager_->anyBusy()) {
		manager_->stopAllAndWait(StopReason::ProfileChange, kProfileChangeBudget);
		notices_.push_back("Plugin outputs were stopped because the OBS profile changed.");
	}
	loadProfile(key);
}

void PluginContext::loadProfile(const std::string &key)
{
	ConfigRepository::LoadResult result = repository_->load(key);
	std::string error;
	if (!manager_->loadProfile(result.config, error)) {
		notices_.push_back("Could not load the plugin configuration: " + error);
		managerChanged();
		return;
	}
	profileKey_ = key;
	readOnly_ = result.readOnly;
	loaded_ = true;
	for (const std::string &warning : result.warnings)
		notices_.push_back(warning);
	if (!result.existed)
		persist(); // fixes the new profileId so credential names stay stable
	MF_INFO("profile '%s': %zu destination(s) loaded (all idle; nothing is started automatically)",
		sanitize_message(key).c_str(), manager_->config().destinations.size());
	managerChanged();
}

void PluginContext::persist()
{
	if (!loaded_ || readOnly_)
		return;
	std::string error;
	if (!repository_->save(profileKey_, manager_->config(), error)) {
		MF_ERROR("could not save configuration: %s", error.c_str());
		notices_.push_back("Could not save the plugin configuration: " + error);
	}
}

void PluginContext::dismissNotices()
{
	notices_.clear();
	managerChanged();
}

void PluginContext::destinationChanged(const std::string &id)
{
	if (changeHandler_)
		changeHandler_(id);
}

void PluginContext::managerChanged()
{
	if (changeHandler_)
		changeHandler_({});
}

SaveOutcome PluginContext::saveDestination(const DestinationConfig &input, SecretEdits &edits)
{
	SaveOutcome outcome;
	if (readOnly_) {
		outcome.error = kReadOnlyError;
		return outcome;
	}
	DestinationConfig config = input;
	config.name = trim(config.name);
	if (config.id.empty())
		config.id = new_uuid();
	const DestinationConfig *existing = manager_->config().find(config.id);
	if (!existing && manager_->config().destinations.size() >= kMaxDestinations) {
		outcome.error = "At most 32 destinations can be saved per profile.";
		return outcome;
	}
	config.credentials = existing ? existing->credentials : CredentialRefs{};
	if (config.platform != Platform::Custom)
		config.useAuth = false;

	Issues issues = validate_destination(config);
	if (has_errors(issues)) {
		outcome.error = first_error(issues);
		return outcome;
	}
	bool secretChange = edits.streamKey.changed || edits.username.changed || edits.password.changed;
	if (existing && manager_->destinationLocked(config.id) &&
	    (secretChange || !existing->sameTransportSettings(config))) {
		outcome.error = "Stop this destination before changing its server, credentials or reconnect settings.";
		return outcome;
	}

	/* Secrets first: a refused secure write must not leave a half-saved destination. */
	for (SecretField field : {SecretField::StreamKey, SecretField::Username, SecretField::Password}) {
		SecretEdit &edit = edits.of(field);
		bool unused = !config.useAuth && field != SecretField::StreamKey;
		std::string &ref = config.credentials.ref(field);
		if (unused) {
			vault_->remove(profileId(), config.id, field, ref);
			ref.clear();
			continue;
		}
		if (!edit.changed)
			continue;
		if (edit.value.empty()) {
			vault_->remove(profileId(), config.id, field, ref);
			ref.clear();
			continue;
		}
		SecretVault::StoreResult stored =
			vault_->store(profileId(), config.id, field, edit.value, edit.allowSessionOnly);
		if (stored.location == SecretLocation::None) {
			outcome.needsSessionConsent = true;
			outcome.consentField = field;
			outcome.storageError = stored.error;
			outcome.error = "Secure storage failed: " + stored.error;
			return outcome;
		}
		if (stored.location == SecretLocation::SessionOnly && !ref.empty()) {
			/* Do not leave an older persistent value behind a session-only replacement. */
			std::string stale = ref;
			ref.clear();
			if (credentialStore_ && credentialStore_->available())
				credentialStore_->remove(stale);
		} else {
			ref = stored.target;
		}
	}

	std::string error;
	bool ok = existing ? manager_->updateDestination(config, error) : manager_->addDestination(config, error);
	if (!ok) {
		if (!existing)
			vault_->removeAll(profileId(), config); // nothing would ever reference these entries
		outcome.error = error;
		return outcome;
	}
	persist();
	outcome.ok = true;
	outcome.savedId = config.id;
	return outcome;
}

bool PluginContext::duplicateDestination(const std::string &id, std::string &newId, std::string &error)
{
	if (readOnly_) {
		error = kReadOnlyError;
		return false;
	}
	const DestinationConfig *source = manager_->config().find(id);
	if (!source) {
		error = "Destination not found.";
		return false;
	}
	DestinationConfig copy = *source;
	copy.id = new_uuid();
	copy.name = source->name.substr(0, kMaxNameLength - 7) + " (copy)";
	copy.enabled = false;  // a duplicate never joins a bulk start by surprise
	copy.credentials = {}; // and never inherits keys
	if (!manager_->addDestination(copy, error))
		return false;
	newId = copy.id;
	persist();
	return true;
}

bool PluginContext::deleteDestination(const std::string &id, std::string &error)
{
	if (readOnly_) {
		error = kReadOnlyError;
		return false;
	}
	const DestinationConfig *existing = manager_->config().find(id);
	if (!existing) {
		error = "Destination not found.";
		return false;
	}
	DestinationConfig removed = *existing;
	if (!manager_->removeDestination(id, error))
		return false;
	vault_->removeAll(profileId(), removed);
	persist();
	return true;
}

bool PluginContext::setEnabled(const std::string &id, bool enabled, std::string &error)
{
	if (readOnly_) {
		error = kReadOnlyError;
		return false;
	}
	if (!manager_->setEnabled(id, enabled, error))
		return false;
	persist();
	return true;
}

bool PluginContext::setEncoderProfile(const EncoderProfile &profile, std::string &error)
{
	if (readOnly_) {
		error = kReadOnlyError;
		return false;
	}
	if (!manager_->setEncoderProfile(profile, error))
		return false;
	persist();
	return true;
}

bool PluginContext::exportTo(const std::string &path, std::string &error, int *queriesRemoved)
{
	return write_text_file_atomic(path, serialize_export(manager_->config(), queriesRemoved), error);
}

ImportResult PluginContext::readImport(const std::string &path)
{
	std::string text;
	if (!read_text_file(path, text)) {
		ImportResult result;
		result.problems.push_back("The file could not be read.");
		return result;
	}
	return parse_import(text);
}

bool PluginContext::applyImport(const ImportResult &import, bool replace, std::string &error)
{
	if (readOnly_) {
		error = kReadOnlyError;
		return false;
	}
	if (!import.ok) {
		error = "The file was rejected.";
		return false;
	}
	if (replace) {
		std::vector<DestinationConfig> previous = manager_->config().destinations;
		if (!manager_->replaceAll(import.destinations, import.hasEncoder ? &import.encoder : nullptr, error))
			return false;
		for (const DestinationConfig &old : previous)
			vault_->removeAll(profileId(), old);
	} else {
		if (manager_->config().destinations.size() + import.destinations.size() > kMaxDestinations) {
			error = "Importing would exceed 32 destinations.";
			return false;
		}
		for (const DestinationConfig &dest : import.destinations) {
			if (!manager_->addDestination(dest, error))
				return false;
		}
	}
	persist();
	return true;
}

SecretLocation PluginContext::secretLocation(const DestinationConfig &config, SecretField field) const
{
	return vault_->locate(profileId(), config.id, field, config.credentials.ref(field));
}

bool PluginContext::loadSecret(const DestinationConfig &config, SecretField field, std::string &value)
{
	return vault_->load(profileId(), config.id, field, config.credentials.ref(field), value).ok();
}

std::string PluginContext::diagnostics() const
{
	std::string out = "MatchFrame Multistream " + std::string(PLUGIN_VERSION) + "\n";
	out += "Profile: " + sanitize_message(profileKey_) + (readOnly_ ? " (read-only configuration)" : "") + "\n";
	out += "Credential store: " + vault_->persistentName() + "\n";
	out += manager_->diagnostics();
	return out;
}

} // namespace mf
