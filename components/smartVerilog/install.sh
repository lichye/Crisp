#!/usr/bin/env bash
# One-off local setup for SMART. No container, no root: everything lands under
# .deps/ and otherTools/, both of which are gitignored.
#
#   ./install.sh          # fetch + build everything, then verify
#   ./install.sh --deps-only  # install prerequisites for the root Makefile
#   ./install.sh --check  # just report what is missing
#
# What gets installed:
#   .deps/hw-cbmc         EBMC (verification) + the Verilog frontend we link
#   .deps/include         header-only build dependencies
#   .deps/oss-cad-suite   selected Yosys, solvers and simulators
#   otherTools/cvc5       libcvc5 (SyGuS + MSA/MUS), headers and static libs
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

OSS_CAD_SUITE_DATE="${OSS_CAD_SUITE_DATE:-2026-09-01}"
HWCBMC_REPO="https://github.com/diffblue/hw-cbmc.git"
HWCBMC_REV="9b402aa7cfb1157b0399f9e684bdfc1b505ed50d"
NLOHMANN_JSON_VERSION="3.11.3"
DEPS_DIR="$here/.deps"
HWCBMC_DIR="$DEPS_DIR/hw-cbmc"
jobs="${JOBS:-$(nproc)}"
suite_bin="${OSS_CAD_BIN:-$DEPS_DIR/oss-cad-suite/bin}"

mode="${1:-}"
case "$mode" in
    ''|--check|--deps-only) ;;
    *) echo "Usage: $0 [--check|--deps-only]" >&2; exit 2 ;;
esac

missing=0
need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[MISSING] $1 — $2"
        missing=1
    else
        echo "[ok]      $1"
    fi
}

echo "== host toolchain =="
need g++ "a C++17 compiler"
need make "GNU make"
need flex "flex"
need bison "bison"
need git "git"
need patch "patch (used by the pinned CBMC build)"
need curl "curl (cvc5 and CBMC download dependencies)"
need cmake "cmake >= 3.16"
need python3 "python3 (cvc5's build scripts)"

# cvc5's code generators use local Python modules fetched by tools/build-cvc5.sh.
if command -v python3 >/dev/null 2>&1; then
    if python3 -m pip --version >/dev/null 2>&1; then
        echo "[ok]      python3-pip"
    else
        echo "[MISSING] python3-pip — needed to fetch cvc5's build-time modules"
        echo "          Debian/Ubuntu: apt-get install python3-pip"
        missing=1
    fi
fi

if [ "$missing" = 1 ] && [ "$mode" != "--check" ]; then
    echo
    echo "Install the missing tools above first; everything else is local."
    exit 2
fi

if [ "$mode" = "--check" ]; then
    for d in .deps/hw-cbmc/src/ebmc/ebmc \
             "$suite_bin/verilator" "$suite_bin/yosys-config"; do
        [ -x "$d" ] && echo "[ok]      $d" || { echo "[MISSING] $d"; missing=1; }
    done
    d=otherTools/cvc5/include/cvc5/cvc5.h
    [ -e "$d" ] && echo "[ok]      $d" || { echo "[MISSING] $d"; missing=1; }
    d=.deps/include/nlohmann/json.hpp
    [ -e "$d" ] && echo "[ok]      $d" || { echo "[MISSING] $d"; missing=1; }
    for d in "$suite_bin/iverilog" "$suite_bin/vvp"; do
        if [ -x "$d" ]; then
            echo "[ok]      $d (optional Icarus backend)"
        else
            echo "[optional] $d — only needed for --simulator iverilog"
        fi
    done
    exit $missing
fi

echo
echo "== hw-cbmc (EBMC + Verilog frontend) =="
mkdir -p "$DEPS_DIR"
if [ ! -e "$HWCBMC_DIR/.git" ]; then
    git init "$HWCBMC_DIR"
    git -C "$HWCBMC_DIR" remote add origin "$HWCBMC_REPO"
fi
if ! git -C "$HWCBMC_DIR" cat-file -e "${HWCBMC_REV}^{commit}" 2>/dev/null; then
    git -C "$HWCBMC_DIR" fetch --depth 1 origin "$HWCBMC_REV"
fi
git -C "$HWCBMC_DIR" checkout --detach "$HWCBMC_REV"
git -C "$HWCBMC_DIR" submodule sync --recursive
git -C "$HWCBMC_DIR" submodule update --init --recursive --depth 1
./tools/build-hw-cbmc.sh "$jobs"

echo
echo "== header dependencies =="
json_header="$DEPS_DIR/include/nlohmann/json.hpp"
if [ ! -e "$json_header" ]; then
    mkdir -p "$(dirname "$json_header")"
    curl --fail --location --retry 3 \
        --output "$json_header.part" \
        "https://github.com/nlohmann/json/releases/download/v${NLOHMANN_JSON_VERSION}/json.hpp"
    mv "$json_header.part" "$json_header"
else
    echo "already present: $json_header"
fi

echo
echo "== OSS CAD Suite (Yosys, proof tools and simulators) =="
if [ -z "${OSS_CAD_BIN:-}" ]; then
    stamp="${OSS_CAD_SUITE_DATE//-/}"
    package="$DEPS_DIR/oss-cad-suite-$stamp"
    suite_bin="$package/bin"
    if [ ! -d "$package" ]; then
        (
            staging=$(mktemp -d "$DEPS_DIR/.cad-download.XXXXXX")
            trap 'rm -rf -- "$staging"' EXIT
            tarball="oss-cad-suite-linux-x64-$stamp.tgz"
            curl --fail --location --retry 3 --output "$staging/$tarball" \
                "https://github.com/YosysHQ/oss-cad-suite-build/releases/download/$OSS_CAD_SUITE_DATE/$tarball"
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
    ln -sfnT "oss-cad-suite-$stamp" "$DEPS_DIR/oss-cad-suite"
fi
echo "using $suite_bin"

echo
echo "== libcvc5 (SyGuS + MSA/MUS) =="
./tools/build-cvc5.sh "$jobs"

if [ "$mode" = "--deps-only" ]; then
    echo "Dependencies ready. Run make build from the Crisp repository root."
    exit 0
fi

echo
echo "== build smart =="
cmake -B build -S .
cmake --build build --target smart -j"$jobs"

echo
echo "== verify =="
# For an external suite, make the selected simulators visible to the self-check.
PATH="$PATH:$suite_bin" ./build/smart --check-env
echo
echo "Done. Run ./build/smart --help for mining options."
