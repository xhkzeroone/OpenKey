#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
PREFIX="${PREFIX:-/usr}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
TARGET_USER="${SUDO_USER:-${USER:-}}"

is_user_prefix() {
  [[ -n "${HOME:-}" ]] && [[ "$PREFIX" == "$HOME" || "$PREFIX" == "$HOME/"* ]]
}

cmake_install() {
  if is_user_prefix; then
    echo "[openkey] Installing"
    cmake --install "$BUILD_DIR"
  else
    echo "[openkey] Installing (sudo)"
    sudo cmake --install "$BUILD_DIR"
  fi
}

update_icon_cache() {
  if ! command -v gtk-update-icon-cache >/dev/null 2>&1; then
    return
  fi

  local icon_theme_dir="$PREFIX/share/icons/hicolor"
  if [[ ! -d "$icon_theme_dir" ]]; then
    return
  fi

  echo "[openkey] Updating icon cache (optional)"
  if is_user_prefix; then
    gtk-update-icon-cache -f -t "$icon_theme_dir" >/dev/null 2>&1 || true
  else
    sudo gtk-update-icon-cache -f -t "$icon_theme_dir" >/dev/null 2>&1 || true
  fi
}

install_deps_debian() {
  echo "[openkey] Installing build dependencies (apt)"
  sudo apt-get update

  local common_packages=(
    build-essential
    cmake
    pkg-config
    extra-cmake-modules
    gettext
    golang-go
    fcitx5
    fcitx5-config-qt
    fcitx5-frontend-gtk3
    fcitx5-frontend-qt5
  )
  local dev_packages=(
    libfcitx5core-dev
    libfcitx5config-dev
    libfcitx5utils-dev
  )

  if ! sudo apt-get install -y "${common_packages[@]}" "${dev_packages[@]}"; then
    echo "[openkey] Fallback: trying fcitx5-dev metapackage"
    sudo apt-get install -y "${common_packages[@]}" fcitx5-dev
  fi
}

install_build_deps() {
  if [[ "$(uname -s)" != "Linux" ]]; then
    return
  fi
  if [[ ! -r /etc/os-release ]]; then
    echo "[openkey] Skipping dependency auto-install: /etc/os-release missing"
    return
  fi

  . /etc/os-release

  if command -v apt-get >/dev/null 2>&1; then
    case " ${ID:-} ${ID_LIKE:-} " in
      *" ubuntu "*|*" debian "*)
        install_deps_debian
        return
        ;;
    esac
  fi

  echo "[openkey] Skipping dependency auto-install: unsupported distro (${PRETTY_NAME:-unknown})"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --user)
      PREFIX="${HOME:?HOME is required for --user}/.local"
      shift
      ;;
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
Usage: $(basename "$0") [--user] [--prefix PREFIX] [--build-dir DIR] [--build-type TYPE]

Environment:
  PREFIX=/usr
  BUILD_DIR=./build
  BUILD_TYPE=RelWithDebInfo

Options:
  --user  Install to \$HOME/.local without sudo, like the README user-local flow.
EOF
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

case "$PREFIX" in
  "~")
    PREFIX="${HOME:?HOME is required for ~ prefix}"
    ;;
  "~/"*)
    PREFIX="${HOME:?HOME is required for ~/ prefix}/${PREFIX#"~/"}"
    ;;
esac

echo "[openkey] Configuring: buildDir=$BUILD_DIR prefix=$PREFIX buildType=$BUILD_TYPE"

if is_user_prefix; then
  echo "[openkey] Skipping dependency auto-install for user-local prefix"
else
  install_build_deps
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"

echo "[openkey] Building"
cmake --build "$BUILD_DIR" -j

cmake_install

if [[ "$(uname -s)" == "Linux" ]] && is_user_prefix; then
  echo "[openkey] Skipping uinput system setup for user-local prefix"
elif [[ "$(uname -s)" == "Linux" ]]; then
  nonpreedit_server="$PREFIX/libexec/openkey-nonpreedit-server"
  if [[ -x "$nonpreedit_server" ]] && command -v setcap >/dev/null 2>&1; then
    echo "[openkey] Granting priority capability to nonpreedit server (best-effort)"
    sudo setcap cap_sys_nice+ep "$nonpreedit_server" >/dev/null 2>&1 || true
  fi

  if [[ ! -e /dev/uinput ]]; then
    echo "[openkey] /dev/uinput missing; trying: sudo modprobe uinput (best-effort)"
    sudo modprobe uinput >/dev/null 2>&1 || true
  fi

  if [[ -e /dev/uinput ]]; then
    if ! getent group uinput >/dev/null 2>&1; then
      echo "[openkey] Creating system group: uinput (best-effort)"
      sudo groupadd --system uinput >/dev/null 2>&1 || true
    fi

    if [[ -n "$TARGET_USER" ]]; then
      echo "[openkey] Granting uinput access to user: $TARGET_USER"
      sudo usermod -aG uinput "$TARGET_USER" >/dev/null 2>&1 || true
    fi

    if [[ -d /etc/udev/rules.d ]] && command -v udevadm >/dev/null 2>&1; then
      echo "[openkey] Installing udev rule for /dev/uinput (best-effort)"
      sudo tee /etc/udev/rules.d/99-openkey-uinput.rules >/dev/null <<'EOF' || true
# OpenKey: allow members of group "uinput" to access /dev/uinput.
KERNEL=="uinput", SUBSYSTEM=="misc", OPTIONS+="static_node=uinput", MODE="0660", GROUP="uinput"
EOF
      sudo udevadm control --reload-rules >/dev/null 2>&1 || true
      sudo udevadm trigger --name-match=uinput >/dev/null 2>&1 || true
    fi

    sudo chgrp uinput /dev/uinput >/dev/null 2>&1 || true
    sudo chmod 0660 /dev/uinput >/dev/null 2>&1 || true
  else
    echo "[openkey] Warning: /dev/uinput not found; uinput backspace injection won't work"
  fi
fi

update_icon_cache

echo "[openkey] Restarting fcitx5"
fcitx5 -rd >/dev/null 2>&1 || true

cat <<EOF
[openkey] Done.

Next:
  - Open \`fcitx5-configtool\` -> Add input method -> "OpenKey"
  - If you don't see it yet, try log out/in and run: \`fcitx5 -rd\`
  - If \`/dev/uinput\` still says permission denied, log out/in once so group \`uinput\` takes effect
EOF
