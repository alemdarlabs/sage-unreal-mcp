#!/usr/bin/env bash
#
# Build SageBridge UE plugin standalone via UAT BuildPlugin.
# Output: build/plugin/

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN="$PROJECT_ROOT/plugin/SageBridge.uplugin"
OUTPUT="$PROJECT_ROOT/build/plugin"

UE_ROOT="${SAGE_UE_ROOT:-/Users/Shared/Epic Games/UE_5.7}"
RUN_UAT="$UE_ROOT/Engine/Build/BatchFiles/RunUAT.command"

if [[ ! -f "$RUN_UAT" ]]; then
    if [[ -f "$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" ]]; then
        RUN_UAT="$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh"
    else
        echo "ERROR: RunUAT not found under $UE_ROOT/Engine/Build/BatchFiles/" >&2
        echo "       Set SAGE_UE_ROOT to your UE 5.7 install path." >&2
        exit 1
    fi
fi

if [[ ! -f "$PLUGIN" ]]; then
    echo "ERROR: $PLUGIN not found" >&2
    exit 1
fi

# Detect platform for -TargetPlatforms default
case "$(uname -s)" in
    Darwin) PLATFORM="${SAGE_BUILD_PLATFORM:-Mac}" ;;
    Linux)  PLATFORM="${SAGE_BUILD_PLATFORM:-Linux}" ;;
    *)      PLATFORM="${SAGE_BUILD_PLATFORM:-Mac}" ;;
esac

mkdir -p "$OUTPUT"

echo "==> Building plugin"
echo "    Plugin:   $PLUGIN"
echo "    Output:   $OUTPUT"
echo "    Engine:   $UE_ROOT"
echo "    Platform: $PLATFORM"

"$RUN_UAT" BuildPlugin \
    -Plugin="$PLUGIN" \
    -Package="$OUTPUT" \
    -Rocket \
    -TargetPlatforms="$PLATFORM"

echo "==> Done. Packaged plugin under $OUTPUT"
