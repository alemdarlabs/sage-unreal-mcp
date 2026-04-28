#!/usr/bin/env python3
"""Phase 4.5-r2 batch 2 — socket smoke test.

Exercises asset.list_sockets / asset.add_socket / asset.remove_socket on a
StaticMesh target (and SkeletalMesh if present in /Game). Restores state at
the end so the test is idempotent.

Usage: python3 scripts/smoke/sockets.py
"""

from __future__ import annotations

import json
import sys
import urllib.request

SERVER = "http://127.0.0.1:7777/mcp"


def call(name: str, args: dict, *, _id: int = 1) -> dict:
    payload = json.dumps(
        {"jsonrpc": "2.0", "method": "tools/call", "id": _id,
         "params": {"name": name, "arguments": args}}
    ).encode()
    req = urllib.request.Request(SERVER, data=payload,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        body = json.loads(resp.read())
    if "error" in body:
        raise RuntimeError(f"{name}: {body['error']}")
    text = body["result"]["content"][0]["text"]
    return json.loads(text)


def find_static_mesh() -> str:
    r = call("asset.search",
             {"query": "Cube", "class": "/Script/Engine.StaticMesh",
              "max_results": 5})
    if not r["matches"]:
        raise RuntimeError("no StaticMesh found")
    return r["matches"][0]["path"]


def find_skeletal_mesh() -> str | None:
    for q in ("Manny", "Quinn", "Mannequin", "SK", "_"):
        try:
            r = call("asset.search",
                     {"query": q, "class": "/Script/Engine.SkeletalMesh",
                      "max_results": 5})
            if r.get("matches"):
                return r["matches"][0]["path"]
        except RuntimeError:
            continue
    # Fallback: list /Game and pick first SkeletalMesh
    try:
        r = call("asset.list",
                 {"directory": "/Game", "recursive": True, "max_results": 5000})
        for m in r["assets"]:
            if m["kind"] == "SkeletalMesh":
                return m["path"]
    except RuntimeError:
        pass
    return None


def hr(s: str) -> None:
    print(f"\n=== {s} ===")


def main() -> int:
    sm_path = find_static_mesh()
    print(f"target StaticMesh = {sm_path}")

    hr("StaticMesh: pre-cleanup")
    pre = call("asset.list_sockets", {"path": sm_path})
    for s in pre["sockets"]:
        if s["name"].startswith("Test_") or s["name"].startswith("Sage_"):
            print(f"  removing leftover {s['name']}")
            call("asset.remove_socket", {"path": sm_path, "name": s["name"]})

    hr("StaticMesh: list (baseline)")
    base = call("asset.list_sockets", {"path": sm_path})
    base_count = base["count"]
    print(f"baseline sockets = {base_count}")

    hr("StaticMesh: add Test_Socket")
    add = call("asset.add_socket", {
        "path": sm_path, "name": "Test_Socket",
        "location": [10.0, 20.0, 30.0],
        "rotation": [0.0, 45.0, 90.0],
        "scale":    [1.5, 1.5, 1.5],
    })
    print(f"added socket: {add['socket']}")
    assert add["socket"]["name"] == "Test_Socket", "name mismatch"
    loc = add["socket"]["location"]
    assert abs(loc[0] - 10.0) < 1e-3, f"loc[0] {loc} != 10.0"

    hr("StaticMesh: list (after add)")
    after_add = call("asset.list_sockets", {"path": sm_path})
    assert after_add["count"] == base_count + 1, \
        f"count {after_add['count']} != {base_count + 1}"
    print(f"sockets now = {after_add['count']}")

    hr("StaticMesh: add duplicate (expect -32602)")
    try:
        call("asset.add_socket", {"path": sm_path, "name": "Test_Socket"})
        print("FAIL: duplicate add did not error")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("StaticMesh: remove Test_Socket")
    rm = call("asset.remove_socket", {"path": sm_path, "name": "Test_Socket"})
    print(f"removed: {rm}")
    assert rm["remaining"] == base_count, \
        f"remaining {rm['remaining']} != {base_count}"

    hr("StaticMesh: remove missing (expect -32602)")
    try:
        call("asset.remove_socket", {"path": sm_path, "name": "Bogus_Sock"})
        print("FAIL: missing remove did not error")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    sk_path = find_skeletal_mesh()
    if sk_path:
        hr(f"SkeletalMesh: probe {sk_path}")
        try:
            sk_base = call("asset.list_sockets", {"path": sk_path})
            print(f"sk baseline mesh-only sockets = {sk_base['count']}")
            sk_add = call("asset.add_socket", {
                "path": sk_path, "name": "Sage_Test_Sock",
                "bone": "root",
                "location": [1.0, 2.0, 3.0],
            })
            print(f"sk added: {sk_add['socket']['name']} bone={sk_add['socket']['bone']}")
            sk_rm = call("asset.remove_socket",
                         {"path": sk_path, "name": "Sage_Test_Sock"})
            print(f"sk removed; remaining={sk_rm['remaining']}")
        except RuntimeError as e:
            print(f"sk path skipped: {e}")
    else:
        print("\n(no SkeletalMesh in /Game — skeletal path skipped)")

    print("\n*** sockets smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
