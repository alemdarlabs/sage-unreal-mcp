#!/usr/bin/env python3
"""Phase 4.x — gameplay domain smoke (45 tools).

Covers:
  AI assets       : create_behavior_tree, create_blackboard, create_eqs_query,
                    create_state_tree, create_smart_object_def,
                    list_behavior_trees, list_eqs_queries, list_state_trees,
                    get_behavior_tree_info, read_behavior_tree_graph
  Input system    : create_input_action, create_input_mapping,
                    list_input_assets, list_input_mappings, read_imc,
                    add_imc_mapping (note), remove_imc_mapping (note),
                    set_imc_mapping_action (note), set_imc_mapping_key (note),
                    set_mapping_modifiers (note)
  Framework BPs   : create_game_mode, create_game_state, create_hud,
                    create_player_controller, create_player_state,
                    set_world_game_mode, get_framework_info
  Actor mutation  : set_collision_enabled, set_collision_profile,
                    set_simulate_physics, set_physics_properties,
                    add_perception (note), configure_sense (note),
                    add_state_tree_component (note),
                    add_smart_object_component (note)
  Navigation      : spawn_nav_modifier, project_to_nav, rebuild_navigation,
                    get_navmesh_info, get_navmesh_details
  PIE-only        : apply_damage_in_pie, inspect_pie,
                    get_pie_anim_state, get_pie_anim_properties,
                    get_pie_subsystem_state — all error gracefully outside PIE
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
    with urllib.request.urlopen(req, timeout=20) as resp:
        body = json.loads(resp.read())
    if "error" in body:
        raise RuntimeError(f"{name}: {body['error']}")
    return json.loads(body["result"]["content"][0]["text"])


def hr(s: str) -> None:
    print(f"\n=== {s} ===")


def main() -> int:
    bt   = "/Game/Sage_Smoke/BT_Sage_Test"
    bb   = "/Game/Sage_Smoke/BB_Sage_Test"
    eqs  = "/Game/Sage_Smoke/EQS_Sage_Test"
    st   = "/Game/Sage_Smoke/ST_Sage_Test"
    so   = "/Game/Sage_Smoke/SO_Sage_Test"
    ia   = "/Game/Sage_Smoke/IA_Sage_Test"
    imc  = "/Game/Sage_Smoke/IMC_Sage_Test"
    gm   = "/Game/Sage_Smoke/GM_Sage_Test"
    gs   = "/Game/Sage_Smoke/GS_Sage_Test"
    pc   = "/Game/Sage_Smoke/PC_Sage_Test"
    ps   = "/Game/Sage_Smoke/PS_Sage_Test"
    hud  = "/Game/Sage_Smoke/HUD_Sage_Test"
    paths = [bt, bb, eqs, st, so, ia, imc, gm, gs, pc, ps, hud]
    spawned: list[str] = []

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": paths})
    except RuntimeError:
        pass

    try:
        # ===================================================================
        # AI asset CRUD
        # ===================================================================
        hr("gameplay.create_behavior_tree")
        b = call("gameplay.create_behavior_tree", {"path": bt})
        print(json.dumps(b, indent=2))
        assert "path" in b

        hr("gameplay.create_behavior_tree missing path (-32602)")
        try:
            call("gameplay.create_behavior_tree", {})
            print("FAIL: missing path accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        hr("gameplay.create_blackboard")
        bbres = call("gameplay.create_blackboard", {"path": bb})
        print(json.dumps(bbres, indent=2))

        hr("gameplay.create_eqs_query")
        e = call("gameplay.create_eqs_query", {"path": eqs})
        print(json.dumps(e, indent=2))

        hr("gameplay.create_state_tree (plugin-gated)")
        try:
            s = call("gameplay.create_state_tree", {"path": st})
            print(json.dumps(s, indent=2))
        except RuntimeError as e:
            print(f"  (plugin not loaded — OK: {e})")
            paths.remove(st)

        hr("gameplay.create_smart_object_def (plugin-gated)")
        try:
            so_res = call("gameplay.create_smart_object_def", {"path": so})
            print(json.dumps(so_res, indent=2))
        except RuntimeError as e:
            print(f"  (plugin not loaded — OK: {e})")
            paths.remove(so)

        # ===================================================================
        # Reads
        # ===================================================================
        hr("gameplay.list_behavior_trees /Game/Sage_Smoke")
        lbt = call("gameplay.list_behavior_trees", {"path": "/Game/Sage_Smoke"})
        print(f"  count={lbt['count']}")
        assert lbt["count"] >= 1

        hr("gameplay.list_eqs_queries /Game/Sage_Smoke")
        leqs = call("gameplay.list_eqs_queries", {"path": "/Game/Sage_Smoke"})
        print(f"  count={leqs['count']}")

        hr("gameplay.list_state_trees /Game/Sage_Smoke")
        lst = call("gameplay.list_state_trees", {"path": "/Game/Sage_Smoke"})
        print(f"  count={lst['count']}")

        hr("gameplay.get_behavior_tree_info BT_Sage_Test")
        gbi = call("gameplay.get_behavior_tree_info", {"path": bt})
        print(f"  result keys: {list(gbi.keys())[:5]}")

        hr("gameplay.read_behavior_tree_graph BT_Sage_Test")
        rbg = call("gameplay.read_behavior_tree_graph", {"path": bt})
        print(f"  result keys: {list(rbg.keys())[:5]}")

        # ===================================================================
        # Input system
        # ===================================================================
        hr("gameplay.create_input_action")
        cia = call("gameplay.create_input_action", {"path": ia})
        print(json.dumps(cia, indent=2))

        hr("gameplay.create_input_mapping")
        cim = call("gameplay.create_input_mapping", {"path": imc})
        print(json.dumps(cim, indent=2))

        hr("gameplay.list_input_assets /Game/Sage_Smoke")
        lia = call("gameplay.list_input_assets", {"path": "/Game/Sage_Smoke"})
        print(f"  count={lia.get('count', '?')}")

        hr("gameplay.list_input_mappings on IMC asset (alias of read_imc)")
        lim = call("gameplay.list_input_mappings", {"path": imc})
        print(f"  result keys: {list(lim.keys())[:5]}")

        hr("gameplay.read_imc")
        rimc = call("gameplay.read_imc", {"path": imc})
        print(f"  result keys: {list(rimc.keys())[:5]}")

        hr("gameplay.read_imc missing path (-32602)")
        try:
            call("gameplay.read_imc", {})
            print("FAIL: missing path accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # IMC mapping mutation (note-only stubs)
        for tool in (
            "gameplay.add_imc_mapping",
            "gameplay.remove_imc_mapping",
            "gameplay.set_imc_mapping_action",
            "gameplay.set_imc_mapping_key",
            "gameplay.set_mapping_modifiers",
        ):
            res = call(tool, {"path": imc})
            assert "note" in res, f"{tool} missing note"
            print(f"  {tool:38s} → note ok")

        # ===================================================================
        # Framework BPs
        # ===================================================================
        for tool, path in [
            ("gameplay.create_game_mode",         gm),
            ("gameplay.create_game_state",        gs),
            ("gameplay.create_player_controller", pc),
            ("gameplay.create_player_state",      ps),
            ("gameplay.create_hud",               hud),
        ]:
            hr(tool)
            res = call(tool, {"path": path})
            print(json.dumps(res, indent=2))
            assert "path" in res

        hr("gameplay.get_framework_info")
        gfi = call("gameplay.get_framework_info", {})
        print(json.dumps(gfi, indent=2))

        hr("gameplay.set_world_game_mode")
        try:
            sw = call("gameplay.set_world_game_mode",
                      {"game_mode_class": gm + "_C"})
            print(json.dumps(sw, indent=2))
        except RuntimeError as e:
            print(f"  (soft-skip: {e})")

        hr("gameplay.set_world_game_mode missing class (-32602)")
        try:
            call("gameplay.set_world_game_mode", {})
            print("FAIL: missing class accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # ===================================================================
        # Actor mutation: spawn target, tweak collision/physics
        # ===================================================================
        hr("Spawn target actor")
        actor = call("spawn_actor", {
            "class":    "/Script/Engine.StaticMeshActor",
            "location": [300.0, 300.0, 200.0],
            "label":    "GameplaySmokeTarget",
        })
        spawned.append(actor["actor_id"])
        aid = actor["actor_id"]

        hr("gameplay.set_collision_enabled mode=QueryAndPhysics")
        sce = call("gameplay.set_collision_enabled",
                   {"actor_id": aid, "mode": "QueryAndPhysics"})
        print(json.dumps(sce, indent=2))

        hr("gameplay.set_collision_enabled missing mode (-32602)")
        try:
            call("gameplay.set_collision_enabled", {"actor_id": aid})
            print("FAIL: missing mode accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        hr("gameplay.set_collision_profile profile=BlockAll")
        scp = call("gameplay.set_collision_profile",
                   {"actor_id": aid, "profile": "BlockAll"})
        print(json.dumps(scp, indent=2))

        hr("gameplay.set_simulate_physics simulate=False")
        ssp = call("gameplay.set_simulate_physics",
                   {"actor_id": aid, "simulate": False})
        print(json.dumps(ssp, indent=2))

        hr("gameplay.set_physics_properties")
        spp = call("gameplay.set_physics_properties",
                   {"actor_id": aid, "mass": 50.0,
                    "linear_damping": 0.1})
        print(json.dumps(spp, indent=2))

        # Note-only / stub mutators
        hr("gameplay.add_perception (note-only)")
        ap = call("gameplay.add_perception", {"path": pc})
        assert "note" in ap

        hr("gameplay.configure_sense (note-only)")
        cs = call("gameplay.configure_sense", {"path": pc})
        assert "note" in cs

        hr("gameplay.add_state_tree_component (note-only)")
        astc = call("gameplay.add_state_tree_component", {"path": pc})
        assert "note" in astc or "actor_id" in astc

        hr("gameplay.add_smart_object_component (note-only)")
        asoc = call("gameplay.add_smart_object_component", {"path": pc})
        assert "note" in asoc or "actor_id" in asoc

        # ===================================================================
        # Navigation
        # ===================================================================
        hr("gameplay.spawn_nav_modifier at [400,400,200]")
        try:
            snm = call("gameplay.spawn_nav_modifier",
                       {"location": [400.0, 400.0, 200.0]})
            print(json.dumps(snm, indent=2))
            if "actor_id" in snm:
                spawned.append(snm["actor_id"])
        except RuntimeError as e:
            print(f"  (nav volume class not found — OK: {e})")

        hr("gameplay.project_to_nav at [0,0,200]")
        ptn = call("gameplay.project_to_nav", {"location": [0.0, 0.0, 200.0]})
        print(json.dumps(ptn, indent=2))

        hr("gameplay.rebuild_navigation")
        try:
            rn = call("gameplay.rebuild_navigation", {})
            print(json.dumps(rn, indent=2))
        except RuntimeError as e:
            print(f"  (soft-skip: {e})")

        hr("gameplay.get_navmesh_info")
        gni = call("gameplay.get_navmesh_info", {})
        print(f"  result keys: {list(gni.keys())[:5]}")

        hr("gameplay.get_navmesh_details")
        gnd = call("gameplay.get_navmesh_details", {})
        print(f"  result keys: {list(gnd.keys())[:5]}")

        # ===================================================================
        # PIE-only tools (should error-gracefully when not in PIE)
        # ===================================================================
        for tool in (
            "gameplay.inspect_pie",
            "gameplay.get_pie_anim_state",
            "gameplay.get_pie_subsystem_state",
        ):
            hr(f"{tool} (no PIE — expect graceful error or empty)")
            try:
                res = call(tool, {})
                print(f"  result keys: {list(res.keys())[:5]}")
            except RuntimeError as e:
                print(f"  (no PIE — OK: {e})")

        # PIE-only with required args
        hr("gameplay.apply_damage_in_pie missing actor_id (-32602 or PIE error)")
        try:
            call("gameplay.apply_damage_in_pie", {})
            print("FAIL: missing actor_id accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        hr("gameplay.get_pie_anim_properties missing actor_id (-32602)")
        try:
            call("gameplay.get_pie_anim_properties", {})
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

        hr("Cleanup assets")
        try:
            cleanup = call("asset.delete_batch", {"paths": paths})
            print(f"  deleted: {cleanup['deleted']}")
        except RuntimeError as e:
            print(f"  (soft-fail: {e})")

    print("\n*** gameplay smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
