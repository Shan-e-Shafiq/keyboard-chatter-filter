#!/usr/bin/env bash
# keyboard-chatter-filter installer for macOS and Linux.
#
#   curl -fsSL https://raw.githubusercontent.com/Shan-e-Shafiq/keyboard-chatter-filter/main/install.sh | bash
#
# Downloads the prebuilt release binary for this OS and CPU from GitHub Releases, verifies its
# SHA-256 checksum, installs it, registers the background service and starts it.
#
# Environment variables (all optional):
#   KCF_REPO               GitHub "owner/repo" to install from (default: Shan-e-Shafiq/keyboard-chatter-filter)
#   KCF_VERSION            release tag, e.g. v0.1.0 (default: the latest release)
#   KCF_INSTALL_DIR        binary location (default: ~/.local/bin on macOS, /usr/local/bin on Linux)
#   KCF_VERIFY_ATTESTATION 1 = also verify the GitHub build-provenance attestation (needs the gh CLI,
#                          logged in: gh attestation verify requires authentication)
#   KCF_ARCHIVE_DIR        install from a directory that already holds the release archive and
#                          SHA256SUMS (offline installs); the checksum is still verified
#
# Nothing is compiled. Nothing but the release files is downloaded. The script only touches:
#   macOS: the binary, ~/Library/LaunchAgents/<label>.plist, ~/.config/keyboard-chatter-filter,
#          ~/Library/Logs/keyboard-chatter-filter
#   Linux: the binary, /etc/systemd/system/keyboard-chatter-filter.service,
#          /etc/keyboard-chatter-filter, /etc/modules-load.d/keyboard-chatter-filter.conf
set -euo pipefail

readonly DEFAULT_REPO="Shan-e-Shafiq/keyboard-chatter-filter"
readonly PROGRAM="keyboard-chatter-filter"

say() { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not installed"
}

detect_os() {
    case "$(uname -s)" in
        Darwin) echo macos ;;
        Linux) echo linux ;;
        *) die "unsupported operating system: $(uname -s). On Windows use install.ps1." ;;
    esac
}

detect_arch() {
    local os="$1" machine
    machine="$(uname -m)"
    case "$machine" in
        arm64 | aarch64) echo arm64 ;;
        x86_64 | amd64)
            # An Intel shell under Rosetta on Apple silicon: install the native build.
            if [ "$os" = macos ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
                echo arm64
            else
                echo x86_64
            fi
            ;;
        *) die "unsupported CPU architecture: $machine" ;;
    esac
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        die "neither sha256sum nor shasum is available to verify the download"
    fi
}

download() {
    # HTTPS only, including redirects.
    curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --retry 3 --output "$2" "$1"
}

main() {
    need curl
    need tar
    need uname

    local repo version os arch asset base tmp
    repo="${KCF_REPO:-$DEFAULT_REPO}"
    version="${KCF_VERSION:-latest}"
    [[ "$repo" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || die "KCF_REPO must look like owner/repo"
    [[ "$version" =~ ^(latest|v[0-9A-Za-z.+-]+)$ ]] || die "KCF_VERSION must be 'latest' or a tag like v0.1.0"

    os="$(detect_os)"
    arch="$(detect_arch "$os")"
    asset="${PROGRAM}-${os}-${arch}.tar.gz"
    if [ "$version" = latest ]; then
        base="https://github.com/${repo}/releases/latest/download"
    else
        base="https://github.com/${repo}/releases/download/${version}"
    fi

    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064  # expand now: $tmp is local
    trap "rm -rf '$tmp'" EXIT

    if [ -n "${KCF_ARCHIVE_DIR:-}" ]; then
        say "Using ${asset} from ${KCF_ARCHIVE_DIR}"
        cp "${KCF_ARCHIVE_DIR}/${asset}" "${tmp}/${asset}" || die "${asset} not found in ${KCF_ARCHIVE_DIR}"
        cp "${KCF_ARCHIVE_DIR}/SHA256SUMS" "${tmp}/SHA256SUMS" || die "SHA256SUMS not found in ${KCF_ARCHIVE_DIR}"
    else
        say "Downloading ${asset} (${version}) from github.com/${repo}"
        download "${base}/${asset}" "${tmp}/${asset}" || die "download failed: ${base}/${asset}"
        download "${base}/SHA256SUMS" "${tmp}/SHA256SUMS" || die "download failed: ${base}/SHA256SUMS"
    fi

    local expected actual
    expected="$(awk -v name="$asset" '$2 == name || $2 == "*" name {print $1}' "${tmp}/SHA256SUMS")"
    [ -n "$expected" ] || die "${asset} is not listed in SHA256SUMS"
    actual="$(sha256_of "${tmp}/${asset}")"
    [ "$expected" = "$actual" ] || die "checksum mismatch for ${asset} (expected ${expected}, got ${actual})"
    say "Checksum verified (SHA-256 ${actual})"

    if [ "${KCF_VERIFY_ATTESTATION:-0}" = 1 ]; then
        need gh
        gh attestation verify "${tmp}/${asset}" --repo "$repo" >/dev/null ||
            die "build provenance attestation could not be verified"
        say "Build provenance attestation verified"
    fi

    mkdir -p "${tmp}/extract"
    tar -xzf "${tmp}/${asset}" -C "${tmp}/extract" "$PROGRAM"
    local binary="${tmp}/extract/${PROGRAM}"
    [ -f "$binary" ] && [ ! -L "$binary" ] || die "the archive does not contain the ${PROGRAM} binary"
    chmod 0755 "$binary"

    local dir target
    if [ "$os" = macos ]; then
        dir="${KCF_INSTALL_DIR:-$HOME/.local/bin}"
        target="${dir}/${PROGRAM}"
        mkdir -p "$dir"
        # Atomic replace: a running copy keeps its old file until it restarts.
        cp "$binary" "${target}.new"
        mv -f "${target}.new" "$target"
        say "Installed ${target}"
        say ""
        "$target" install || die "registering the LaunchAgent failed"
    else
        dir="${KCF_INSTALL_DIR:-/usr/local/bin}"
        target="${dir}/${PROGRAM}"
        local sudo=""
        if [ "$(id -u)" -ne 0 ]; then
            need sudo
            sudo="sudo"
            say "Administrator rights are needed to install the system service (sudo)."
        fi
        $sudo mkdir -p "$dir"
        $sudo install -m 0755 -o root -g root "$binary" "${target}.new"
        $sudo mv -f "${target}.new" "$target"
        say "Installed ${target}"
        say ""
        $sudo "$target" install || die "registering the systemd service failed"
    fi

    say ""
    case ":${PATH}:" in
        *":${dir}:"*) ;;
        *) say "Note: ${dir} is not on your PATH. Add it, or run ${target} directly." ;;
    esac
    say "Done. Useful commands:"
    say "  ${PROGRAM} status      show what the filter is doing"
    say "  ${PROGRAM} config      show the configuration file and settings"
    say "  ${PROGRAM} uninstall   remove everything again"
    if [ "$os" = macos ]; then
        say ""
        say "macOS: filtering only starts after you allow Accessibility access for ${PROGRAM}"
        say "(System Settings > Privacy & Security > Accessibility). The keyboard works normally until then."
    fi
}

main "$@"
