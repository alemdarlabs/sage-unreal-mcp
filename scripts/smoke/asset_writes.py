#!/usr/bin/env python3
"""Phase 4.5-r2 batch 4 — write essentials.

asset.create_data_asset → asset.delete_batch (with status) → asset.reload_package
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
    hr("Setup: bp.create concrete UDataAsset subclass (BP_DA_Sage)")
    bp_path = "/Game/Sage_Smoke/BP_DA_Sage"
    bp_class = bp_path + "." + bp_path.rsplit("/", 1)[-1] + "_C"
    inst_path = "/Game/Sage_Smoke/Inst_DA_Sage"

    # Pre-clean
    try:
        call("asset.delete_batch", {"paths": [inst_path, bp_path]})
    except RuntimeError:
        pass

    # UDataAsset is `abstract, BlueprintType` (NOT Blueprintable) — so
    # bp.create rejects it. UPrimaryDataAsset adds Blueprintable.
    bp = call("bp.create",
              {"path": bp_path, "parent_class": "/Script/Engine.PrimaryDataAsset"})
    print(json.dumps(bp, indent=2))

    hr("asset.create_data_asset using BP-derived class")
    created = call("asset.create_data_asset",
                   {"path": inst_path, "class": bp_class})
    print(json.dumps(created, indent=2))
    assert created["path"].startswith("/Game/Sage_Smoke/Inst_DA_Sage")

    hr("asset.create_data_asset bad class (Actor — not UDataAsset)")
    try:
        call("asset.create_data_asset",
             {"path": "/Game/Sage_Smoke/NotADA",
              "class": "/Script/Engine.Actor"})
        print("FAIL: bad class accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("asset.create_data_asset duplicate (-32602)")
    try:
        call("asset.create_data_asset",
             {"path": inst_path, "class": bp_class})
        print("FAIL: duplicate accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("asset.delete_batch (mix of present + missing)")
    bulk = call("asset.delete_batch", {
        "paths": [
            inst_path,
            "/Game/Sage_Smoke/Bogus_DoesNotExist",
            "/Game/Sage_Smoke/AlsoBogus",
            bp_path,
        ],
    })
    print(json.dumps(bulk, indent=2))
    assert bulk["deleted"] == 2, f"deleted {bulk['deleted']} != 2"
    assert bulk["missing"] == 2, f"missing {bulk['missing']} != 2"
    assert bulk["total"] == 4

    hr("asset.reload_package (existing /Game asset)")
    # Re-use a known package — SM_Cube
    cube = "/Game/LevelPrototyping/Meshes/SM_Cube"
    rp = call("asset.reload_package", {"path": cube})
    print(json.dumps(rp, indent=2))
    assert rp["status"] == "reloaded"
    assert rp["package"].endswith("SM_Cube")

    hr("asset.reload_package full object path form")
    rp2 = call("asset.reload_package",
               {"path": cube + ".SM_Cube"})  # /Game/.../SM_Cube.SM_Cube
    print(json.dumps(rp2, indent=2))
    assert rp2["status"] == "reloaded"

    hr("asset.reload_package bogus (-32602)")
    try:
        call("asset.reload_package", {"path": "/Game/Bogus_Pkg/Nothing"})
        print("FAIL: bogus reload accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    print("\n*** asset_writes smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
