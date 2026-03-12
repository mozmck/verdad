#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
install_mode=""
prefix=""
prefix_supplied=0
assume_yes=0
sudo_cmd=""

source_bin=""
source_data=""
source_license=""
source_licenses=""

usage() {
    cat <<'EOF'
Usage: ./install.sh [options]

Install a prebuilt Verdad bundle without requiring CMake on the target system.

Expected bundle layout:
  bin/verdad
  share/verdad/
  install.sh

The script also accepts a built source tree for developer testing:
  build/verdad
  data/
  LICENSE
  LICENSES/

Default behavior:
  - If root or working sudo access is available, install under /usr/local.
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

run_with_privilege() {
    if [[ -n "$sudo_cmd" ]]; then
        "$sudo_cmd" "$@"
    else
        "$@"
    fi
}

escape_desktop_exec() {
    local value=$1
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    printf '"%s"\n' "$value"
}

have_sudo_access() {
    if [[ $EUID -eq 0 ]]; then
        return 0
    fi

    if ! command -v sudo >/dev/null 2>&1; then
        return 1
    fi

    if sudo -n true 2>/dev/null; then
        sudo_cmd="sudo"
        return 0
    fi

    if can_prompt && sudo -v; then
        sudo_cmd="sudo"
        return 0
    fi

    return 1
}

detect_source_layout() {
    if [[ -x "$script_dir/bin/verdad" && -d "$script_dir/share/verdad" ]]; then
        source_bin="$script_dir/bin/verdad"
        source_data="$script_dir/share/verdad"
        source_license="$script_dir/share/verdad/LICENSE"
        source_licenses="$script_dir/share/verdad/LICENSES"
        return 0
    fi

    if [[ -x "$script_dir/build/verdad" && -d "$script_dir/data" ]]; then
        source_bin="$script_dir/build/verdad"
        source_data="$script_dir/data"
        source_license="$script_dir/LICENSE"
        source_licenses="$script_dir/LICENSES"
        return 0
    fi

    if [[ -x "$script_dir/verdad" && -d "$script_dir/data" ]]; then
        source_bin="$script_dir/verdad"
        source_data="$script_dir/data"
        source_license="$script_dir/LICENSE"
        source_licenses="$script_dir/LICENSES"
        return 0
    fi

    return 1
}

write_desktop_entry() {
    local desktop_file=$1
    local exec_path=$2
    local exec_value
    exec_value=$(escape_desktop_exec "$exec_path")

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
    run_with_privilege install -m 0644 "$tmp_file" "$desktop_file"
    rm -f "$tmp_file"
}

update_desktop_cache() {
    local applications_dir=$1
    local icon_root=$2

    if command -v update-desktop-database >/dev/null 2>&1; then
        run_with_privilege update-desktop-database "$applications_dir" >/dev/null 2>&1 || true
    fi

    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        run_with_privilege gtk-update-icon-cache -q -t -f "$icon_root" >/dev/null 2>&1 || true
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

detect_source_layout || die "Could not find a Verdad bundle. Expected bin/verdad with share/verdad, or a built source tree with build/verdad and data/."

[[ -x "$source_bin" ]] || die "Missing executable: $source_bin"
[[ -f "$source_data/master.css" ]] || die "Missing runtime asset: $source_data/master.css"
[[ -f "$source_data/help.html" ]] || die "Missing runtime asset: $source_data/help.html"
[[ -f "$source_data/verdad_icon.png" ]] || die "Missing runtime asset: $source_data/verdad_icon.png"

if [[ -z "$install_mode" ]]; then
    if have_sudo_access; then
        install_mode="system"
    else
        install_mode="user"
    fi
fi

if [[ "$install_mode" == "system" ]]; then
    prefix=${prefix:-/usr/local}
    if ! have_sudo_access; then
        printf 'No sudo access available. Falling back to a user install.\n'
        install_mode="user"
        sudo_cmd=""
    fi
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

dest_bin_dir="$prefix/bin"
dest_data_dir="$prefix/share/verdad"
dest_applications_dir="$prefix/share/applications"
dest_pixmaps_dir="$prefix/share/pixmaps"
dest_icon_dir="$prefix/share/icons/hicolor/128x128/apps"

icon_source="$source_data/verdad_icon_128.png"
if [[ ! -f "$icon_source" ]]; then
    icon_source="$source_data/verdad_icon.png"
fi

printf 'Install mode: %s\n' "$install_mode"
printf 'Install prefix: %s\n' "$prefix"
printf 'Source executable: %s\n' "$source_bin"
printf 'Source data: %s\n' "$source_data"

run_with_privilege install -d "$dest_bin_dir" "$dest_data_dir" \
    "$dest_applications_dir" "$dest_pixmaps_dir" "$dest_icon_dir"

run_with_privilege install -m 0755 "$source_bin" "$dest_bin_dir/verdad"
run_with_privilege install -m 0644 "$source_data/master.css" "$dest_data_dir/master.css"
run_with_privilege install -m 0644 "$source_data/help.html" "$dest_data_dir/help.html"
run_with_privilege install -m 0644 "$source_data/verdad_icon.png" "$dest_data_dir/verdad_icon.png"

if [[ -f "$source_data/verdad_icon_128.png" ]]; then
    run_with_privilege install -m 0644 "$source_data/verdad_icon_128.png" \
        "$dest_data_dir/verdad_icon_128.png"
fi

if [[ -f "$source_license" ]]; then
    run_with_privilege install -m 0644 "$source_license" "$dest_data_dir/LICENSE"
fi

if [[ -d "$source_licenses" ]]; then
    run_with_privilege install -d "$dest_data_dir/LICENSES"
    run_with_privilege cp -a "$source_licenses/." "$dest_data_dir/LICENSES/"
fi

run_with_privilege install -m 0644 "$icon_source" "$dest_pixmaps_dir/verdad.png"
run_with_privilege install -m 0644 "$icon_source" "$dest_icon_dir/verdad.png"

desktop_file="$dest_applications_dir/verdad.desktop"
write_desktop_entry "$desktop_file" "$dest_bin_dir/verdad"
update_desktop_cache "$dest_applications_dir" "$prefix/share/icons/hicolor"

printf '\nInstalled Verdad to %s\n' "$prefix"
printf 'Binary: %s/bin/verdad\n' "$prefix"
printf 'Desktop entry: %s/share/applications/verdad.desktop\n' "$prefix"

if [[ "$install_mode" == "user" && "$prefix" == "$HOME/.local" ]]; then
    printf 'If %s/bin is not on your PATH, start Verdad with:\n' "$HOME/.local"
    printf '  %s/bin/verdad\n' "$HOME/.local"
fi
