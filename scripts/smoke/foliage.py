#!/usr/bin/env python3
"""Phase 4.x — foliage domain smoke.

foliage.create_type → list_types → get_settings → set_settings → cleanup.
foliage.paint/erase/sample are stub-noted (FEdModeFoliage required).
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
    ft_path = "/Game/Sage_Smoke/FT_Foliage_Test"

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [ft_path]})
    except RuntimeError:
        pass

    hr("foliage.create_type")
    cre = call("foliage.create_type", {"path": ft_path})
    print(json.dumps(cre, indent=2))
    assert cre["class"] == "FoliageType_InstancedStaticMesh"
    assert cre["path"].startswith(ft_path)

    hr("foliage.create_type missing path (-32602)")
    try:
        call("foliage.create_type", {})
        print("FAIL: missing path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("foliage.list_types path=/Game/Sage_Smoke")
    listed = call("foliage.list_types", {"path": "/Game/Sage_Smoke"})
    print(f"  count={listed['count']}")
    assert listed["count"] >= 1
    assert any(t["name"] == "FT_Foliage_Test" for t in listed["types"])

    hr("foliage.get_settings (full property dump)")
    g = call("foliage.get_settings", {"path": ft_path})
    assert g["path"].startswith(ft_path)
    assert isinstance(g["properties"], list)
    print(f"  properties: {len(g['properties'])}")
    # Spot-check: a few well-known FoliageType_InstancedStaticMesh fields
    names = {p["name"] for p in g["properties"]}
    assert "Density" in names, f"expected Density in {sorted(names)[:8]}…"

    hr("foliage.get_settings missing path (-32602)")
    try:
        call("foliage.get_settings", {})
        print("FAIL: missing path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("foliage.set_settings Density=42.0 (write+verify)")
    s = call("foliage.set_settings", {"path": ft_path, "Density": 42.0})
    print(json.dumps(s, indent=2))
    assert s["fields_set"] >= 1
    g2 = call("foliage.get_settings", {"path": ft_path})
    density = next(p["value"] for p in g2["properties"] if p["name"] == "Density")
    assert abs(float(density) - 42.0) < 1e-3, f"Density readback wrong: {density}"

    hr("foliage.sample (note-only stub)")
    smp = call("foliage.sample", {})
    print(json.dumps(smp, indent=2))
    assert "note" in smp

    hr("foliage.paint (note-only stub)")
    pnt = call("foliage.paint", {})
    print(json.dumps(pnt, indent=2))
    assert "note" in pnt

    hr("foliage.erase (note-only stub)")
    ers = call("foliage.erase", {})
    print(json.dumps(ers, indent=2))
    assert "note" in ers

    hr("Cleanup")
    cleanup = call("asset.delete_batch", {"paths": [ft_path]})
    assert cleanup["deleted"] == 1

    print("\n*** foliage smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
