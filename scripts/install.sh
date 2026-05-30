#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
PREFIX="${PREFIX:-/usr}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)
      PREFIX="${2:?missing value for --prefix}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:?missing value for --build-dir}"
      shift 2
      ;;
    --build-type)
      BUILD_TYPE="${2:?missing value for --build-type}"
      shift 2
      ;;
    -h|--help)
      cat <<EOF
Usage: $(basename "$0") [--prefix PREFIX] [--build-dir DIR] [--build-type TYPE]

Environment:
  PREFIX=/usr
  BUILD_DIR=./build
  BUILD_TYPE=RelWithDebInfo
EOF
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

echo "[openkey] Configuring: buildDir=$BUILD_DIR prefix=$PREFIX buildType=$BUILD_TYPE"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"

echo "[openkey] Building"
cmake --build "$BUILD_DIR" -j

echo "[openkey] Installing (sudo)"
sudo cmake --install "$BUILD_DIR"

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  echo "[openkey] Updating icon cache (optional)"
  sudo gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi

echo "[openkey] Restarting fcitx5"
fcitx5 -rd >/dev/null 2>&1 || true

cat <<EOF
[openkey] Done.

Next:
  - Open \`fcitx5-configtool\` -> Add input method -> "OpenKey"
  - If you don't see it yet, try log out/in and run: \`fcitx5 -rd\`
EOF

