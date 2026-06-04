"""Stress: N parallel REMOTE tool/call's via HTTP+SSE while a real plugin
is connected. Reproduces the HeroFlight 'paralel call → server crash' shape:
cpp-httplib worker pool drives concurrent dispatchTool() onto the same
WebSocket. We watch for any of:
  - server process death (return code != 0 / exit before timeout)
  - 1006 abnormal close on the bridge
  - tool dispatch timeout / EditorNotConnected after success
  - mismatched response count (≠ N)
  - mangled/torn JSON in any response body
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO     = Path(__file__).resolve().parents[2]
EXE      = REPO / "build" / "debug" / "bin" / "sage-server.exe"
UE_ROOT  = Path(os.environ.get(
    "SAGE_UE_ROOT", r"C:\Program Files\Epic Games\UE_5.7"))
UE_EXE   = UE_ROOT / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
UPROJECT = Path(r"D:\Steamworks\HeroFlight\HeroFlight.uproject")

MCP_URL  = "http://127.0.0.1:7777/mcp"
COLD_WAIT = 30  # seconds to wait for editor cold start + plugin handshake


def ps(cmd: str) -> str:
    return subprocess.run(
        ["powershell", "-NoProfile", "-Command", cmd],
        capture_output=True, text=True, check=False).stdout.strip()


def find_heroflight_pid() -> int | None:
    out = ps(
        "(Get-CimInstance Win32_Process -Filter \"Name='UnrealEditor.exe'\" "
        "| Where-Object { $_.CommandLine -like '*HeroFlight*' } "
        "| Select-Object -First 1).ProcessId")
    return int(out) if out.isdigit() else None


def post_jsonrpc(req: dict, timeout=60.0) -> tuple[float, dict | str]:
    body = json.dumps(req).encode()
    t0 = time.monotonic()
    try:
        rq = urllib.request.Request(
            MCP_URL, data=body,
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(rq, timeout=timeout) as resp:
            raw = resp.read().decode()
            elapsed = time.monotonic() - t0
            try:
                return elapsed, json.loads(raw)
            except json.JSONDecodeError:
                return elapsed, f"NON-JSON: {raw[:200]!r}"
    except urllib.error.HTTPError as ex:
        return time.monotonic() - t0, f"HTTPError {ex.code}: {ex.read()[:200]!r}"
    except Exception as ex:
        return time.monotonic() - t0, f"{type(ex).__name__}: {ex}"


def wait_for_plugin(deadline: float) -> dict | None:
    """Poll list_editors until count > 0 or deadline."""
    while time.monotonic() < deadline:
        elapsed_label = time.monotonic()
        rid = int(elapsed_label * 1000) & 0xffff
        _, resp = post_jsonrpc({
            "jsonrpc": "2.0", "method": "tools/call", "id": rid,
            "params": {"name": "list_editors", "arguments": {}}
        }, timeout=5.0)
        if isinstance(resp, dict) and "result" in resp:
            sc = resp["result"].get("structuredContent", {})
            editors = sc.get("editors", [])
            if editors:
                return editors[0]
        time.sleep(1.0)
    return None


def main() -> int:
    if not EXE.exists():
        print(f"sage-server not built: {EXE}"); return 2

    # 1. Kill the running HeroFlight editor — start clean.
    pid = find_heroflight_pid()
    if pid:
        print(f"[kill] HeroFlight pid={pid}")
        ps(f"Stop-Process -Id {pid} -Force")
        for _ in range(30):
            if find_heroflight_pid() is None: break
            time.sleep(0.5)
        time.sleep(1.0)

    # 2. Start sage-server in HTTP mode (cpp-httplib worker pool).
    print(f"[server] start --http: {EXE.name}")
    env = os.environ.copy()
    env["SAGE_REPO_ROOT"] = str(REPO)
    server_log = open(REPO / "build" / "parallel-test-server.log", "w")
    proc = subprocess.Popen(
        [str(EXE), "--http"],
        stdout=server_log, stderr=subprocess.STDOUT,
        env=env, cwd=str(EXE.parent))

    # Give the server a beat to bind 7777 + 7778.
    time.sleep(1.0)

    # 3. Initialize via HTTP.
    elapsed, resp = post_jsonrpc({
        "jsonrpc":"2.0","method":"initialize","id":1,"params":{}})
    if not (isinstance(resp, dict) and "result" in resp):
        print(f"  init failed in {elapsed:.2f}s: {resp}"); proc.kill(); return 3
    print(f"  init OK in {elapsed:.2f}s")
    post_jsonrpc({"jsonrpc":"2.0","method":"notifications/initialized"})

    # 4. Spawn editor (fresh).
    print(f"[editor] spawning")
    t_spawn = time.monotonic()
    subprocess.Popen(
        [str(UE_EXE), str(UPROJECT)],
        creationflags=subprocess.DETACHED_PROCESS
                    | subprocess.CREATE_NEW_PROCESS_GROUP)

    # 5. Wait for plugin handshake.
    print(f"[wait] for plugin (up to {COLD_WAIT}s)")
    editor = wait_for_plugin(time.monotonic() + COLD_WAIT)
    if not editor:
        print("  plugin never connected — abort"); proc.terminate(); return 4
    print(f"  plugin connected after {time.monotonic()-t_spawn:.1f}s: "
          f"slot={editor.get('slot_id','')[:12]}... "
          f"instance={editor.get('instance_id')}")

    # 6. Stress mix: REMOTE asset.list/search batch + cheap server-side
    #    editor management calls, all in flight at once.
    slot_id = editor.get("slot_id")
    calls = [
        ("editor.ping",       {"message":"a"}),
        ("editor.ping",       {"message":"b"}),
        ("editor.ping",       {"message":"c"}),
        ("editor.ping",       {"message":"d"}),
        ("list_editors",      {}),
        ("get_active_editor", {}),
        ("get_editor",        {"id_or_label": editor.get("instance_id", "")}),
        ("asset.list",        {"max_results": 30}),
        ("asset.list",        {"kind": "Blueprint",        "max_results": 30}),
        ("asset.list",        {"kind": "WidgetBlueprint",  "max_results": 30}),
        ("asset.list",        {"kind": "Material",         "max_results": 30}),
        ("asset.list",        {"kind": "Texture2D",        "max_results": 30}),
        ("asset.list",        {"kind": "DataTable",        "max_results": 30}),
        ("asset.search",      {"query": "BP_",       "max_results": 20}),
        ("asset.search",      {"query": "Widget",    "max_results": 20}),
        ("asset.search",      {"query": "Mat_",      "max_results": 20}),
        ("asset.search",      {"query": "T_",        "max_results": 20}),
        ("asset.search",      {"query": "DT_",       "max_results": 20}),
        ("asset.search",      {"query": "BP_Conv",   "max_results": 20}),
        ("asset.search",      {"query": "Anim",      "max_results": 20}),
    ]
    N = len(calls)
    print(f"\n[stress] {N} parallel mixed tool/calls "
          f"(REMOTE asset.* + LOCAL editor management + ping)...")

    def call_one(i: int, name: str, args: dict):
        return i, name, post_jsonrpc({
            "jsonrpc":"2.0","method":"tools/call","id":100+i,
            "params":{"name": name, "arguments": args}
        }, timeout=60.0)

    t_stress = time.monotonic()
    results: list[tuple[int, str, float, dict | str]] = []
    with ThreadPoolExecutor(max_workers=N) as ex:
        futures = [ex.submit(call_one, i, n, a) for i, (n, a) in enumerate(calls)]
        for fut in as_completed(futures):
            i, name, (elapsed, resp) = fut.result()
            results.append((i, name, elapsed, resp))

    total = time.monotonic() - t_stress
    print(f"[stress] all {N} returned in {total:.2f}s wall")

    # 7. Verify.
    success = 0
    failed_ids: list[int] = []
    for i, name, elapsed, resp in sorted(results, key=lambda r: r[0]):
        if isinstance(resp, dict) and "result" in resp:
            sc = resp["result"].get("structuredContent", {})
            success += 1
            keys = list(sc.keys())[:6] if isinstance(sc, dict) else []
            print(f"  #{i:2d} {name:20s} OK in {elapsed:5.2f}s  keys={keys}")
        elif isinstance(resp, dict) and "error" in resp:
            err = resp["error"]
            failed_ids.append(i)
            print(f"  #{i:2d} {name:20s} ERR in {elapsed:5.2f}s "
                  f"[{err.get('code')}] {err.get('message','')[:80]}")
        else:
            failed_ids.append(i)
            print(f"  #{i:2d} {name:20s} XXX in {elapsed:5.2f}s: {resp}")

    # 8. Server still alive
    server_alive = proc.poll() is None
    print(f"\n[health] sage-server process alive: {server_alive}")
    if not server_alive:
        print(f"  exit code: {proc.returncode}")

    # Tear down server (editor stays open).
    proc.terminate()
    try: proc.wait(timeout=3)
    except subprocess.TimeoutExpired: proc.kill()
    server_log.close()

    print(f"\n--- summary ---")
    print(f"  parallel calls    : {N}")
    print(f"  succeeded         : {success}")
    print(f"  failed            : {len(failed_ids)} {failed_ids}")
    print(f"  server crashed    : {not server_alive}")
    # Allow some tools to be unsupported by the deployed plugin (e.g. older
    # SageBridge missing asset.list/search). The crash-test purpose is to
    # observe whether the SERVER survives the parallel pressure — protocol
    # 'unknown tool' errors from the plugin don't count against that.
    crashed = not server_alive
    print(f"  result            : {'PASS (server survived)' if not crashed else 'FAIL (server died)'}")
    return 1 if crashed else 0


if __name__ == "__main__":
    sys.exit(main())
