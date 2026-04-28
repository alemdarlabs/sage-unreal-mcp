#!/usr/bin/env python3
"""Phase 4.x — PCG (Procedural Content Generation) domain smoke (16 tools).

Coverage:
  asset CRUD : create_graph, list_graphs, read_graph
  notes      : add_node, connect_nodes, set_node_settings, remove_node,
               read_node_settings, set_static_mesh_spawner_meshes,
               toggle_graph, cleanup
  level      : add_volume (creates PCGVolume actor in level)
  runtime    : execute, force_regenerate (need PCG component)
  reads      : get_components, get_component_details

Skips gracefully if the PCG plugin isn't loaded.
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
    graph_path = "/Game/Sage_Smoke/PCG_Sage_Test"
    spawned: list[str] = []

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [graph_path]})
    except RuntimeError:
        pass

    try:
        # --- create_graph ----------------------------------------------------
        hr("pcg.create_graph")
        try:
            cg = call("pcg.create_graph", {"path": graph_path})
            print(json.dumps(cg, indent=2))
            assert cg["class"] == "PCGGraph"
        except RuntimeError as e:
            if "PCG plugin required" in str(e):
                print(f"  (skipping — PCG plugin not loaded: {e})")
                return 0
            raise

        hr("pcg.create_graph missing path (-32602)")
        try:
            call("pcg.create_graph", {})
            print("FAIL: missing path accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # --- list_graphs / read_graph ---------------------------------------
        hr("pcg.list_graphs /Game/Sage_Smoke")
        lg = call("pcg.list_graphs", {"path": "/Game/Sage_Smoke"})
        print(f"  count={lg['count']}")
        assert lg["count"] >= 1
        assert any(g["name"] == "PCG_Sage_Test" for g in lg["graphs"])

        hr("pcg.read_graph")
        rg = call("pcg.read_graph", {"path": graph_path})
        print(json.dumps(rg, indent=2))
        assert rg["class"] == "PCGGraph"
        assert rg["path"].startswith(graph_path)

        hr("pcg.read_graph missing path (-32602)")
        try:
            call("pcg.read_graph", {})
            print("FAIL: missing path accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # --- node-level tools (note-only stubs) -----------------------------
        for tool in ("pcg.add_node", "pcg.connect_nodes",
                     "pcg.set_node_settings", "pcg.remove_node",
                     "pcg.read_node_settings", "pcg.set_static_mesh_spawner_meshes",
                     "pcg.toggle_graph"):
            hr(f"{tool} (note-only stub)")
            res = call(tool, {})
            assert "note" in res, f"{tool} missing note: {res}"
            print(f"  note: {res['note'][:80]}…")

        hr("pcg.cleanup")
        c = call("pcg.cleanup", {})
        print(json.dumps(c, indent=2))
        assert c["cleaned"] is True

        # --- level: add a PCG volume ----------------------------------------
        hr("pcg.add_volume at [500,500,300]")
        try:
            vol = call("pcg.add_volume", {"location": [500.0, 500.0, 300.0]})
            print(json.dumps(vol, indent=2))
            assert "actor_id" in vol
            spawned.append(vol["actor_id"])
        except RuntimeError as e:
            if "PCG plugin required" in str(e):
                print(f"  (skipping volume — {e})")
            else:
                raise

        # --- list components in level (likely 0 unless volume has PCGComp) --
        hr("pcg.get_components")
        gc = call("pcg.get_components", {})
        print(f"  count={gc['count']}")
        assert "components" in gc

        # --- get_component_details on a non-PCG actor (still returns) -------
        if spawned:
            hr("pcg.get_component_details on PCG volume")
            gcd = call("pcg.get_component_details", {"actor_id": spawned[0]})
            print(f"  actor_id={gcd['actor_id']} props={'properties' in gcd}")
            assert gcd["actor_id"] == spawned[0]

        hr("pcg.get_component_details missing actor_id (-32602)")
        try:
            call("pcg.get_component_details", {})
            print("FAIL: missing actor_id accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # --- execute / force_regenerate (use spawned PCG volume if present) -
        if spawned:
            hr("pcg.execute on PCG volume (console-driven)")
            ex = call("pcg.execute", {"actor_id": spawned[0]})
            print(json.dumps(ex, indent=2))
            assert ex["triggered"] is True

            hr("pcg.force_regenerate")
            fr = call("pcg.force_regenerate", {"actor_id": spawned[0]})
            assert fr["triggered"] is True

        hr("pcg.execute missing actor_id (-32602)")
        try:
            call("pcg.execute", {})
            print("FAIL: missing actor_id accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

    finally:
        hr("Cleanup spawned actors")
        for aid in spawned:
            try:
                call("delete_actor", {"actor_id": aid})
                print(f"  deleted actor: {aid.rsplit('.', 1)[-1]}")
            except RuntimeError as e:
                print(f"  (soft-fail: {e})")

        hr("Cleanup graph asset")
        try:
            cleanup = call("asset.delete_batch", {"paths": [graph_path]})
            print(f"  deleted: {cleanup['deleted']}")
        except RuntimeError as e:
            print(f"  (soft-fail: {e})")

    print("\n*** pcg smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
