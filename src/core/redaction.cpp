// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "redaction.hpp"
#include "rtmp-url.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace mf {

namespace {

constexpr size_t kMaxMessageLength = 300;
constexpr size_t kMaxPlainSegment = 16;

bool plain_segment(const std::string &segment)
{
	if (segment.empty() || segment.size() > kMaxPlainSegment)
		return false;
	return std::all_of(segment.begin(), segment.end(),
			   [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}

std::string percent_encode(const std::string &value)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : value) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back((char)c);
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0xF]);
		}
	}
	return out;
}

void replace_all(std::string &text, const std::string &needle, const std::string &replacement)
{
	if (needle.empty())
		return;
	size_t pos = 0;
	while ((pos = text.find(needle, pos)) != std::string::npos) {
		text.replace(pos, needle.size(), replacement);
		pos += replacement.size();
	}
}

bool url_char(unsigned char c)
{
	return !(std::isspace(c) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`' || c < 0x20);
}

} // namespace

std::string redact_url(const std::string &url)
{
	RtmpUrl parsed = parse_rtmp_url(url);
	if (!parsed.parsed || parsed.host.empty())
		return "[unrecognised URL]";

	std::string out = parsed.scheme + "://";
	if (!parsed.userInfo.empty())
		out += std::string(kRedacted) + "@";
	out += parsed.hostIsIpv6 ? "[" + parsed.host + "]" : parsed.host;
	if (parsed.portPresent)
		out += ":" + (parsed.portValid ? std::to_string(parsed.port) : std::string("?"));

	std::vector<std::string> segments;
	size_t pos = 0;
	while (pos < parsed.path.size()) {
		size_t next = parsed.path.find('/', pos);
		if (next == std::string::npos)
			next = parsed.path.size();
		if (next > pos)
			segments.push_back(parsed.path.substr(pos, next - pos));
		pos = next + 1;
	}
	for (size_t i = 0; i < segments.size(); i++)
		out += "/" + ((i == 0 && plain_segment(segments[i])) ? segments[i] : std::string(kRedacted));
	if (!parsed.path.empty() && parsed.path.back() == '/')
		out += "/";

	if (!parsed.query.empty())
		out += std::string("?") + kRedacted;
	if (!parsed.fragment.empty())
		out += std::string("#") + kRedacted;
	return out;
}

std::string scrub_secrets(std::string text, const std::vector<std::string> &secrets)
{
	for (const std::string &secret : secrets) {
		/* Very short values (e.g. a one-letter username) would mangle ordinary text. */
		if (secret.size() < 3)
			continue;
		replace_all(text, secret, kRedacted);
		std::string encoded = percent_encode(secret);
		if (encoded != secret)
			replace_all(text, encoded, kRedacted);
	}
	return text;
}

std::string sanitize_message(const std::string &text, const std::vector<std::string> &secrets)
{
	std::string scrubbed = scrub_secrets(text, secrets);

	std::string out;
	out.reserve(scrubbed.size());
	size_t i = 0;
	while (i < scrubbed.size()) {
		size_t sep = scrubbed.find("://", i);
		if (sep == std::string::npos) {
			out.append(scrubbed, i, std::string::npos);
			break;
		}
		size_t schemeStart = sep;
		while (schemeStart > i && std::isalpha((unsigned char)scrubbed[schemeStart - 1]))
			schemeStart--;
		if (schemeStart == sep) {
			out.append(scrubbed, i, sep + 3 - i);
			i = sep + 3;
			continue;
		}
		size_t end = sep + 3;
		while (end < scrubbed.size() && url_char((unsigned char)scrubbed[end]))
			end++;
		/* Keep trailing sentence punctuation outside the URL. */
		while (end > sep + 3 &&
		       (scrubbed[end - 1] == '.' || scrubbed[end - 1] == ',' || scrubbed[end - 1] == ')'))
			end--;
		out.append(scrubbed, i, schemeStart - i);
		out += redact_url(scrubbed.substr(schemeStart, end - schemeStart));
		i = end;
	}

	for (char &c : out) {
		if ((unsigned char)c < 0x20)
			c = ' ';
	}

	if (out.size() > kMaxMessageLength) {
		size_t cut = kMaxMessageLength;
		/* Do not split a UTF-8 sequence. */
		while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80)
			cut--;
		out.resize(cut);
		out += "...";
	}
	return out;
}

void secure_clear(std::string &value)
{
	volatile char *p = value.empty() ? nullptr : &value[0];
	for (size_t i = 0; i < value.size(); i++)
		p[i] = 0;
	value.clear();
	value.shrink_to_fit();
}

} // namespace mf
