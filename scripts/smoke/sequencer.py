#!/usr/bin/env python3
"""Phase 4.6-r3-b6 — sequencer minimal smoke.

seq.create → seq.add_track → seq.list_tracks → cleanup.
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
    seq_path = "/Game/Sage_Smoke/LS_Sage_Test"
    try:
        call("asset.delete_batch", {"paths": [seq_path]})
    except RuntimeError:
        pass

    hr("seq.create")
    cre = call("seq.create", {"path": seq_path})
    print(json.dumps(cre, indent=2))
    assert cre["class"] == "LevelSequence"
    assert cre["track_count"] == 0

    hr("seq.list_tracks (empty)")
    lt0 = call("seq.list_tracks", {"path": seq_path})
    print(json.dumps(lt0, indent=2))
    assert lt0["track_count"] == 0

    hr("seq.add_track CameraCutTrack")
    at1 = call("seq.add_track", {
        "path":        seq_path,
        "track_class": "/Script/MovieSceneTracks.MovieSceneCameraCutTrack",
    })
    print(json.dumps(at1, indent=2))
    assert at1["track_count"] == 1
    assert at1["track_class"] == "MovieSceneCameraCutTrack"

    hr("seq.list_tracks (after camera cut)")
    lt1 = call("seq.list_tracks", {"path": seq_path})
    print(json.dumps(lt1, indent=2))
    assert lt1["track_count"] == 1
    assert any(t["class"] == "MovieSceneCameraCutTrack" for t in lt1["tracks"])

    hr("seq.add_track bad class (-32602)")
    try:
        call("seq.add_track", {
            "path":        seq_path,
            "track_class": "/Script/Engine.Actor",
        })
        print("FAIL: bad track class accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("seq.add_track abstract (-32602)")
    try:
        call("seq.add_track", {
            "path":        seq_path,
            "track_class": "/Script/MovieScene.MovieSceneTrack",
        })
        print("FAIL: abstract track accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("seq.list_tracks on non-sequence (-32602)")
    try:
        call("seq.list_tracks",
             {"path": "/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube"})
        print("FAIL: non-sequence accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("Cleanup")
    call("asset.delete_batch", {"paths": [seq_path]})

    print("\n*** sequencer smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
