// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "rtmp-url.hpp"

#include <algorithm>
#include <cctype>

namespace mf {

namespace {

std::string to_lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

bool is_ipv4(const std::string &host)
{
	int parts = 0;
	size_t i = 0;
	while (i <= host.size()) {
		size_t dot = host.find('.', i);
		if (dot == std::string::npos)
			dot = host.size();
		std::string part = host.substr(i, dot - i);
		if (part.empty() || part.size() > 3)
			return false;
		if (!std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); }))
			return false;
		if (std::stoi(part) > 255)
			return false;
		parts++;
		i = dot + 1;
		if (dot == host.size())
			break;
	}
	return parts == 4;
}

} // namespace

std::string RtmpUrl::application() const
{
	size_t start = path.find_first_not_of('/');
	if (start == std::string::npos)
		return {};
	size_t end = path.find('/', start);
	return path.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool is_valid_host(const std::string &host, bool ipv6Literal)
{
	if (host.empty() || host.size() > 253)
		return false;

	if (ipv6Literal) {
		/* Syntactic check only: hex digits, colons, optional embedded IPv4 and zone id. */
		bool sawColon = false;
		for (unsigned char c : host) {
			if (c == ':')
				sawColon = true;
			else if (!(std::isxdigit(c) || c == '.' || c == '%'))
				return false;
		}
		return sawColon;
	}

	bool allNumericDots =
		std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; });
	if (allNumericDots)
		return is_ipv4(host);

	size_t i = 0;
	while (i < host.size()) {
		size_t dot = host.find('.', i);
		if (dot == std::string::npos)
			dot = host.size();
		std::string label = host.substr(i, dot - i);
		if (label.empty() || label.size() > 63)
			return false;
		if (label.front() == '-' || label.back() == '-')
			return false;
		for (unsigned char c : label) {
			if (!(std::isalnum(c) || c == '-' || c == '_'))
				return false;
		}
		i = dot + 1;
	}
	return host.back() != '.';
}

RtmpUrl parse_rtmp_url(const std::string &url)
{
	RtmpUrl out;

	size_t schemeEnd = url.find("://");
	if (schemeEnd == std::string::npos || schemeEnd == 0)
		return out;

	out.scheme = to_lower(url.substr(0, schemeEnd));
	std::string rest = url.substr(schemeEnd + 3);

	size_t hashPos = rest.find('#');
	if (hashPos != std::string::npos) {
		out.fragment = rest.substr(hashPos + 1);
		rest.resize(hashPos);
	}

	size_t queryPos = rest.find('?');
	if (queryPos != std::string::npos) {
		out.query = rest.substr(queryPos + 1);
		rest.resize(queryPos);
	}

	size_t pathPos = rest.find('/');
	std::string authority = pathPos == std::string::npos ? rest : rest.substr(0, pathPos);
	if (pathPos != std::string::npos)
		out.path = rest.substr(pathPos);

	size_t at = authority.rfind('@');
	if (at != std::string::npos) {
		out.userInfo = authority.substr(0, at);
		authority = authority.substr(at + 1);
	}

	std::string portText;
	if (!authority.empty() && authority.front() == '[') {
		size_t close = authority.find(']');
		if (close == std::string::npos)
			return out;
		out.host = authority.substr(1, close - 1);
		out.hostIsIpv6 = true;
		std::string after = authority.substr(close + 1);
		if (!after.empty()) {
			if (after.front() != ':')
				return out;
			out.portPresent = true;
			portText = after.substr(1);
		}
	} else {
		size_t colon = authority.rfind(':');
		if (colon != std::string::npos) {
			out.host = authority.substr(0, colon);
			out.portPresent = true;
			portText = authority.substr(colon + 1);
		} else {
			out.host = authority;
		}
	}

	if (out.portPresent) {
		bool digits =
			!portText.empty() && portText.size() <= 5 &&
			std::all_of(portText.begin(), portText.end(), [](unsigned char c) { return std::isdigit(c); });
		out.port = digits ? std::stoi(portText) : 0;
		out.portValid = digits && out.port >= 1 && out.port <= 65535;
	}

	out.host = to_lower(out.host);
	out.parsed = true;
	return out;
}

std::string public_query(const std::string &query)
{
	static const char *const kPublic[] = {"backup"};
	std::string kept;
	size_t begin = 0;
	while (begin <= query.size()) {
		size_t end = query.find('&', begin);
		if (end == std::string::npos)
			end = query.size();
		std::string param = query.substr(begin, end - begin);
		std::string name = param.substr(0, param.find('='));
		for (const char *allowed : kPublic) {
			if (!param.empty() && name == allowed) {
				kept += (kept.empty() ? "" : "&") + param;
				break;
			}
		}
		begin = end + 1;
	}
	return kept;
}

} // namespace mf
