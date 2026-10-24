// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#pragma once

#include "integration/obs-harness.hpp"

#include <functional>
#include <string>
#include <vector>

namespace mftest {

struct ScenarioResult {
	bool passed = true;
	std::vector<std::string> notes; // measured values, for the test report
};

struct Scenario {
	const char *name;
	const char *covers; // acceptance test numbers from the brief
	std::function<void(ObsHarness &, ScenarioResult &)> run;
};

const std::vector<Scenario> &all_scenarios();
void run_soak(ObsHarness &obs, int minutes, int destinations, int segmentSeconds, ScenarioResult &result);

} // namespace mftest
