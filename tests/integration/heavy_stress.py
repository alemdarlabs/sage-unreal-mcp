"""Heavy parallel stress: N parallel REMOTE editor.ping × R rounds against
a sage-server already up with a connected plugin. Reuses the running
editor from a prior test (no kill/spawn). Looks for any of:
  - server crash mid-stress
  - plugin disconnect (counts editors before/after)
  - response count != N×R
  - non-200 / mangled JSON
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXE  = REPO / "build" / "debug" / "bin" / "sage-server.exe"
URL  = "http://127.0.0.1:7777/mcp"

UE_ROOT  = Path(os.environ.get(
    "SAGE_UE_ROOT", r"C:\Program Files\Epic Games\UE_5.7"))
UE_EXE   = UE_ROOT / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
UPROJECT = Path(r"D:\Steamworks\HeroFlight\HeroFlight.uproject")

PARALLEL = int(os.environ.get("STRESS_PARALLEL", "50"))
ROUNDS   = int(os.environ.get("STRESS_ROUNDS",   "3"))
SKIP_EDITOR_CYCLE = os.environ.get("STRESS_SKIP_CYCLE", "0") == "1"


def ps(cmd: str) -> str:
    return subprocess.run(
        ["powershell", "-NoProfile", "-Command", cmd],
        capture_output=True, text=True, check=False).stdout.strip()


def find_pid() -> int | None:
    out = ps(
        "(Get-CimInstance Win32_Process -Filter \"Name='UnrealEditor.exe'\" "
        "| Where-Object { $_.CommandLine -like '*HeroFlight*' } "
        "| Select-Object -First 1).ProcessId")
    return int(out) if out.isdigit() else None


def post(req: dict, timeout=30.0) -> tuple[float, dict | str]:
    body = json.dumps(req).encode()
    t0 = time.monotonic()
    try:
        rq = urllib.request.Request(URL, data=body,
                                     headers={"Content-Type":"application/json"})
        with urllib.request.urlopen(rq, timeout=timeout) as resp:
            raw = resp.read().decode()
            try: return time.monotonic()-t0, json.loads(raw)
            except json.JSONDecodeError: return time.monotonic()-t0, f"BAD-JSON {raw[:160]!r}"
    except urllib.error.HTTPError as ex:
        return time.monotonic()-t0, f"HTTP {ex.code}: {ex.read()[:160]!r}"
    except Exception as ex:
        return time.monotonic()-t0, f"{type(ex).__name__}: {ex}"


def main() -> int:
    # Optionally cycle the editor to guarantee a clean reconnect (default ON
    # — when SKIP_EDITOR_CYCLE=1 we just restart the server and reuse the
    # already-running editor).
    if not SKIP_EDITOR_CYCLE:
        pid = find_pid()
        if pid:
            print(f"[kill] HeroFlight pid={pid}")
            ps(f"Stop-Process -Id {pid} -Force")
            for _ in range(30):
                if find_pid() is None: break
                time.sleep(0.5)
            time.sleep(1.0)

    env = os.environ.copy()
    env["SAGE_REPO_ROOT"] = str(REPO)
    log = open(REPO / "build" / "heavy-stress-server.log", "w")
    print(f"[server] start --http")
    proc = subprocess.Popen(
        [str(EXE), "--http"],
        stdout=log, stderr=subprocess.STDOUT, env=env, cwd=str(EXE.parent))
    time.sleep(1.0)

    # init
    _, init = post({"jsonrpc":"2.0","method":"initialize","id":1,"params":{}})
    if not (isinstance(init, dict) and "result" in init):
        print(f"  init failed: {init}"); proc.kill(); return 2
    post({"jsonrpc":"2.0","method":"notifications/initialized"})

    if not SKIP_EDITOR_CYCLE:
        # spawn editor fresh
        print(f"[editor] spawn")
        subprocess.Popen(
            [str(UE_EXE), str(UPROJECT)],
            creationflags=subprocess.DETACHED_PROCESS
                        | subprocess.CREATE_NEW_PROCESS_GROUP)
        wait_seconds = 45
    else:
        print(f"[editor] reusing already-running editor (plugin should reconnect)")
        wait_seconds = 60  # plugin backoff might be at the 30s cap

    # wait for plugin
    print(f"[wait] plugin handshake (up to {wait_seconds}s)...")
    plugin = None
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        _, r = post({"jsonrpc":"2.0","method":"tools/call","id":99,
                     "params":{"name":"list_editors","arguments":{}}})
        if isinstance(r, dict) and "result" in r:
            eds = r["result"].get("structuredContent", {}).get("editors", [])
            if eds: plugin = eds[0]; break
        time.sleep(1.0)
    if not plugin:
        print("  no plugin connected — abort"); proc.terminate(); return 3
    print(f"  plugin: {plugin.get('instance_id')} slot={plugin.get('slot_id','')[:12]}...")

    # stress
    total_ok = 0
    total_fail = 0
    fail_examples: list[str] = []
    for r in range(ROUNDS):
        print(f"\n[round {r+1}/{ROUNDS}] {PARALLEL} parallel editor.ping...")
        t0 = time.monotonic()
        with ThreadPoolExecutor(max_workers=PARALLEL) as ex:
            futures = []
            for i in range(PARALLEL):
                req = {"jsonrpc":"2.0","method":"tools/call",
                       "id": 1000 + r*PARALLEL + i,
                       "params":{"name":"editor.ping",
                                 "arguments":{"message":f"r{r}-{i}"}}}
                futures.append(ex.submit(post, req, 30.0))
            for fut in as_completed(futures):
                elapsed, resp = fut.result()
                if isinstance(resp, dict) and "result" in resp:
                    sc = resp["result"].get("structuredContent", {})
                    if sc.get("echoed_by") == "plugin":
                        total_ok += 1
                    else:
                        total_fail += 1
                        if len(fail_examples) < 3: fail_examples.append(f"BAD-SC {sc}")
                else:
                    total_fail += 1
                    if len(fail_examples) < 3: fail_examples.append(str(resp)[:120])
        print(f"  round took {time.monotonic()-t0:.2f}s "
              f"(ok={total_ok} fail={total_fail})")

    # health
    server_alive = proc.poll() is None
    _, eds_after = post({"jsonrpc":"2.0","method":"tools/call","id":9999,
                         "params":{"name":"list_editors","arguments":{}}})
    plugin_after = 0
    if isinstance(eds_after, dict) and "result" in eds_after:
        plugin_after = len(eds_after["result"].get("structuredContent", {}).get("editors", []))

    proc.terminate()
    try: proc.wait(timeout=3)
    except subprocess.TimeoutExpired: proc.kill()
    log.close()

    print(f"\n--- summary ---")
    print(f"  total calls       : {PARALLEL*ROUNDS}")
    print(f"  succeeded         : {total_ok}")
    print(f"  failed            : {total_fail}")
    if fail_examples:
        print(f"  fail examples     : {fail_examples}")
    print(f"  plugin still bound: {plugin_after}")
    print(f"  server alive end  : {server_alive}")
    crashed = not server_alive
    print(f"  result            : {'PASS' if not crashed and total_ok == PARALLEL*ROUNDS else 'FAIL'}")
    return 0 if (not crashed and total_ok == PARALLEL*ROUNDS) else 1


if __name__ == "__main__":
    sys.exit(main())
