#!/usr/bin/env python3
"""Phase 4.5-r2 batch 9 — FBX import wrappers via export round-trip.

Round-trip plan:
  1. asset.export SM_Cube → /tmp/SM_Cube.fbx
  2. asset.import_static_mesh /tmp/SM_Cube.fbx → /Game/Sage_Smoke/SM_Imported
  3. verify class is StaticMesh
  4. (optional) export SKM_Manny_Simple → /tmp/manny.fbx, then
     asset.import_skeletal_mesh round-trip
  5. cleanup
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
    with urllib.request.urlopen(req, timeout=60) as resp:
        body = json.loads(resp.read())
    if "error" in body:
        raise RuntimeError(f"{name}: {body['error']}")
    return json.loads(body["result"]["content"][0]["text"])


def hr(s: str) -> None:
    print(f"\n=== {s} ===")


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="sage_smoke_fbx_"))
    print(f"tmp: {tmp}")

    sm_dest = "/Game/Sage_Smoke/SM_Imported"
    sk_dest = "/Game/Sage_Smoke/SK_Imported"

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [sm_dest, sk_dest]})
    except RuntimeError:
        pass

    hr("Step 1: export SM_Cube → FBX")
    sm_src = call("asset.search",
                  {"query": "Cube", "class": "/Script/Engine.StaticMesh",
                   "max_results": 1})["matches"][0]["path"]
    fbx = tmp / "SM_Cube.fbx"
    exp = call("asset.export", {"path": sm_src, "file": str(fbx)})
    print(f"  exported: {exp['file_size']} bytes")
    assert fbx.exists() and fbx.stat().st_size > 100

    hr("Step 2: asset.import_static_mesh")
    imp = call("asset.import_static_mesh", {
        "file":        str(fbx),
        "destination": sm_dest,
    })
    print(json.dumps(imp, indent=2))
    assert imp["count"] >= 1
    assert imp["expected_class"] == "StaticMesh"

    hr("Step 3: verify imported asset")
    sm_imported = next(
        (p for p in imp["imported"] if p.endswith("SM_Imported")), None)
    assert sm_imported, f"no SM_Imported in {imp['imported']}"
    info = call("asset.read_properties", {"path": sm_imported})
    assert info["class"] == "StaticMesh", \
        f"unexpected class {info['class']}"
    print(f"  {sm_imported} class={info['class']}")
    print(f"  side-products: {[p for p in imp['imported'] if p != sm_imported]}")

    hr("Step 4: import_static_mesh missing file (-32602)")
    try:
        call("asset.import_static_mesh",
             {"file": "/tmp/nope.fbx",
              "destination": "/Game/Sage_Smoke/Nope"})
        print("FAIL: missing accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("Step 5: import_static_mesh on a PNG (class assert -32000)")
    # Generate a tiny PNG via the same path
    import struct, zlib
    def write_png(p, w, h, rgba):
        sig = b"\x89PNG\r\n\x1a\n"
        def chunk(tag, data):
            crc = zlib.crc32(tag + data)
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)
        ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
        raw = b""
        for y in range(h):
            raw += b"\x00" + rgba[y*w*4:(y+1)*w*4]
        idat = zlib.compress(raw, 9)
        Path(p).write_bytes(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))

    png = tmp / "trick.png"
    write_png(png, 4, 4, b"\xff\xff\x00\xff" * 16)
    try:
        call("asset.import_static_mesh", {
            "file":        str(png),
            "destination": "/Game/Sage_Smoke/SM_FromPNG",
        })
        # Cleanup: maybe import succeeded as Texture2D, must remove
        try: call("asset.delete_batch", {"paths": ["/Game/Sage_Smoke/SM_FromPNG"]})
        except RuntimeError: pass
        print("FAIL: PNG → static_mesh accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")
        # Cleanup any artifact created
        try: call("asset.delete_batch", {"paths": ["/Game/Sage_Smoke/SM_FromPNG"]})
        except RuntimeError: pass

    hr("Cleanup")
    # Include any side-product paths from the import
    cleanup_paths = list(set([sm_dest] + [
        p.split(".")[0] for p in imp["imported"]
    ]))
    cleanup = call("asset.delete_batch", {"paths": cleanup_paths})
    print(f"  cleanup: deleted={cleanup['deleted']} from {len(cleanup_paths)}")
    fbx.unlink(missing_ok=True)
    png.unlink(missing_ok=True)
    tmp.rmdir()

    print("\n*** import_fbx smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
