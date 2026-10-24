// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <string>

namespace mf {

/* Result of a syntactic parse of an ingest URL. No network access, no DNS. */
struct RtmpUrl {
	bool parsed = false;  // structure could be split into scheme/authority/path
	std::string scheme;   // lower-case, e.g. "rtmp", "rtmps", "srt"
	std::string userInfo; // anything before '@' in the authority (must be empty to be valid)
	std::string host;     // IPv6 literals without brackets
	bool hostIsIpv6 = false;
	bool portPresent = false;
	bool portValid = true;
	int port = 0;
	std::string path;     // starts with '/' when present
	std::string query;    // without '?'
	std::string fragment; // without '#'

	bool isRtmpFamily() const { return scheme == "rtmp" || scheme == "rtmps"; }
	bool secure() const { return scheme == "rtmps"; }
	int effectivePort() const { return portPresent ? port : (secure() ? 443 : 1935); }
	/* First non-empty path segment, i.e. the RTMP application name. */
	std::string application() const;
};

RtmpUrl parse_rtmp_url(const std::string &url);

/*
 * The part of a query string (without '?') that is known not to be secret and is kept in exports:
 * only parameters on a short allowlist, today YouTube's backup-ingest flag "backup". Everything
 * else may be a token and is dropped. Parameters keep their order; returns "" if none are left.
 */
std::string public_query(const std::string &query);

/* Hostname syntax check (RFC 1123 labels), IPv4 dotted quad, or bracket-free IPv6 literal. */
bool is_valid_host(const std::string &host, bool ipv6Literal);

} // namespace mf
