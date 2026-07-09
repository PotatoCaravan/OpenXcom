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
   > "You are commanding the aliens. Call `wait_for_alien_decision`; when a unit needs orders, read
   > its situation and call `submit_alien_action` with a sensible move or attack. Repeat until it's
   > the human's turn again, then wait."

   The engine blocks (waiting) on each alien until the LLM answers, so play is turn-based and the
   LLM is never rushed.

## Capabilities & limits

The REST API is an **alien-decision** API, not full player parity:

- Controls **alien-side units on the alien turn** only (not your soldiers).
- Roughly **two actions per unit per activation** (the engine's AI cadence), not free TU-budget
  sequencing.
- Supported actions: `WALK` (+`run`), `KNEEL`, `TURN`/`final_facing`, `SNAPSHOT`/`AUTOSHOT`/
  `AIMEDSHOT`, `THROW`, `HIT`, `USE`, `LAUNCH` (+waypoints), `MINDCONTROL`, `PANIC`, `NONE`.
- **Partial observability:** the request contains the acting unit, its inventory, visible enemies
  and the map size — but not the full map, reachable tiles, or valid fire solutions. The LLM must
  infer legality; **illegal actions are ignored by the engine** (the unit simply idles), so expect
  some wasted moves until the payload is enriched.

See `../docs/REST_AI.md` for the full API and schemas.

## Testing

```powershell
python mcp-server/openxcom_ai_mcp.py --self-check   # offline: protocol + YAML builder
./scripts/test-mcp.ps1                               # end-to-end: MCP over stdio -> engine and back
```
