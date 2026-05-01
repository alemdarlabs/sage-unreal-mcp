"""End-to-end: kill running HeroFlight editor, start sage-server (stdio),
spawn fresh editor, observe plugin handshake timing via notifications +
wait_for_editor.

Goal: see whether a plugin starting fresh (no prior failed attempts, no
backoff carry-over) connects in 1-3s as it should.
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO     = Path(__file__).resolve().parents[2]
EXE      = REPO / "build" / "debug" / "bin" / "sage-server.exe"
UE_ROOT  = Path(os.environ.get(
    "SAGE_UE_ROOT", r"C:\Program Files\Epic Games\UE_5.7"))
UE_EXE   = UE_ROOT / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
UPROJECT = Path(r"D:\Steamworks\HeroFlight\HeroFlight.uproject")
WAIT_TIMEOUT_MS = 180000  # editor cold start + plugin connect


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


def main() -> int:
    if not EXE.exists():
        print(f"sage-server not built: {EXE}", file=sys.stderr); return 2
    if not UE_EXE.exists():
        print(f"UE editor not found: {UE_EXE}", file=sys.stderr); return 2
    if not UPROJECT.exists():
        print(f"uproject not found: {UPROJECT}", file=sys.stderr); return 2

    # 1. Kill the running HeroFlight editor (if any).
    pid = find_heroflight_pid()
    if pid:
        print(f"[kill] HeroFlight UnrealEditor pid={pid}")
        ps(f"Stop-Process -Id {pid} -Force")
        # Wait for the OS to actually release the process so the .dll lock
        # doesn't keep the bridge socket lingering.
        for _ in range(30):
            if find_heroflight_pid() is None: break
            time.sleep(0.5)
        time.sleep(1.0)
        print(f"[kill] done")
    else:
        print("[kill] no existing HeroFlight editor")

    # 2. Start sage-server BEFORE spawning the editor — first plugin Connect()
    #    should land on a live bridge with zero backoff.
    print(f"[server] start: {EXE}")
    env = os.environ.copy()
    env["SAGE_REPO_ROOT"] = str(REPO)
    proc = subprocess.Popen(
        [str(EXE)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True, bufsize=1, env=env, cwd=str(EXE.parent))

    def send(o):
        proc.stdin.write(json.dumps(o) + "\n"); proc.stdin.flush()

    # initialize
    send({"jsonrpc":"2.0","method":"initialize","id":1,"params":{}})
    init = json.loads(proc.stdout.readline())
    print(f"[server] init: {init['result']['serverInfo']['name']} "
          f"v{init['result']['serverInfo']['version']}")
    send({"jsonrpc":"2.0","method":"notifications/initialized"})

    # 3. Spawn the editor (detached so it survives this script).
    print(f"[editor] spawning {UE_EXE.name} {UPROJECT.name}")
    t_spawn = time.monotonic()
    subprocess.Popen(
        [str(UE_EXE), str(UPROJECT)],
        creationflags=subprocess.DETACHED_PROCESS
                    | subprocess.CREATE_NEW_PROCESS_GROUP)

    # 4. Issue wait_for_editor — timer measures spawn -> handshake latency.
    print(f"[server] wait_for_editor (timeout {WAIT_TIMEOUT_MS}ms)")
    send({"jsonrpc":"2.0","method":"tools/call","id":2,
          "params":{"name":"wait_for_editor",
                    "arguments":{"timeout_ms": WAIT_TIMEOUT_MS}}})

    notifications = []
    result = None
    failed = False
    while True:
        line = proc.stdout.readline()
        if not line:
            print("[server] stdout EOF", file=sys.stderr); failed = True; break
        elapsed = time.monotonic() - t_spawn
        try:
            msg = json.loads(line)
        except json.JSONDecodeError as ex:
            print(f"  [{elapsed:7.2f}s] non-JSON: {line!r}", file=sys.stderr)
            failed = True; continue

        if "method" in msg and msg["method"].startswith("notifications/"):
            d = msg.get("params", {}).get("data", {})
            print(f"  [+{elapsed:7.2f}s post-spawn] notification "
                  f"event={d.get('event')!r} "
                  f"instance={d.get('instance_id')!r} "
                  f"slot={(d.get('slot_id') or '')[:12]}...")
            notifications.append({"at": elapsed, "msg": msg})
            continue

        if msg.get("id") == 2:
            if "result" in msg:
                sc = msg["result"]["structuredContent"]
                print(f"  [+{elapsed:7.2f}s post-spawn] wait_for_editor OK "
                      f"already={sc['already_connected']} "
                      f"instance={sc['instance_id']!r}")
                result = sc
            else:
                print(f"  [+{elapsed:7.2f}s post-spawn] wait_for_editor ERROR "
                      f"{msg.get('error')}")
                failed = True
            break

    # 5. Wind down.
    try: proc.stdin.close()
    except Exception: pass
    try: proc.wait(timeout=3)
    except subprocess.TimeoutExpired: proc.kill()

    print("\n--- summary ---")
    print(f"  notifications captured : {len(notifications)}")
    for n in notifications:
        d = n["msg"]["params"]["data"]
        print(f"    +{n['at']:6.2f}s {d.get('event'):14s} {d.get('instance_id')}")
    print(f"  result                 : {'PASS' if not failed and result else 'FAIL'}")
    return 0 if not failed and result else 1


if __name__ == "__main__":
    sys.exit(main())
