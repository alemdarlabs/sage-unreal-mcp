#!/usr/bin/env python3
"""Phase 4.x — GAS (GameplayAbilities) domain smoke.

Coverage:
  asset writes      : create_ability, create_effect, create_cue
  asset mutate      : set_ability_tags, set_effect_modifier
  actor mutate      : add_asc on a freshly spawned target dummy actor
  diagnostics       : get_info on actor (with + without ASC)
  stub-only (note)  : create_attribute_set, add_attribute

Skips gracefully when GameplayAbilities plugin classes are unavailable.
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
    ability_path = "/Game/Sage_Smoke/GA_Sage_Test"
    effect_path  = "/Game/Sage_Smoke/GE_Sage_Test"
    cue_path     = "/Game/Sage_Smoke/GCN_Sage_Test"

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch",
             {"paths": [ability_path, effect_path, cue_path]})
    except RuntimeError:
        pass

    # --- create_ability ------------------------------------------------------
    hr("gas.create_ability")
    try:
        a = call("gas.create_ability", {"path": ability_path})
        print(json.dumps(a, indent=2))
        assert a["parent_class"] == "GameplayAbility"
        gas_available = True
    except RuntimeError as e:
        msg = str(e)
        if "GameplayAbilities plugin required" in msg:
            print(f"  (skipping — GAS plugin not loaded: {msg})")
            return 0
        raise

    hr("gas.create_ability missing path (-32602)")
    try:
        call("gas.create_ability", {})
        print("FAIL: missing path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    # --- create_effect -------------------------------------------------------
    hr("gas.create_effect")
    e_obj = call("gas.create_effect", {"path": effect_path})
    print(json.dumps(e_obj, indent=2))
    assert e_obj["class"] == "GameplayEffect"

    # --- create_cue ----------------------------------------------------------
    hr("gas.create_cue")
    c_obj = call("gas.create_cue", {"path": cue_path})
    print(json.dumps(c_obj, indent=2))
    assert c_obj["class"] == "GameplayCueNotify_Static"

    # --- set_ability_tags ----------------------------------------------------
    hr("gas.set_ability_tags (informational reflection only)")
    tags = call("gas.set_ability_tags", {
        "path": ability_path,
        "tags": ["Ability.Smoke.Test", "Ability.Sage"],
    })
    print(json.dumps(tags, indent=2))
    assert tags["path"].startswith(ability_path)
    assert tags["tags_requested"] == ["Ability.Smoke.Test", "Ability.Sage"]

    hr("gas.set_ability_tags missing path (-32602)")
    try:
        call("gas.set_ability_tags", {"tags": ["X"]})
        print("FAIL: missing path accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    # --- set_effect_modifier -------------------------------------------------
    hr("gas.set_effect_modifier (set DurationPolicy field)")
    mod = call("gas.set_effect_modifier", {
        "path":           effect_path,
        # DurationPolicy is an enum on GameplayEffect — InstantApplication = 0
        "DurationPolicy": "Instant",
    })
    print(json.dumps(mod, indent=2))
    assert mod["path"].startswith(effect_path)

    # --- add_asc on a spawned actor -----------------------------------------
    hr("spawn target actor (StaticMeshActor)")
    actor = call("spawn_actor", {
        "class":    "/Script/Engine.StaticMeshActor",
        "location": [0.0, 0.0, 100.0],
        "label":    "GASSmokeTarget",
    })
    print(json.dumps(actor, indent=2))
    actor_id = actor["actor_id"]

    hr("gas.get_info (no ASC yet)")
    g0 = call("gas.get_info", {"actor_id": actor_id})
    print(json.dumps(g0, indent=2))
    assert g0["has_asc"] is False

    hr("gas.add_asc")
    add = call("gas.add_asc", {"actor_id": actor_id})
    print(json.dumps(add, indent=2))
    assert add["component"]
    # idempotent
    add2 = call("gas.add_asc", {"actor_id": actor_id})
    print(json.dumps(add2, indent=2))
    assert add2.get("already_exists") is True

    hr("gas.get_info (after ASC)")
    g1 = call("gas.get_info", {"actor_id": actor_id})
    print(json.dumps(g1, indent=2))
    assert g1["has_asc"] is True
    assert "asc_name" in g1

    hr("gas.add_asc missing actor_id (-32602)")
    try:
        call("gas.add_asc", {})
        print("FAIL: missing actor_id accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    # --- stub-only tools -----------------------------------------------------
    hr("gas.create_attribute_set (note-only stub)")
    s1 = call("gas.create_attribute_set", {})
    print(json.dumps(s1, indent=2))
    assert "note" in s1

    hr("gas.add_attribute (note-only stub)")
    s2 = call("gas.add_attribute", {})
    print(json.dumps(s2, indent=2))
    assert "note" in s2

    # --- cleanup -------------------------------------------------------------
    hr("Cleanup actor")
    try:
        call("delete_actor", {"actor_id": actor_id})
    except RuntimeError as e:
        print(f"  (actor cleanup soft-fail: {e})")

    hr("Cleanup assets")
    cleanup = call("asset.delete_batch",
                   {"paths": [ability_path, effect_path, cue_path]})
    print(f"  deleted: {cleanup['deleted']}")

    print("\n*** gas smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
