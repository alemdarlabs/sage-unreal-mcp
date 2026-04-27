---
name: unreal-open
description: Launch Unreal Editor with the SageTest project (or any .uproject path). Use right after `unreal-close` + plugin .dylib swap so the freshly built SageBridge plugin reloads. Async — returns once the OS hands the project to UE; editor takes ~10-20s to boot, after which the SageBridge plugin auto-connects to the running sage-server (look for "Bridge: connection opened" in /tmp/sage-server.log).
---

# Open Unreal Editor

Hands the SageTest .uproject to macOS, which launches the associated UE
binary (`open` uses LaunchServices → resolves `.uproject` → UE 5.7 from
`/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app`).

## Recipe

```bash
PROJECT="${1:-/Users/mahmutalemdar/Developer/alemdarlabs/SageTest/SageTest.uproject}"

if [ ! -f "$PROJECT" ]; then
    echo "ERROR: project not found: $PROJECT" >&2
    exit 1
fi

if pgrep -f UnrealEditor >/dev/null; then
    echo "WARN: UnrealEditor already running — call unreal-close first if you want a fresh load"
fi

open "$PROJECT"
echo "launched: $PROJECT — editor boot ~10-20s; SageBridge auto-connects on PostEngineInit"
```

## Verifying readiness

After ~15s, the plugin should have completed its handshake with the
running sage-server. Check via:

```bash
tail -5 /tmp/sage-server.log
# Expect:
#   Bridge: connection opened from 127.0.0.1 (id=N)
#   Bridge handshake: slot=<blake3>, label='', project='/.../SageTest.uproject', engine=5.7.4
```

Or poll `list_editors` against the MCP HTTP endpoint:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"list_editors","arguments":{}}}' \
  http://127.0.0.1:7777/mcp | python3 -c "import sys,json; print(json.load(sys.stdin)['result']['structuredContent'])"
# count: 1 → ready
```

## Companion skills

- `unreal-close` — pair to terminate before relaunching with a new plugin binary.
