#!/usr/bin/env python3
# [AI-MODS] Minimal REST "brain" for the OpenXcom rest-ai-server branch.
#
# Polls a running engine (launched with --restai, once you are in a battle and it is the alien
# turn) for pending alien decisions and answers each one with a simple action. This is enough to
# drive the whole alien turn end to end and prove the REST loop works. It intentionally does NOT
# parse the request YAML (so it needs only the Python standard library -- no PyYAML); a real brain
# would read /pending-decision and choose a WALK/shot target.
#
# Usage:
#   python mock-brain.py [--host 127.0.0.1] [--port 8765] [--action NONE]
#                        [--seconds 120] [--verbose]
#
# Answering with NONE makes every alien idle, so the alien turn ends quickly. Try --action WALK
# with a real brain that fills in a target, or --verbose to print the request payloads.

import argparse
import sys
import time
import urllib.error
import urllib.request


def http(method, url, body=None, timeout=5):
    data = body.encode("utf-8") if body is not None else None
    req = urllib.request.Request(
        url, data=data, method=method,
        headers={"Content-Type": "application/x-yaml"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")
    except urllib.error.URLError as e:
        return None, str(e.reason)


def main():
    ap = argparse.ArgumentParser(description="Mock alien-AI brain for the rest-ai-server branch.")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--action", default="NONE",
                    help="BattleActionType to answer with (NONE, WALK, SNAPSHOT, ...)")
    ap.add_argument("--seconds", type=float, default=120.0, help="max run time")
    ap.add_argument("--poll", type=float, default=0.1, help="poll interval in seconds")
    ap.add_argument("--verbose", action="store_true", help="print each request payload")
    args = ap.parse_args()

    base = "http://%s:%d" % (args.host, args.port)
    action_yaml = "action:\n  type: %s\n" % args.action
    served = 0
    deadline = time.time() + args.seconds
    print("[mock-brain] driving %s, answering every decision with type=%s"
          % (base, args.action), flush=True)

    while time.time() < deadline:
        status, body = http("GET", base + "/pending-decision")
        if status == 200:
            if args.verbose:
                print("[mock-brain] pending-decision:\n%s" % body, flush=True)
            st, _ = http("POST", base + "/action", action_yaml)
            served += 1
            if args.verbose:
                print("[mock-brain] answered (POST /action -> %s), served=%d"
                      % (st, served), flush=True)
        elif status is None:
            # Server not reachable yet (or gone) -- keep trying until the deadline.
            pass
        time.sleep(args.poll)

    print("[mock-brain] done; decisions served: %d" % served, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
