#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
PREFIX="${PREFIX:-/usr}"
LIBDIR=""
RESET_USER_DATA=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [--build-dir DIR] [--prefix PREFIX] [--reset-user-data]

Uninstalls the fcitx5-openkey addon files installed by CMake.
Prefer using build/install_manifest.txt when available.

Environment:
  BUILD_DIR=./build
  PREFIX=/usr
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="${2:?missing value for --build-dir}"
      shift 2
      ;;
    --prefix)
      PREFIX="${2:?missing value for --prefix}"
      shift 2
      ;;
    --reset-user-data)
      RESET_USER_DATA=1
      shift 1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

manifest="$BUILD_DIR/install_manifest.txt"
if [[ -f "$manifest" ]]; then
  echo "[openkey] Uninstalling using install manifest: $manifest"
  # shellcheck disable=SC2002
  cat "$manifest" | while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    sudo rm -f -- "$f" || true
  done
else
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists Fcitx5Core; then
    PREFIX="$(pkg-config --variable=prefix Fcitx5Core)"
    LIBDIR="$(pkg-config --variable=libdir Fcitx5Core)"
  else
    LIBDIR="$PREFIX/lib"
  fi

  echo "[openkey] No install manifest found; removing known paths (best-effort)"
  sudo rm -f -- "$LIBDIR/fcitx5/openkey.so" || true
  sudo rm -f -- "$PREFIX/share/fcitx5/addon/openkey.conf" || true
  sudo rm -f -- "$PREFIX/share/fcitx5/inputmethod/openkey.conf" || true

  sudo rm -f -- "$PREFIX/share/icons/hicolor/scalable/apps/openkey.svg" || true
  sudo rm -f -- "$PREFIX/share/icons/hicolor/scalable/apps/fcitx-openkey.svg" || true
  sudo rm -f -- "$PREFIX/share/icons/hicolor/scalable/apps/org.fcitx.Fcitx5.fcitx-openkey.svg" || true

  sudo rm -f -- "$PREFIX/share/icons/hicolor/symbolic/apps/openkey-symbolic.svg" || true
  sudo rm -f -- "$PREFIX/share/icons/hicolor/symbolic/apps/fcitx-openkey-symbolic.svg" || true
  sudo rm -f -- "$PREFIX/share/icons/hicolor/symbolic/apps/org.fcitx.Fcitx5.fcitx-openkey-symbolic.svg" || true
fi

if [[ "$RESET_USER_DATA" == "1" ]]; then
  echo "[openkey] Removing user fcitx5 config/data (DANGEROUS)"
  rm -rf ~/.config/fcitx5 ~/.local/share/fcitx5 ~/.cache/fcitx5 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  echo "[openkey] Updating icon cache (optional)"
  sudo gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi

echo "[openkey] Restarting fcitx5"
fcitx5 -rd >/dev/null 2>&1 || true

echo "[openkey] Done."

