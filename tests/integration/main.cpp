// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Usage: mf-integration-tests --obs-prefix <OBS install> --work <dir> --scripts <tests/scripts>
//                             [scenario ...] [--soak <minutes> [--soak-destinations N]]

#include "integration/scenarios.hpp"

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>

namespace {

FILE *g_obsLog = nullptr;

void log_handler(int level, const char *format, va_list args, void *)
{
	char buffer[8192];
	vsnprintf(buffer, sizeof(buffer), format, args);
	if (g_obsLog) {
		fprintf(g_obsLog, "%s\n", buffer);
		fflush(g_obsLog);
	}
	if (level <= LOG_WARNING || strstr(buffer, "[matchframe-multistream]"))
		fprintf(stdout, "      obs: %s\n", buffer);
}

} // namespace

int main(int argc, char **argv)
{
	mftest::HarnessPaths paths;
	std::set<std::string> selected;
	int soakMinutes = 0;
	int width = 1280, height = 720;
	int soakDestinations = 4;
	int soakSegmentSeconds = 300;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--obs-prefix" && i + 1 < argc)
			paths.obsPrefix = argv[++i];
		else if (arg == "--work" && i + 1 < argc)
			paths.workDir = argv[++i];
		else if (arg == "--scripts" && i + 1 < argc)
			paths.scriptsDir = argv[++i];
		else if (arg == "--soak" && i + 1 < argc)
			soakMinutes = std::atoi(argv[++i]);
		else if (arg == "--soak-destinations" && i + 1 < argc)
			soakDestinations = std::atoi(argv[++i]);
		else if (arg == "--soak-segment" && i + 1 < argc)
			soakSegmentSeconds = std::atoi(argv[++i]);
		else if (arg == "--size" && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &width, &height);
		else
			selected.insert(arg);
	}
	if (paths.obsPrefix.empty() || paths.workDir.empty() || paths.scriptsDir.empty()) {
		fprintf(stderr, "usage: %s --obs-prefix DIR --work DIR --scripts DIR [scenario...] [--soak MIN]\n",
			argv[0]);
		return 2;
	}

	g_obsLog = fopen((paths.workDir + "/obs.log").c_str(), "a");
	base_set_log_handler(log_handler, nullptr);

	mftest::ObsHarness obs;
	std::string error;
	if (!obs.start(paths, width, height, 30, error)) {
		fprintf(stderr, "harness start failed: %s\n", error.c_str());
		return 2;
	}
	printf("libobs %s started (%dx%d@30, 48 kHz stereo)\n", obs_get_version_string(), width, height);

	std::ofstream report(paths.workDir + "/integration-report.txt", std::ios::app);
	std::time_t now = std::time(nullptr);
	report << "=== run " << std::ctime(&now);
	int failed = 0;
	int ran = 0;

	for (const mftest::Scenario &scenario : mftest::all_scenarios()) {
		if (!selected.empty() && !selected.count(scenario.name) && !selected.count("all"))
			continue;
		if (selected.empty() && soakMinutes > 0)
			continue;
		printf("\n[ RUN  ] %s (acceptance %s)\n", scenario.name, scenario.covers);
		fflush(stdout);
		mftest::ScenarioResult result;
		scenario.run(obs, result);
		ran++;
		failed += result.passed ? 0 : 1;
		printf("[ %s ] %s\n", result.passed ? " OK " : "FAIL", scenario.name);
		report << (result.passed ? "PASS " : "FAIL ") << scenario.name << " (acceptance " << scenario.covers
		       << ")\n";
		for (const std::string &note : result.notes)
			report << "    " << note << "\n";
		report.flush();
	}

	if (soakMinutes > 0) {
		printf("\n[ RUN  ] soak %d min, %d destinations\n", soakMinutes, soakDestinations);
		mftest::ScenarioResult result;
		mftest::run_soak(obs, soakMinutes, soakDestinations, soakSegmentSeconds, result);
		ran++;
		failed += result.passed ? 0 : 1;
		printf("[ %s ] soak\n", result.passed ? " OK " : "FAIL");
		report << (result.passed ? "PASS " : "FAIL ") << "soak " << soakMinutes << " min x " << soakDestinations
		       << " destinations (acceptance 11)\n";
		for (const std::string &note : result.notes)
			report << "    " << note << "\n";
	}

	obs.shutdown();
	printf("\n%d scenario(s) run, %d failed\n", ran, failed);
	report << ran << " scenario(s), " << failed << " failed\n";
	if (g_obsLog)
		fclose(g_obsLog);
	return failed == 0 && ran > 0 ? 0 : 1;
}
