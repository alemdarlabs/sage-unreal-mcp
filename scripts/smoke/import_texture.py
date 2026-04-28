#!/usr/bin/env python3
"""Phase 4.5-r2 batch 7 — import + reimport.

Generates a 4×4 RGBA PNG on disk, imports it into /Game/Sage_Smoke,
verifies dimensions via asset.get_texture_info, mutates the source file,
calls asset.reimport, verifies the new dimensions, then cleans up.
"""

from __future__ import annotations

import json
import struct
import sys
import tempfile
import urllib.request
import zlib
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


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    """Minimal PNG encoder (no compression of filter byte, deflate the IDAT)."""
    sig = b"\x89PNG\r\n\x1a\n"
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data)
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    raw = b""
    stride = width * 4
    assert len(rgba) == stride * height
    for y in range(height):
        raw += b"\x00" + rgba[y * stride:(y + 1) * stride]
    idat = zlib.compress(raw, 9)
    iend = b""
    out = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", iend)
    path.write_bytes(out)


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="sage_smoke_tex_"))
    src = tmp / "sage_test.png"
    # 4x4 red
    write_png(src, 4, 4, b"\xff\x00\x00\xff" * (4 * 4))
    print(f"wrote PNG: {src}")

    dest = "/Game/Sage_Smoke/T_Sage_Imported"

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [dest]})
    except RuntimeError:
        pass

    hr("asset.import_texture (PNG 4x4)")
    imp = call("asset.import_texture", {
        "file":        str(src),
        "destination": dest,
    })
    print(json.dumps(imp, indent=2))
    assert imp["count"] >= 1
    imported_path = imp["imported"][0]

    hr("verify via get_texture_info")
    info = call("asset.get_texture_info", {"path": imported_path})
    print(json.dumps(info, indent=2))
    assert info["width"] == 4 and info["height"] == 4

    hr("rewrite source PNG to 8x8, then asset.reimport")
    write_png(src, 8, 8, b"\x00\xff\x00\xff" * (8 * 8))  # green
    re = call("asset.reimport", {"path": imported_path})
    print(json.dumps(re, indent=2))
    assert re["reimported"] is True

    hr("verify dimensions changed (4 → 8)")
    info2 = call("asset.get_texture_info", {"path": imported_path})
    assert info2["width"] == 8 and info2["height"] == 8, \
        f"expected 8x8 after reimport, got {info2['width']}x{info2['height']}"
    print(f"  width:  {info['width']} → {info2['width']}")
    print(f"  height: {info['height']} → {info2['height']}")

    hr("asset.reimport bogus path (-32602)")
    try:
        call("asset.reimport", {"path": "/Game/Bogus/Nope"})
        print("FAIL: bogus path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("asset.import_texture missing file (-32602)")
    try:
        call("asset.import_texture", {
            "file":        "/tmp/does_not_exist.png",
            "destination": "/Game/Sage_Smoke/T_Should_Not_Be",
        })
        print("FAIL: missing file accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("Cleanup")
    call("asset.delete_batch", {"paths": [dest]})
    src.unlink(missing_ok=True)
    tmp.rmdir()

    print("\n*** import_texture smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
