#!/usr/bin/env python3
"""Phase 4.5-r2 batch 8 — asset.export smoke.

Tries each supported export path:
  - StaticMesh → FBX (SM_Cube)
  - Texture2D  → PNG (T_Quinn_01_D)
  - Texture2D  → TGA
"""

from __future__ import annotations

import json
import sys
import tempfile
import urllib.request
from pathlib import Path

SERVER = "http://127.0.0.1:7777/mcp"


def call(name: str, args: dict, *, _id: int = 1) -> dict:
    payload = json.dumps(
        {"jsonrpc": "2.0", "method": "tools/call", "id": _id,
         "params": {"name": name, "arguments": args}}
    ).encode()
    req = urllib.request.Request(SERVER, data=payload,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        body = json.loads(resp.read())
    if "error" in body:
        raise RuntimeError(f"{name}: {body['error']}")
    return json.loads(body["result"]["content"][0]["text"])


def hr(s: str) -> None:
    print(f"\n=== {s} ===")


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="sage_smoke_export_"))
    print(f"export dir: {tmp}")

    sm = call("asset.search",
              {"query": "Cube", "class": "/Script/Engine.StaticMesh",
               "max_results": 1})
    sm_path = sm["matches"][0]["path"]
    tex = call("asset.search",
               {"query": "T_Quinn", "class": "/Script/Engine.Texture2D",
                "max_results": 1})
    tex_path = tex["matches"][0]["path"]

    hr(f"StaticMesh → FBX  ({sm_path})")
    fbx_out = tmp / "SM_Cube.fbx"
    r1 = call("asset.export", {"path": sm_path, "file": str(fbx_out)})
    print(json.dumps({k: v for k, v in r1.items() if k != "errors"}, indent=2))
    if "errors" in r1:
        print(f"  errors: {r1['errors']}")
    assert r1["exported"] is True
    assert fbx_out.exists() and fbx_out.stat().st_size > 0

    hr(f"Texture2D → PNG  ({tex_path})")
    png_out = tmp / "T_Quinn_01_D.png"
    r2 = call("asset.export", {"path": tex_path, "file": str(png_out)})
    print(json.dumps({k: v for k, v in r2.items() if k != "errors"}, indent=2))
    assert r2["exported"] is True
    assert png_out.exists() and png_out.stat().st_size > 100

    hr(f"Texture2D → TGA  ({tex_path})")
    tga_out = tmp / "T_Quinn_01_D.tga"
    r3 = call("asset.export", {"path": tex_path, "file": str(tga_out)})
    print(json.dumps({k: v for k, v in r3.items() if k != "errors"}, indent=2))
    assert r3["exported"] is True
    assert tga_out.exists() and tga_out.stat().st_size > 100

    hr("Bogus extension (-32602)")
    try:
        call("asset.export",
             {"path": tex_path, "file": str(tmp / "out.bogus")})
        print("FAIL: bogus ext accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("Asset not found (-32602)")
    try:
        call("asset.export",
             {"path": "/Game/Bogus/None", "file": str(tmp / "x.png")})
        print("FAIL: missing asset accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("Cleanup")
    for p in tmp.iterdir():
        p.unlink()
    tmp.rmdir()

    print("\n*** export smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
