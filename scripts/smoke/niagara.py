#!/usr/bin/env python3
"""Phase 4.x — Niagara domain smoke (26 tools).

Coverage:
  asset CRUD : create (NiagaraSystem), create_emitter, create_system_from_spec
  reads      : list, get_info, list_emitters, get_emitter_info,
               list_modules, list_renderers, list_system_parameters
  reflection : set_parameter (via UPROPERTY)
  notes      : add_emitter, add_renderer, remove_renderer,
               set_emitter_property, set_renderer_property,
               inspect_data_interfaces, get_compiled_hlsl,
               list_module_inputs, set_module_input,
               list_static_switches, set_static_switch,
               create_module_from_hlsl, create_scratch_module, batch
  level      : spawn (NiagaraActor — stays in level)

Skips gracefully if the Niagara plugin isn't loaded.
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
    sys_path = "/Game/Sage_Smoke/NS_Sage_Test"
    em_path  = "/Game/Sage_Smoke/NE_Sage_Test"
    spawned: list[str] = []

    hr("Pre-cleanup")
    try:
        call("asset.delete_batch", {"paths": [sys_path, em_path]})
    except RuntimeError:
        pass

    try:
        # --- create system + emitter ----------------------------------------
        hr("niagara.create (NiagaraSystem)")
        try:
            cs = call("niagara.create", {"path": sys_path})
            print(json.dumps(cs, indent=2))
            assert cs["class"] == "NiagaraSystem"
        except RuntimeError as e:
            if "Niagara plugin required" in str(e):
                print(f"  (skipping — Niagara plugin not loaded)")
                return 0
            raise

        hr("niagara.create missing path (-32602)")
        try:
            call("niagara.create", {})
            print("FAIL: missing path accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        hr("niagara.create_emitter")
        ce = call("niagara.create_emitter", {"path": em_path})
        print(json.dumps(ce, indent=2))
        assert ce["class"] == "NiagaraEmitter"

        hr("niagara.create_system_from_spec (delegates to create)")
        spec_path = "/Game/Sage_Smoke/NS_Sage_FromSpec"
        try:
            spec = call("niagara.create_system_from_spec", {"path": spec_path})
            assert spec["class"] == "NiagaraSystem"
            call("asset.delete_batch", {"paths": [spec_path]})
        except RuntimeError as e:
            print(f"  (soft-skip: {e})")

        # --- reads -----------------------------------------------------------
        hr("niagara.list /Game/Sage_Smoke")
        nl = call("niagara.list", {"path": "/Game/Sage_Smoke"})
        print(f"  count={nl['count']}")
        types = {a["type"] for a in nl["assets"]}
        assert "NiagaraSystem" in types
        assert "NiagaraEmitter" in types

        hr("niagara.get_info on system")
        gi = call("niagara.get_info", {"path": sys_path})
        print(json.dumps(gi, indent=2))
        assert gi["class"] == "NiagaraSystem"

        hr("niagara.get_info missing path (-32602)")
        try:
            call("niagara.get_info", {})
            print("FAIL: missing path accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        hr("niagara.get_emitter_info on emitter (delegates to get_info)")
        gei = call("niagara.get_emitter_info", {"path": em_path})
        print(json.dumps(gei, indent=2))
        assert gei["class"] == "NiagaraEmitter"

        hr("niagara.list_emitters on system")
        le = call("niagara.list_emitters", {"path": sys_path})
        print(f"  emitters_field present: {'emitters' in le}")
        assert "emitters" in le

        hr("niagara.list_modules /Game/Sage_Smoke")
        lm = call("niagara.list_modules", {"path": "/Game/Sage_Smoke"})
        print(f"  count={lm['count']}")
        assert "modules" in lm

        hr("niagara.list_renderers on system")
        lr = call("niagara.list_renderers", {"path": sys_path})
        print(f"  count={lr['count']}")
        assert "renderers" in lr

        hr("niagara.list_system_parameters on system")
        lsp = call("niagara.list_system_parameters", {"path": sys_path})
        print(f"  params={len(lsp['params'])}")
        assert "params" in lsp

        # --- set_parameter (reflection-based UPROPERTY write) ---------------
        hr("niagara.set_parameter (try a known UPROPERTY)")
        # Most NiagaraSystem props are visible via reflection. We test
        # the failure path (unknown name) for stability across versions.
        sp = call("niagara.set_parameter", {
            "path":  sys_path,
            "name":  "DefinitelyNotARealUPROPERTY_FromSmoke",
            "value": 1.0,
        })
        print(json.dumps(sp, indent=2))
        assert sp["set"] is False
        assert "note" in sp

        hr("niagara.set_parameter missing name (-32602)")
        try:
            call("niagara.set_parameter", {"path": sys_path})
            print("FAIL: missing name accepted")
            return 1
        except RuntimeError as e:
            print(f"OK rejected: {e}")

        # --- spawn NiagaraActor in level ------------------------------------
        hr("niagara.spawn NiagaraActor")
        sp_actor = call("niagara.spawn", {
            "path":     sys_path,
            "location": [800.0, 0.0, 200.0],
        })
        print(json.dumps(sp_actor, indent=2))
        assert "actor_id" in sp_actor
        spawned.append(sp_actor["actor_id"])

        # --- note-only stubs (15 tools) -------------------------------------
        for tool in (
            "niagara.add_emitter",
            "niagara.set_emitter_property",
            "niagara.add_renderer",
            "niagara.remove_renderer",
            "niagara.set_renderer_property",
            "niagara.get_compiled_hlsl",
            "niagara.list_module_inputs",
            "niagara.set_module_input",
            "niagara.list_static_switches",
            "niagara.set_static_switch",
            "niagara.create_module_from_hlsl",
            "niagara.create_scratch_module",
            "niagara.batch",
        ):
            res = call(tool, {})
            assert "note" in res, f"{tool} missing note"
            print(f"  {tool:36s} → note ok")

        hr("niagara.inspect_data_interfaces")
        idi = call("niagara.inspect_data_interfaces", {"path": sys_path})
        print(json.dumps(idi, indent=2))
        assert "note" in idi

        hr("niagara.inspect_data_interfaces missing path (-32602)")
        try:
            call("niagara.inspect_data_interfaces", {})
            print("FAIL: missing path accepted")
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
            cleanup = call("asset.delete_batch",
                           {"paths": [sys_path, em_path]})
            print(f"  deleted: {cleanup['deleted']}")
        except RuntimeError as e:
            print(f"  (soft-fail: {e})")

    print("\n*** niagara smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
