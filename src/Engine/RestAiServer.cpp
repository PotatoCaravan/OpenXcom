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

// httplib.h must come first: on Windows it pulls <winsock2.h>, which has to precede any <windows.h>
// (dragged in later by SDL / CrossPlatform). We only use plain HTTP -- CPPHTTPLIB_OPENSSL_SUPPORT is
// deliberately left undefined, so no OpenSSL/crypt32 dependency. It self-links ws2_32 via a
// #pragma comment on MSVC, so no build-system link edits are needed there.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include "../../libs/httplib/httplib.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "RestAiServer.h"
#include "CrossPlatform.h"
#include "Logger.h"
#include "Verify.h"
#include "Game.h"
#include "Yaml.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/Position.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/TileEngine.h"
#include "../Savegame/Tile.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/BattleItem.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleInventory.h"
#include "../Mod/Unit.h"

namespace OpenXcom
{

namespace
{

// ---------------------------------------------------------------------------
// Command-line parsing (independent of Options' parser, like Verify::hasFlag)
// ---------------------------------------------------------------------------

/// Strips a leading -, --, or / from an option token and lower-cases it; "" if not an option.
std::string normalizeArg(const std::string& arg)
{
	if (arg.size() < 2 || (arg[0] != '-' && arg[0] != '/'))
	{
		return std::string();
	}
	size_t dashes = (arg.size() > 2 && arg[1] == '-') ? 2 : 1;
	std::string a = arg.substr(dashes);
	std::transform(a.begin(), a.end(), a.begin(), ::tolower);
	return a;
}

/// True if a valueless flag `name` is present on the command line.
bool hasFlag(const char* name)
{
	for (const auto& arg : CrossPlatform::getArgs())
	{
		if (normalizeArg(arg) == name)
		{
			return true;
		}
	}
	return false;
}

/// Reads the token following `--name` into `out`. Returns false if absent or has no value.
bool argValue(const char* name, std::string& out)
{
	const auto& args = CrossPlatform::getArgs();
	for (size_t i = 0; i < args.size(); ++i)
	{
		if (normalizeArg(args[i]) == name)
		{
			if (i + 1 < args.size())
			{
				out = args[i + 1];
				return true;
			}
			return false;
		}
	}
	return false;
}

/// Reads an integer option, or returns `fallback` if absent/unparseable.
int argInt(const char* name, int fallback)
{
	std::string v;
	if (argValue(name, v))
	{
		try
		{
			return std::stoi(v);
		}
		catch (...)
		{
			return fallback;
		}
	}
	return fallback;
}

// ---------------------------------------------------------------------------
// Server + decision-exchange state (single process-wide instance)
// ---------------------------------------------------------------------------

struct ServerState
{
	std::unique_ptr<httplib::Server> svr;
	std::thread thread;
	std::atomic<bool> running{ false };
	std::atomic<bool> shutdownReq{ false }; // set by POST /shutdown, polled by runServerMode()
	int boundPort = -1;

	// The mailbox exchanged between the main thread and the server thread.
	std::mutex mx;
	std::condition_variable cv;
	bool hasRequest = false;
	std::string requestYaml;
	bool hasAction = false;
	std::string actionYaml;
	std::string lastActionYaml; // harness mode: most recent action body, for GET /last-action
	bool hasLastAction = false;

	// On-demand query side-channel. Live game state (pathfinding, tiles, line-of-fire) may only be
	// touched by the MAIN thread, so a query received on the server thread is parked here and
	// answered by the main thread inside decide()'s wait loop (only while a decision is active).
	std::atomic<bool> decisionActive{ false };
	std::condition_variable qcv;
	bool hasQuery = false;
	bool queryDone = false;
	std::string queryKind;   // "map" | "validate"
	std::string queryBody;   // e.g. the action YAML for "validate"
	std::string queryAnswer;
};

/// Meyers singleton -> no static-init-order issues; lives for the whole process.
ServerState& S()
{
	static ServerState s;
	return s;
}

/// A representative v1 request payload (matches writeRequest's schema). Handy for developing a
/// webserver against the standalone server, and served at GET /sample-request in harness mode.
const char* sampleRequestYaml()
{
	return
		"request:\n"
		"  turn: 3\n"
		"  side: HOSTILE\n"
		"  difficulty: 2\n"
		"  map: {sizeX: 60, sizeY: 60, sizeZ: 4}\n"
		"  unit:\n"
		"    id: 1000123\n"
		"    type: STR_SECTOID_SOLDIER\n"
		"    position: {x: 10, y: 12, z: 1}\n"
		"    direction: 4\n"
		"    tu: 54\n"
		"    energy: 60\n"
		"    health: 30\n"
		"    kneeling: false\n"
		"    items:\n"
		"      - {id: 2001, type: STR_PLASMA_RIFLE, slot: STR_RIGHT_HAND, ammo: 20}\n"
		"      - {id: 2002, type: STR_ALIEN_GRENADE, slot: STR_BELT}\n"
		"  visibleEnemies:\n"
		"    - {id: 500, type: STR_SOLDIER, faction: PLAYER, position: {x: 8, y: 9, z: 1}}\n";
}

/// Server-thread side of the query channel: park a query, wake the main thread (decide's loop),
/// and wait for it to fill in the answer. Only valid while an alien decision is active.
void handleQuery(const std::string& kind, const std::string& body, httplib::Response& res)
{
	ServerState& s = S();
	if (!s.decisionActive)
	{
		res.status = 409; // Conflict: nothing to answer about right now.
		res.set_content("{\"error\":\"no active alien decision (it is not the alien turn)\"}",
			"application/json");
		return;
	}
	std::unique_lock<std::mutex> lk(s.mx);
	// Serialize queries: wait for any in-flight one to finish first.
	if (!s.qcv.wait_for(lk, std::chrono::seconds(2), [&] { return !s.hasQuery; }))
	{
		res.status = 503;
		res.set_content("{\"error\":\"busy with another query\"}", "application/json");
		return;
	}
	s.hasQuery = true;
	s.queryDone = false;
	s.queryKind = kind;
	s.queryBody = body;
	s.queryAnswer.clear();
	s.cv.notify_all(); // wake the decide() wait loop so the main thread answers
	if (s.qcv.wait_for(lk, std::chrono::seconds(3), [&] { return s.queryDone; }))
	{
		res.set_content(s.queryAnswer, "application/x-yaml");
	}
	else
	{
		s.hasQuery = false;
		res.status = 504;
		res.set_content("{\"error\":\"query timed out (main thread did not answer)\"}",
			"application/json");
	}
	s.queryDone = false;
}

void registerHandlers(httplib::Server& svr, bool harness)
{
	// Liveness probe for the external webserver.
	svr.Get("/health", [](const httplib::Request&, httplib::Response& res)
	{
		res.set_content("{\"status\":\"ok\"}", "application/json");
	});

	// The webserver polls this to learn what decision (if any) the engine is waiting on.
	svr.Get("/pending-decision", [](const httplib::Request&, httplib::Response& res)
	{
		ServerState& s = S();
		std::lock_guard<std::mutex> lk(s.mx);
		if (s.hasRequest)
		{
			res.set_content(s.requestYaml, "application/x-yaml");
		}
		else
		{
			res.status = 204; // No Content: nothing to decide right now.
		}
	});

	// The webserver submits the chosen action here, unblocking the waiting unit.
	svr.Post("/action", [](const httplib::Request& req, httplib::Response& res)
	{
		ServerState& s = S();
		{
			std::lock_guard<std::mutex> lk(s.mx);
			s.actionYaml = req.body;
			s.hasAction = true;
			s.lastActionYaml = req.body;
			s.hasLastAction = true;
		}
		s.cv.notify_all();
		res.set_content("{\"status\":\"accepted\"}", "application/json");
	});

	// Lets a client ask the standalone --restserver process to exit cleanly.
	svr.Post("/shutdown", [](const httplib::Request&, httplib::Response& res)
	{
		S().shutdownReq = true;
		res.set_content("{\"status\":\"shutting-down\"}", "application/json");
	});

	// Live queries about the alien currently being decided; answered by the main thread using real
	// game state. Available in interactive play too (they 409 when it is not the alien turn).
	svr.Get("/map", [](const httplib::Request&, httplib::Response& res)
	{
		handleQuery("map", "", res);
	});
	svr.Post("/validate", [](const httplib::Request& req, httplib::Response& res)
	{
		handleQuery("validate", req.body, res);
	});

	if (!harness)
	{
		return;
	}

	// --- mock-harness endpoints (standalone --restserver only) ---
	// They let a client simulate the engine side of the exchange without a live battle, so a
	// webserver can be developed/integration-tested against a real engine build. Deliberately not
	// registered during interactive play so they can't interfere with a real battle's exchange.

	// Inject a pending decision (as if the engine were waiting on one).
	svr.Post("/publish", [](const httplib::Request& req, httplib::Response& res)
	{
		RestAiServer::publishRequest(req.body);
		res.set_content("{\"status\":\"published\"}", "application/json");
	});

	// Read back the most recently submitted action (for verifying a round trip).
	svr.Get("/last-action", [](const httplib::Request&, httplib::Response& res)
	{
		ServerState& s = S();
		std::lock_guard<std::mutex> lk(s.mx);
		if (s.hasLastAction)
		{
			res.set_content(s.lastActionYaml, "application/x-yaml");
		}
		else
		{
			res.status = 204;
		}
	});

	// A representative request payload, so a webserver author can see the exact schema.
	svr.Get("/sample-request", [](const httplib::Request&, httplib::Response& res)
	{
		res.set_content(sampleRequestYaml(), "application/x-yaml");
	});
}

/// Shared server bring-up. `harness` also exposes the mock endpoints (standalone mode only).
int startImpl(int port, bool harness)
{
	ServerState& s = S();
	if (s.running)
	{
		return s.boundPort;
	}

	s.svr.reset(new httplib::Server());
	registerHandlers(*s.svr, harness);

	int bound;
	if (port == 0)
	{
		bound = s.svr->bind_to_any_port("0.0.0.0");
	}
	else
	{
		bound = s.svr->bind_to_port("0.0.0.0", port) ? port : -1;
	}
	if (bound <= 0)
	{
		Log(LOG_ERROR) << "[restai] failed to bind HTTP server to port " << port;
		s.svr.reset();
		return -1;
	}

	s.boundPort = bound;
	s.running = true;
	s.shutdownReq = false;
	httplib::Server* p = s.svr.get();
	s.thread = std::thread([p]() { p->listen_after_bind(); });
	Log(LOG_INFO) << "[restai] REST server listening on http://0.0.0.0:" << bound
				  << (harness ? " (mock-harness endpoints enabled)" : "");
	return bound;
}

// ---------------------------------------------------------------------------
// Battlescape <-> YAML (de)serialization (curated v1 schema; see docs/REST_AI.md)
// ---------------------------------------------------------------------------

const char* factionName(UnitFaction f)
{
	switch (f)
	{
	case FACTION_PLAYER:  return "PLAYER";
	case FACTION_HOSTILE: return "HOSTILE";
	case FACTION_NEUTRAL: return "NEUTRAL";
	default:              return "NONE";
	}
}

const char* actionTypeName(BattleActionType t)
{
	switch (t)
	{
	case BA_NONE:        return "NONE";
	case BA_TURN:        return "TURN";
	case BA_WALK:        return "WALK";
	case BA_KNEEL:       return "KNEEL";
	case BA_PRIME:       return "PRIME";
	case BA_UNPRIME:     return "UNPRIME";
	case BA_THROW:       return "THROW";
	case BA_AUTOSHOT:    return "AUTOSHOT";
	case BA_SNAPSHOT:    return "SNAPSHOT";
	case BA_AIMEDSHOT:   return "AIMEDSHOT";
	case BA_HIT:         return "HIT";
	case BA_USE:         return "USE";
	case BA_LAUNCH:      return "LAUNCH";
	case BA_MINDCONTROL: return "MINDCONTROL";
	case BA_PANIC:       return "PANIC";
	default:             return "NONE";
	}
}

/// Maps an action name (case-insensitive, with a few synonyms) or a raw enum int to a
/// BattleActionType. Unknown input yields BA_NONE (a safe idle).
BattleActionType parseActionType(const std::string& raw)
{
	std::string s;
	for (char c : raw)
	{
		if (!std::isspace(static_cast<unsigned char>(c)))
		{
			s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}
	}
	if (s.empty())
		return BA_NONE;
	if (std::isdigit(static_cast<unsigned char>(s[0])))
	{
		try { return static_cast<BattleActionType>(std::stoi(s)); }
		catch (...) { return BA_NONE; }
	}
	if (s == "NONE" || s == "IDLE" || s == "WAIT")      return BA_NONE;
	if (s == "WALK" || s == "MOVE")                     return BA_WALK;
	if (s == "TURN")                                    return BA_TURN;
	if (s == "KNEEL")                                   return BA_KNEEL;
	if (s == "SNAPSHOT" || s == "SNAP")                 return BA_SNAPSHOT;
	if (s == "AUTOSHOT" || s == "AUTO")                 return BA_AUTOSHOT;
	if (s == "AIMEDSHOT" || s == "AIMED" || s == "AIM") return BA_AIMEDSHOT;
	if (s == "THROW")                                   return BA_THROW;
	if (s == "HIT" || s == "MELEE")                     return BA_HIT;
	if (s == "USE")                                     return BA_USE;
	if (s == "LAUNCH" || s == "BLASTER")                return BA_LAUNCH;
	if (s == "MINDCONTROL" || s == "MC")                return BA_MINDCONTROL;
	if (s == "PANIC")                                   return BA_PANIC;
	return BA_NONE;
}

/// True for action types that require a non-null weapon in BattlescapeGame::handleAI's translate
/// switch (which dereferences action.weapon). Used to downgrade a weaponless attack to BA_NONE.
bool isAttackType(BattleActionType t)
{
	switch (t)
	{
	case BA_SNAPSHOT: case BA_AUTOSHOT: case BA_AIMEDSHOT:
	case BA_THROW: case BA_HIT: case BA_USE:
	case BA_MINDCONTROL: case BA_PANIC: case BA_LAUNCH:
		return true;
	default:
		return false;
	}
}

void writePosition(YAML::YamlNodeWriter parent, ryml::csubstr key, const Position& p)
{
	YAML::YamlNodeWriter n = parent[key];
	n.setAsMap();
	n.setFlowStyle();
	n.write("x", static_cast<int>(p.x));
	n.write("y", static_cast<int>(p.y));
	n.write("z", static_cast<int>(p.z));
}

Position readPosition(const YAML::YamlNodeReader& node)
{
	int x = -1, y = -1, z = -1;
	node.tryRead("x", x);
	node.tryRead("y", y);
	node.tryRead("z", z);
	return Position(x, y, z);
}

int deriveDifficulty(BattlescapeGame* game)
{
	if (!game) return 0;
	SavedBattleGame* save = game->getSave();
	if (!save) return 0;
	BattlescapeState* state = save->getBattleState();
	if (!state) return 0;
	Game* g = state->getGame();
	if (!g) return 0;
	SavedGame* sg = g->getSavedGame();
	if (!sg) return 0;
	return sg->getDifficultyCoefficient();
}

/// Serializes the situation facing `unit` into the v1 request YAML (curated, not a raw save dump).
/// Note: string values written here reference live objects (unit/item/slot names, static faction
/// literals) that all outlive the emit() call, per rapidyaml's reference semantics.
std::string writeRequest(BattlescapeGame* game, BattleUnit* unit)
{
	SavedBattleGame* save = game->getSave();
	YAML::YamlRootNodeWriter w(8192);
	w.setAsMap();
	YAML::YamlNodeWriter req = w["request"];
	req.setAsMap();
	req.write("turn", save->getTurn());
	req.write("side", factionName(save->getSide()));
	req.write("difficulty", deriveDifficulty(game));

	YAML::YamlNodeWriter mp = req["map"];
	mp.setAsMap();
	mp.setFlowStyle();
	mp.write("sizeX", save->getMapSizeX());
	mp.write("sizeY", save->getMapSizeY());
	mp.write("sizeZ", save->getMapSizeZ());

	YAML::YamlNodeWriter un = req["unit"];
	un.setAsMap();
	un.write("id", unit->getId());
	un.write("type", unit->getType());
	writePosition(un, "position", unit->getPosition());
	un.write("direction", unit->getDirection());
	un.write("tu", unit->getTimeUnits());
	un.write("energy", unit->getEnergy());
	un.write("health", unit->getHealth());
	un.write("kneeling", unit->isKneeled());

	const std::vector<BattleItem*>* inv = unit->getInventory();
	if (inv && !inv->empty())
	{
		YAML::YamlNodeWriter items = un["items"];
		items.setAsSeq();
		for (BattleItem* it : *inv)
		{
			if (!it) continue;
			YAML::YamlNodeWriter e = items.write();
			e.setAsMap();
			e.setFlowStyle();
			e.write("id", it->getId());
			if (it->getRules()) e.write("type", it->getRules()->getType());
			if (it->getSlot())  e.write("slot", it->getSlot()->getId());
			e.write("ammo", it->getAmmoQuantity());
		}
	}

	std::vector<BattleUnit*>* vis = unit->getVisibleUnits();
	if (vis && !vis->empty())
	{
		YAML::YamlNodeWriter ve = req["visibleEnemies"];
		ve.setAsSeq();
		for (BattleUnit* e : *vis)
		{
			if (!e) continue;
			YAML::YamlNodeWriter en = ve.write();
			en.setAsMap();
			en.setFlowStyle();
			en.write("id", e->getId());
			en.write("type", e->getType());
			en.write("faction", factionName(e->getFaction()));
			writePosition(en, "position", e->getPosition());
		}
	}

	return w.emit().yaml;
}

/// Parsed, game-agnostic view of an incoming action (so it can be unit-tested without a battle).
struct ParsedAction
{
	BattleActionType type = BA_NONE;
	Position target{ -1, -1, -1 };
	bool hasTarget = false;
	int weaponId = -1;
	std::vector<Position> waypoints;
	int finalFacing = -1;
	bool kneel = false;
	bool run = false;
};

ParsedAction parseActionYaml(const std::string& yaml)
{
	ParsedAction pa;
	try
	{
		YAML::YamlRootNodeReader reader(YAML::YamlString(yaml), "restai-action");
		YAML::YamlNodeReader action = reader["action"];
		if (!action)
			return pa;

		std::string typeStr;
		if (action.tryRead("type", typeStr))
			pa.type = parseActionType(typeStr);

		YAML::YamlNodeReader tgt = action["target"];
		if (tgt)
		{
			pa.target = readPosition(tgt);
			pa.hasTarget = true;
		}

		action.tryRead("weapon", pa.weaponId);
		action.tryRead("finalFacing", pa.finalFacing);
		action.tryRead("kneel", pa.kneel);
		action.tryRead("run", pa.run);

		YAML::YamlNodeReader wps = action["waypoints"];
		if (wps && wps.isSeq())
		{
			for (const YAML::YamlNodeReader& wp : wps.children())
			{
				pa.waypoints.push_back(readPosition(wp));
			}
		}
	}
	catch (...)
	{
		return ParsedAction(); // malformed input -> safe idle
	}
	return pa;
}

/// Applies an incoming action YAML onto a BattleAction for `unit`: resolves the weapon id against
/// the unit's inventory (defaulting to the main-hand weapon) and downgrades an attack with no
/// usable weapon to BA_NONE so handleAI's translate switch can't dereference a null weapon.
void applyAction(const std::string& yaml, BattleUnit* unit, BattleAction* out)
{
	const ParsedAction pa = parseActionYaml(yaml);
	out->actor = unit;
	out->type = pa.type;
	if (pa.hasTarget)
		out->target = pa.target;
	out->finalFacing = pa.finalFacing;
	out->kneel = pa.kneel;
	out->run = pa.run;
	out->waypoints.clear();
	for (const Position& wp : pa.waypoints)
		out->waypoints.push_back(wp);

	BattleItem* weapon = nullptr;
	const std::vector<BattleItem*>* inv = unit->getInventory();
	if (pa.weaponId >= 0 && inv)
	{
		for (BattleItem* it : *inv)
		{
			if (it && it->getId() == pa.weaponId) { weapon = it; break; }
		}
	}
	if (!weapon)
		weapon = unit->getMainHandWeapon();
	out->weapon = weapon;

	if (isAttackType(out->type) && out->weapon == nullptr)
	{
		out->type = BA_NONE;
		out->result = "restai: no usable weapon for requested attack";
	}
}

int tileDistance(const Position& a, const Position& b)
{
	int dx = std::abs(a.x - b.x), dy = std::abs(a.y - b.y), dz = std::abs(a.z - b.z);
	int flat = dx > dy ? dx : dy;      // Chebyshev on the horizontal plane
	return flat > dz ? flat : dz;
}

/// Answers a "map" query: where the acting unit can move (reachable tiles + TU), nearby units, and
/// hazards. Computed on the main thread from live game state.
std::string writeVisibleMap(BattlescapeGame* game, BattleUnit* unit)
{
	SavedBattleGame* save = game->getSave();
	const Position uPos = unit->getPosition();
	const int viewRadius = 12;

	YAML::YamlRootNodeWriter w(16384);
	w.setAsMap();
	YAML::YamlNodeWriter m = w["map"];
	m.setAsMap();

	YAML::YamlNodeWriter un = m["unit"];
	un.setAsMap();
	un.write("id", unit->getId());
	writePosition(un, "position", uPos);
	un.write("tu", unit->getTimeUnits());
	un.write("energy", unit->getEnergy());

	// Reachable tiles this turn (where the unit can walk), capped to keep the payload bounded.
	std::vector<int> reachable = save->getPathfinding()->findReachable(unit, BattleActionCost());
	YAML::YamlNodeWriter rw = m["reachable"];
	rw.setAsMap();
	rw.write("count", (int)reachable.size());
	const size_t cap = 400;
	rw.write("truncated", reachable.size() > cap);
	YAML::YamlNodeWriter tiles = rw["tiles"];
	tiles.setAsSeq();
	size_t emitted = 0;
	for (int idx : reachable)
	{
		if (emitted++ >= cap) break;
		Position p = save->getTileCoords(idx);
		YAML::YamlNodeWriter e = tiles.write();
		e.setAsMap();
		e.setFlowStyle();
		e.write("x", (int)p.x);
		e.write("y", (int)p.y);
		e.write("z", (int)p.z);
	}

	// Units within view radius (allies + enemies), so the LLM can reason spatially.
	YAML::YamlNodeWriter nu = m["nearbyUnits"];
	nu.setAsSeq();
	for (BattleUnit* u : *save->getUnits())
	{
		if (!u || u->isOut() || u == unit) continue;
		const int d = tileDistance(u->getPosition(), uPos);
		if (d > viewRadius) continue;
		YAML::YamlNodeWriter e = nu.write();
		e.setAsMap();
		e.setFlowStyle();
		e.write("id", u->getId());
		e.write("type", u->getType());
		e.write("faction", factionName(u->getFaction()));
		writePosition(e, "position", u->getPosition());
		e.write("distance", d);
	}

	// Hazards (fire / smoke) within view radius.
	YAML::YamlNodeWriter hz = m["hazards"];
	hz.setAsSeq();
	for (int dz = -1; dz <= 1; ++dz)
	{
		const int z = uPos.z + dz;
		if (z < 0 || z >= save->getMapSizeZ()) continue;
		for (int dx = -viewRadius; dx <= viewRadius; ++dx)
		{
			for (int dy = -viewRadius; dy <= viewRadius; ++dy)
			{
				const Position p(uPos.x + dx, uPos.y + dy, z);
				Tile* t = save->getTile(p);
				if (!t) continue;
				const int fire = t->getFire();
				const int smoke = t->getSmoke();
				if (fire == 0 && smoke == 0) continue;
				YAML::YamlNodeWriter e = hz.write();
				e.setAsMap();
				e.setFlowStyle();
				e.write("x", (int)p.x);
				e.write("y", (int)p.y);
				e.write("z", (int)p.z);
				if (fire) e.write("fire", fire);
				if (smoke) e.write("smoke", smoke);
			}
		}
	}

	return w.emit().yaml;
}

/// Answers a "validate" query: is the proposed action legal for the acting unit, and what does it
/// cost? Reuses the same pathfinding / line-of-fire / throw checks the built-in AI uses.
std::string validateAction(BattlescapeGame* game, BattleUnit* unit, const std::string& actionYaml)
{
	SavedBattleGame* save = game->getSave();
	const ParsedAction pa = parseActionYaml(actionYaml);

	BattleItem* weapon = nullptr;
	if (pa.weaponId >= 0 && unit->getInventory())
	{
		for (BattleItem* it : *unit->getInventory())
			if (it && it->getId() == pa.weaponId) { weapon = it; break; }
	}
	if (!weapon) weapon = unit->getMainHandWeapon();

	const int tuAvail = unit->getTimeUnits();
	bool valid = false;
	int tuCost = -1;
	std::string reason;

	if (pa.type == BA_NONE)
	{
		valid = true;
		reason = "idle is always allowed";
	}
	else if (pa.type == BA_WALK)
	{
		if (!pa.hasTarget)
		{
			reason = "WALK needs a target";
		}
		else
		{
			Pathfinding* pf = save->getPathfinding();
			pf->calculate(unit, pa.target, BAM_NORMAL);
			if (pf->getStartDirection() == -1)
			{
				reason = "no path to target";
			}
			else
			{
				tuCost = pf->getTotalTUCost();
				valid = tuCost <= tuAvail;
				reason = valid ? "reachable" : "path found but not enough TU";
			}
			pf->abortPath();
		}
	}
	else if (pa.type == BA_SNAPSHOT || pa.type == BA_AUTOSHOT || pa.type == BA_AIMEDSHOT)
	{
		if (!weapon)
		{
			reason = "no weapon";
		}
		else if (!pa.hasTarget)
		{
			reason = "shot needs a target";
		}
		else
		{
			BattleActionCost cost(pa.type, unit, weapon);
			tuCost = cost.Time;
			const bool afford = cost.haveTU();
			Tile* tt = save->getTile(pa.target);
			BattleUnit* target = tt ? tt->getUnit() : nullptr;
			bool lof = false;
			if (target)
			{
				Position origin = save->getTileEngine()->getSightOriginVoxel(unit);
				Position scan;
				lof = save->getTileEngine()->canTargetUnit(&origin, tt, &scan, unit, false, target);
			}
			valid = afford && lof;
			reason = !afford ? "not enough TU"
				: (!target ? "no unit at target tile" : (lof ? "clear shot" : "no line of fire"));
		}
	}
	else if (pa.type == BA_THROW)
	{
		if (!weapon)
		{
			reason = "no item to throw";
		}
		else if (!pa.hasTarget)
		{
			reason = "throw needs a target";
		}
		else
		{
			BattleActionCost cost(BA_THROW, unit, weapon);
			tuCost = cost.Time;
			const bool afford = cost.haveTU();
			BattleAction act;
			act.actor = unit;
			act.weapon = weapon;
			act.type = BA_THROW;
			act.target = pa.target;
			Position origin = save->getTileEngine()->getOriginVoxel(act, 0);
			Tile* tt = save->getTile(pa.target);
			bool throwable = false;
			if (tt)
			{
				Position targetVoxel = pa.target.toVoxel() + Position(8, 8, 2 - tt->getTerrainLevel());
				throwable = save->getTileEngine()->validateThrow(act, origin, targetVoxel, save->getDepth());
			}
			valid = afford && throwable;
			reason = !afford ? "not enough TU" : (throwable ? "can throw" : "cannot reach with throw");
		}
	}
	else
	{
		// HIT / USE / LAUNCH / MINDCONTROL / PANIC: report TU affordability only (no geometry check).
		if (!weapon && isAttackType(pa.type))
		{
			reason = "no weapon";
		}
		else
		{
			BattleActionCost cost(pa.type, unit, weapon);
			tuCost = cost.Time;
			valid = cost.haveTU();
			reason = valid ? "affordable (geometry not checked for this action type)" : "not enough TU";
		}
	}

	YAML::YamlRootNodeWriter w(2048);
	w.setAsMap();
	YAML::YamlNodeWriter v = w["validation"];
	v.setAsMap();
	v.write("type", actionTypeName(pa.type));
	if (pa.hasTarget) writePosition(v, "target", pa.target);
	v.write("valid", valid);
	v.write("reason", reason);
	v.write("tuAvailable", tuAvail);
	if (tuCost >= 0) v.write("tuCost", tuCost);
	return w.emit().yaml;
}

std::string answerQuery(BattlescapeGame* game, BattleUnit* unit, const std::string& kind,
	const std::string& body)
{
	if (kind == "map") return writeVisibleMap(game, unit);
	if (kind == "validate") return validateAction(game, unit, body);
	return "error: unknown query kind\n";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace RestAiServer
{

bool enabled()
{
	return hasFlag("restai");
}

bool serverModeRequested()
{
	return hasFlag("restserver");
}

int configuredPort()
{
	return argInt("restai-port", 8765);
}

int timeoutMs()
{
	int t = argInt("restai-timeout", 8000);
	return t < 0 ? 0 : t;
}

int start(int port)
{
	return startImpl(port, false); // interactive play: no mock-harness endpoints
}

int start()
{
	return start(configuredPort());
}

void stop()
{
	ServerState& s = S();
	if (!s.running)
	{
		return;
	}
	if (s.svr)
	{
		s.svr->stop();
	}
	if (s.thread.joinable())
	{
		s.thread.join();
	}
	s.running = false;
	s.boundPort = -1;
	s.svr.reset();
	{
		std::lock_guard<std::mutex> lk(s.mx);
		s.hasRequest = false;
		s.hasAction = false;
		s.requestYaml.clear();
		s.actionYaml.clear();
	}
	Log(LOG_INFO) << "[restai] REST AI server stopped";
}

bool isRunning()
{
	return S().running;
}

int boundPort()
{
	return S().boundPort;
}

void publishRequest(const std::string& requestYaml)
{
	ServerState& s = S();
	std::lock_guard<std::mutex> lk(s.mx);
	s.requestYaml = requestYaml;
	s.hasRequest = true;
	s.hasAction = false; // a fresh request invalidates any stale action
	s.actionYaml.clear();
}

void clearRequest()
{
	ServerState& s = S();
	std::lock_guard<std::mutex> lk(s.mx);
	s.hasRequest = false;
	s.requestYaml.clear();
}

bool waitForAction(std::string& actionYamlOut, int timeoutMs)
{
	ServerState& s = S();
	const auto start = std::chrono::steady_clock::now();
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lk(s.mx);
			if (s.hasAction)
			{
				actionYamlOut = s.actionYaml;
				s.hasAction = false;
				return true;
			}
			// Sleep briefly, but wake immediately when POST /action notifies us.
			s.cv.wait_for(lk, std::chrono::milliseconds(2));
			if (s.hasAction)
			{
				actionYamlOut = s.actionYaml;
				s.hasAction = false;
				return true;
			}
		}

		// Keep the OS window responsive during the wait (no-op when running headless / pre-video).
		if (SDL_WasInit(SDL_INIT_VIDEO))
		{
			SDL_PumpEvents();
		}

		if (timeoutMs > 0)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			if (elapsed >= timeoutMs)
			{
				return false;
			}
		}
	}
}

bool decide(BattlescapeGame* game, BattleUnit* unit, BattleAction* out)
{
	if (!isRunning() || !game || !unit || !out)
	{
		return false; // server not up / bad args -> let the caller use the built-in AI
	}

	ServerState& s = S();
	publishRequest(writeRequest(game, unit));
	s.decisionActive = true; // enable /map and /validate queries about this unit

	const int tmo = timeoutMs();
	const auto start = std::chrono::steady_clock::now();
	std::string actionYaml;
	bool got = false;

	for (;;)
	{
		std::string queryKind, queryBody;
		{
			std::unique_lock<std::mutex> lk(s.mx);
			if (s.hasAction)
			{
				actionYaml = s.actionYaml;
				s.hasAction = false;
				got = true;
			}
			else if (s.hasQuery)
			{
				queryKind = s.queryKind;   // service it below, outside the lock (pathfinding etc.)
				queryBody = s.queryBody;
			}
			else
			{
				s.cv.wait_for(lk, std::chrono::milliseconds(2));
				if (s.hasAction)
				{
					actionYaml = s.actionYaml;
					s.hasAction = false;
					got = true;
				}
			}
		}

		if (got)
		{
			break;
		}

		if (!queryKind.empty())
		{
			// Answer the query from live game state (safe: only the main thread touches it), then
			// hand the result back to the waiting server thread.
			const std::string answer = answerQuery(game, unit, queryKind, queryBody);
			{
				std::lock_guard<std::mutex> lk(s.mx);
				s.queryAnswer = answer;
				s.hasQuery = false;
				s.queryDone = true;
			}
			s.qcv.notify_all();
			continue;
		}

		if (SDL_WasInit(SDL_INIT_VIDEO))
		{
			SDL_PumpEvents(); // keep the game window responsive while waiting
		}
		if (tmo > 0)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			if (elapsed >= tmo)
			{
				break;
			}
		}
	}

	s.decisionActive = false;
	{
		std::lock_guard<std::mutex> lk(s.mx); // discard any orphaned in-flight query
		s.hasQuery = false;
		s.queryDone = false;
	}
	clearRequest();
	if (!got)
	{
		Log(LOG_INFO) << "[restai] decision timed out for unit #" << unit->getId()
					  << "; falling back to the built-in AI";
		return false;
	}

	applyAction(actionYaml, unit, out);
	return true;
}

int runServerMode()
{
	CrossPlatform::ensureConsoleOutput();
	const int port = startImpl(configuredPort(), true); // standalone: expose mock-harness endpoints
	if (port <= 0)
	{
		Log(LOG_ERROR) << "[restserver] could not start the REST server on port " << configuredPort();
		return EXIT_FAILURE;
	}

	// Safety deadline so an automated run can never hang forever if the client forgets /shutdown.
	const int maxSeconds = argInt("restserver-seconds", 30);
	Log(LOG_INFO) << "[restserver] standalone REST server ready on http://127.0.0.1:" << port
				  << " (POST /shutdown to stop; auto-exit after " << maxSeconds << "s)";

	const auto started = std::chrono::steady_clock::now();
	while (!S().shutdownReq)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (maxSeconds > 0)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - started).count();
			if (elapsed >= maxSeconds)
			{
				Log(LOG_INFO) << "[restserver] safety deadline reached; exiting";
				break;
			}
		}
	}

	stop();
	return EXIT_SUCCESS;
}

} // namespace RestAiServer

// ---------------------------------------------------------------------------
// Self-tests (live in this always-linked TU, like Verify's; run via --selftest).
// ---------------------------------------------------------------------------

OXC_SELFTEST(restai_server_loopback)
{
	const int port = RestAiServer::start(0); // 0 = ephemeral port, avoids collisions in CI
	ctx.check(port > 0, "server failed to bind an ephemeral port");
	if (port <= 0)
	{
		return;
	}

	httplib::Client cli("127.0.0.1", port);
	cli.set_connection_timeout(2, 0);
	cli.set_read_timeout(2, 0);

	// /health is always available.
	auto health = cli.Get("/health");
	ctx.check(health && health->status == 200, "GET /health did not return 200");

	// With nothing pending, /pending-decision is 204 No Content.
	RestAiServer::clearRequest();
	auto pd0 = cli.Get("/pending-decision");
	ctx.check(pd0 && pd0->status == 204, "GET /pending-decision should be 204 when idle");

	// Publish a request on the main-thread side; the server should now serve it verbatim.
	RestAiServer::publishRequest("request:\n  probe: hello-world\n");
	auto pd1 = cli.Get("/pending-decision");
	ctx.check(pd1 && pd1->status == 200 && pd1->body.find("hello-world") != std::string::npos,
		"GET /pending-decision should return the published request body");

	// Submit an action; the main-thread waiter should receive it (already delivered -> no blocking).
	auto pa = cli.Post("/action", "action:\n  type: NONE\n", "application/x-yaml");
	ctx.check(pa && pa->status == 200, "POST /action was not accepted");

	std::string got;
	const bool ok = RestAiServer::waitForAction(got, 1000);
	ctx.check(ok, "waitForAction timed out despite a delivered action");
	ctx.check(got.find("NONE") != std::string::npos, "action body was not delivered to the main thread");

	// The live-query endpoints exist and correctly refuse (409) when no alien decision is active.
	auto mapResp = cli.Get("/map");
	ctx.check(mapResp && mapResp->status == 409, "GET /map is 409 when no decision is active");
	auto valResp = cli.Post("/validate", "action:\n  type: WALK\n", "application/x-yaml");
	ctx.check(valResp && valResp->status == 409, "POST /validate is 409 when no decision is active");

	RestAiServer::stop();
	ctx.check(!RestAiServer::isRunning(), "server did not stop cleanly");
}

OXC_SELFTEST(restai_wire_helpers)
{
	ctx.check(parseActionType("SNAPSHOT") == BA_SNAPSHOT, "parse SNAPSHOT");
	ctx.check(parseActionType("  snap ") == BA_SNAPSHOT, "parse snap synonym + whitespace");
	ctx.check(parseActionType("walk") == BA_WALK, "parse walk lowercase");
	ctx.check(parseActionType("move") == BA_WALK, "parse move synonym");
	ctx.check(parseActionType("MINDCONTROL") == BA_MINDCONTROL, "parse mindcontrol");
	ctx.check(parseActionType("") == BA_NONE, "parse empty -> none");
	ctx.check(parseActionType("wat") == BA_NONE, "parse unknown -> none");
	ctx.check(parseActionType("8") == BA_SNAPSHOT, "parse numeric 8 -> snapshot");

	ctx.check(std::string(actionTypeName(BA_WALK)) == "WALK", "name walk");
	ctx.check(std::string(actionTypeName(BA_AIMEDSHOT)) == "AIMEDSHOT", "name aimed");
	ctx.check(parseActionType(actionTypeName(BA_LAUNCH)) == BA_LAUNCH, "name<->parse round trip");
	ctx.check(std::string(factionName(FACTION_HOSTILE)) == "HOSTILE", "faction hostile name");

	const Position p = readPosition(
		YAML::YamlRootNodeReader(YAML::YamlString("{x: 3, y: 4, z: 5}"), "p").toBase());
	ctx.check(p.x == 3 && p.y == 4 && p.z == 5, "readPosition {x,y,z}");
}

OXC_SELFTEST(restai_action_parse)
{
	{
		const std::string y =
			"action:\n"
			"  type: SNAPSHOT\n"
			"  target: {x: 8, y: 9, z: 1}\n"
			"  weapon: 2001\n"
			"  kneel: true\n";
		const ParsedAction pa = parseActionYaml(y);
		ctx.check(pa.type == BA_SNAPSHOT, "snapshot type");
		ctx.check(pa.hasTarget && pa.target.x == 8 && pa.target.y == 9 && pa.target.z == 1, "snapshot target");
		ctx.check(pa.weaponId == 2001, "snapshot weapon id");
		ctx.check(pa.kneel, "snapshot kneel flag");
		ctx.check(!pa.run, "snapshot run defaults false");
	}
	{
		const std::string y =
			"action:\n"
			"  type: WALK\n"
			"  target: {x: 2, y: 3, z: 0}\n"
			"  run: true\n";
		const ParsedAction pa = parseActionYaml(y);
		ctx.check(pa.type == BA_WALK, "walk type");
		ctx.check(pa.hasTarget && pa.target.x == 2 && pa.target.y == 3, "walk target");
		ctx.check(pa.run, "walk run flag");
		ctx.check(pa.weaponId == -1, "walk no weapon id");
	}
	{
		const std::string y =
			"action:\n"
			"  type: LAUNCH\n"
			"  waypoints:\n"
			"    - {x: 1, y: 1, z: 0}\n"
			"    - {x: 5, y: 6, z: 0}\n";
		const ParsedAction pa = parseActionYaml(y);
		ctx.check(pa.type == BA_LAUNCH, "launch type");
		ctx.check(pa.waypoints.size() == 2, "launch waypoint count");
		if (pa.waypoints.size() == 2)
		{
			ctx.check(pa.waypoints[1].x == 5 && pa.waypoints[1].y == 6, "launch waypoint values");
		}
	}
	{
		// Malformed input must not crash the engine; it degrades to an idle (BA_NONE) action.
		const ParsedAction pa = parseActionYaml("this is: not: valid: yaml: [[[");
		ctx.check(pa.type == BA_NONE, "malformed -> none");
	}
	{
		// Missing action node -> idle.
		const ParsedAction pa = parseActionYaml("something: else\n");
		ctx.check(pa.type == BA_NONE, "missing action node -> none");
	}
	{
		// A raw enum int for type, and unknown fields, are tolerated.
		const std::string y =
			"action:\n"
			"  type: 2\n"                 // BA_WALK
			"  target: {x: 1, y: 1, z: 0}\n"
			"  somethingUnknown: 42\n";
		const ParsedAction pa = parseActionYaml(y);
		ctx.check(pa.type == BA_WALK, "numeric type 2 -> WALK");
		ctx.check(pa.hasTarget && pa.target.x == 1, "numeric-type action still has target");
	}
}

OXC_SELFTEST(restai_sample_request)
{
	// The payload we serve at GET /sample-request must be valid YAML with the documented shape.
	YAML::YamlRootNodeReader r(YAML::YamlString(sampleRequestYaml()), "sample");
	YAML::YamlNodeReader req = r["request"];
	ctx.check((bool)req, "sample has a request node");

	int turn = -1;
	ctx.check(req.tryRead("turn", turn) && turn == 3, "sample turn == 3");
	std::string side;
	ctx.check(req.tryRead("side", side) && side == "HOSTILE", "sample side == HOSTILE");

	YAML::YamlNodeReader unit = req["unit"];
	ctx.check((bool)unit, "sample has a unit node");
	int id = 0;
	ctx.check(unit.tryRead("id", id) && id == 1000123, "sample unit id");
	YAML::YamlNodeReader items = unit["items"];
	ctx.check(items && items.isSeq() && items.childrenCount() == 2, "sample has two items");
}

}
