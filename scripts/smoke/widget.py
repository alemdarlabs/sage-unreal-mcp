#!/usr/bin/env python3
"""Phase 4.11 round 1 — UMG starter smoke.

widget.create → widget.list → widget.read → cleanup.
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
    return json.loads(body["result"]["content"][0]["text"])


def hr(s: str) -> None:
    print(f"\n=== {s} ===")


def main() -> int:
    wb_path = "/Game/Sage_Smoke/WBP_Sage_Test"

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [wb_path]})
    except RuntimeError:
        pass

    hr("widget.create (default UUserWidget parent)")
    created = call("widget.create", {"path": wb_path})
    print(json.dumps(created, indent=2))
    assert created["name"] == "WBP_Sage_Test"
    assert created["parent_class"].endswith("UserWidget")

    hr("widget.create duplicate (-32602)")
    try:
        call("widget.create", {"path": wb_path})
        print("FAIL: duplicate accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("widget.create bad parent_class (Actor)")
    try:
        call("widget.create",
             {"path": "/Game/Sage_Smoke/WBP_Bad",
              "parent_class": "/Script/Engine.Actor"})
        print("FAIL: bad parent accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("widget.list /Game/Sage_Smoke")
    listed = call("widget.list",
                  {"directory": "/Game/Sage_Smoke", "max_results": 50})
    print(json.dumps(listed, indent=2))
    assert listed["total"] >= 1
    assert any(w["name"] == "WBP_Sage_Test" for w in listed["widgets"])

    hr("widget.read")
    read = call("widget.read", {"path": wb_path})
    print(json.dumps(read, indent=2))
    assert read["name"] == "WBP_Sage_Test"
    assert "root" in read or read["widget_count"] == 0
    if "root" in read:
        print(f"  root: {read['root']['class']}")

    hr("widget.read on a non-widget asset (-32602)")
    try:
        call("widget.read",
             {"path": "/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube"})
        print("FAIL: non-widget accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("Cleanup")
    cleanup = call("asset.delete_batch", {"paths": [wb_path]})
    assert cleanup["deleted"] == 1

    print("\n*** widget smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
