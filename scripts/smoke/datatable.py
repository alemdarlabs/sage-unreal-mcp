#!/usr/bin/env python3
"""Phase 4.5-r2 batch 6 — datatable triplet.

asset.create_datatable → asset.reimport_datatable (JSON inline) →
asset.read_datatable → asset.delete_batch (cleanup).

Uses FMirrorTableRow which ships with Engine (Animation/MirrorDataTable.h).
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
    dt_path = "/Game/Sage_Smoke/DT_Mirror_Sage"
    row_struct = "/Script/Engine.MirrorTableRow"

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [dt_path]})
    except RuntimeError:
        pass

    hr("asset.create_datatable")
    created = call("asset.create_datatable",
                   {"path": dt_path, "row_struct": row_struct})
    print(json.dumps(created, indent=2))
    assert created["row_struct"].endswith("MirrorTableRow")

    hr("asset.create_datatable bad row_struct (-32602)")
    try:
        call("asset.create_datatable",
             {"path": "/Game/Sage_Smoke/DT_Bad",
              "row_struct": "/Script/Engine.Vector"})
        print("FAIL: bad struct accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("asset.reimport_datatable inline JSON (3 rows)")
    rows_json = json.dumps([
        {"Name": "spine_l",  "MirroredName": "spine_r",  "MirrorEntryType": "Bone"},
        {"Name": "arm_l",    "MirroredName": "arm_r",    "MirrorEntryType": "Bone"},
        {"Name": "shoot_l",  "MirroredName": "shoot_r",  "MirrorEntryType": "AnimationNotify"},
    ])
    rep = call("asset.reimport_datatable",
               {"path": dt_path, "json": rows_json})
    print(json.dumps(rep, indent=2))
    assert rep["row_count"] == 3, f"expected 3 rows, got {rep['row_count']}"
    assert rep["problem_count"] == 0, f"problems: {rep['problems']}"

    hr("asset.read_datatable")
    r = call("asset.read_datatable", {"path": dt_path, "max_rows": 10})
    print(json.dumps(r, indent=2))
    assert r["row_count"] == 3
    assert r["returned"] == 3
    names = {row["fields"].get("Name") for row in r["rows"]}
    assert names == {"spine_l", "arm_l", "shoot_l"}, f"names mismatch: {names}"

    hr("asset.reimport_datatable replace (1 row, clear_first=true)")
    rep2 = call("asset.reimport_datatable", {
        "path": dt_path,
        "json": json.dumps([{"Name": "only_one", "MirroredName": "yep",
                              "MirrorEntryType": "Curve"}]),
        "clear_first": True,
    })
    print(json.dumps(rep2, indent=2))
    assert rep2["row_count"] == 1

    hr("asset.read_datatable (after replace)")
    r2 = call("asset.read_datatable", {"path": dt_path})
    assert r2["row_count"] == 1
    assert r2["rows"][0]["fields"]["Name"] == "only_one"

    hr("Cleanup: delete_batch")
    cleanup = call("asset.delete_batch", {"paths": [dt_path]})
    print(json.dumps(cleanup, indent=2))
    assert cleanup["deleted"] == 1

    print("\n*** datatable smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
