#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
UUID="openkey-bridge@openkey.dev"
DEST_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/${UUID}"

shell_ver_raw="$(gnome-shell --version | awk '{print $3}')"
shell_ver_major="${shell_ver_raw%%.*}"

if [[ -z "${shell_ver_major}" ]]; then
  echo "[openkey-bridge] Failed to detect GNOME Shell version" >&2
  exit 1
fi

if (( shell_ver_major >= 45 )); then
  SRC_DIR="${ROOT_DIR}/extension/gnome45-50/${UUID}"
else
  SRC_DIR="${ROOT_DIR}/extension/gnome42-44/${UUID}"
fi

if [[ ! -d "${SRC_DIR}" ]]; then
  echo "[openkey-bridge] Missing source folder: ${SRC_DIR}" >&2
  exit 1
fi

mkdir -p "${DEST_DIR}"

if command -v rsync >/dev/null 2>&1; then
  rsync -a --delete "${SRC_DIR}/" "${DEST_DIR}/"
else
  rm -rf -- "${DEST_DIR:?}/"*
  cp -a "${SRC_DIR}/." "${DEST_DIR}/"
fi

echo "[openkey-bridge] Installed to: ${DEST_DIR}"
echo "[openkey-bridge] Enable with: gnome-extensions enable ${UUID}"

