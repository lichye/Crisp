#!/usr/bin/env bash
# Install SMART's headers and pinned cvc5 package. Simulators are installed
# separately or supplied by an existing OSS CAD Suite.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
root=$(cd -- "$here/../../.." && pwd -P)
deps_dir="${SMART_DEPS_ROOT:-$here/.deps}"
cvc5_default="$root/src/components/cvc5/packages/1.4.2"
cvc5_dir="${CVC5_ROOT:-$cvc5_default}"
suite_date="${OSS_CAD_SUITE_DATE:-2026-09-01}"
suite_bin="${OSS_CAD_BIN:-$deps_dir/oss-cad-suite/bin}"
json_header="$deps_dir/include/nlohmann/json.hpp"
jobs="${JOBS:-$(nproc)}"

mode="${1:-}"
case "$mode" in
    ''|--check|--build-deps|--deps-only) ;;
    *) echo "Usage: $0 [--check|--build-deps|--deps-only]" >&2; exit 2 ;;
esac

if [[ "$mode" == --check ]]; then
    status=0
    for file in "$root/src/components/hw-cbmc/src/ebmc/ebmc" "$json_header" \
                "$cvc5_dir/bin/cvc5" "$suite_bin/verilator"; do
        if [[ -f "$file" ]]; then echo "[ok]      $file"
        else echo "[MISSING] $file"; status=1; fi
    done
    exit "$status"
fi

# make deps already ran build-deps; this check also supports direct invocation.
if [[ ! -f "$json_header" ]]; then
    mkdir -p -- "$(dirname -- "$json_header")"
    (
        part=$(mktemp "$json_header.XXXXXXXX")
        trap 'rm -- "$part"' EXIT
        curl --fail --location --retry 3 --output "$part" \
            https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
        mv -f -- "$part" "$json_header"
        trap - EXIT
    )
fi

if [[ "$cvc5_dir" == "$cvc5_default" ]]; then
    "$here/../cvc5/install.sh"
elif [[ ! -x "$cvc5_dir/bin/cvc5" ]]; then
    echo "cvc5 executable missing under $cvc5_dir" >&2
    exit 2
fi
if [[ "$mode" == --build-deps ]]; then
    exit 0
fi

echo "== OSS CAD Suite (Yosys, proof tools and simulators) =="
if [ -z "${OSS_CAD_BIN:-}" ]; then
    stamp="${suite_date//-/}"
    package="$deps_dir/oss-cad-suite-$stamp"
    suite_bin="$package/bin"
    if [ ! -d "$package" ]; then
        (
            staging=$(mktemp -d "$deps_dir/.cad-download.XXXXXX")
            trap 'rm -rf -- "$staging"' EXIT
            tarball="oss-cad-suite-linux-x64-$stamp.tgz"
            curl --fail --location --retry 3 --output "$staging/$tarball" \
                "https://github.com/YosysHQ/oss-cad-suite-build/releases/download/$suite_date/$tarball"
            tar -xzf "$staging/$tarball" -C "$staging"
            mv -nT "$staging/oss-cad-suite" "$package"
        )
    fi
fi
if [ ! -x "$suite_bin/verilator" ] || ! datdir=$("$suite_bin/yosys-config" --datdir) || [ ! -f "$datdir/include/kernel/yosys.h" ]; then
    echo "Incomplete OSS CAD Suite at $suite_bin. Expected verilator, yosys-config and the matching Yosys development headers." >&2
    exit 2
fi
if [ -z "${OSS_CAD_BIN:-}" ]; then
    ln -sfnT "oss-cad-suite-$stamp" "$deps_dir/oss-cad-suite"
fi
echo "using $suite_bin"

if [[ "$mode" == --deps-only ]]; then
    exit 0
fi

make -C "$root" smart JOBS="$jobs" SMART_DEPS_ROOT="$deps_dir" CVC5_ROOT="$cvc5_dir"
PATH="$PATH:$suite_bin" "$here/build/smart" --check-env
