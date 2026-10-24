// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <string>
#include <vector>

namespace mf {

enum class Severity { Info, Warning, Error };

struct Issue {
	Severity severity = Severity::Error;
	std::string field;   // e.g. "serverUrl", "streamKey", "encoder"
	std::string message; // operator-facing, never contains secrets
};

using Issues = std::vector<Issue>;

inline bool has_errors(const Issues &issues)
{
	for (const Issue &issue : issues) {
		if (issue.severity == Severity::Error)
			return true;
	}
	return false;
}

inline std::string first_error(const Issues &issues)
{
	for (const Issue &issue : issues) {
		if (issue.severity == Severity::Error)
			return issue.message;
	}
	return {};
}

inline void append(Issues &into, const Issues &from)
{
	into.insert(into.end(), from.begin(), from.end());
}

} // namespace mf
