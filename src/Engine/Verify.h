#pragma once
/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <string>

// [AI-MODS] Entire file is a fork addition (does not exist upstream). It provides the
// headless verification entry points (--selftest / --validate) that Claude and CI use to
// prove a change without opening a window. Keeping it in its own translation unit keeps the
// upstream merge surface tiny; see docs/UPSTREAM_SYNC.md for the full touch-point list.

namespace OpenXcom
{

/**
 * Context handed to each self-test. Call check() to assert a condition; unlike assert()
 * this records the failure and keeps going, and it works in Release builds (NDEBUG).
 */
class SelfTestContext
{
public:
	/// Records a failure with the given message if the condition is false.
	void check(bool condition, const std::string& message);
	/// Number of failed checks recorded so far.
	int getFailures() const { return _failures; }
	/// First failure message (empty when none), used for reporting.
	const std::string& getFirstFailure() const { return _firstFailure; }
private:
	int _failures = 0;
	std::string _firstFailure;
};

/// Registers a named self-test function. Returns a dummy int so it can seed a static.
int registerSelfTest(const std::string& name, void (*fn)(SelfTestContext&));

/**
 * Headless verification entry points, invoked from main() before a window is created.
 * @sa docs/TESTING.md
 */
namespace Verify
{
	/// Was -selftest / --selftest passed on the command line?
	bool selfTestRequested();
	/// Was -validate / --validate passed on the command line?
	bool validateRequested();
	/// Runs every registered self-test. Returns an exit code (0 = all passed).
	int runSelfTests();
	/// Loads the active mod set with strict validation and reports the result.
	/// Returns an exit code: 0 = clean, 1 = validation failure, 2 = no game data / environment.
	int runModValidation();
}

/**
 * Defines and registers a self-test in one statement. The body receives `SelfTestContext& ctx`.
 * Keep OXC_SELFTEST definitions inside Verify.cpp (or another translation unit that is already
 * linked) so the linker can never drop the static registrar.
 *
 * Example:
 *   OXC_SELFTEST(my_test)
 *   {
 *       ctx.check(1 + 1 == 2, "math is broken");
 *   }
 */
#define OXC_SELFTEST(testName) \
	static void testName(OpenXcom::SelfTestContext& ctx); \
	[[maybe_unused]] static int _reg_##testName = OpenXcom::registerSelfTest(#testName, testName); \
	static void testName(OpenXcom::SelfTestContext& ctx)

}
