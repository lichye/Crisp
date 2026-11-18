#!/usr/bin/env bash
# Build the single-file checker against the pinned EBMC fork, outside the checkout.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)
source "$root/src/components/pins.sh"
cache=${CRISP_ENGINE_CACHE:-/var/tmp/crisp-ebmc-$(id -u)}
jobs=${JOBS:-4}
mkdir -p -m 700 -- "$cache"
if [[ ! -O $cache || -L $cache ]]; then echo "Expected a private build cache at $cache" >&2; exit 2; fi
exec 9>"$cache/build.lock"
flock 9
engine="$cache/$ebmc_revision"
if [[ ! -d $engine ]]; then
    staging=$(mktemp -d -- "$cache/source.XXXXXXXX")
    trap 'rm -r -- "$staging"' EXIT
    curl -fsSL --retry 2 "https://codeload.github.com/lichye/ebmc/tar.gz/$ebmc_revision" |
        tar -xz -C "$staging" --strip-components=1
    curl -fsSL --retry 2 "https://codeload.github.com/diffblue/cbmc/tar.gz/$cbmc_revision" |
        tar -xz -C "$staging/lib/cbmc" --strip-components=1
    mv -- "$staging" "$engine"
    trap - EXIT
fi
if [[ ! -f $engine/lib/cbmc/minisat-2.2.1/minisat/core/Solver.h ]]; then
    make -C "$engine/lib/cbmc/src" minisat2-download
fi
make -C "$engine/src" -j"$jobs" ebmc.dir GIT_INFO="${ebmc_revision:0:12}"
driver="$engine/src/ebmc"
copy_changed() { if ! cmp -s -- "$1" "$2"; then cp -- "$1" "$2"; fi; }
copy_changed "$root/src/direct.cpp" "$driver/crisp_direct.cpp"
copy_changed "$root/components/veval/src/util/process/RunExternalCommand.cpp" "$driver/crisp_command.cpp"
notices="$cache/notices.txt"
{
    echo "EBMC from lichye/ebmc $ebmc_revision and CBMC $cbmc_revision"
    cat "$engine/LICENSE" "$engine/lib/cbmc/LICENSE" "$engine/lib/cbmc/minisat-2.2.1/LICENSE"
    cat "$engine/3p/copyright.aiger-1.9"
} > "$notices"
{ printf '%s\n' 'R"CrispLicense('; cat "$notices"; printf '%s\n' ')CrispLicense"'; } > "$cache/notices.inc"
copy_changed "$cache/notices.inc" "$driver/crisp_notices.inc"
make -C "$driver" -j"$jobs" -f Makefile -f "$root/src/components/ebmc/Crisp.mk" \
    CRISP_ROOT="$root" GIT_INFO="${ebmc_revision:0:12}" crisp
mkdir -p -- "$root/build"
new=$(mktemp -- "$root/build/.crisp.XXXXXXXX")
trap 'rm -f -- "$new"' EXIT
install -m 755 "$driver/crisp" "$new"
mv -fT -- "$new" "$root/build/crisp"
trap - EXIT
