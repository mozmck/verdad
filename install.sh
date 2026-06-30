#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
install_mode=""
prefix=""
prefix_supplied=0
assume_yes=0

source_bin=""
source_data=""
source_license=""
source_licenses=""
source_lib=""
source_sword=""
source_layout=""
source_build_dir=""
existing_sword_data=""
skip_sword_data_install=0

usage() {
    cat <<'EOF'
Usage: ./install.sh [options]

Install Verdad from a prebuilt bundle, or from a local source build.

Expected bundle layout:
  bin/verdad
  lib/verdad/
  share/sword/
  share/verdad/
  install.sh

Expected source build layout:
  build/verdad
  build/cmake_install.cmake
  data/

Default behavior:
  - If run as root, install under /usr/local.
  - Otherwise, install for the current user under ~/.local.

Options:
  --user               Force a user-local install.
  --system             Force a system-wide install.
  --prefix DIR         Install under DIR instead of the default prefix.
  -y, --yes            Accept defaults without prompting.
  -h, --help           Show this help text.
EOF
}

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

expand_path() {
    case "$1" in
        "~")
            printf '%s\n' "$HOME"
            ;;
        "~/"*)
            printf '%s/%s\n' "$HOME" "${1#~/}"
            ;;
        *)
            printf '%s\n' "$1"
            ;;
    esac
}

can_prompt() {
    [[ -t 0 && -t 1 ]]
}

desktop_owner_home() {
    if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
        local home_dir
        home_dir=$(getent passwd "$SUDO_USER" | cut -d: -f6)
        if [[ -n "$home_dir" ]]; then
            printf '%s\n' "$home_dir"
            return
        fi
    fi

    printf '%s\n' "$HOME"
}

desktop_owner_ids() {
    if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
        printf '%s:%s\n' "${SUDO_UID:-$(id -u "$SUDO_USER")}" \
            "${SUDO_GID:-$(id -g "$SUDO_USER")}"
        return
    fi

    printf '%s:%s\n' "$(id -u)" "$(id -g)"
}

sword_dir_has_data() {
    local path=$1
    [[ -d "$path" ]] || return 1

    if [[ -d "$path/locales.d" && -f "$path/mods.d/globals.conf" ]]; then
        return 0
    fi

    if [[ -d "$path/mods.d" ]] && compgen -G "$path/mods.d/*.conf" >/dev/null; then
        return 0
    fi

    return 1
}

find_existing_sword_data_dir() {
    local home_dir
    local candidate

    if [[ -n "${SWORD_PATH:-}" ]]; then
        while IFS= read -r candidate; do
            [[ -n "$candidate" ]] || continue
            if sword_dir_has_data "$candidate"; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done < <(printf '%s\n' "$SWORD_PATH" | tr ':' '\n')
    fi

    if [[ "$install_mode" == "user" ]]; then
        home_dir=$(desktop_owner_home)
        for candidate in "$home_dir/.sword" "$home_dir/sword"; do
            if sword_dir_has_data "$candidate"; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done
    fi

    for candidate in /usr/local/share/sword /usr/share/sword /opt/homebrew/share/sword; do
        if sword_dir_has_data "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

desktop_dir_for_home() {
    local home_dir=$1
    local config_file="$home_dir/.config/user-dirs.dirs"
    local desktop_dir="$home_dir/Desktop"

    if [[ -f "$config_file" ]]; then
        local line
        line=$(grep '^XDG_DESKTOP_DIR=' "$config_file" | tail -n 1 || true)
        if [[ -n "$line" ]]; then
            desktop_dir=${line#XDG_DESKTOP_DIR=}
            desktop_dir=${desktop_dir#\"}
            desktop_dir=${desktop_dir%\"}
            desktop_dir=${desktop_dir//\$HOME/$home_dir}
            desktop_dir=${desktop_dir//\$\{HOME\}/$home_dir}
        fi
    fi

    printf '%s\n' "$desktop_dir"
}

escape_desktop_exec() {
    local value=$1
    value=${value//\\/\\\\}
    value=${value// /\\ }
    value=${value//$'\t'/\\t}
    value=${value//\"/\\\"}
    printf '%s\n' "$value"
}

detect_source_layout() {
    if [[ -x "$script_dir/bin/verdad" &&
          -d "$script_dir/lib/verdad" &&
          -d "$script_dir/share/sword" &&
          -d "$script_dir/share/verdad" ]]; then
        source_bin="$script_dir/bin/verdad"
        source_lib="$script_dir/lib/verdad"
        source_sword="$script_dir/share/sword"
        source_data="$script_dir/share/verdad"
        source_license="$script_dir/share/verdad/LICENSE"
        source_licenses="$script_dir/share/verdad/LICENSES"
        source_layout="bundle"
        return 0
    fi

    if [[ -x "$script_dir/build/verdad" &&
          -f "$script_dir/build/cmake_install.cmake" &&
          -d "$script_dir/data" ]]; then
        source_bin="$script_dir/build/verdad"
        source_data="$script_dir/data"
        source_license="$script_dir/LICENSE"
        source_licenses="$script_dir/LICENSES"
        source_build_dir="$script_dir/build"
        source_layout="source-build"
        return 0
    fi

    return 1
}

write_desktop_entry() {
    local desktop_file=$1
    local exec_path=$2
    local exec_value
    exec_value=$(escape_desktop_exec "$exec_path")

    local tmp_file
    tmp_file=$(mktemp)
    cat >"$tmp_file" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Verdad
GenericName=Bible Study
Comment=Study the Bible with CrossWire SWORD modules
Exec=$exec_value
TryExec=$exec_value
Icon=verdad
Terminal=false
Categories=Education;
Keywords=Bible;Study;SWORD;Commentary;Dictionary;
StartupNotify=true
EOF
    install -m 0644 "$tmp_file" "$desktop_file"
    rm -f "$tmp_file"
}

install_desktop_launcher() {
    local source_desktop_file=$1
    local owner_home
    local owner_ids
    local desktop_dir
    local desktop_launcher

    owner_home=$(desktop_owner_home)
    owner_ids=$(desktop_owner_ids)
    desktop_dir=$(desktop_dir_for_home "$owner_home")
    desktop_launcher="$desktop_dir/verdad.desktop"

    install -d "$desktop_dir"
    install -m 0755 "$source_desktop_file" "$desktop_launcher"

    if [[ $EUID -eq 0 ]]; then
        chown "${owner_ids%:*}:${owner_ids#*:}" "$desktop_dir" "$desktop_launcher"
    fi

    printf 'Desktop launcher: %s\n' "$desktop_launcher"
}

update_desktop_cache() {
    local applications_dir=$1
    local icon_root=$2

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$applications_dir" >/dev/null 2>&1 || true
    fi

    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f "$icon_root" >/dev/null 2>&1 || true
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --user)
            [[ "$install_mode" != "system" ]] || die "Choose either --user or --system, not both."
            install_mode="user"
            shift
            ;;
        --system)
            [[ "$install_mode" != "user" ]] || die "Choose either --user or --system, not both."
            install_mode="system"
            shift
            ;;
        --prefix)
            [[ $# -ge 2 ]] || die "--prefix requires a directory."
            prefix=$(expand_path "$2")
            prefix_supplied=1
            shift 2
            ;;
        -y|--yes)
            assume_yes=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unknown option: $1"
            ;;
    esac
done

detect_source_layout || die "Could not find a complete Verdad bundle or source build. Expected bin/verdad, lib/verdad, share/sword, and share/verdad; or build/verdad, build/cmake_install.cmake, and data/."

[[ -x "$source_bin" ]] || die "Missing executable: $source_bin"
[[ -f "$source_data/master.css" ]] || die "Missing runtime asset: $source_data/master.css"
[[ -f "$source_data/help.html" ]] || die "Missing runtime asset: $source_data/help.html"
[[ -f "$source_data/verdad_icon.png" ]] || die "Missing runtime asset: $source_data/verdad_icon.png"

if [[ "$source_layout" == "bundle" ]]; then
    [[ -d "$source_lib" ]] || die "Missing private library directory: $source_lib"
    [[ -d "$source_sword/locales.d" ]] || die "Missing SWORD locales: $source_sword/locales.d"
    [[ -f "$source_sword/mods.d/globals.conf" ]] || die "Missing SWORD globals.conf: $source_sword/mods.d/globals.conf"

    sword_runtime_found=0
    for candidate in "$source_lib"/libsword.so*; do
        if [[ -f "$candidate" && ! -L "$candidate" ]]; then
            sword_runtime_found=1
            break
        fi
    done
    [[ $sword_runtime_found -eq 1 ]] || die "Missing private libsword runtime in: $source_lib"
else
    command -v cmake >/dev/null 2>&1 || die "Installing from a source build requires cmake on PATH."
    [[ -f "$source_build_dir/cmake_install.cmake" ]] || die "Missing CMake install script: $source_build_dir/cmake_install.cmake"
fi

if [[ -z "$install_mode" ]]; then
    if [[ $EUID -eq 0 ]]; then
        install_mode="system"
    else
        install_mode="user"
    fi
fi

if [[ "$install_mode" == "system" ]]; then
    [[ $EUID -eq 0 ]] || die "System install requires running the script with sudo or as root."
    prefix=${prefix:-/usr/local}
fi

if [[ "$install_mode" == "user" ]]; then
    prefix=${prefix:-"$HOME/.local"}
    if [[ $prefix_supplied -eq 0 && $assume_yes -eq 0 ]] && can_prompt; then
        printf 'User install prefix [%s]: ' "$prefix"
        read -r reply
        if [[ -n "$reply" ]]; then
            prefix=$(expand_path "$reply")
        fi
    fi
fi

prefix=$(expand_path "$prefix")

if existing_sword_data=$(find_existing_sword_data_dir); then
    skip_sword_data_install=1
fi

dest_bin_dir="$prefix/bin"
dest_lib_dir="$prefix/lib/verdad"
dest_sword_dir="$prefix/share/sword"
dest_data_dir="$prefix/share/verdad"
dest_applications_dir="$prefix/share/applications"
dest_pixmaps_dir="$prefix/share/pixmaps"
dest_icon_128_dir="$prefix/share/icons/hicolor/128x128/apps"
dest_icon_24_dir="$prefix/share/icons/hicolor/24x24/apps"

icon_128_source="$source_data/verdad_icon_128.png"
if [[ ! -f "$icon_128_source" ]]; then
    icon_128_source="$source_data/verdad_icon.png"
fi

icon_24_source="$source_data/verdad_icon_24.png"
if [[ ! -f "$icon_24_source" ]]; then
    icon_24_source="$icon_128_source"
fi

printf 'Install mode: %s\n' "$install_mode"
printf 'Install prefix: %s\n' "$prefix"
printf 'Source layout: %s\n' "$source_layout"
printf 'Source executable: %s\n' "$source_bin"
printf 'Source data: %s\n' "$source_data"
if [[ $skip_sword_data_install -eq 1 ]]; then
    printf 'Existing SWORD data: %s\n' "$existing_sword_data"
    printf 'Bundled SWORD data: skipped\n'
fi

desktop_file="$dest_applications_dir/verdad.desktop"

if [[ "$source_layout" == "source-build" ]]; then
    printf 'Source CMake build: %s\n' "$source_build_dir"
    source_build_config=""
    if [[ -f "$source_build_dir/CMakeCache.txt" ]]; then
        source_build_config=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' \
            "$source_build_dir/CMakeCache.txt" | tail -n 1)
    fi
    source_build_config=${source_build_config:-Release}

    VERDAD_SKIP_BUNDLED_SWORD_DATA_INSTALL=$skip_sword_data_install cmake \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_CONFIG_NAME="$source_build_config" \
        -DCMAKE_INSTALL_LOCAL_ONLY=1 \
        -P "$source_build_dir/cmake_install.cmake"

    if [[ ! -f "$desktop_file" ]]; then
        install -d "$dest_applications_dir"
        write_desktop_entry "$desktop_file" "$dest_bin_dir/verdad"
    fi

    update_desktop_cache "$dest_applications_dir" "$prefix/share/icons/hicolor"

    if [[ $assume_yes -eq 0 ]] && can_prompt; then
        desktop_dir=$(desktop_dir_for_home "$(desktop_owner_home)")
        printf 'Create a desktop launcher in %s? [y/N] ' "$desktop_dir"
        read -r reply
        if [[ "$reply" =~ ^[Yy]$ ]]; then
            install_desktop_launcher "$desktop_file"
        fi
    fi

    printf '\nInstalled Verdad to %s\n' "$prefix"
    printf 'Binary: %s/bin/verdad\n' "$prefix"
    printf 'Desktop entry: %s/share/applications/verdad.desktop\n' "$prefix"

    if [[ "$install_mode" == "user" && "$prefix" == "$HOME/.local" ]]; then
        printf 'If %s/bin is not on your PATH, start Verdad with:\n' "$HOME/.local"
        printf '  %s/bin/verdad\n' "$HOME/.local"
    fi

    exit 0
fi

printf 'Source private libraries: %s\n' "$source_lib"
if [[ $skip_sword_data_install -eq 0 ]]; then
    printf 'Source SWORD data: %s\n' "$source_sword"
fi

install -d "$dest_bin_dir" "$dest_lib_dir" "$dest_data_dir" \
    "$dest_applications_dir" "$dest_pixmaps_dir" \
    "$dest_icon_128_dir" "$dest_icon_24_dir"
if [[ $skip_sword_data_install -eq 0 ]]; then
    install -d "$dest_sword_dir"
fi

install -m 0755 "$source_bin" "$dest_bin_dir/verdad"
cp -a "$source_lib/." "$dest_lib_dir/"
if [[ $skip_sword_data_install -eq 0 ]]; then
    cp -a "$source_sword/." "$dest_sword_dir/"
fi
install -m 0644 "$source_data/master.css" "$dest_data_dir/master.css"
install -m 0644 "$source_data/help.html" "$dest_data_dir/help.html"
install -m 0644 "$source_data/verdad_icon.png" "$dest_data_dir/verdad_icon.png"

if [[ -f "$source_data/verdad_icon_128.png" ]]; then
    install -m 0644 "$source_data/verdad_icon_128.png" \
        "$dest_data_dir/verdad_icon_128.png"
fi

if [[ -f "$source_data/verdad_icon_24.png" ]]; then
    install -m 0644 "$source_data/verdad_icon_24.png" \
        "$dest_data_dir/verdad_icon_24.png"
fi

if [[ -f "$source_license" ]]; then
    install -m 0644 "$source_license" "$dest_data_dir/LICENSE"
fi

if [[ -d "$source_licenses" ]]; then
    install -d "$dest_data_dir/LICENSES"
    cp -a "$source_licenses/." "$dest_data_dir/LICENSES/"
fi

install -m 0644 "$icon_128_source" "$dest_pixmaps_dir/verdad.png"
install -m 0644 "$icon_128_source" "$dest_icon_128_dir/verdad.png"
install -m 0644 "$icon_24_source" "$dest_icon_24_dir/verdad.png"

write_desktop_entry "$desktop_file" "$dest_bin_dir/verdad"
update_desktop_cache "$dest_applications_dir" "$prefix/share/icons/hicolor"

if [[ $assume_yes -eq 0 ]] && can_prompt; then
    desktop_dir=$(desktop_dir_for_home "$(desktop_owner_home)")
    printf 'Create a desktop launcher in %s? [y/N] ' "$desktop_dir"
    read -r reply
    if [[ "$reply" =~ ^[Yy]$ ]]; then
        install_desktop_launcher "$desktop_file"
    fi
fi

printf '\nInstalled Verdad to %s\n' "$prefix"
printf 'Binary: %s/bin/verdad\n' "$prefix"
printf 'Desktop entry: %s/share/applications/verdad.desktop\n' "$prefix"

if [[ "$install_mode" == "user" && "$prefix" == "$HOME/.local" ]]; then
    printf 'If %s/bin is not on your PATH, start Verdad with:\n' "$HOME/.local"
    printf '  %s/bin/verdad\n' "$HOME/.local"
fi
