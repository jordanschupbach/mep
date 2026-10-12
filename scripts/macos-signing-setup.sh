#!/usr/bin/env bash
# One-time macOS setup: a self-signed code-signing certificate in the login
# keychain, which `just install` then signs mep with.
#
# Why: an unsigned (ad-hoc, linker-signed) mep is identified by macOS by
# the hash of its exact bytes, so every rebuild is a "new program" to the
# Keychain and the mail client's saved password asks for permission again.
# Signed with one certificate, every build shares a stable identity
# (identifier "mep" + this certificate), and "Always Allow" sticks across
# rebuilds. The certificate only needs to exist -- it is never trusted
# system-wide, so this needs no admin rights.
#
# Run it in a terminal: it asks for your login password once, so that
# codesign may use the key without a dialog. Safe to re-run.
#
# Usage: scripts/macos-signing-setup.sh [identity name]
set -euo pipefail

name="${1:-mep Local Code Signing}"
keychain="$HOME/Library/Keychains/login.keychain-db"

if [ "$(uname)" != Darwin ]; then
    echo "macOS only" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if security find-certificate -c "$name" "$keychain" >/dev/null 2>&1; then
    echo "\"$name\" is already in the login keychain."
else
    cat >"$tmp/cert.cnf" <<CNF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $name
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
CNF
    # The system LibreSSL on purpose: OpenSSL 3's default PKCS#12 encryption is
    # one `security import` cannot read.
    /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$tmp/cert.cnf" \
        -keyout "$tmp/key.pem" -out "$tmp/cert.pem" 2>/dev/null
    pass="$(/usr/bin/openssl rand -hex 16)"
    /usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -name "$name" \
        -out "$tmp/id.p12" -passout "pass:$pass"
    # -T: codesign may use the private key; nothing else may.
    security import "$tmp/id.p12" -k "$keychain" -P "$pass" -T /usr/bin/codesign >/dev/null
    echo "Created \"$name\" in the login keychain."
fi

# Without Apple's tools in the key's partition list, codesign fails with
# errSecInternalComponent (or asks every time). This step asks for the
# login keychain's password -- normally your Mac login password.
echo "Letting codesign use the key (enter your login password):"
security set-key-partition-list -S apple-tool:,apple: -s -l "$name" "$keychain" >/dev/null

# Check: sign a throwaway copy of a small binary.
cp /usr/bin/true "$tmp/signtest"
if codesign -f -s "$name" -i mep-signtest "$tmp/signtest" 2>"$tmp/err"; then
    echo "Signing works. \`just install\` now signs mep with \"$name\"."
else
    echo "Signing failed: $(cat "$tmp/err")" >&2
    exit 1
fi
