# OpenXcom AI — MCP server

Let an LLM play the **alien side** of an OpenXcom Extended battle against a human. The human plays
X-COM in the normal game window; the LLM drives the aliens on their turn through the engine's REST
alien-AI API (the `rest-ai-server` branch).

```
 LLM  <--MCP/stdio-->  openxcom_ai_mcp.py  <--HTTP/REST-->  OpenXcom.exe --restai
(Claude)                (this server)                        (the game + embedded server)
```

- **No dependencies.** Pure Python standard library (3.8+) — no `pip install`, no `mcp` package.
- The server is an MCP server to the LLM and an HTTP **client** to the engine.

## Tools exposed to the LLM

| Tool | What it does |
|------|--------------|
| `game_status` | Is the engine reachable, and is an alien decision pending right now? |
| `wait_for_alien_decision` | Block until it's the aliens' turn, then return the acting unit's situation (YAML). Times out during the human's turn — call again. |
| `get_visible_map` | For the current alien: reachable tiles (+ TU), nearby units (allies/enemies), and hazards (fire/smoke). |
| `check_action` | Ask whether a proposed action is legal and what it costs, *before* committing to it (reachability/TU/line-of-fire). |
| `submit_alien_action` | Submit the action for the alien the engine is waiting on (`action_type` plus optional `target_x/y/z`, `weapon_id`, `waypoints`, `kneel`, `run`, `final_facing`). |
| `get_sample_request` | A representative request payload (schema reference; standalone mock mode only). |

## Running a game

1. **Launch the engine with REST enabled**, and tell it to wait for the LLM rather than fall back
   to the built-in AI (`--restai-timeout 0` = wait forever, so the LLM has unlimited think time):

   ```powershell
   $env:PATH = "bin\x64;$env:PATH"
   bin\x64\Release\OpenXcom.exe --restai --restai-port 8765 --restai-timeout 0
   ```

2. **Start a New Battle** in the game (Main Menu → New Battle → OK). Play your X-COM turn normally.
   When you end your turn, the engine hands each alien's decision to the LLM.

3. **Register this MCP server with your LLM client.**

   **Claude Desktop** — add to `claude_desktop_config.json`:
   ```json
   {
     "mcpServers": {
       "openxcom-ai": {
         "command": "python",
         "args": ["B:\\OpenXcom Extended AI Mods\\source code\\OpenXcom\\mcp-server\\openxcom_ai_mcp.py"],
         "env": { "OPENXCOM_REST_HOST": "127.0.0.1", "OPENXCOM_REST_PORT": "8765" }
       }
     }
   }
   ```

   **Claude Code** — from the repo root:
   ```bash
   claude mcp add openxcom-ai -e OPENXCOM_REST_PORT=8765 -- python "mcp-server/openxcom_ai_mcp.py"
   ```

4. **Tell the LLM to play the aliens**, e.g.:
   > "You are commanding the aliens. Loop: call `wait_for_alien_decision`; when a unit needs orders,
   > use `get_visible_map` to see where it can move and who is nearby, `check_action` to confirm a
   > move/shot is legal, then `submit_alien_action`. Repeat until it's the human's turn, then wait."

   The engine blocks (waiting) on each alien until the LLM answers, so play is turn-based and the
   LLM is never rushed.

## Capabilities & limits

The REST API is an **alien-decision** API, not full player parity:

- Controls **alien-side units on the alien turn** only (not your soldiers).
- Roughly **two actions per unit per activation** (the engine's AI cadence), not free TU-budget
  sequencing.
- Supported actions: `WALK` (+`run`), `KNEEL`, `TURN`/`final_facing`, `SNAPSHOT`/`AUTOSHOT`/
  `AIMEDSHOT`, `THROW`, `HIT`, `USE`, `LAUNCH` (+waypoints), `MINDCONTROL`, `PANIC`, `NONE`.
- **Observability** comes from two live queries: `get_visible_map` (reachable tiles + TU, nearby
  units, hazards) and `check_action` (validate a move/shot: reachability, TU, line-of-fire) before
  committing. Illegal actions submitted anyway are ignored by the engine (the unit idles), so
  validate first when unsure. The base request is still a curated view (no full terrain grid yet).

See `../docs/REST_AI.md` for the full API and schemas.

## Testing

```powershell
python mcp-server/openxcom_ai_mcp.py --self-check   # offline: protocol + YAML builder
./scripts/test-mcp.ps1                               # end-to-end: MCP over stdio -> engine and back
```
