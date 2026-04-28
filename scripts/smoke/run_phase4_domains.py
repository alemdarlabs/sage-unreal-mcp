#!/usr/bin/env python3
"""Run all Phase 4 domain smoke scripts in sequence.

Each test exits non-zero on failure; this runner aggregates pass/fail
and surfaces stderr.

Skipped (already covered by older smoke files):
  asset_writes, datatable, export_asset, import_fbx, import_texture,
  mesh_materials, sequencer, sockets, textures, type_matrix,
  widget, widget_authoring
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

PHASE4_DOMAINS = [
    "audio.py",
    "foliage.py",
    "gas.py",
    "networking.py",
    "level.py",
    "landscape.py",
    "pcg.py",
    "niagara.py",
    "animation.py",
    "gameplay.py",
]


def main() -> int:
    here = Path(__file__).resolve().parent
    pass_count = 0
    fail_count = 0
    fails: list[str] = []

    for name in PHASE4_DOMAINS:
        script = here / name
        if not script.exists():
            print(f"  [skip] {name} — not found")
            continue
        print(f"\n>>>>>>>>>> {name} <<<<<<<<<<")
        proc = subprocess.run([sys.executable, str(script)],
                              capture_output=True, text=True)
        # Pass-through last few lines so the runner stays scannable.
        tail = proc.stdout.strip().rsplit("\n", 6)[-1] if proc.stdout else ""
        if proc.returncode == 0:
            pass_count += 1
            print(f"  PASS — {tail}")
        else:
            fail_count += 1
            fails.append(name)
            print(f"  FAIL (exit {proc.returncode})")
            print("  --- stderr ---")
            print(proc.stderr[-2000:] if proc.stderr else "(empty)")
            print("  --- last stdout ---")
            print((proc.stdout or "")[-1500:])

    print(f"\n=== Phase 4 domain smoke summary ===")
    print(f"  pass : {pass_count}/{len(PHASE4_DOMAINS)}")
    print(f"  fail : {fail_count}")
    if fails:
        print(f"  failures: {', '.join(fails)}")

    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
