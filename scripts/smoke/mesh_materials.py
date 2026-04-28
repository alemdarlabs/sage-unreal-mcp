#!/usr/bin/env python3
"""Phase 4.5-r2 batch 5 — mesh material slot management.

asset.list_mesh_materials / asset.set_mesh_material / asset.set_sk_material_slots.

Round-trips a StaticMesh slot 0 binding and (if present) bulk-edits a
SkeletalMesh slot list. Restores originals on exit.
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


def find_static_mesh() -> str:
    r = call("asset.search",
             {"query": "Cube", "class": "/Script/Engine.StaticMesh",
              "max_results": 5})
    if not r["matches"]:
        raise RuntimeError("no StaticMesh found")
    return r["matches"][0]["path"]


def find_skeletal_mesh() -> str | None:
    for q in ("Manny", "Quinn", "SK", "Mannequin"):
        try:
            r = call("asset.search",
                     {"query": q, "class": "/Script/Engine.SkeletalMesh",
                      "max_results": 5})
            if r.get("matches"):
                return r["matches"][0]["path"]
        except RuntimeError:
            continue
    return None


def find_material() -> str | None:
    r = call("asset.search",
             {"query": "Material", "class": "/Script/Engine.Material",
              "max_results": 5})
    if r.get("matches"):
        return r["matches"][0]["path"]
    return None


def main() -> int:
    sm_path = find_static_mesh()
    print(f"target StaticMesh = {sm_path}")

    hr("StaticMesh: list_mesh_materials")
    base = call("asset.list_mesh_materials", {"path": sm_path})
    print(json.dumps(base, indent=2))
    assert base["kind"] == "static_mesh"
    assert base["count"] >= 1, "expected at least one slot"
    orig_mat = base["slots"][0]["material"]

    test_mat = find_material()
    if test_mat is None:
        # Use existing material binding as the swap target
        test_mat = orig_mat
        if not test_mat:
            print("no Material available — skipping swap test")
            return 0
    print(f"test material = {test_mat}")

    hr("StaticMesh: set_mesh_material slot=0")
    sm = call("asset.set_mesh_material",
              {"path": sm_path, "slot": 0, "material": test_mat})
    print(json.dumps(sm, indent=2))

    hr("StaticMesh: clear slot=0 (empty material)")
    sc = call("asset.set_mesh_material",
              {"path": sm_path, "slot": 0, "material": ""})
    print(json.dumps(sc, indent=2))
    after = call("asset.list_mesh_materials", {"path": sm_path})
    assert after["slots"][0]["material"] == "", \
        f"expected cleared, got {after['slots'][0]['material']!r}"

    hr("StaticMesh: restore slot=0")
    if orig_mat:
        call("asset.set_mesh_material",
             {"path": sm_path, "slot": 0, "material": orig_mat})
    final = call("asset.list_mesh_materials", {"path": sm_path})
    print(f"  slot 0 restored to: {final['slots'][0]['material']!r}")

    hr("StaticMesh: out-of-range slot (-32602)")
    try:
        call("asset.set_mesh_material",
             {"path": sm_path, "slot": 99, "material": ""})
        print("FAIL: oor slot accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("StaticMesh: bogus material (-32602)")
    try:
        call("asset.set_mesh_material",
             {"path": sm_path, "slot": 0,
              "material": "/Game/Bogus_Mat/NotAMat"})
        print("FAIL: bogus mat accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    sk_path = find_skeletal_mesh()
    if sk_path:
        hr(f"SkeletalMesh: list_mesh_materials {sk_path}")
        sk_base = call("asset.list_mesh_materials", {"path": sk_path})
        print(f"  count = {sk_base['count']}")
        if sk_base["count"] > 0:
            orig_sk = [(s["index"], s["material"]) for s in sk_base["slots"]]

            hr("SkeletalMesh: set_sk_material_slots (clear slot 0)")
            up = call("asset.set_sk_material_slots", {
                "path": sk_path,
                "slots": [{"index": 0, "material": ""}],
            })
            print(json.dumps(up, indent=2))
            after_sk = call("asset.list_mesh_materials", {"path": sk_path})
            assert after_sk["slots"][0]["material"] == "", "sk slot 0 not cleared"

            hr("SkeletalMesh: restore originals")
            restore = [{"index": i, "material": m} for i, m in orig_sk]
            call("asset.set_sk_material_slots",
                 {"path": sk_path, "slots": restore})
            verify = call("asset.list_mesh_materials", {"path": sk_path})
            for (i, m), s in zip(orig_sk, verify["slots"]):
                assert s["material"] == m, \
                    f"slot {i}: expected {m!r} got {s['material']!r}"
            print("  restored OK")
    else:
        print("\n(no SkeletalMesh — sk path skipped)")

    print("\n*** mesh_materials smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
