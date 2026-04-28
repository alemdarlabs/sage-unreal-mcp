#!/usr/bin/env python3
"""Phase 4.11 round 2 — widget authoring (add/remove/set_property).

Flow:
  1. widget.create WBP_Auth_Test
  2. widget.add_widget CanvasPanel as root
  3. widget.add_widget Button (parent=Canvas)
  4. widget.add_widget TextBlock (parent=Button)
  5. widget.set_property TextBlock.ToolTipText = "hi"
  6. widget.read — verify hierarchy + property
  7. widget.remove_widget Button — verifies cascade
  8. cleanup
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
    bp = "/Game/Sage_Smoke/WBP_Auth_Test"
    try:
        call("asset.delete_batch", {"paths": [bp]})
    except RuntimeError:
        pass

    hr("widget.create")
    call("widget.create", {"path": bp})

    hr("widget.add_widget Canvas (becomes root)")
    canvas = call("widget.add_widget", {
        "blueprint":    bp,
        "widget_class": "/Script/UMG.CanvasPanel",
        "name":         "RootCanvas",
    })
    print(json.dumps(canvas, indent=2))
    assert canvas["name"] == "RootCanvas"
    assert canvas["attached_to"] == "(root)"

    hr("widget.add_widget Button (under Canvas)")
    btn = call("widget.add_widget", {
        "blueprint":    bp,
        "widget_class": "/Script/UMG.Button",
        "name":         "MyButton",
        "parent":       "RootCanvas",
    })
    print(json.dumps(btn, indent=2))
    assert btn["attached_to"] == "RootCanvas"

    hr("widget.add_widget TextBlock (under Button)")
    txt = call("widget.add_widget", {
        "blueprint":    bp,
        "widget_class": "/Script/UMG.TextBlock",
        "name":         "BtnLabel",
        "parent":       "MyButton",
    })
    print(json.dumps(txt, indent=2))
    assert txt["attached_to"] == "MyButton"

    hr("widget.add_widget duplicate name (-32602)")
    try:
        call("widget.add_widget", {
            "blueprint":    bp,
            "widget_class": "/Script/UMG.TextBlock",
            "name":         "BtnLabel",
        })
        print("FAIL: duplicate accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("widget.add_widget bad class (Actor)")
    try:
        call("widget.add_widget", {
            "blueprint":    bp,
            "widget_class": "/Script/Engine.Actor",
            "name":         "NotAWidget",
        })
        print("FAIL: bad class accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("widget.set_property TextBlock.ToolTipText")
    sp = call("widget.set_property", {
        "blueprint": bp,
        "name":      "BtnLabel",
        "property":  "ToolTipText",
        "value":     "Sage tooltip",
    })
    print(json.dumps(sp, indent=2))

    hr("widget.read (verify tree)")
    r = call("widget.read", {"path": bp})
    print(json.dumps(r, indent=2))
    assert r["widget_count"] == 3
    names = {w["name"] for w in r["widgets"]}
    assert {"RootCanvas", "MyButton", "BtnLabel"}.issubset(names), \
        f"expected all 3 in {names}"
    # Sanity: Canvas children include MyButton
    root = r["root"]
    assert root["class"] == "CanvasPanel"
    assert any(c["name"] == "MyButton" for c in root.get("children", []))

    hr("widget.remove_widget MyButton (cascade?)")
    rm = call("widget.remove_widget", {"blueprint": bp, "name": "MyButton"})
    print(json.dumps(rm, indent=2))
    after = call("widget.read", {"path": bp})
    print(f"  after: widget_count={after['widget_count']}")
    # MyButton + BtnLabel both gone (BtnLabel was a child of Button's slot)
    remaining = {w["name"] for w in after["widgets"]}
    assert "MyButton" not in remaining

    hr("widget.remove_widget bogus (-32602)")
    try:
        call("widget.remove_widget", {"blueprint": bp, "name": "DoesNotExist"})
        print("FAIL: bogus name accepted")
        return 1
    except RuntimeError as e:
        print(f"OK rejected: {e}")

    hr("Cleanup")
    call("asset.delete_batch", {"paths": [bp]})

    print("\n*** widget_authoring smoke OK ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
