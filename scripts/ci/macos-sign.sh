#!/bin/bash
# Signs the macOS binary for release.
#
#   macos-sign.sh ad-hoc <binary>        ad-hoc signature with a stable identifier (no Apple account)
#   macos-sign.sh developer-id <binary>  Developer ID + hardened runtime + notarization
#
# Developer ID mode reads CERTIFICATE_P12_BASE64, CERTIFICATE_PASSWORD, SIGNING_IDENTITY,
# NOTARY_APPLE_ID, NOTARY_PASSWORD (app-specific password) and NOTARY_TEAM_ID from the environment.
set -euo pipefail

mode="${1:?mode}"
binary="${2:?binary}"
identifier="keyboard-chatter-filter"

case "$mode" in
ad-hoc)
    codesign --force --sign - --identifier "$identifier" "$binary"
    ;;
developer-id)
    keychain="$RUNNER_TEMP/signing.keychain-db"
    keychain_password="$(uuidgen)"
    certificate="$RUNNER_TEMP/certificate.p12"
    echo "$CERTIFICATE_P12_BASE64" | base64 --decode > "$certificate"
    security create-keychain -p "$keychain_password" "$keychain"
    security set-keychain-settings -lut 21600 "$keychain"
    security unlock-keychain -p "$keychain_password" "$keychain"
    security import "$certificate" -P "$CERTIFICATE_PASSWORD" -A -t cert -f pkcs12 -k "$keychain"
    security set-key-partition-list -S apple-tool:,apple: -k "$keychain_password" "$keychain" >/dev/null
    existing=()
    while IFS= read -r line; do
        line="${line//\"/}"
        existing+=("${line// /}")
    done < <(security list-keychains -d user)
    security list-keychains -d user -s "$keychain" "${existing[@]}"
    rm -f "$certificate"

    codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" --identifier "$identifier" "$binary"

    # Bare executables cannot be stapled; Gatekeeper looks the ticket up online.
    archive="$RUNNER_TEMP/notarize.zip"
    ditto -c -k --keepParent "$binary" "$archive"
    xcrun notarytool submit "$archive" --apple-id "$NOTARY_APPLE_ID" --password "$NOTARY_PASSWORD" \
        --team-id "$NOTARY_TEAM_ID" --wait
    rm -f "$archive"
    security delete-keychain "$keychain"
    ;;
*)
    echo "unknown mode: $mode" >&2
    exit 2
    ;;
esac

codesign --verify --strict --verbose=2 "$binary"
codesign --display --verbose=2 "$binary" 2>&1 | grep -E 'Identifier|Authority|Signature' || true
