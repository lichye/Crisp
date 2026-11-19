#!/usr/bin/env bash
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
prefix="$here/packages/1.4.2"
if [[ -x "$prefix/bin/cvc5" && -f "$prefix/lib/libmpfr.a" ]]; then
    exit 0
fi
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo 'Set CVC5_ROOT to an installed cvc5 1.4.2 static package for this platform.' >&2
    exit 1
fi

mkdir -p "$here/packages"
staging=$(mktemp -d "$here/packages/.download.XXXXXXXX")
trap 'rm -r -- "$staging"' EXIT
asset=cvc5-Linux-x86_64-static
archive="$staging/$asset.zip"
curl --fail --location --retry 3 -o "$archive" \
    "https://github.com/cvc5/cvc5/releases/download/cvc5-1.4.2/$asset.zip"
printf '%s  %s\n' 7eb18f8c814c36a46c5a7d003983e9cfbc4b0a9828d6dd596239291bd4cc61d8 "$archive" |
    sha256sum --check --status
unzip -q "$archive" -d "$staging"
mv -nT -- "$staging/$asset" "$prefix"
test -x "$prefix/bin/cvc5" && test -f "$prefix/lib/libmpfr.a"
