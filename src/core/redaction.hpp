// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <string>
#include <vector>

namespace mf {

/* Placeholder shown wherever a secret would otherwise appear. */
inline constexpr const char *kRedacted = "[redacted]";

/* Fixed-width mask for UI summaries. Never reveals length or any character of the key. */
inline constexpr const char *kMaskedSecret =
	"\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";

/*
 * Returns a form of an ingest URL that is safe for logs, diagnostics and error text:
 * scheme, host and port are kept; credentials, query and fragment are removed; the
 * first path segment (the RTMP application, e.g. "live2" or "app") is kept only when
 * it is short and plain, every further segment is replaced by [redacted].
 */
std::string redact_url(const std::string &url);

/* Replaces every occurrence of each non-trivial secret (and its percent-encoded form). */
std::string scrub_secrets(std::string text, const std::vector<std::string> &secrets);

/*
 * Makes backend text safe to show and log: scrubs the given secrets, redacts any
 * embedded URLs, replaces control characters and bounds the length.
 */
std::string sanitize_message(const std::string &text, const std::vector<std::string> &secrets = {});

/* Overwrites the string's buffer before clearing it (best effort; prevents casual reuse). */
void secure_clear(std::string &value);

} // namespace mf
