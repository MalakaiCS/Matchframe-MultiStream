// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "common/test-framework.hpp"

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

/* Quiet libobs logging during unit tests unless MF_TEST_VERBOSE is set. */
void quiet_log(int level, const char *format, va_list args, void *)
{
	if (level > LOG_WARNING && !getenv("MF_TEST_VERBOSE"))
		return;
	char buffer[4096];
	vsnprintf(buffer, sizeof(buffer), format, args);
	fprintf(stderr, "      log: %s\n", buffer);
}

} // namespace

int main(int argc, char **argv)
{
	base_set_log_handler(quiet_log, nullptr);
	return mftest::run_all(argc, argv);
}
