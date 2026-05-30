#!/usr/bin/env bash
set -euo pipefail

RESET_USER_DATA=0
PURGE=0
DEBUG="${DEBUG:-0}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--purge] [--reset-user-data]

Reinstall Fcitx5 stack on Debian/Ubuntu using apt.

Options:
  --purge            Purge all fcitx5 packages first (clean reinstall).
  --reset-user-data  Remove user fcitx5 config/data (~/.config/fcitx5, ...).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --purge)
      PURGE=1
      shift 1
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

if ! command -v apt-get >/dev/null 2>&1; then
  echo "[fcitx5] This script currently supports Debian/Ubuntu (apt-get only)." >&2
  exit 1
fi

if [[ "$DEBUG" == "1" ]]; then
  set -x
fi

echo "[fcitx5] Checking sudo (you may be prompted for password)"
sudo -v

echo "[fcitx5] Stopping fcitx5"
if command -v fcitx5-remote >/dev/null 2>&1; then
  # Graceful exit when possible.
  fcitx5-remote -e >/dev/null 2>&1 || true
fi
# IMPORTANT: do NOT use `pkill -f fcitx5` here because this script name
# contains "fcitx5" and would kill itself.
pkill -x fcitx5 >/dev/null 2>&1 || true

if [[ "$RESET_USER_DATA" == "1" ]]; then
  echo "[fcitx5] Removing user config/data (DANGEROUS)"
  rm -rf ~/.config/fcitx5 ~/.local/share/fcitx5 ~/.cache/fcitx5 || true
fi

if [[ "$PURGE" == "1" ]]; then
  echo "[fcitx5] Purging fcitx5 packages"
  sudo apt-get purge -y 'fcitx5*' || true
  sudo apt-get autoremove -y --purge || true
fi

echo "[fcitx5] Installing fcitx5 packages"
sudo apt-get update
sudo apt-get install -y \
  fcitx5 \
  fcitx5-frontend-gtk3 \
  fcitx5-frontend-qt5 \
  fcitx5-frontend-gtk2 \
  fcitx5-module-xorg \
  im-config

if command -v im-config >/dev/null 2>&1; then
  echo "[fcitx5] Setting input method to fcitx5 (im-config)"
  im-config -n fcitx5 >/dev/null 2>&1 || true
fi

echo "[fcitx5] Starting fcitx5"
fcitx5 -rd >/dev/null 2>&1 || true

cat <<EOF
[fcitx5] Done.

Important:
  - Log out and log back in (or reboot) for IM env vars to apply system-wide.
  - Then open: fcitx5-configtool
EOF
