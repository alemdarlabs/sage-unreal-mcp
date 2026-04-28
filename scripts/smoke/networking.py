#!/usr/bin/env python3
"""Phase 4.x — networking domain smoke (11 tools).

Spawn target actor → mutate every networking flag → get_info readback.

Note: networking.set_property_replicated is a stub (returns note only)
because UPROPERTY replication needs C++ source or BP variable flags.
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
    hr("Spawn target actor")
    actor = call("spawn_actor", {
        "class":    "/Script/Engine.StaticMeshActor",
        "location": [50.0, 50.0, 100.0],
        "label":    "NetSmokeTarget",
    })
    print(json.dumps(actor, indent=2))
    aid = actor["actor_id"]

    try:
        hr("networking.get_info (defaults)")
        info0 = call("networking.get_info", {"actor_id": aid})
        print(json.dumps(info0, indent=2))
        # Defaults captured for sanity (engine ships DORM_Awake for AActor by default)
        assert "replicates" in info0
        assert "dormancy" in info0

        hr("networking.set_replicates true")
        r = call("networking.set_replicates", {"actor_id": aid, "replicates": True})
        assert r["replicates"] is True

        hr("networking.set_replicate_movement true")
        rm = call("networking.set_replicate_movement",
                  {"actor_id": aid, "replicate_movement": True})
        assert rm["replicate_movement"] is True

        hr("networking.set_always_relevant true")
        ar = call("networking.set_always_relevant",
                  {"actor_id": aid, "always_relevant": True})
        assert ar["always_relevant"] is True

        hr("networking.set_only_relevant_to_owner true")
        orto = call("networking.set_only_relevant_to_owner",
                    {"actor_id": aid, "only_relevant_to_owner": True})
        assert orto["only_relevant_to_owner"] is True

        hr("networking.set_net_load_on_client false")
        nl = call("networking.set_net_load_on_client",
                  {"actor_id": aid, "net_load_on_client": False})
        assert nl["net_load_on_client"] is False

        hr("networking.set_dormancy DORM_DormantAll")
        d = call("networking.set_dormancy",
                 {"actor_id": aid, "dormancy": "DORM_DormantAll"})
        assert d["dormancy"] == "DORM_DormantAll"

        hr("networking.set_priority 7.5")
        p = call("networking.set_priority", {"actor_id": aid, "net_priority": 7.5})
        assert abs(p["net_priority"] - 7.5) < 1e-3

        hr("networking.configure_net_frequency 60/5")
        f = call("networking.configure_net_frequency", {
            "actor_id":                 aid,
            "net_update_frequency":     60.0,
            "min_net_update_frequency": 5.0,
        })
        assert abs(f["net_update_frequency"] - 60.0) < 1e-3
        assert abs(f["min_net_update_frequency"] - 5.0) < 1e-3

        hr("networking.configure_cull_distance 1500")
        c = call("networking.configure_cull_distance",
                 {"actor_id": aid, "cull_distance": 1500.0})
        assert abs(c["cull_distance"] - 1500.0) < 1e-3

        hr("networking.get_info (after all writes)")
        info1 = call("networking.get_info", {"actor_id": aid})
        print(json.dumps(info1, indent=2))
        # Verify each flag readback
        assert info1["replicates"] is True
        assert info1["always_relevant"] is True
        assert info1["only_relevant_to_owner"] is True
        assert info1["net_load_on_client"] is False
        assert info1["dormancy"] == "DORM_DormantAll"
        assert abs(info1["net_priority"] - 7.5) < 1e-3
        assert abs(info1["net_update_frequency"] - 60.0) < 1e-3
        assert abs(info1["min_net_update_frequency"] - 5.0) < 1e-3
        assert abs(info1["net_cull_distance"] - 1500.0) < 1e-3

        hr("networking.set_property_replicated (note-only stub)")
        sp = call("networking.set_property_replicated", {})
        print(json.dumps(sp, indent=2))
        assert "note" in sp

        hr("networking.set_replicates missing actor_id (-32602)")
        try:
            call("networking.set_replicates", {})
            print("FAIL: missing actor_id accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        hr("networking.get_info bogus actor (-32602)")
        try:
            call("networking.get_info", {"actor_id": "/Game/NoSuch.Actor:NotReal"})
            print("FAIL: bogus actor accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

    finally:
        hr("Cleanup actor")
        try:
            call("delete_actor", {"actor_id": aid})
        except RuntimeError as e:
            print(f"  (actor cleanup soft-fail: {e})")

    print("\n*** networking smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
