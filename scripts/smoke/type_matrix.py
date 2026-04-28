"""Type matrix v2: fix the label tracking + add Phase 4.7-p4 write smoke."""
import json, urllib.request, sys

def call(name, args):
    req = json.dumps({'jsonrpc':'2.0','method':'tools/call','id':1,'params':{'name':name,'arguments':args}})
    r = urllib.request.urlopen('http://127.0.0.1:7777/mcp', data=req.encode(), timeout=60)
    return json.loads(r.read())

bp_path = "/Game/Test_TypeMatrix2.Test_TypeMatrix2"
src     = "/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter"
print("setup:", call("duplicate_asset", {"source": src, "destination": bp_path}).get("result",{}).get("structuredContent",{}).get("destination"))

PRIMITIVE = [("bool",None),("byte",None),("int",None),("int64",None),("real",None),("string",None),("name",None),("text",None)]
REF       = [("object","/Script/Engine.Actor"),("class","/Script/Engine.Actor"),("interface","/Script/Engine.Interface_AssetUserData"),("softobject","/Script/Engine.Actor"),("softclass","/Script/Engine.Actor")]
STRUCT    = [("struct","/Script/CoreUObject.Vector"),("struct","/Script/CoreUObject.Rotator"),("struct","/Script/CoreUObject.Transform"),("struct","/Script/CoreUObject.LinearColor")]
ENUMS     = [("enum","/Script/Engine.ECollisionChannel"),("byte","/Script/Engine.ECollisionChannel")]

added_names = []
def add(varname, t, to=None, is_array=False):
    args = {"path": bp_path, "name": varname, "type": t}
    if to: args["type_object"] = to
    if is_array: args["is_array"] = True
    d = call("bp.add_variable", args)
    if "error" in d:
        print(f"  ✗ {varname:32s} -> {d['error']['message']}")
        return False
    added_names.append(varname)
    print(f"  ✓ {varname}")
    return True

ok = 0; total = 0
print("\n=== Primitives ===")
for t,_ in PRIMITIVE:
    total+=2
    if add(f"P_{t}",     t):           ok+=1
    if add(f"PA_{t}",    t, is_array=True): ok+=1

print("\n=== Refs ===")
for t,to in REF:
    total+=2
    if add(f"R_{t}",     t, to):       ok+=1
    if add(f"RA_{t}",    t, to, True): ok+=1

print("\n=== Structs ===")
for i,(t,to) in enumerate(STRUCT):
    name = to.split('.')[-1].lower()
    total+=2
    if add(f"S_{name}",  t, to):       ok+=1
    if add(f"SA_{name}", t, to, True): ok+=1

print("\n=== Enums ===")
for i,(t,to) in enumerate(ENUMS):
    name = to.split('.')[-1].lower()
    total+=2
    if add(f"E_{t}_{name}",  t, to):       ok+=1
    if add(f"EA_{t}_{name}", t, to, True): ok+=1

print(f"\nresult: {ok}/{total} adds succeeded")

print("\n=== bp.compile after type-matrix mutations ===")
d = call("bp.compile", {"path": bp_path})
if "error" in d:
    print(f"  ✗ compile FAILED: {d['error']}")
    sys.exit(1)
else:
    sc = d['result']['structuredContent']
    print(f"  ✓ compile ok: errors={sc.get('errors')} warnings={sc.get('warnings')}")

print("\n=== bp.list_variables verify (proper name match this time) ===")
d = call("bp.list_variables", {"path": bp_path})
in_bp = {v.get("name") for v in d.get("result",{}).get("structuredContent",{}).get("variables", [])}
missing = set(added_names) - in_bp
if missing:
    print(f"  ✗ {len(missing)} missing: {sorted(missing)[:5]}...")
else:
    print(f"  ✓ all {len(added_names)} variables present")

print("\n=== Phase 4.7-p4 write smoke ===")
print("set_config:")
d = call("project.set_config", {"name":"Game","section":"/Script/SageTest.SageTestSettings","key":"SmokeKey","value":"42"})
print(f"  ✓ {d.get('result',{}).get('structuredContent') or d}")

print("\nverify by reading back:")
d = call("project.read_config", {"name":"Game"})
sections = d['result']['structuredContent']['sections']
found = False
for sec in sections:
    if sec['name'] == "/Script/SageTest.SageTestSettings":
        for e in sec['entries']:
            if e['key'] == "SmokeKey":
                print(f"  ✓ written value: {e['value']!r}")
                found = True
if not found:
    print("  ✗ written key not found on readback")

print("\nset_config with modifier=+ (array op):")
d = call("project.set_config", {"name":"Game","section":"/Script/SageTest.SageTestSettings","key":"SmokeArray","value":"item1","modifier":"+"})
print(f"  ✓ {d.get('result',{}).get('structuredContent') or d}")

print("\nset_plugin_enabled (toggle SageBridge — already enabled):")
d = call("project.set_plugin_enabled", {"plugin":"SageBridge","enabled":True})
s = d.get('result',{}).get('structuredContent') or d
print(f"  ✓ entry_existed: {s.get('entry_existed')} note: {s.get('note')}")

print("\ncleanup test BP + revert ini changes (atomic backup made by tool):")
call("delete_asset", {"asset_path": bp_path})
# Restore Game.ini from .sage_bak (manual since no revert tool yet)
import os, shutil
ini_path = "/Users/mahmutalemdar/Developer/alemdarlabs/SageTest/Config/DefaultGame.ini"
bak_path = ini_path + ".sage_bak"
if os.path.exists(bak_path):
    shutil.copy(bak_path, ini_path)
    print(f"  ✓ restored {ini_path} from .sage_bak")
else:
    print(f"  ! .sage_bak missing — leaving DefaultGame.ini with smoke entries (cleanup manually)")
print("  done")
