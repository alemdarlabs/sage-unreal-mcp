#!/usr/bin/env python3
"""Phase 4.x — audio domain smoke.

audio.create_cue → audio.create_metasound (optional) → audio.list
→ audio.spawn_ambient → audio.play_at_location → cleanup.
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
    cue_path  = "/Game/Sage_Smoke/SC_Audio_Test"
    meta_path = "/Game/Sage_Smoke/MS_Audio_Test"

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [cue_path, meta_path]})
    except RuntimeError:
        pass

    hr("audio.create_cue")
    cue = call("audio.create_cue", {"path": cue_path})
    print(json.dumps(cue, indent=2))
    assert cue["class"] == "SoundCue"
    assert cue["path"].startswith(cue_path)

    hr("audio.create_cue missing path (-32602)")
    try:
        call("audio.create_cue", {})
        print("FAIL: missing path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("audio.create_metasound (optional — needs plugin)")
    meta_available = True
    try:
        meta = call("audio.create_metasound", {"path": meta_path})
        print(json.dumps(meta, indent=2))
        assert meta["class"] == "MetaSoundSource"
    except RuntimeError as e:
        print(f"  (skipped — {e})")
        meta_available = False

    hr("audio.list type=SoundCue path=/Game/Sage_Smoke")
    listed = call("audio.list",
                  {"path": "/Game/Sage_Smoke", "type": "SoundCue"})
    print(f"  count={listed['count']}")
    assert listed["count"] >= 1
    assert any(a["name"] == "SC_Audio_Test" for a in listed["assets"]), \
        f"expected SC_Audio_Test in {[a['name'] for a in listed['assets']]}"

    if meta_available:
        hr("audio.list type=MetaSound")
        listed_meta = call("audio.list",
                           {"path": "/Game/Sage_Smoke", "type": "MetaSound"})
        print(f"  count={listed_meta['count']}")
        assert any(a["name"] == "MS_Audio_Test" for a in listed_meta["assets"])

    hr("audio.list no type filter (default)")
    listed_all = call("audio.list", {"path": "/Game/Sage_Smoke"})
    print(f"  count={listed_all['count']}")
    assert listed_all["count"] >= 1

    hr("audio.spawn_ambient at origin")
    ambient = call("audio.spawn_ambient", {
        "path":     cue_path,
        "location": [0.0, 0.0, 200.0],
    })
    print(json.dumps(ambient, indent=2))
    assert ambient["sound_path"] == cue_path
    assert "actor_id" in ambient
    actor_id = ambient["actor_id"]

    hr("audio.spawn_ambient missing path (-32602)")
    try:
        call("audio.spawn_ambient", {})
        print("FAIL: missing path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("audio.play_at_location (console exec)")
    played = call("audio.play_at_location", {
        "path":     cue_path,
        "location": [100.0, 100.0, 100.0],
        "volume":   0.5,
    })
    print(json.dumps(played, indent=2))
    assert played["triggered"] is True
    assert played["volume"] == 0.5

    hr("Cleanup actor")
    try:
        call("delete_actor", {"actor_id": actor_id})
    except RuntimeError as e:
        print(f"  (actor cleanup soft-fail: {e})")

    hr("Cleanup assets")
    paths = [cue_path] + ([meta_path] if meta_available else [])
    cleanup = call("asset.delete_batch", {"paths": paths})
    print(f"  deleted: {cleanup['deleted']}")

    print("\n*** audio smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
