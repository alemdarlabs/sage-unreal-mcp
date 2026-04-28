#!/usr/bin/env python3
"""Phase 4.x — level domain smoke (22 tools).

Coverage:
  reads  : list, get_outliner, list_volumes, get_actors_by_class,
           count_actors_by_class, get_actor_details, get_actor_bounds,
           resolve_actor, get_runtime_virtual_texture_summary
  writes : spawn_light + set_light_properties,
           spawn_volume + set_volume_properties,
           set_world_settings (with revert),
           set_fog_properties (only if fog actor exists)

Skipped (need specific level state):
  - load / create  : would mutate the editor world
  - build_lighting : long-running, console-driven
  - set_actor_material : needs preexisting mesh+material slot
  - set_spline_points / get_spline_info : needs spline actor
  - set_water_body_property : needs water plugin
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
    spawned: list[str] = []
    original_gravity = -980.0  # restored at the end

    try:
        # --- Reads on existing level state -----------------------------------
        hr("level.list /Game (recursive)")
        levels = call("level.list", {"path": "/Game", "recursive": True})
        print(f"  count={levels['count']}")
        assert levels["count"] >= 1

        hr("level.get_outliner")
        outliner = call("level.get_outliner", {})
        print(f"  count={outliner['count']}")
        baseline_count = outliner["count"]
        assert baseline_count >= 1

        hr("level.count_actors_by_class")
        hist = call("level.count_actors_by_class", {})
        print(f"  total={hist['total']} unique_classes={len(hist['histogram'])}")
        assert hist["total"] >= 1
        assert isinstance(hist["histogram"], list)

        hr("level.get_runtime_virtual_texture_summary")
        rvt = call("level.get_runtime_virtual_texture_summary", {})
        print(json.dumps(rvt, indent=2))
        assert "count" in rvt

        # --- Spawn + mutate light --------------------------------------------
        hr("level.spawn_light PointLight")
        light = call("level.spawn_light", {
            "class":    "/Script/Engine.PointLight",
            "location": [0.0, 0.0, 500.0],
        })
        print(json.dumps(light, indent=2))
        light_id = light["actor_id"]
        spawned.append(light_id)
        assert "PointLight" in light["class"]

        hr("level.set_light_properties intensity=12345 color=[1,0.5,0.25]")
        slp = call("level.set_light_properties", {
            "actor_id":  light_id,
            "intensity": 12345.0,
            "color":     [1.0, 0.5, 0.25],
        })
        print(json.dumps(slp, indent=2))
        assert slp["modified"] is True

        hr("level.get_actor_details on light")
        ad = call("level.get_actor_details", {"actor_id": light_id})
        print(f"  class={ad['class']} components={len(ad['components'])}")
        assert ad["class"] in ("PointLight", "PointLight_C")
        assert any(c["class"].endswith("LightComponent") for c in ad["components"])

        hr("level.get_actor_bounds on light")
        bounds = call("level.get_actor_bounds", {"actor_id": light_id})
        print(json.dumps(bounds, indent=2))
        assert "origin" in bounds and "extent" in bounds

        # --- Spawn + mutate volume -------------------------------------------
        hr("level.spawn_volume BlockingVolume")
        vol = call("level.spawn_volume", {
            "class":    "/Script/Engine.BlockingVolume",
            "location": [200.0, 0.0, 200.0],
        })
        print(json.dumps(vol, indent=2))
        vol_id = vol["actor_id"]
        spawned.append(vol_id)
        assert "BlockingVolume" in vol["class"] or vol["class"] == "BlockingVolume"

        hr("level.list_volumes filter=BlockingVolume")
        lv = call("level.list_volumes", {"class": "BlockingVolume"})
        print(f"  count={lv['count']}")
        assert lv["count"] >= 1
        assert any(v["actor_id"] == vol_id for v in lv["volumes"])

        hr("level.set_volume_properties hidden=True")
        svp = call("level.set_volume_properties", {
            "actor_id": vol_id,
            "hidden":   True,
        })
        print(json.dumps(svp, indent=2))
        assert svp["modified"] is True

        # --- Class-based queries ---------------------------------------------
        hr("level.get_actors_by_class /Script/Engine.PointLight")
        gabc = call("level.get_actors_by_class",
                    {"class": "/Script/Engine.PointLight"})
        print(f"  count={gabc['count']}")
        assert gabc["count"] >= 1
        assert any(a["actor_id"] == light_id for a in gabc["actors"])

        hr("level.get_actors_by_class missing class (-32602)")
        try:
            call("level.get_actors_by_class", {})
            print("FAIL: missing class accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # --- Resolve by label ------------------------------------------------
        hr("level.resolve_actor by label (rough by simple name)")
        # spawn_actor sets label = ClassName by default
        try:
            res = call("level.resolve_actor", {"name": "BlockingVolume"})
            print(f"  resolved → {res.get('actor_id', 'n/a')}")
        except RuntimeError as e:
            print(f"  (no exact match; that's OK — labels vary): {e}")

        hr("level.resolve_actor missing name (-32602)")
        try:
            call("level.resolve_actor", {})
            print("FAIL: missing name accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # --- World settings round-trip ---------------------------------------
        hr("level.set_world_settings gravity_z=-1234")
        ws = call("level.set_world_settings", {"gravity_z": -1234.0})
        print(json.dumps(ws, indent=2))
        assert ws["modified"] is True
        assert abs(ws["gravity_z"] - (-1234.0)) < 1.0

        hr("level.set_world_settings revert gravity_z=-980")
        ws2 = call("level.set_world_settings", {"gravity_z": original_gravity})
        assert abs(ws2["gravity_z"] - original_gravity) < 1.0

        # --- Fog properties (only if fog actor exists) ----------------------
        hr("level.set_fog_properties (auto-detect ExpFog)")
        try:
            fog = call("level.set_fog_properties", {"fog_density": 0.05})
            print(json.dumps(fog, indent=2))
            assert fog["modified"] is True
        except RuntimeError as e:
            if "no ExponentialHeightFog" in str(e):
                print(f"  (no fog in level — skipping)")
            else:
                raise

        # --- Outliner grew --------------------------------------------------
        hr("level.get_outliner (after spawns)")
        outliner2 = call("level.get_outliner", {})
        print(f"  count={outliner2['count']} (was {baseline_count})")
        # We added 2 actors
        assert outliner2["count"] >= baseline_count + 2

        # --- Error case -----------------------------------------------------
        hr("level.get_actor_details missing actor_id (-32602)")
        try:
            call("level.get_actor_details", {})
            print("FAIL: missing actor_id accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

    finally:
        hr("Cleanup spawned actors")
        for aid in spawned:
            try:
                call("delete_actor", {"actor_id": aid})
                print(f"  deleted: {aid.rsplit('.', 1)[-1]}")
            except RuntimeError as e:
                print(f"  (soft-fail: {e})")

    print("\n*** level smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
