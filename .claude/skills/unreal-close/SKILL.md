---
name: unreal-close
description: Close the running Unreal Editor process. Use when a plugin .dylib needs to be replaced and reloaded — macOS keeps the binary memory-mapped while UE is running, so the editor must be terminated before the swap takes effect. Safe for test/throwaway projects (no interactive save dialog — sends SIGTERM, escalates to SIGKILL after 5s if still alive). Do NOT use on a project with unsaved real work — pair with `save_assets` / `save_level` first.
---

# Close Unreal Editor

Closes any running `UnrealEditor` process. Designed for the Sage plugin
rebuild → reload loop on the SageTest project where data loss is acceptable.

## Recipe

```bash
# Send SIGTERM (graceful)
pkill -TERM -f UnrealEditor 2>&1 || { echo "no UnrealEditor running"; exit 0; }

# Wait up to 8s for graceful shutdown
for i in 1 2 3 4 5 6 7 8; do
    sleep 1
    if ! pgrep -f UnrealEditor >/dev/null; then
        echo "UnrealEditor closed in ${i}s"
        exit 0
    fi
done

# Still alive → force
echo "still running after 8s, sending SIGKILL"
pkill -KILL -f UnrealEditor
sleep 1
pgrep -f UnrealEditor >/dev/null && echo "WARN: still pgrep visible" || echo "killed"
```

## Notes

- `pkill -f UnrealEditor` matches the process *and* the `UnrealEditor` substring in the command line; the `.app` bundle launches the same binary, so this catches both `open`-launched and CLI-launched instances.
- The grace window (8s) is enough for UE to flush in-memory state without dropping anything that's already saved to disk; unsaved edits are abandoned.
- Use as part of a rebuild loop:
  1. `unreal-close`
  2. UAT BuildPlugin → new `.dylib`
  3. `cp` the new dylib over the project's `Plugins/<X>/Binaries/Mac/`
  4. `unreal-open`
