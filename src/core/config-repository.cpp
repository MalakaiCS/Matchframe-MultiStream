// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "config-repository.hpp"
#include "log.hpp"

#include <util/bmem.h>
#include <util/platform.h>

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace mf {

namespace {

std::string timestamp()
{
	char buffer[32];
	std::time_t now = std::time(nullptr);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
	return buffer;
}

} // namespace

bool read_text_file(const std::string &path, std::string &out)
{
	char *text = os_quick_read_utf8_file(path.c_str());
	if (!text)
		return false;
	out = text;
	bfree(text);
	return true;
}

bool write_text_file_atomic(const std::string &path, const std::string &content, std::string &error)
{
	/* os_quick_write_utf8_file_safe writes <path>.tmp, then replaces <path> keeping <path>.bak. */
	if (!os_quick_write_utf8_file_safe(path.c_str(), content.c_str(), content.size(), false, "tmp", "bak")) {
		error = "Could not write " + path;
		return false;
	}
	return true;
}

ConfigRepository::ConfigRepository(std::string directory) : directory_(std::move(directory))
{
	while (!directory_.empty() && (directory_.back() == '/' || directory_.back() == '\\'))
		directory_.pop_back();
}

std::string ConfigRepository::sanitize_key(const std::string &profileKey)
{
	std::string out;
	for (unsigned char c : profileKey) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == ' ' || c == '.')
			out.push_back((char)c);
		else
			out.push_back('_');
	}
	while (!out.empty() && (out.front() == '.' || out.front() == ' '))
		out.erase(out.begin());
	while (!out.empty() && (out.back() == '.' || out.back() == ' '))
		out.pop_back();
	return out.empty() ? std::string("default") : out;
}

std::string ConfigRepository::pathFor(const std::string &profileKey) const
{
	return directory_ + "/" + sanitize_key(profileKey) + ".json";
}

bool ConfigRepository::exists(const std::string &profileKey) const
{
	return os_file_exists(pathFor(profileKey).c_str());
}

ConfigRepository::LoadResult ConfigRepository::load(const std::string &profileKey) const
{
	LoadResult result;
	std::string path = pathFor(profileKey);
	std::string backup = path + ".bak";

	bool mainExists = os_file_exists(path.c_str());
	bool backupExists = os_file_exists(backup.c_str());
	result.existed = mainExists || backupExists;

	auto fresh = [&]() {
		result.config = ProfileConfig{};
		result.config.profileId = new_uuid();
	};

	if (!result.existed) {
		fresh();
		return result;
	}

	std::string text;
	ParseResult parsed;
	if (mainExists && read_text_file(path, text))
		parsed = parse_config(text);

	if (parsed.newerSchema) {
		fresh();
		result.readOnly = true;
		result.warnings.push_back(parsed.error + " It was not modified; changes made now will not be saved.");
		return result;
	}

	if (!parsed.ok && backupExists) {
		std::string backupText;
		ParseResult fromBackup;
		if (read_text_file(backup, backupText))
			fromBackup = parse_config(backupText);
		if (fromBackup.ok) {
			result.warnings.push_back("The saved configuration was unreadable (" + parsed.error +
						  "); the previous backup was restored.");
			parsed = std::move(fromBackup);
		}
	}

	if (!parsed.ok) {
		/* Move the unreadable file aside so the next save cannot destroy it. */
		std::string aside = path + ".unreadable-" + timestamp();
		if (mainExists && os_rename(path.c_str(), aside.c_str()) == 0) {
			result.warnings.push_back("The saved configuration could not be read (" + parsed.error +
						  "). It was kept as " + aside +
						  " and an empty configuration was started.");
		} else {
			result.readOnly = mainExists;
			result.warnings.push_back("The saved configuration could not be read (" + parsed.error +
						  "). Changes will not be saved until the file is fixed or removed.");
		}
		fresh();
		return result;
	}

	result.config = std::move(parsed.config);
	for (std::string &warning : parsed.warnings)
		result.warnings.push_back(std::move(warning));
	return result;
}

bool ConfigRepository::save(const std::string &profileKey, const ProfileConfig &config, std::string &error) const
{
	if (os_mkdirs(directory_.c_str()) == MKDIR_ERROR) {
		error = "Could not create the configuration directory.";
		return false;
	}
	return write_text_file_atomic(pathFor(profileKey), serialize_config(config), error);
}

bool ConfigRepository::move(const std::string &fromKey, const std::string &toKey, std::string &error) const
{
	std::string from = pathFor(fromKey);
	std::string to = pathFor(toKey);
	if (from == to)
		return true;
	if (!os_file_exists(from.c_str()))
		return true;
	if (os_file_exists(to.c_str())) {
		error = "A configuration already exists for the renamed profile.";
		return false;
	}
	if (os_rename(from.c_str(), to.c_str()) != 0) {
		error = "Could not move the configuration for the renamed profile.";
		return false;
	}
	std::string fromBackup = from + ".bak";
	if (os_file_exists(fromBackup.c_str()))
		os_rename(fromBackup.c_str(), (to + ".bak").c_str());
	return true;
}

} // namespace mf
