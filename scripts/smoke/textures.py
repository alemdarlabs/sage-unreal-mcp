#!/usr/bin/env python3
"""Phase 4.5-r2 batch 3 — texture introspection + settings.

Lists every UTexture in /Game, reads info on the first one, mutates
selected settings, verifies they round-trip, then restores.
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


def main() -> int:
    print("=== asset.list_textures /Game ===")
    listed = call("asset.list_textures",
                  {"directory": "/Game", "max_results": 5000})
    print(f"  total textures: {listed['total']} (returned {listed['returned']})")
    if listed["returned"] == 0:
        print("no textures in /Game — skipping")
        return 0

    target = listed["textures"][0]["path"]
    print(f"  target: {target}")

    print("\n=== asset.get_texture_info ===")
    info = call("asset.get_texture_info", {"path": target})
    print(json.dumps(info, indent=2))

    orig = {
        "filter":       info["filter"],
        "srgb":         info["srgb"],
        "lod_bias":     info["lod_bias"],
        "compression":  info["compression"],
    }

    print("\n=== asset.set_texture_settings (mutate) ===")
    new_filter = "Nearest" if orig["filter"] != "Nearest" else "Trilinear"
    mutated = call("asset.set_texture_settings", {
        "path":     target,
        "filter":   new_filter,
        "srgb":     not orig["srgb"],
        "lod_bias": orig["lod_bias"] + 1,
    })
    print(json.dumps(mutated, indent=2))
    assert mutated["changed"] is True

    print("\n=== verify (re-read) ===")
    after = call("asset.get_texture_info", {"path": target})
    assert after["filter"] == new_filter, f"filter {after['filter']} != {new_filter}"
    assert after["srgb"] == (not orig["srgb"]), "srgb did not flip"
    assert after["lod_bias"] == orig["lod_bias"] + 1, "lod_bias not incremented"
    print(f"  filter: {orig['filter']} → {after['filter']}")
    print(f"  srgb:   {orig['srgb']} → {after['srgb']}")
    print(f"  lod_bias: {orig['lod_bias']} → {after['lod_bias']}")

    print("\n=== restore originals ===")
    call("asset.set_texture_settings", {
        "path":        target,
        "filter":      orig["filter"],
        "srgb":        orig["srgb"],
        "lod_bias":    orig["lod_bias"],
        "compression": orig["compression"],
    })
    final = call("asset.get_texture_info", {"path": target})
    assert final["filter"] == orig["filter"]
    assert final["srgb"] == orig["srgb"]
    assert final["lod_bias"] == orig["lod_bias"]
    print("restored OK")

    print("\n=== invalid value (expect -32602) ===")
    try:
        call("asset.set_texture_settings",
             {"path": target, "filter": "Bogus"})
        print("FAIL: invalid filter accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    print("\n*** textures smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
