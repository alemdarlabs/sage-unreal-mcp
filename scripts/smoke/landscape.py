#!/usr/bin/env python3
"""Phase 4.x — landscape domain smoke (11 tools).

SageTest level has no Landscape actor — these tools are designed to
report `landscape_found=false` or return informational notes rather
than hard-fail. This smoke verifies that contract.

Tools that require a landscape (`set_material`, `import_heightmap`)
return -32602 errors gracefully.

Tool count: 11
  reads  : get_info, list_layers, list_splines, get_component,
           get_material_usage_summary
  notes  : sample, sculpt, paint_layer, add_layer_info, import_heightmap
  writes : set_material (errors with no landscape)
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
    hr("landscape.get_info")
    info = call("landscape.get_info", {})
    print(json.dumps(info, indent=2))
    has_landscape = "actor_id" in info
    if not has_landscape:
        assert info.get("landscape_found") is False
        assert "note" in info
        print("  (no landscape in level — domain is in graceful-skip mode)")
    else:
        assert "properties" in info

    hr("landscape.list_layers")
    layers = call("landscape.list_layers", {})
    print(json.dumps(layers, indent=2))
    assert "layers" in layers and "count" in layers

    hr("landscape.list_splines (note-only stub)")
    splines = call("landscape.list_splines", {})
    print(json.dumps(splines, indent=2))
    assert "note" in splines

    hr("landscape.sample at [100,200,0]")
    smp = call("landscape.sample", {"location": [100.0, 200.0, 0.0]})
    print(json.dumps(smp, indent=2))
    assert smp["location"] == [100.0, 200.0, 0.0]
    assert "note" in smp

    hr("landscape.sculpt (note-only stub)")
    sclpt = call("landscape.sculpt", {})
    print(json.dumps(sclpt, indent=2))
    assert "note" in sclpt

    hr("landscape.paint_layer (note-only stub)")
    pnt = call("landscape.paint_layer", {})
    print(json.dumps(pnt, indent=2))
    assert "note" in pnt

    hr("landscape.add_layer_info (note-only stub)")
    ali = call("landscape.add_layer_info", {})
    print(json.dumps(ali, indent=2))
    assert "note" in ali

    hr("landscape.import_heightmap missing file (-32602)")
    try:
        call("landscape.import_heightmap", {})
        print("FAIL: missing file accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("landscape.import_heightmap file=/tmp/heightmap.png (note returned)")
    imp = call("landscape.import_heightmap", {"file": "/tmp/heightmap.png"})
    print(json.dumps(imp, indent=2))
    assert imp["file"] == "/tmp/heightmap.png"
    assert "note" in imp

    hr("landscape.get_material_usage_summary")
    mus = call("landscape.get_material_usage_summary", {})
    print(json.dumps(mus, indent=2))
    if not has_landscape:
        assert mus.get("landscape_found") is False

    hr("landscape.get_component missing actor_id (-32602)")
    try:
        call("landscape.get_component", {})
        print("FAIL: missing actor_id accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("landscape.set_material missing material_path (-32602)")
    try:
        call("landscape.set_material", {})
        print("FAIL: missing material_path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    if not has_landscape:
        hr("landscape.set_material with no landscape in level (-32602)")
        try:
            call("landscape.set_material",
                 {"material_path": "/Engine/EditorMaterials/PhAT_FloorMat"})
            print("FAIL: should error when no landscape")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

    print("\n*** landscape smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
