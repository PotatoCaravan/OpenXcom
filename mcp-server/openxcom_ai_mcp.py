#!/usr/bin/env python3
# [AI-MODS] MCP server that lets an LLM play the ALIEN side of an OpenXcom Extended battle by
# driving the engine's REST alien-AI API (the rest-ai-server branch). The human plays X-COM in the
# normal GUI; the LLM controls the aliens on their turn.
#
# Transport: Model Context Protocol over stdio (newline-delimited JSON-RPC 2.0). Implemented with
# only the Python standard library -- no `mcp` package or pip install needed, so it runs under any
# Python 3.8+. Point your MCP client (Claude Desktop / Claude Code) at this file; see
# mcp-server/README.md for the config and how to run a game.
#
# The engine is the HTTP server; this process is an HTTP *client* to it (and an MCP server to the
# LLM). Configure the engine endpoint with OPENXCOM_REST_HOST / OPENXCOM_REST_PORT.

import json
import os
import sys
import time
import urllib.error
import urllib.request

SERVER_NAME = "openxcom-ai"
SERVER_VERSION = "0.1.0"
DEFAULT_PROTOCOL = "2024-11-05"

ACTION_TYPES = [
    "NONE", "WALK", "SNAPSHOT", "AUTOSHOT", "AIMEDSHOT", "THROW",
    "HIT", "USE", "LAUNCH", "MINDCONTROL", "PANIC", "KNEEL", "TURN",
]


def log(msg):
    # MCP uses stdout for the protocol, so all diagnostics MUST go to stderr.
    print("[openxcom-ai-mcp] %s" % msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# REST client (talks to the engine's rest-ai-server API)
# ---------------------------------------------------------------------------

class RestClient:
    def __init__(self, host, port):
        self.base = "http://%s:%d" % (host, port)

    def _req(self, method, path, body=None, timeout=5):
        data = body.encode("utf-8") if body is not None else None
        req = urllib.request.Request(
            self.base + path, data=data, method=method,
            headers={"Content-Type": "application/x-yaml"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except urllib.error.URLError as e:
            return 0, "connection error: %s" % (e.reason,)
        except Exception as e:  # noqa: BLE001 - report any transport failure as status 0
            return 0, "error: %s" % (e,)

    def health(self):
        return self._req("GET", "/health")

    def pending(self):
        return self._req("GET", "/pending-decision")

    def submit(self, action_yaml):
        return self._req("POST", "/action", action_yaml)

    def sample(self):
        return self._req("GET", "/sample-request")

    # Harness-only endpoints (standalone --restserver); used by the integration test.
    def publish(self, request_yaml):
        return self._req("POST", "/publish", request_yaml)

    def last_action(self):
        return self._req("GET", "/last-action")


# ---------------------------------------------------------------------------
# Action YAML builder
# ---------------------------------------------------------------------------

def build_action_yaml(args):
    """Turns submit_alien_action arguments into the engine's action YAML."""
    action_type = str(args.get("action_type", "NONE")).upper()
    lines = ["action:", "  type: %s" % action_type]

    tx, ty, tz = args.get("target_x"), args.get("target_y"), args.get("target_z")
    if tx is not None and ty is not None and tz is not None:
        lines.append("  target: {x: %d, y: %d, z: %d}" % (int(tx), int(ty), int(tz)))

    if args.get("weapon_id") is not None:
        lines.append("  weapon: %d" % int(args["weapon_id"]))

    waypoints = args.get("waypoints")
    if waypoints:
        lines.append("  waypoints:")
        for wp in waypoints:
            lines.append("    - {x: %d, y: %d, z: %d}" % (int(wp["x"]), int(wp["y"]), int(wp["z"])))

    if args.get("final_facing") is not None:
        lines.append("  finalFacing: %d" % int(args["final_facing"]))
    if args.get("kneel") is not None:
        lines.append("  kneel: %s" % ("true" if args["kneel"] else "false"))
    if args.get("run") is not None:
        lines.append("  run: %s" % ("true" if args["run"] else "false"))

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Tool implementations
# ---------------------------------------------------------------------------

def tool_game_status(client, _args):
    hs, _ = client.health()
    if hs != 200:
        return ("Engine NOT reachable at %s (status %s). Start OpenXcom with "
                "`--restai --restai-timeout 0` and begin a battle." % (client.base, hs))
    ps, pb = client.pending()
    if ps == 200:
        return "Engine reachable. A pending alien decision IS waiting:\n\n%s" % pb
    if ps == 204:
        return ("Engine reachable. No pending alien decision right now "
                "(it is not the alien turn, or no battle is running yet).")
    return "Engine reachable (health ok) but /pending-decision returned status %s." % ps


def tool_wait_for_alien_decision(client, args):
    timeout = float(args.get("timeout_seconds", 30))
    timeout = max(1.0, min(timeout, 120.0))
    deadline = time.time() + timeout
    while time.time() < deadline:
        ps, pb = client.pending()
        if ps == 200:
            return ("An alien needs a decision. Read the situation below, then call "
                    "submit_alien_action.\n\n%s" % pb)
        if ps == 0:
            return ("Engine not reachable at %s (%s). Start OpenXcom with "
                    "`--restai --restai-timeout 0` and begin a battle." % (client.base, pb))
        time.sleep(0.5)
    return ("No pending alien decision within %ds. It is probably still the human's turn -- "
            "wait and call wait_for_alien_decision again." % int(timeout))


def tool_submit_alien_action(client, args):
    if not args.get("action_type"):
        return "ERROR: action_type is required (e.g. WALK, SNAPSHOT, THROW, NONE)."
    action_yaml = build_action_yaml(args)
    s, b = client.submit(action_yaml)
    if s == 200:
        return "Action accepted by the engine:\n\n%s" % action_yaml
    return "Action POST failed (status %s): %s\nYAML sent was:\n%s" % (s, b, action_yaml)


def tool_get_sample_request(client, _args):
    s, b = client.sample()
    if s == 200:
        return b
    return ("/sample-request returned status %s. It is only available when the engine runs in "
            "standalone --restserver mock mode; during a live battle use wait_for_alien_decision "
            "to see the real request instead." % s)


TOOLS = {
    "game_status": {
        "fn": tool_game_status,
        "description": "Check whether the OpenXcom engine is reachable and whether an alien "
                       "decision is currently pending.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    "wait_for_alien_decision": {
        "fn": tool_wait_for_alien_decision,
        "description": "Block until the engine is waiting on an alien decision (i.e. it is the "
                       "aliens' turn), then return that unit's situation as YAML. Call this to get "
                       "your next move. If it times out it is still the human's turn -- call again.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "timeout_seconds": {
                    "type": "number",
                    "description": "How long to wait for a decision (1-120, default 30).",
                },
            },
        },
    },
    "submit_alien_action": {
        "fn": tool_submit_alien_action,
        "description": "Submit the chosen action for the alien the engine is waiting on. Pick a "
                       "target/weapon consistent with the situation from wait_for_alien_decision. "
                       "NONE ends this unit's activation. Illegal actions are ignored by the engine.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action_type": {
                    "type": "string", "enum": ACTION_TYPES,
                    "description": "The action to take.",
                },
                "target_x": {"type": "integer", "description": "Target tile X (with target_y/z)."},
                "target_y": {"type": "integer", "description": "Target tile Y."},
                "target_z": {"type": "integer", "description": "Target tile Z (level)."},
                "weapon_id": {
                    "type": "integer",
                    "description": "BattleItem id from the unit's items; omit for the main-hand weapon.",
                },
                "waypoints": {
                    "type": "array",
                    "description": "Blaster-launch path for LAUNCH.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "x": {"type": "integer"}, "y": {"type": "integer"}, "z": {"type": "integer"},
                        },
                    },
                },
                "final_facing": {"type": "integer", "description": "Direction (0-7) to face after moving; -1 = none."},
                "kneel": {"type": "boolean"},
                "run": {"type": "boolean"},
            },
            "required": ["action_type"],
        },
    },
    "get_sample_request": {
        "fn": tool_get_sample_request,
        "description": "Fetch a representative request payload (schema reference). Only works when "
                       "the engine runs in standalone --restserver mode.",
        "inputSchema": {"type": "object", "properties": {}},
    },
}


def tools_list_payload():
    return [
        {"name": name, "description": spec["description"], "inputSchema": spec["inputSchema"]}
        for name, spec in TOOLS.items()
    ]


# ---------------------------------------------------------------------------
# MCP (JSON-RPC 2.0 over stdio) message handling
# ---------------------------------------------------------------------------

def _result(mid, result):
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def _error(mid, code, message):
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}


def handle_message(msg, client):
    """Returns a JSON-RPC response dict, or None for notifications / no-reply."""
    if not isinstance(msg, dict):
        return None
    method = msg.get("method")
    mid = msg.get("id")

    if method == "initialize":
        params = msg.get("params") or {}
        protocol = params.get("protocolVersion") or DEFAULT_PROTOCOL
        return _result(mid, {
            "protocolVersion": protocol,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })

    if method in ("notifications/initialized", "initialized"):
        return None  # notification, no response

    if method == "ping":
        return _result(mid, {})

    if method == "tools/list":
        return _result(mid, {"tools": tools_list_payload()})

    if method == "tools/call":
        params = msg.get("params") or {}
        name = params.get("name")
        args = params.get("arguments") or {}
        spec = TOOLS.get(name)
        if spec is None:
            return _result(mid, {
                "content": [{"type": "text", "text": "Unknown tool: %s" % name}],
                "isError": True,
            })
        try:
            text = spec["fn"](client, args)
            return _result(mid, {"content": [{"type": "text", "text": text}], "isError": False})
        except Exception as e:  # noqa: BLE001 - surface tool errors to the LLM, don't crash
            return _result(mid, {
                "content": [{"type": "text", "text": "tool error: %s" % e}],
                "isError": True,
            })

    if mid is not None:
        return _error(mid, -32601, "method not found: %s" % method)
    return None  # unknown notification


def serve_stdio(client):
    # MCP stdio: UTF-8, newline-delimited JSON. Keep stdout for protocol only.
    try:
        sys.stdin.reconfigure(encoding="utf-8")
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    log("serving on stdio; engine endpoint = %s" % client.base)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            log("ignoring non-JSON line")
            continue
        resp = handle_message(msg, client)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
    log("stdin closed; exiting")


# ---------------------------------------------------------------------------
# Offline self-check (no engine / no MCP client needed)
# ---------------------------------------------------------------------------

def self_check():
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)

    # build_action_yaml
    y = build_action_yaml({"action_type": "snapshot", "target_x": 8, "target_y": 9,
                           "target_z": 1, "weapon_id": 2001, "kneel": True})
    check("type: SNAPSHOT" in y, "action_type upper-cased")
    check("target: {x: 8, y: 9, z: 1}" in y, "target formatted")
    check("weapon: 2001" in y, "weapon id")
    check("kneel: true" in y, "kneel bool")
    y2 = build_action_yaml({"action_type": "NONE"})
    check("target:" not in y2, "no target when coords omitted")
    y3 = build_action_yaml({"action_type": "LAUNCH",
                            "waypoints": [{"x": 1, "y": 1, "z": 0}, {"x": 5, "y": 6, "z": 0}]})
    check(y3.count("- {x:") == 2, "waypoints rendered")

    dummy = RestClient("127.0.0.1", 1)  # not used for offline checks

    # initialize
    r = handle_message({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                        "params": {"protocolVersion": "2025-06-18"}}, dummy)
    check(r["result"]["protocolVersion"] == "2025-06-18", "echoes client protocol version")
    check(r["result"]["serverInfo"]["name"] == SERVER_NAME, "serverInfo name")

    # notification -> no reply
    check(handle_message({"jsonrpc": "2.0", "method": "notifications/initialized"}, dummy) is None,
          "initialized notification has no reply")

    # tools/list
    r = handle_message({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, dummy)
    names = {t["name"] for t in r["result"]["tools"]}
    check(names == set(TOOLS.keys()), "tools/list exposes all tools")
    for t in r["result"]["tools"]:
        check(t["inputSchema"]["type"] == "object", "tool %s has object schema" % t["name"])

    # unknown tool -> isError
    r = handle_message({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                        "params": {"name": "nope", "arguments": {}}}, dummy)
    check(r["result"]["isError"] is True, "unknown tool is an error")

    # unknown method -> JSON-RPC error
    r = handle_message({"jsonrpc": "2.0", "id": 4, "method": "bogus/method"}, dummy)
    check("error" in r and r["error"]["code"] == -32601, "unknown method -> -32601")

    if failures:
        for f in failures:
            print("FAIL %s" % f)
        print("MCP SELF-CHECK FAILED (%d)" % len(failures))
        return 1
    print("MCP SELF-CHECK PASSED (%d checks)" % 16)
    return 0


def main(argv):
    if "--self-check" in argv:
        return self_check()
    host = os.environ.get("OPENXCOM_REST_HOST", "127.0.0.1")
    port = int(os.environ.get("OPENXCOM_REST_PORT", "8765"))
    serve_stdio(RestClient(host, port))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
