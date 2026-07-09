#!/usr/bin/env python3
# [AI-MODS] End-to-end test for the OpenXcom AI MCP server.
#
# Assumes an engine is already running in standalone mock mode:
#     OpenXcom.exe --restserver --restai-port <PORT>
# (scripts/test-mcp.ps1 launches that for you). This test then:
#   1. publishes a synthetic pending decision via the engine's /publish harness endpoint,
#   2. spawns openxcom_ai_mcp.py as a subprocess and speaks MCP JSON-RPC to it over stdio
#      (initialize -> tools/list -> tools/call), and
#   3. verifies wait_for_alien_decision returns the published request and submit_alien_action
#      lands the action (checked back through the engine's /last-action harness endpoint).
#
# Standard library only.

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import openxcom_ai_mcp as mcp  # noqa: E402  (path set above)

FAILS = 0


def check(cond, msg):
    global FAILS
    if cond:
        print("PASS %s" % msg)
    else:
        print("FAIL %s" % msg)
        FAILS += 1


def send(proc, obj):
    proc.stdin.write(json.dumps(obj) + "\n")
    proc.stdin.flush()


def recv(proc):
    line = proc.stdout.readline()
    if not line:
        return None
    return json.loads(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()

    client = mcp.RestClient(args.host, args.port)

    # Precondition: engine reachable.
    hs, _ = client.health()
    check(hs == 200, "engine reachable at %s" % client.base)
    if hs != 200:
        print("MCP INTEGRATION FAILED (engine not reachable)")
        return 1

    # 1. Publish a synthetic pending decision (marker lets us confirm round-trip).
    marker = "unit-42-probe"
    pub_status, _ = client.publish("request:\n  turn: 5\n  side: HOSTILE\n  unit:\n    id: 42\n    tag: %s\n" % marker)
    check(pub_status == 200, "published a pending decision via /publish")

    # 2. Spawn the MCP server and speak MCP to it over stdio.
    env = dict(os.environ, OPENXCOM_REST_HOST=args.host, OPENXCOM_REST_PORT=str(args.port))
    proc = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "openxcom_ai_mcp.py")],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None,
        text=True, encoding="utf-8", bufsize=1, env=env)

    try:
        # initialize
        send(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": "2025-06-18", "capabilities": {}}})
        r = recv(proc)
        check(r and r.get("id") == 1 and r["result"]["serverInfo"]["name"] == "openxcom-ai",
              "initialize handshake")
        check(r and r["result"]["protocolVersion"] == "2025-06-18", "protocol version echoed")

        # initialized notification (no reply)
        send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized"})

        # tools/list
        send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        r = recv(proc)
        names = {t["name"] for t in r["result"]["tools"]} if r else set()
        expected = {"game_status", "wait_for_alien_decision", "submit_alien_action", "get_sample_request"}
        check(names == expected, "tools/list exposes the expected tools")

        # tools/call wait_for_alien_decision -> should return the published request (marker present)
        send(proc, {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                    "params": {"name": "wait_for_alien_decision", "arguments": {"timeout_seconds": 10}}})
        r = recv(proc)
        text = r["result"]["content"][0]["text"] if r else ""
        check(marker in text, "wait_for_alien_decision returns the published request")

        # tools/call submit_alien_action -> POSTs the action to the engine
        send(proc, {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                    "params": {"name": "submit_alien_action",
                               "arguments": {"action_type": "SNAPSHOT", "target_x": 7,
                                             "target_y": 8, "target_z": 1, "weapon_id": 2001}}})
        r = recv(proc)
        text = r["result"]["content"][0]["text"] if r else ""
        check(r and r["result"]["isError"] is False and "accepted" in text,
              "submit_alien_action accepted by the engine")

        # 3. Confirm the action actually reached the engine (via the harness /last-action).
        la_status, la_body = client.last_action()
        check(la_status == 200 and "SNAPSHOT" in la_body and "x: 7" in la_body,
              "engine /last-action reflects the submitted SNAPSHOT")
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    if FAILS == 0:
        print("MCP INTEGRATION PASSED (all checks)")
        return 0
    print("MCP INTEGRATION FAILED (%d check(s))" % FAILS)
    return 1


if __name__ == "__main__":
    sys.exit(main())
