// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "profile-config.hpp"

#include <string>
#include <vector>

namespace mf {

/*
 * Profile-scoped persistence in OBS's plugin configuration directory
 * (<OBS config>/plugin_config/matchframe-multistream/profiles/<profile dir>.json).
 * Writes are atomic (temp file + replace, previous version kept as .bak). A file that
 * cannot be parsed is moved aside, never overwritten in place. A file written by a
 * newer plugin version is loaded read-only and never saved over.
 */
class ConfigRepository {
public:
	explicit ConfigRepository(std::string directory);

	struct LoadResult {
		ProfileConfig config;
		bool existed = false;
		bool readOnly = false; // newer schema: refuse to save
		std::vector<std::string> warnings;
	};

	LoadResult load(const std::string &profileKey) const;
	bool save(const std::string &profileKey, const ProfileConfig &config, std::string &error) const;
	/* Used when OBS renames a profile directory. Refuses to overwrite an existing target. */
	bool move(const std::string &fromKey, const std::string &toKey, std::string &error) const;
	bool exists(const std::string &profileKey) const;
	std::string pathFor(const std::string &profileKey) const;
	const std::string &directory() const { return directory_; }

	/* File name for a profile directory name. ASCII letters, digits and " -_." are kept, other ASCII
	 * becomes '_', and every non-ASCII byte becomes ~XX so that profiles named in other scripts
	 * (which OBS keeps in their directory names) get files of their own. */
	static std::string sanitize_key(const std::string &profileKey);

private:
	/* The 0.1.0 file name, which mapped every non-ASCII byte to '_'. */
	static std::string legacy_key(const std::string &profileKey);
	std::string legacyPathFor(const std::string &profileKey) const;

	std::string directory_;
};

bool read_text_file(const std::string &path, std::string &out);
bool write_text_file_atomic(const std::string &path, const std::string &content, std::string &error);

} // namespace mf
