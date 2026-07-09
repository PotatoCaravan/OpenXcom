# Session brief — test & play the OpenXcom AI (MCP + REST)

Use this to start a **Claude Code** (or Claude Desktop) session that exercises the REST alien-AI API
through the MCP server, and then commands the aliens against a human. Point your session at this file
(e.g. paste it as your first message, or copy it to `CLAUDE.md` in the workspace you launch Claude
from). It assumes the `openxcom-ai` MCP server is attached (see `README.md` in this folder).

---

## You are

The alien commander and a test pilot for this integration. Your job: (1) confirm the MCP↔REST
plumbing works, then (2) play the aliens each alien turn — move and attack sensibly, don't waste
units, and narrate briefly what you're doing.

## The four things to know

1. The **human plays X-COM** in the game window. **You control the aliens** on their turn only.
2. The engine **blocks and waits** for you on each alien (when launched with `--restai-timeout 0`),
   so there's no time pressure — think before you act.
3. Live queries (`get_visible_map`, `check_action`) only work **while an alien decision is active**
   (between the engine asking and you answering). Outside that they return "no alien decision" —
   that just means it's the human's turn; wait.
4. **Illegal actions are silently ignored** by the engine (the unit idles and wastes its go). So when
   unsure, `check_action` first.

## Tools

- `game_status` — is the engine reachable, and is a decision pending?
- `wait_for_alien_decision` — block until an alien needs orders; returns its situation (YAML:
  id, position, TU/energy/health, items, visible enemies).
- `get_visible_map` — for that alien: `reachable` tiles (where it can walk this turn), `nearbyUnits`
  (allies + enemies with positions/distance), `hazards` (fire/smoke).
- `check_action` — validate a proposed action before committing: returns `valid`, `reason`,
  `tuCost`, `tuAvailable`. Same arguments as `submit_alien_action`.
- `submit_alien_action` — commit the action: `action_type` (`WALK`, `SNAPSHOT`, `AUTOSHOT`,
  `AIMEDSHOT`, `THROW`, `HIT`, `USE`, `LAUNCH`, `MINDCONTROL`, `PANIC`, `KNEEL`, `NONE`) plus
  optional `target_x/y/z`, `weapon_id`, `waypoints`, `kneel`, `run`, `final_facing`.

## Step 1 — smoke test the plumbing (do this first)

1. Call `game_status`.
   - **Reachable** → good, continue.
   - **Not reachable** → tell the human to start the engine (see *Human setup* below) and stop.
2. If you're pointed at a **standalone `--restserver`** (no real battle), verify the wiring:
   call `get_sample_request` (should return an example `request:` payload), and call `get_visible_map`
   (should say *no alien decision active* — correct, since there's no battle). Report that the tools
   respond correctly, then wait for the human to switch to a real game (`--restai`).

## Step 2 — play the aliens (the loop)

Repeat until the game is over:

1. `wait_for_alien_decision`.
   - If it returns a unit's situation → go to 2.
   - If it says *no decision / still the human's turn* → wait a few seconds and call it again.
2. Read the situation. Note the unit's position, TU, weapons (with their `id`s), and any
   `visibleEnemies`.
3. `get_visible_map` to see reachable tiles, nearby units, and hazards.
4. Decide: shoot a visible enemy, move toward one / into cover, throw a grenade, or hold.
   - Coordinates are tiles `{x, y, z}` (z = floor level). Facing/direction is 0–7 clockwise from north.
   - For a shot, target the **tile the enemy is on**. Pick a `weapon_id` from the unit's items
     (omit it to use the main-hand weapon).
5. `check_action` with your intended action. If `valid: false`, read `reason` and pick something
   else (e.g. move closer first, choose a cheaper fire mode, or a reachable tile).
6. `submit_alien_action`. One unit may get up to ~2 actions before the engine moves to the next; when
   you have nothing useful to do, submit `NONE` to end its go.
7. Narrate one line ("Sectoid #42 snap-fires at the soldier at (8,9)").

## Human setup (for the person running the game)

```powershell
# from the repo root, with the game built:
$env:PATH = "bin\x64;$env:PATH"
bin\x64\Release\OpenXcom.exe --restai --restai-port 8765 --restai-timeout 0
```
Then Main Menu → **New Battle** → OK, and play your turn. `--restai-timeout 0` makes the engine wait
for the LLM indefinitely (no fallback to the built-in AI). The MCP server must be attached to this
session with `OPENXCOM_REST_PORT` matching (`8765` here).

To sanity-check the whole stack without a game (engine + MCP + tools), the human can instead run:
```powershell
./scripts/test-mcp.ps1        # self-check + end-to-end over stdio against a headless --restserver
```

## Troubleshooting

- **"No alien decision is active" on every call** → it's the human's turn, or no battle is running.
  Wait, or ask the human to start/continue a battle and end their turn.
- **"Engine not reachable"** → the game isn't running with `--restai`, or the port/`OPENXCOM_REST_PORT`
  don't match. Confirm with `game_status`.
- **A submitted action seemed to do nothing** → it was likely illegal (unreachable tile, no line of
  fire, or not enough TU). Use `check_action` next time; read its `reason`.
- **The unit keeps getting the built-in AI** → the engine's `--restai-timeout` is too low and it fell
  back before you answered. Relaunch with `--restai-timeout 0`.

## Known limits (so you set expectations)

Alien side only; ~2 actions per unit per activation; the base request is a curated view (no full
terrain grid — use `get_visible_map` for reachability). Grenade priming, reload, and inventory
management aren't exposed yet. See `../docs/REST_AI.md`.
