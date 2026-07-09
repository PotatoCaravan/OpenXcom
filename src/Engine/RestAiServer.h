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

// [AI-MODS] Entire file is a fork addition (does not exist upstream). It lets an external
// webserver drive the alien battlescape AI over REST: the engine embeds an HTTP server on a
// background thread and, when REST mode is enabled, routes the one AI decision call to it instead
// of the built-in AIModule. Kept in its own translation unit so the upstream merge surface stays
// tiny (the only gameplay touch-point is a single hook in BattlescapeGame::handleAI). See
// docs/REST_AI.md and docs/UPSTREAM_SYNC.md.
//
// NOTE: the implementation includes <httplib.h> (which pulls <winsock2.h>); never include that
// from this header, so callers stay free of the winsock/windows.h include-order hazard.

namespace OpenXcom
{

class BattlescapeGame;
class BattleUnit;
struct BattleAction;

/**
 * Embedded REST server that hands alien AI decisions off to an external webserver.
 *
 * Threading: the HTTP server runs on its own background thread and only ever touches a
 * mutex-guarded string mailbox (never the live SavedBattleGame). All game-state (de)serialization
 * happens on the main thread inside decide(). @sa docs/REST_AI.md
 */
namespace RestAiServer
{
	// --- configuration (parsed from the command line, à la Verify::hasFlag) ---

	/// Was --restai passed? Enables REST-controlled alien AI during battles (interactive play).
	bool enabled();
	/// Was --restserver passed? Runs the REST endpoint standalone/headless (no window, no game
	/// data) so the external webserver can be integration-tested against a real engine build.
	bool serverModeRequested();
	/// TCP port to listen on (--restai-port <n>, default 8765).
	int configuredPort();
	/// How long decide() waits for an action before falling back to the built-in AI, in ms
	/// (--restai-timeout <ms>, default 8000; 0 = wait forever).
	int timeoutMs();

	// --- server lifecycle ---

	/// Starts the HTTP server on a background thread bound to `port` (0 = an ephemeral port).
	/// Returns the actually-bound port, or -1 on failure. Idempotent (returns the current port
	/// if already running).
	int start(int port);
	/// Starts on configuredPort(). Returns the bound port, or -1 on failure.
	int start();
	/// Stops the server thread and clears any pending exchange state.
	void stop();
	/// Is the server thread currently running?
	bool isRunning();
	/// The port the server is bound to, or -1 if not running.
	int boundPort();

	// --- decision exchange (generic string layer: main thread <-> server thread) ---
	// These operate purely on strings so they can be unit-tested without a live battle.

	/// Main thread: publish the current pending decision request (YAML) for GET /pending-decision.
	void publishRequest(const std::string& requestYaml);
	/// Main thread: clear any pending request (no decision outstanding -> GET returns 204).
	void clearRequest();
	/// Main thread: block until POST /action delivers an action or `timeoutMs` elapses
	/// (0 = forever), pumping SDL meanwhile so the OS window stays responsive. Returns true and
	/// fills `actionYamlOut` on success, false on timeout.
	bool waitForAction(std::string& actionYamlOut, int timeoutMs);

	// --- the battlescape hook ---

	/// Obtains an alien decision for `unit` from the external webserver and writes it into `*out`.
	/// Returns true on success; false on timeout / disabled so the caller falls back to the
	/// built-in AIModule (the game therefore never hangs if the webserver is down).
	bool decide(BattlescapeGame* game, BattleUnit* unit, BattleAction* out);

	// --- headless standalone server mode (dispatched from main() for --restserver) ---

	/// Runs the REST server headlessly until POST /shutdown or the --restserver-seconds safety
	/// deadline (default 30s), then returns an exit code. Needs no window and no game data, so it
	/// runs in CI and lets an external client exercise the real server over a socket.
	int runServerMode();
}

}
