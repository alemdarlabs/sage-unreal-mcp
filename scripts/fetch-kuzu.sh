#!/usr/bin/env bash
#
# Download a KuzuDB prebuilt release into third_party/kuzu/.
# vcpkg has no port for kuzu (community lib). FetchContent could build from
# source but takes 10+ minutes; the upstream prebuilt archive ships in
# seconds. The CMake build picks it up via third_party/kuzu/{include,lib}.
#
# Override version with KUZU_VERSION env var.

set -euo pipefail

VERSION="${KUZU_VERSION:-v0.11.3}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_DIR="${1:-$PROJECT_ROOT/third_party/kuzu}"

OS_NAME="$(uname -s)"
ARCH_NAME="$(uname -m)"

case "$OS_NAME" in
    Darwin)
        ASSET="libkuzu-osx-universal.tar.gz"
        ;;
    Linux)
        case "$ARCH_NAME" in
            aarch64|arm64) ASSET="libkuzu-linux-aarch64.tar.gz" ;;
            x86_64)        ASSET="libkuzu-linux-x86_64.tar.gz" ;;
            *) echo "unsupported Linux arch: $ARCH_NAME" >&2; exit 1 ;;
        esac
        ;;
    *)
        echo "unsupported OS: $OS_NAME (use scripts/fetch-kuzu.ps1 on Windows)" >&2
        exit 1
        ;;
esac

URL="https://github.com/kuzudb/kuzu/releases/download/$VERSION/$ASSET"

if [ -d "$TARGET_DIR" ] && find "$TARGET_DIR" -name "kuzu.hpp" -o -name "kuzu.h" 2>/dev/null | grep -q .; then
    echo "kuzu already present: $TARGET_DIR"
    find "$TARGET_DIR" -maxdepth 2 -type d | sed 's|^|  |'
    exit 0
fi

echo "==> downloading $URL"
mkdir -p "$TARGET_DIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL --progress-bar "$URL" -o "$TMP/kuzu.tar.gz"
tar -xzf "$TMP/kuzu.tar.gz" -C "$TARGET_DIR"

echo "==> extracted into $TARGET_DIR"
find "$TARGET_DIR" -maxdepth 3 \( -name "*.h" -o -name "*.hpp" -o -name "*.dylib" -o -name "*.so" -o -name "*.a" \) | sed 's|^|  |' | head -20
