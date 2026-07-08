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

// [AI-MODS] Entire file is a fork addition (does not exist upstream); see docs/UPSTREAM_SYNC.md.

#include "Verify.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "CrossPlatform.h"
#include "Logger.h"
#include "Options.h"
#include "Collections.h"
#include "../fmath.h"
#include "../Mod/Mod.h"

namespace OpenXcom
{

// ---------------------------------------------------------------------------
// Self-test registry
// ---------------------------------------------------------------------------

namespace
{

struct SelfTestCase
{
	std::string name;
	void (*fn)(SelfTestContext&);
};

/**
 * Function-local static registry (Meyers singleton). Constructed on first use, so it is
 * immune to the static-initialization-order fiasco even though tests register during
 * static init.
 */
std::vector<SelfTestCase>& registry()
{
	static std::vector<SelfTestCase> cases;
	return cases;
}

/**
 * Scans the raw command line for a valueless flag. Accepts both -name and --name and is
 * case-insensitive. Independent of Options' own parser so it works before Options::init().
 */
bool hasFlag(const char* name)
{
	for (const auto& arg : CrossPlatform::getArgs())
	{
		if (arg.size() < 2 || (arg[0] != '-' && arg[0] != '/'))
		{
			continue;
		}
		size_t dashes = (arg.size() > 2 && arg[1] == '-') ? 2 : 1;
		std::string a = arg.substr(dashes);
		std::transform(a.begin(), a.end(), a.begin(), ::tolower);
		if (a == name)
		{
			return true;
		}
	}
	return false;
}

/**
 * Emits the human-readable report to stdout AND, if the OXCE_VERIFY_OUT environment variable
 * names a file, writes it there too. The file channel is what the wrapper scripts / CI read,
 * because a /SUBSYSTEM:WINDOWS (GUI) build's stdout cannot be reliably captured by a launcher.
 */
void emitReport(const std::string& text)
{
	std::cout << text;
	std::cout.flush();
	if (const char* path = std::getenv("OXCE_VERIFY_OUT"))
	{
		std::ofstream f(path, std::ios::out | std::ios::trunc);
		if (f)
		{
			f << text;
		}
	}
}

}

void SelfTestContext::check(bool condition, const std::string& message)
{
	if (!condition)
	{
		if (_failures == 0)
		{
			_firstFailure = message;
		}
		++_failures;
	}
}

int registerSelfTest(const std::string& name, void (*fn)(SelfTestContext&))
{
	registry().push_back(SelfTestCase{ name, fn });
	return 0;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

namespace Verify
{

bool selfTestRequested()
{
	return hasFlag("selftest");
}

bool validateRequested()
{
	return hasFlag("validate");
}

int runSelfTests()
{
	CrossPlatform::ensureConsoleOutput();
	std::ostringstream report;
	int passed = 0;
	int failed = 0;
	for (const auto& c : registry())
	{
		SelfTestContext ctx;
		try
		{
			c.fn(ctx);
		}
		catch (std::exception& e)
		{
			ctx.check(false, std::string("threw exception: ") + e.what());
		}
		catch (...)
		{
			ctx.check(false, "threw unknown exception");
		}

		if (ctx.getFailures() == 0)
		{
			report << "PASS " << c.name << "\n";
			++passed;
		}
		else
		{
			report << "FAIL " << c.name << ": " << ctx.getFirstFailure()
				   << " (" << ctx.getFailures() << " failed check(s))\n";
			++failed;
		}
	}
	report << "SELFTEST: " << passed << " passed, " << failed << " failed\n";
	emitReport(report.str());
	return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int runModValidation()
{
	CrossPlatform::ensureConsoleOutput();
	try
	{
		Log(LOG_INFO) << "[validate] scanning mods and file maps...";
		Options::updateMods();

		// [AI-MODS] Validate at the game's shipping default strictness (LOG_WARNING), forced
		// in memory only so we never rewrite the user's options.cfg. Lower settings could hide
		// real problems; higher settings would fail the base game on benign notices. Script and
		// ruleset errors that are logged instead of thrown are caught by the error counter below.
		Options::oxceModValidationLevel = LOG_WARNING;

		// [AI-MODS] Headless: no audio device is opened, so skip sound loading. Otherwise every
		// sound file logs "Audio device hasn't been opened" and pollutes the error count. Ruleset,
		// script and sprite validation are unaffected by muting.
		Options::mute = true;

		int errorsBefore = CrossPlatform::getLogErrorCount();
		std::string master = Options::getActiveMaster();
		Log(LOG_INFO) << "[validate] loading master '" << master << "' + active mods...";

		Mod::resetGlobalStatics();
		std::unique_ptr<Mod> mod(new Mod());
		mod->loadAll();

		int newErrors = CrossPlatform::getLogErrorCount() - errorsBefore;
		if (newErrors > 0)
		{
			std::ostringstream report;
			report << "VALIDATE: FAIL master=" << master << " (" << newErrors
				   << " error(s) logged during load; see openxcom.log)\n";
			emitReport(report.str());
			return EXIT_FAILURE;
		}

		emitReport("VALIDATE: OK master=" + master + "\n");
		return EXIT_SUCCESS;
	}
	catch (std::exception& e)
	{
		std::string what = e.what();
		emitReport("VALIDATE: FAIL " + what + "\n");
		// Distinguish "the game data simply isn't here" (an environment problem, exit 2) from a
		// genuine mod/ruleset defect (exit 1) so callers/CI can react differently.
		if (what.find("No X-COM installations") != std::string::npos)
		{
			return 2;
		}
		return EXIT_FAILURE;
	}
}

}

// ---------------------------------------------------------------------------
// Self-tests
//
// These live in this translation unit on purpose: because Verify.cpp is always linked (its
// entry points are referenced from main()), the static registrars below can never be stripped.
// They were migrated from the historical NDEBUG assert blocks in main.cpp so they now run in
// Release builds too, via `OpenXcom.exe --selftest`.
// ---------------------------------------------------------------------------

namespace
{

/// Move-semantics stress type: mishandled self-move/copy would corrupt its value.
struct BadMove
{
	int i;

	BadMove(int b) : i(b) {}
	BadMove(BadMove&& b) { i = b.i; b.i = {}; }
	BadMove(const BadMove& b) { i = b.i; }
	BadMove& operator=(BadMove&& b)
	{
		i = {};
		i = b.i;
		b.i = {};
		return *this;
	}
	bool operator==(const BadMove& b) const { return i == b.i; }
};

struct DummyVectDouble
{
	double x, y, z;

	bool operator==(const DummyVectDouble& a) const
	{
		return AreSame(x, a.x) && AreSame(y, a.y) && AreSame(z, a.z);
	}
};

}

OXC_SELFTEST(collections_removeIf_int)
{
	{
		std::vector<int> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](int& i) { return i < 3; });
		ctx.check(v == std::vector<int>{ 3, 4 }, "removeIf i<3");
	}
	{
		std::vector<int> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](int& i) { return i > 2; });
		ctx.check(v == std::vector<int>{ 1, 2 }, "removeIf i>2");
	}
	{
		std::vector<int> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](int& i) { return i < 2 || i == 4; });
		ctx.check(v == std::vector<int>{ 2, 3 }, "removeIf i<2||i==4");
	}
	{
		std::vector<int> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](int& i) { if (i < 2 || i == 4) { return true; } else { i += 10; return false; } });
		ctx.check(v == std::vector<int>{ 12, 13 }, "removeIf mutate+remove");
	}
	{
		std::vector<int> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](int&) { return false; });
		ctx.check(v == std::vector<int>{ 1, 2, 3, 4 }, "removeIf none");
	}
	{
		std::vector<int> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](int&) { return true; });
		ctx.check(v == std::vector<int>{ }, "removeIf all");
	}
	{
		std::vector<int> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](int& i) { i += 10; return false; });
		ctx.check(v == std::vector<int>{ 11, 12, 13, 14 }, "removeIf mutate keep");
	}
}

OXC_SELFTEST(collections_removeIf_moveType)
{
	{
		std::vector<BadMove> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](BadMove& i) { return i.i < 3; });
		ctx.check(v == std::vector<BadMove>{ 3, 4 }, "removeIf move i<3");
	}
	{
		std::vector<BadMove> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](BadMove& i) { return i.i > 2; });
		ctx.check(v == std::vector<BadMove>{ 1, 2 }, "removeIf move i>2");
	}
	{
		std::vector<BadMove> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](BadMove& i) { if (i.i < 2 || i.i == 4) { return true; } else { i.i += 10; return false; } });
		ctx.check(v == std::vector<BadMove>{ 12, 13 }, "removeIf move mutate+remove");
	}
	{
		std::vector<BadMove> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](BadMove&) { return false; });
		ctx.check(v == std::vector<BadMove>{ 1, 2, 3, 4 }, "removeIf move none");
	}
	{
		std::vector<BadMove> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](BadMove& i) { i.i += 10; return false; });
		ctx.check(v == std::vector<BadMove>{ 11, 12, 13, 14 }, "removeIf move mutate keep");
	}
	{
		std::vector<BadMove> v = { 1, 2, 3, 4 };
		Collections::removeIf(v, [](BadMove&) { return true; });
		ctx.check(v == std::vector<BadMove>{ }, "removeIf move all");
	}
}

OXC_SELFTEST(fmath_vectors)
{
	const DummyVectDouble x { 1, 0, 0 };
	const DummyVectDouble y { 0, 1, 0 };
	const DummyVectDouble z { 0, 0, 1 };

	const DummyVectDouble x256 { 256, 0, 0 };
	const DummyVectDouble y256 { 0, 256, 0 };
	const DummyVectDouble z256 { 0, 0, 256 };

	ctx.check(x == VectNormalize(x), "normalize x");
	ctx.check(y == VectNormalize(y), "normalize y");
	ctx.check(z == VectNormalize(z), "normalize z");
	ctx.check(x == VectNormalize(x256), "normalize x256");
	ctx.check(y == VectNormalize(y256), "normalize y256");
	ctx.check(z == VectNormalize(z256), "normalize z256");
	ctx.check(x256 == VectNormalize(x256, 256), "normalize x256 to 256");
	ctx.check(y256 == VectNormalize(y256, 256), "normalize y256 to 256");
	ctx.check(z256 == VectNormalize(z256, 256), "normalize z256 to 256");

	ctx.check(z == VectCrossProduct(x, y), "cross x*y");
	ctx.check(x == VectCrossProduct(y, z), "cross y*z");
	ctx.check(y == VectCrossProduct(z, x), "cross z*x");

	ctx.check(z256 == VectCrossProduct(x256, y), "cross x256*y");
	ctx.check(x256 == VectCrossProduct(y256, z), "cross y256*z");
	ctx.check(y256 == VectCrossProduct(z256, x), "cross z256*x");

	ctx.check(z256 == VectCrossProduct(x256, y256, 256), "cross x256*y256 to 256");
	ctx.check(x256 == VectCrossProduct(y256, z256, 256), "cross y256*z256 to 256");
	ctx.check(y256 == VectCrossProduct(z256, x256, 256), "cross z256*x256 to 256");
}

}
