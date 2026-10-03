#!/usr/bin/env bash
# Removes keyboard-chatter-filter from macOS or Linux. Safe to run more than once.
#
#   bash uninstall.sh            # keep the configuration file and logs
#   bash uninstall.sh --purge    # also remove the configuration file and logs
#
# Normally `keyboard-chatter-filter uninstall` does the same; this script also works when the binary
# is already gone. It only deletes files this project creates.
set -euo pipefail

readonly PROGRAM="keyboard-chatter-filter"
readonly LABEL="keyboard-chatter-filter"

purge=0
for arg in "$@"; do
    case "$arg" in
        --purge) purge=1 ;;
        -h | --help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

remove() {
    # remove <path>: delete a regular file or symlink if present
    if [ -e "$1" ] || [ -L "$1" ]; then
        ${SUDO:-} rm -f -- "$1" && echo "Removed $1"
    fi
}

remove_dir_if_empty() {
    [ -d "$1" ] && ${SUDO:-} rmdir -- "$1" 2>/dev/null && echo "Removed $1" || true
}

find_binary() {
    local candidate
    for candidate in "$(command -v "$PROGRAM" 2>/dev/null || true)" "$HOME/.local/bin/$PROGRAM" "/usr/local/bin/$PROGRAM"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            echo "$candidate"
            return
        fi
    done
}

uninstall_macos() {
    local plist="$HOME/Library/LaunchAgents/${LABEL}.plist"
    local binary
    binary="$(find_binary || true)"
    if [ -n "$binary" ]; then
        if [ "$purge" = 1 ]; then "$binary" uninstall --purge || true; else "$binary" uninstall || true; fi
    fi
    # Anything left over (binary already deleted, or an interrupted uninstall).
    launchctl bootout "gui/$(id -u)/${LABEL}" 2>/dev/null || true
    remove "$plist"
    remove "$HOME/.local/bin/$PROGRAM"
    remove "$HOME/Library/Application Support/$PROGRAM/status.sock"
    remove "$HOME/Library/Application Support/$PROGRAM/daemon.lock"
    remove_dir_if_empty "$HOME/Library/Application Support/$PROGRAM"
    if [ "$purge" = 1 ]; then
        remove "$HOME/.config/$PROGRAM/config.toml"
        remove_dir_if_empty "$HOME/.config/$PROGRAM"
        remove "$HOME/Library/Logs/$PROGRAM/$PROGRAM.log"
        remove "$HOME/Library/Logs/$PROGRAM/$PROGRAM.log.1"
        remove_dir_if_empty "$HOME/Library/Logs/$PROGRAM"
    fi
    echo "Uninstalled. You may also remove ${PROGRAM} from System Settings > Privacy & Security > Accessibility."
}

uninstall_linux() {
    SUDO=""
    if [ "$(id -u)" -ne 0 ]; then
        SUDO="sudo"
    fi
    local binary unit="/etc/systemd/system/${LABEL}.service"
    binary="$(find_binary || true)"
    if [ -n "$binary" ]; then
        if [ "$purge" = 1 ]; then $SUDO "$binary" uninstall --purge || true; else $SUDO "$binary" uninstall || true; fi
    fi
    if command -v systemctl >/dev/null 2>&1; then
        $SUDO systemctl disable --now "${LABEL}.service" 2>/dev/null || true
    fi
    remove "$unit"
    remove "/etc/modules-load.d/${LABEL}.conf"
    if command -v systemctl >/dev/null 2>&1; then
        $SUDO systemctl daemon-reload 2>/dev/null || true
        $SUDO systemctl reset-failed "${LABEL}.service" 2>/dev/null || true
    fi
    remove "/usr/local/bin/$PROGRAM"
    if [ "$purge" = 1 ]; then
        remove "/etc/$PROGRAM/config.toml"
        remove_dir_if_empty "/etc/$PROGRAM"
    fi
    echo "Uninstalled."
}

case "$(uname -s)" in
    Darwin) uninstall_macos ;;
    Linux) uninstall_linux ;;
    *) echo "unsupported operating system; on Windows use uninstall.ps1" >&2; exit 1 ;;
esac
