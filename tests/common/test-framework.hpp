// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Minimal self-contained test runner (no third-party dependency).

#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace mftest {

struct TestCase {
	const char *name;
	std::function<void()> fn;
};

inline std::vector<TestCase> &registry()
{
	static std::vector<TestCase> tests;
	return tests;
}

struct Registrar {
	Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Abort {};

inline int &current_failures()
{
	static int failures = 0;
	return failures;
}

inline void fail(const char *file, int line, const std::string &what)
{
	current_failures()++;
	std::fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, what.c_str());
}

template<typename A, typename B> std::string describe_pair(const A &a, const B &b)
{
	std::ostringstream out;
	out << "[" << a << "] vs [" << b << "]";
	return out.str();
}

inline int run_all(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : nullptr;
	int failedTests = 0;
	int ran = 0;
	for (const TestCase &test : registry()) {
		if (filter && !std::strstr(test.name, filter))
			continue;
		ran++;
		int before = current_failures();
		std::fprintf(stderr, "[ RUN  ] %s\n", test.name);
		try {
			test.fn();
		} catch (const Abort &) {
		} catch (const std::exception &e) {
			fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
		}
		bool ok = current_failures() == before;
		if (!ok)
			failedTests++;
		std::fprintf(stderr, "[ %s ] %s\n", ok ? " OK " : "FAIL", test.name);
	}
	std::fprintf(stderr, "\n%d test(s) run, %d failed\n", ran, failedTests);
	return failedTests == 0 && ran > 0 ? 0 : 1;
}

} // namespace mftest

#define MF_TEST(name)                                                    \
	static void name();                                              \
	static const mftest::Registrar name##_registrar(#name, name);   \
	static void name()

#define CHECK(cond)                                              \
	do {                                                     \
		if (!(cond))                                     \
			mftest::fail(__FILE__, __LINE__, #cond); \
	} while (0)

#define REQUIRE(cond)                                            \
	do {                                                     \
		if (!(cond)) {                                   \
			mftest::fail(__FILE__, __LINE__, #cond); \
			throw mftest::Abort{};                   \
		}                                                \
	} while (0)

#define CHECK_EQ(a, b)                                                                                      \
	do {                                                                                                \
		auto &&_a = (a);                                                                            \
		auto &&_b = (b);                                                                            \
		if (!(_a == _b))                                                                            \
			mftest::fail(__FILE__, __LINE__, std::string(#a " == " #b " ") +                   \
								 mftest::describe_pair(_a, _b));            \
	} while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
	do {                                                                                       \
		double _a = (a), _b = (b);                                                         \
		if (std::fabs(_a - _b) > (eps))                                                    \
			mftest::fail(__FILE__, __LINE__,                                           \
				     std::string(#a " ~= " #b " ") + mftest::describe_pair(_a, _b)); \
	} while (0)
