#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
PREFIX="${PREFIX:-/usr}"
RESET_USER_DATA=0
RESET_OPENKEY_CONFIG=0
REMOVE_GNOME_EXTENSION=0
ALL_PREFIXES=0
DRY_RUN=0

is_user_prefix_value() {
  local prefix="$1"
  [[ -n "${HOME:-}" ]] && [[ "$prefix" == "$HOME" || "$prefix" == "$HOME/"* ]]
}

is_user_prefix() {
  is_user_prefix_value "$PREFIX"
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [--user] [--build-dir DIR] [--prefix PREFIX] [options]

Uninstalls the fcitx5-openkey addon files installed by CMake.
Uses build/install_manifest.txt when available, then removes known OpenKey
paths as a best-effort cleanup.

Options:
  --user                  Remove the \$HOME/.local install without sudo, matching
                          install.sh --user.
  --all-prefixes          Also remove known OpenKey files from /usr, /usr/local,
                          and \$HOME/.local. Useful if an old local install still
                          shadows the system install.
  --reset-openkey-config  Remove only OpenKey-related user config files.
  --remove-gnome-extension
                          Remove the optional GNOME Shell bridge extension.
  --reset-user-data       Remove all user fcitx5 config/data (DANGEROUS).
  --dry-run               Print what would be removed without deleting.

Environment:
  BUILD_DIR=./build
  PREFIX=/usr
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --user)
      PREFIX="${HOME:?HOME is required for --user}/.local"
      shift 1
      ;;
    --build-dir)
      BUILD_DIR="${2:?missing value for --build-dir}"
      shift 2
      ;;
    --prefix)
      PREFIX="${2:?missing value for --prefix}"
      shift 2
      ;;
    --all-prefixes)
      ALL_PREFIXES=1
      shift 1
      ;;
    --reset-openkey-config)
      RESET_OPENKEY_CONFIG=1
      shift 1
      ;;
    --remove-gnome-extension)
      REMOVE_GNOME_EXTENSION=1
      shift 1
      ;;
    --reset-user-data)
      RESET_USER_DATA=1
      shift 1
      ;;
    --dry-run)
      DRY_RUN=1
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

case "$PREFIX" in
  "~")
    PREFIX="${HOME:?HOME is required for ~ prefix}"
    ;;
  "~/"*)
    PREFIX="${HOME:?HOME is required for ~/ prefix}/${PREFIX#"~/"}"
    ;;
esac

run_sudo_rm_file() {
  local path="$1"
  [[ -n "$path" ]] || return 0
  if [[ "$DRY_RUN" == "1" ]]; then
    echo "  rm -f $path"
    return 0
  fi
  sudo rm -f -- "$path" || true
}

run_sudo_rm_dir() {
  local path="$1"
  [[ -n "$path" ]] || return 0
  if [[ "$DRY_RUN" == "1" ]]; then
    echo "  rm -rf $path"
    return 0
  fi
  sudo rm -rf -- "$path" || true
}

run_user_rm_file() {
  local path="$1"
  [[ -n "$path" ]] || return 0
  if [[ "$DRY_RUN" == "1" ]]; then
    echo "  rm -f $path"
    return 0
  fi
  rm -f -- "$path" || true
}

run_user_rm_dir() {
  local path="$1"
  [[ -n "$path" ]] || return 0
  if [[ "$DRY_RUN" == "1" ]]; then
    echo "  rm -rf $path"
    return 0
  fi
  rm -rf -- "$path" || true
}

run_rm_file_for_path() {
  local path="$1"
  if is_user_prefix_value "$path"; then
    run_user_rm_file "$path"
  else
    run_sudo_rm_file "$path"
  fi
}

update_icon_cache_for_prefix() {
  local prefix="$1"
  local icon_theme_dir="$prefix/share/icons/hicolor"

  if ! command -v gtk-update-icon-cache >/dev/null 2>&1; then
    return
  fi
  if [[ ! -d "$icon_theme_dir" ]]; then
    return
  fi

  echo "[openkey] Updating icon cache for prefix: $prefix (optional)"
  if [[ "$DRY_RUN" == "1" ]]; then
    if is_user_prefix_value "$prefix"; then
      echo "  gtk-update-icon-cache -f -t $icon_theme_dir"
    else
      echo "  sudo gtk-update-icon-cache -f -t $icon_theme_dir"
    fi
    return
  fi

  if is_user_prefix_value "$prefix"; then
    gtk-update-icon-cache -f -t "$icon_theme_dir" >/dev/null 2>&1 || true
  else
    sudo gtk-update-icon-cache -f -t "$icon_theme_dir" >/dev/null 2>&1 || true
  fi
}

stop_running_openkey() {
  echo "[openkey] Stopping fcitx5/OpenKey helper processes"
  if [[ "$DRY_RUN" == "1" ]]; then
    echo "  fcitx5-remote -e || true"
    echo "  pkill -x openkey-nonpreedit-server || true"
    echo "  pkill -x openkey-uinput-server || true"
    echo "  pkill -x fcitx5 || true"
    return 0
  fi

  if command -v fcitx5-remote >/dev/null 2>&1; then
    fcitx5-remote -e >/dev/null 2>&1 || true
  fi
  pkill -x openkey-nonpreedit-server >/dev/null 2>&1 || true
  pkill -x openkey-uinput-server >/dev/null 2>&1 || true
  pkill -x fcitx5 >/dev/null 2>&1 || true
}

add_unique_prefix() {
  local prefix="$1"
  [[ -n "$prefix" ]] || return 0
  for existing in "${PREFIXES[@]}"; do
    [[ "$existing" == "$prefix" ]] && return 0
  done
  PREFIXES+=("$prefix")
}

remove_known_paths_for_prefix() {
  local prefix="$1"
  local libdir="$2"
  local rm_file=run_sudo_rm_file
  local rm_dir=run_sudo_rm_dir
  if [[ "$prefix" == "$HOME/.local" || "$prefix" == "$HOME/.local/"* ]]; then
    rm_file=run_user_rm_file
    rm_dir=run_user_rm_dir
  fi

  echo "[openkey] Removing known OpenKey paths under prefix: $prefix"

  "$rm_file" "$libdir/fcitx5/openkey.so"
  "$rm_file" "$prefix/lib/fcitx5/openkey.so"
  "$rm_file" "$prefix/lib64/fcitx5/openkey.so"

  if [[ -d "$prefix/lib" ]]; then
    while IFS= read -r path; do
      "$rm_file" "$path"
    done < <(find "$prefix/lib" -path '*/fcitx5/openkey.so' -type f 2>/dev/null || true)
  fi
  if [[ -d "$prefix/lib64" ]]; then
    while IFS= read -r path; do
      "$rm_file" "$path"
    done < <(find "$prefix/lib64" -path '*/fcitx5/openkey.so' -type f 2>/dev/null || true)
  fi

  "$rm_file" "$prefix/libexec/openkey-nonpreedit-server"

  "$rm_file" "$prefix/share/fcitx5/addon/openkey.conf"
  "$rm_file" "$prefix/share/fcitx5/inputmethod/openkey.conf"

  "$rm_file" "$prefix/share/icons/hicolor/scalable/apps/openkey.svg"
  "$rm_file" "$prefix/share/icons/hicolor/scalable/apps/fcitx-openkey.svg"
  "$rm_file" "$prefix/share/icons/hicolor/scalable/apps/org.fcitx.Fcitx5.fcitx-openkey.svg"
  "$rm_file" "$prefix/share/icons/hicolor/symbolic/apps/openkey-symbolic.svg"
  "$rm_file" "$prefix/share/icons/hicolor/symbolic/apps/fcitx-openkey-symbolic.svg"
  "$rm_file" "$prefix/share/icons/hicolor/symbolic/apps/org.fcitx.Fcitx5.fcitx-openkey-symbolic.svg"
  if [[ -d "$prefix/share/icons/hicolor" ]]; then
    while IFS= read -r path; do
      "$rm_file" "$path"
    done < <(find "$prefix/share/icons/hicolor" \
      \( -name 'openkey.*' -o -name 'fcitx-openkey.*' -o -name 'org.fcitx.Fcitx5.fcitx-openkey.*' \) \
      -type f 2>/dev/null || true)
  fi

  # Legacy files from older Linux packaging experiments.
  "$rm_file" "$prefix/bin/openkey-appmodes"
  "$rm_file" "$prefix/bin/openkey-settings"
  "$rm_file" "$prefix/bin/openkey-uinput-server"
  "$rm_file" "$prefix/lib/systemd/system/openkey-uinput-server@.service"
  "$rm_dir" "$prefix/share/fcitx5-openkey"
}

report_remaining_openkey_files() {
  local found=0
  while IFS= read -r path; do
    if [[ "$found" == "0" ]]; then
      echo "[openkey] Remaining OpenKey/Fcitx metadata files:"
      found=1
    fi
    echo "  $path"
  done < <(
    {
      find /usr /usr/local "$HOME/.local" \
        \( -path '*/fcitx5/openkey.so' \
           -o -path '*/share/fcitx5/addon/openkey.conf' \
           -o -path '*/share/fcitx5/inputmethod/openkey.conf' \
           -o -path '*/libexec/openkey-nonpreedit-server' \
           -o -path '*/share/icons/hicolor/*/apps/*openkey*' \) \
        2>/dev/null
      find "$HOME/.config/fcitx5" \
        \( -name 'openkey.conf' \
           -o -name 'openkey-appmodes.conf' \
           -o -name 'openkey-appmodes-x11.conf' \
           -o -name 'openkey-appmodes-wayland.conf' \
           -o -name 'openkey-macro-table.conf' \) \
        2>/dev/null
    } | sort
  )

  if [[ "$found" == "0" ]]; then
    echo "[openkey] No OpenKey/Fcitx metadata files found in common install paths."
  fi
}

if [[ "$DRY_RUN" != "1" && ( "$ALL_PREFIXES" == "1" || ! is_user_prefix ) ]]; then
  echo "[openkey] Checking sudo (you may be prompted for password)"
  sudo -v
fi

stop_running_openkey

manifest="$BUILD_DIR/install_manifest.txt"
if [[ -f "$manifest" ]]; then
  echo "[openkey] Uninstalling using install manifest: $manifest"
  while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    if [[ "$ALL_PREFIXES" == "1" || ! is_user_prefix || "$path" == "$PREFIX/"* ]]; then
      run_rm_file_for_path "$path"
    fi
  done < "$manifest"
else
  echo "[openkey] No install manifest found: $manifest"
fi

PKG_PREFIX=""
PKG_LIBDIR=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists Fcitx5Core; then
  PKG_PREFIX="$(pkg-config --variable=prefix Fcitx5Core 2>/dev/null || true)"
  PKG_LIBDIR="$(pkg-config --variable=libdir Fcitx5Core 2>/dev/null || true)"
fi

PREFIXES=()
add_unique_prefix "$PREFIX"
if [[ "$ALL_PREFIXES" == "1" || ! is_user_prefix ]]; then
  add_unique_prefix "$PKG_PREFIX"
fi
if [[ "$ALL_PREFIXES" == "1" ]]; then
  add_unique_prefix "/usr"
  add_unique_prefix "/usr/local"
  add_unique_prefix "$HOME/.local"
fi

for prefix in "${PREFIXES[@]}"; do
  libdir="$prefix/lib"
  if [[ -n "$PKG_PREFIX" && "$prefix" == "$PKG_PREFIX" && -n "$PKG_LIBDIR" ]]; then
    libdir="$PKG_LIBDIR"
  fi
  remove_known_paths_for_prefix "$prefix" "$libdir"
done

if [[ "$RESET_OPENKEY_CONFIG" == "1" ]]; then
  echo "[openkey] Removing OpenKey user config files"
  run_user_rm_file "$HOME/.config/fcitx5/conf/openkey.conf"
  run_user_rm_file "$HOME/.config/fcitx5/conf/openkey-appmodes.conf"
  run_user_rm_file "$HOME/.config/fcitx5/conf/openkey-appmodes-x11.conf"
  run_user_rm_file "$HOME/.config/fcitx5/conf/openkey-appmodes-wayland.conf"
  run_user_rm_file "$HOME/.config/fcitx5/conf/openkey-macro-table.conf"
fi

if [[ "$REMOVE_GNOME_EXTENSION" == "1" ]]; then
  echo "[openkey] Removing optional GNOME Shell bridge extension"
  run_user_rm_dir "$HOME/.local/share/gnome-shell/extensions/openkey-bridge@openkey.dev"
fi

if [[ "$RESET_USER_DATA" == "1" ]]; then
  echo "[openkey] Removing all user fcitx5 config/data (DANGEROUS)"
  run_user_rm_dir "$HOME/.config/fcitx5"
  run_user_rm_dir "$HOME/.local/share/fcitx5"
  run_user_rm_dir "$HOME/.cache/fcitx5"
fi

if [[ ( "$ALL_PREFIXES" == "1" || ! is_user_prefix ) && -d /etc/udev/rules.d ]]; then
  echo "[openkey] Removing udev rule"
  run_sudo_rm_file /etc/udev/rules.d/99-openkey-uinput.rules
  if [[ "$DRY_RUN" != "1" ]] && command -v udevadm >/dev/null 2>&1; then
    sudo udevadm control --reload-rules >/dev/null 2>&1 || true
    sudo udevadm trigger --name-match=uinput >/dev/null 2>&1 || true
  fi
fi

for prefix in "${PREFIXES[@]}"; do
  update_icon_cache_for_prefix "$prefix"
done

echo "[openkey] Restarting fcitx5"
if [[ "$DRY_RUN" != "1" ]]; then
  fcitx5 -rd >/dev/null 2>&1 || true
fi

if [[ "$DRY_RUN" != "1" ]]; then
  report_remaining_openkey_files
fi

cat <<EOF
[openkey] Done.

If OpenKey still appears in fcitx5-configtool, run:
  ./scripts/uninstall.sh --all-prefixes --reset-openkey-config

If the optional GNOME bridge extension still appears, run:
  ./scripts/uninstall.sh --remove-gnome-extension

Then log out/in or run:
  fcitx5 -rd
EOF
