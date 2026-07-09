# REST-controlled alien AI (`rest-ai-server` branch)

This branch lets an **external webserver drive the alien battlescape AI over REST** instead of the
built-in `AIModule`. The engine embeds a small HTTP **server**; your webserver calls in to fetch
the decision the engine is waiting on and to submit the action to take. It is a thin actuator —
all the intelligence lives in your webserver.

> Security is intentionally out of scope: the server binds `0.0.0.0`, has no auth, and speaks plain
> HTTP. Run it on a trusted network / behind your own webserver.

Everything here is a fork addition, tagged `// [AI-MODS]`; see `docs/UPSTREAM_SYNC.md` for the
exact touch-points.

---

## How it hooks in

The engine's single decision point — `BattlescapeGame::handleAI()` → `unit->think(&action)` — is
replaced (only when REST mode is on) by a call that asks the webserver. The returned action is a
normal `BattleAction`, so the existing engine machinery (pathfinding, TU spend, reaction fire,
turn rotation) runs unchanged:

```cpp
// src/Battlescape/BattlescapeGame.cpp, handleAI()
if (!RestAiServer::enabled() || !RestAiServer::decide(this, unit, &action))
{
    unit->think(&action); // built-in AI: disabled, or REST timed out / errored
}
```

The built-in `AIModule` is **not removed** — it is the fallback. If REST mode is off, or the
webserver does not answer within the timeout, the alien uses the built-in AI and the game never
hangs.

### Threading

The engine is single-threaded around the SDL loop. The HTTP server runs on its **own background
thread** and only touches a mutex-guarded string mailbox — never the live `SavedBattleGame`. All
game-state (de)serialization happens on the main thread inside `decide()`. While waiting for an
action the main thread pumps `SDL_PumpEvents()` so the window stays responsive.

---

## Command-line flags

| Flag | Meaning | Default |
|------|---------|---------|
| `--restai` | Enable REST-controlled alien AI for this session (interactive play). | off |
| `--restai-port <n>` | Port the server listens on. | `8765` |
| `--restai-timeout <ms>` | How long `decide()` waits for an action before falling back to the built-in AI. `0` = wait forever. | `8000` |
| `--restserver` | Run the REST endpoint **standalone / headless** (no window, no game data) for integration-testing your webserver against a real engine build. | off |
| `--restserver-seconds <n>` | Safety auto-exit for `--restserver` if no `POST /shutdown` arrives. | `30` |

---

## Endpoints

| Method + path | Purpose | Response |
|---------------|---------|----------|
| `GET /health` | Liveness probe. | `200` `{"status":"ok"}` |
| `GET /pending-decision` | The decision the engine is currently waiting on (request YAML), or nothing. | `200` + YAML, or `204` when idle |
| `POST /action` | Submit the chosen action (action YAML in the body); unblocks the waiting alien. | `200` `{"status":"accepted"}` |
| `POST /shutdown` | Ask a standalone `--restserver` process to exit. | `200` `{"status":"shutting-down"}` |

Content type is `application/x-yaml`. `GET /state` and `POST /start-battle` are reserved for a
later iteration (see *Limitations*).

**Mock-harness endpoints** (standalone `--restserver` only — not exposed during interactive play,
so they can't interfere with a real battle's exchange). They let you develop and integration-test a
webserver against a real engine build without a live battle:

| Method + path | Purpose |
|---------------|---------|
| `POST /publish` | Inject a pending decision (body = request YAML), as if the engine were waiting on one. |
| `GET /last-action` | Read back the most recently submitted action (to verify a round trip). |
| `GET /sample-request` | A representative request payload, so you can see the exact schema. |

A full cross-process round trip is therefore: `POST /publish` → `GET /pending-decision` (returns it)
→ `POST /action` → `GET /last-action` (echoes it). `scripts/run-restai.ps1` exercises exactly this.

### Request payload (`GET /pending-decision`)

A curated, compact view (not a raw save dump), v1:

```yaml
request:
  turn: 3
  side: HOSTILE
  difficulty: 2
  map: {sizeX: 60, sizeY: 60, sizeZ: 4}
  unit:
    id: 1000123
    type: STR_SECTOID_SOLDIER
    position: {x: 10, y: 12, z: 1}
    direction: 4
    tu: 54
    energy: 60
    health: 30
    kneeling: false
    items:                        # ids let you reference a weapon in the action
      - {id: 2001, type: STR_PLASMA_RIFLE, slot: STR_RIGHT_HAND, ammo: 20}
      - {id: 2002, type: STR_ALIEN_GRENADE, slot: STR_BELT}
  visibleEnemies:
    - {id: 500, type: STR_SOLDIER, faction: PLAYER, position: {x: 8, y: 9, z: 1}}
```

### Action payload (`POST /action`)

```yaml
action:
  type: SNAPSHOT       # NONE | WALK | SNAPSHOT | AUTOSHOT | AIMEDSHOT | THROW | HIT | USE | LAUNCH | MINDCONTROL | PANIC
  target: {x: 8, y: 9, z: 1}
  weapon: 2001         # BattleItem id from the request's items; optional -> main-hand weapon
  waypoints: []        # for LAUNCH (blaster path)
  finalFacing: -1      # direction to face after moving; -1 = none
  kneel: false
  run: false
```

- `type` accepts a name (case-insensitive, a few synonyms like `SNAP`/`MOVE`/`MELEE`) or the raw
  enum integer.
- Robustness: malformed YAML, a missing `action` node, or an attack with no usable weapon all
  degrade safely to an idle (`NONE`) action — the engine will not crash on bad input.
- `NONE` makes the alien idle (ends its activation), which is the simplest valid answer.

---

## Running it

### Interactive (drive a real battle)

```powershell
$env:PATH = "bin\x64;$env:PATH"
bin\x64\Release\OpenXcom.exe --restai --restai-port 8765
```

Start a **New Battle** from the menu. On the alien turn the engine will serve `GET
/pending-decision` for each alien and wait for your `POST /action`. Drive it by hand with `curl`,
from your webserver, or with the bundled mock brain:

```powershell
python scripts/mock-brain.py --port 8765 --action NONE --verbose
```

### Headless server (integration-test your webserver)

```powershell
./scripts/run-restai.ps1 -Port 8791          # autonomous smoke: launches --restserver + checks it
```

or launch it yourself and poke the endpoints:

```powershell
bin\x64\Release\OpenXcom.exe --restserver --restai-port 8765 --restserver-seconds 60
curl http://127.0.0.1:8765/health
curl -X POST http://127.0.0.1:8765/shutdown
```

---

## Verifying a change

- `./scripts/build-check.ps1` — compile gate.
- `./scripts/run-selftest.ps1` — includes `restai_server_loopback` (server + exchange + wire
  round-trip, in-process), `restai_wire_helpers`, and `restai_action_parse`. No game data needed.
- `./scripts/run-restai.ps1` — external-process smoke against `--restserver`. No game data needed.
- Live end-to-end: `--restai` + a New Battle + `scripts/mock-brain.py` (needs game data).

---

## Limitations / next steps

- **No headless auto-battle yet.** A battle still starts through the normal UI (New Battle). The
  `--restserver` mode runs the server standalone without a battle (so `/pending-decision` is always
  `204` there); use `--restai` interactively for live decisions. A programmatic quick-battle
  bring-up and the `GET /state` / `POST /start-battle` lifecycle endpoints are the planned next
  iteration.
- **v1 request** ships raw state and trusts the brain to pick a legal move; server-side enumeration
  of legal moves (reachable tiles, valid shots) is a future extension.
- `--restserver` needs no game data; the live path does.
