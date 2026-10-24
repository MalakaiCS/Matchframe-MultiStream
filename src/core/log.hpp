// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include <util/base.h>

/*
 * Logging rule for this plugin: the format string is always a literal and every
 * caller-supplied value goes through a %s argument that has already been passed
 * through mf::redact_url() / mf::sanitize_message(). Stream keys, passwords and
 * raw service settings are never logged.
 */
#define MF_LOG(level, format, ...) blog(level, "[matchframe-multistream] " format, ##__VA_ARGS__)
#define MF_INFO(format, ...) MF_LOG(LOG_INFO, format, ##__VA_ARGS__)
#define MF_WARN(format, ...) MF_LOG(LOG_WARNING, format, ##__VA_ARGS__)
#define MF_ERROR(format, ...) MF_LOG(LOG_ERROR, format, ##__VA_ARGS__)
#define MF_DEBUG(format, ...) MF_LOG(LOG_DEBUG, format, ##__VA_ARGS__)
