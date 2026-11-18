#!/usr/bin/env bash
# Assemble the portable Linux checker and the optional component runtime.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
source "$root/src/components/pins.sh"
work=$(mktemp -d /tmp/crisp-package.XXXXXXXX)
trap 'rm -r -- "$work"' EXIT
export LC_ALL=C
veval="$root/components/veval/build"
smart="$root/components/smartVerilog/build"
runtime="$work/runtime"
bundle="$work/package"
mkdir -p "$bundle" "$runtime/src" "$runtime/runtime/"{bin,lib} \
    "$runtime/components/veval/build/crisp" "$runtime/components/smartVerilog/build"

# The C++ plugins must use the exact Yosys that supplied their build headers.
config=$(sed -n 's/^VEVAL_BUILD_YOSYS_CONFIG:INTERNAL=//p' "$veval/CMakeCache.txt")
[[ -x $config ]] || { echo 'Reconfigure veval with the OSS CAD Suite used for the release.' >&2; exit 2; }
suite=$(cd -- "$(dirname -- "$config")/.." && pwd -P)
deps=$(sed -n 's/^SMART_DEPS_ROOT:PATH=//p' "$smart/CMakeCache.txt")
spot=${SPOT_BIN:-$(command -v ltl2tgba || :)}
if [[ -z $spot && -x $HOME/.local/spot/bin/ltl2tgba ]]; then spot="$HOME/.local/spot/bin/ltl2tgba"; fi
[[ -x $spot ]] || { echo 'Set SPOT_BIN to the ltl2tgba executable to package.' >&2; exit 2; }
install -m 755 "$root/build/crisp" "$bundle/crisp"
strip --strip-unneeded "$bundle/crisp"
cat "$root/examples/token/rtl/token.sv" "$root/examples/token/spec.sv" > "$bundle/token.sv"
"$bundle/crisp" --licenses > "$bundle/THIRD-PARTY.txt"
install -m 755 "$root/src/cli.sh" "$runtime/src/"
install -m 755 "$veval/crisp/crisp_driver" "$runtime/components/veval/build/crisp/"
install -m 755 "$smart/smart" "$runtime/components/smartVerilog/build/"
install -m 755 "$deps/hw-cbmc/src/ebmc/ebmc" "$spot" "$runtime/runtime/bin/"
for file in "$veval"/{veval,veval_extract_subtree,consflow_*}; do
    [[ -f $file ]] && install -m 755 "$file" "$runtime/components/veval/build/"
done
# Native helpers use the host's Linux ABI. Supply their non-base shared libraries.
ldd "$veval/veval" "$spot" |
    awk '$2=="=>" && $3 ~ /^\// && $1 ~ /^(libsqlite3|libcrypto|libz\.|libspot|libbddx)/ {print $1, $3}' |
    sort -u | while read -r soname path; do
        cp -L -- "$path" "$runtime/runtime/lib/$soname"
    done
find "$runtime/components" "$runtime/runtime/lib" -type f -exec strip --strip-unneeded {} +
strip --strip-unneeded "$runtime/runtime/bin/"{ebmc,ltl2tgba}

dest="$runtime/runtime/suite"
mkdir -p "$dest/"{bin,libexec,lib,share/yosys}
for tool in yosys yosys-config yosys-smtbmc yosys-abc yosys-witness boolector iverilog vvp \
            verilator verilator_bin tabbypy3; do
    cp -L -- "$suite/bin/$tool" "$dest/bin/$tool"
    if [[ -f $suite/libexec/$tool ]]; then cp -L -- "$suite/libexec/$tool" "$dest/libexec/$tool"; fi
done
cp -L -- "$suite/libexec/"{ivl,ivlpp} "$dest/libexec/"
cp -a -- "$suite/lib/ivl" "$dest/lib/"
mkdir -p "$dest/share/verilator"
tar -C "$suite/share/verilator" --exclude='./examples' -cf - . | tar -C "$dest/share/verilator" -xf -
tar -C "$suite/share/yosys" --exclude='./include' --exclude='./plugins' --exclude='./python3' -cf - . |
    tar -C "$dest/share/yosys" -xf -
mkdir -p "$dest/share/yosys/python3"
cp -- "$suite/share/yosys/python3/"{smtio,ywio}.py "$dest/share/yosys/python3/"
for path in "$suite"/lib/python3.[0-9]*; do
    if [[ -d $path/site-packages && -x $suite/libexec/${path##*/} ]]; then pymod=${path##*/}; break; fi
done
: "${pymod:?Missing OSS CAD Suite Python runtime}"
mkdir -p "$dest/lib/$pymod/site-packages"
tar -C "$suite/lib/$pymod" --exclude='./site-packages' --exclude='__pycache__' --exclude='test' -cf - . |
    tar -C "$dest/lib/$pymod" -xf -
tar -C "$suite/lib/$pymod/site-packages" --exclude='__pycache__' -cf - click |
    tar -C "$dest/lib/$pymod/site-packages" -xf -
cp -L -- "$suite/libexec/$pymod" "$dest/libexec/"
ln -s tabbypy3 "$dest/bin/python3"
# The fork supplies the SBY entry point and its own modules. Run it with the suite interpreter.
mkdir "$work/sby"
if [[ -n ${SBY_SOURCE:-} ]] && git -C "$SBY_SOURCE" cat-file -e "$sby_revision^{commit}" 2>/dev/null; then
    git -C "$SBY_SOURCE" archive "$sby_revision" | tar -x -C "$work/sby"
else
    curl -fsSL --retry 2 "https://codeload.github.com/lichye/sby/tar.gz/$sby_revision" |
        tar -xz -C "$work/sby" --strip-components=1
fi
printf '%s\n' "${sby_revision:0:12}" > "$work/sby/.gittag"
make -s -C "$work/sby" install PREFIX="$work/sby-install"
cp -- "$work/sby-install/bin/sby" "$dest/libexec/sby"
cp -- "$work/sby-install/share/yosys/python3/"sby_*.py "$dest/share/yosys/python3/"
cat > "$dest/bin/sby" <<'SH'
#!/usr/bin/env bash
bin=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec "$bin/tabbypy3" "$bin/../libexec/sby" "$@"
SH
chmod +x "$dest/bin/sby"

# Copy the runtime closure selected by the suite loader, including libraries
# required by Python extension modules and by the Icarus VPI targets.
loader="$suite/lib/ld-linux-x86-64.so.2"
cp -L -- "$loader" "$dest/lib/"
while IFS= read -r -d '' file; do
    if ! readelf -h "$file" > /dev/null 2>&1; then continue; fi
    dependencies=$("$loader" --library-path "$suite/lib" --list "$file" 2>/dev/null) || continue
    while read -r name path; do
        name=${name##*/}
        [[ -f $path && ! -e $dest/lib/$name ]] || continue
        cp -L -- "$path" "$dest/lib/$name"
    done < <(awk '$2=="=>" && $3 ~ /^\// {print $1, $3}' <<< "$dependencies")
done < <(find "$dest/libexec" "$dest/lib/ivl" "$dest/lib/$pymod/lib-dynload" "$dest/bin" -type f -print0)

notices="$runtime/licenses"
mkdir -p "$notices/spot"
cp -a "$suite/license/." "$notices/"
cp "$work/sby/COPYING" "$notices/SBY-fork.txt"
cp "$root/components/PROVENANCE.md" "$notices/Crisp-components.txt"
cp "$root/components/veval/LICENSE" "$notices/veval.txt"
cp "$root/components/veval/src/frontend/hw_cbmc/LICENSE" "$notices/veval-hw-cbmc.txt"
cp "$root/components/veval/src/util/cbmc/LICENSE" "$notices/veval-cbmc.txt"
cp "$deps/hw-cbmc/LICENSE" "$notices/SMART-hw-cbmc.txt"
cp "$deps/hw-cbmc/lib/cbmc/LICENSE" "$notices/SMART-cbmc.txt"
cp "$deps/hw-cbmc/lib/cbmc/minisat-2.2.1/LICENSE" "$notices/SMART-MiniSAT.txt"
for license in "$suite/lib/$pymod/site-packages"/click-*.dist-info/{licenses/,}LICENSE*; do
    if [[ -f $license ]]; then cp "$license" "$notices/Click.txt"; break; fi
done
spot_version=$("$spot" --version | awk 'NR==1 {print $NF}')
spot_source="https://www.lre.epita.fr/dload/spot/spot-$spot_version.tar.gz"
curl -fsSL --retry 2 "$spot_source" | tar -xz -C "$notices/spot" --strip-components=1 \
    "spot-$spot_version/"{AUTHORS,COPYING,debian/copyright,buddy/README}
cvc5_prefix=$(sed -n 's/^CVC5_ROOT:PATH=//p' "$smart/CMakeCache.txt")
cvc5_version=$(sed -n 's/^set(PACKAGE_VERSION "\([0-9][^"]*\)")/\1/p' \
    "$cvc5_prefix"/lib*/cmake/cvc5/cvc5ConfigVersion.cmake)
if [[ -f $cvc5_prefix/../cvc5-src/COPYING ]]; then
    cp "$cvc5_prefix/../cvc5-src/COPYING" "$notices/SMART-cvc5.txt"
else
    curl -fsSL --retry 2 "https://raw.githubusercontent.com/cvc5/cvc5/cvc5-$cvc5_version/COPYING" \
        -o "$notices/SMART-cvc5.txt"
fi
for license in /usr/share/licenses/openssl-libs/LICENSE.txt /usr/share/doc/libssl3{,t64}/copyright; do
    if [[ -f $license ]]; then cp "$license" "$notices/OpenSSL.txt"; break; fi
done
for license in /usr/share/licenses/zlib/README /usr/share/doc/zlib1g/copyright; do
    if [[ -f $license ]]; then cp "$license" "$notices/zlib.txt"; break; fi
done
cat > "$runtime/versions.txt" <<VERSIONS
Crisp source revision: $(git -C "$root" rev-parse HEAD)
Single-file EBMC fork: lichye/ebmc $ebmc_revision
CBMC submodule: diffblue/cbmc $cbmc_revision
SBY fork: lichye/sby $sby_revision
OSS CAD Suite: $(cat "$suite/VERSION")
Yosys: $("$suite/bin/yosys" -V)
Boolector: $("$suite/bin/boolector" --version)
Spot: $spot_version ($spot_source)
cvc5: $cvc5_version (https://github.com/cvc5/cvc5/tree/cvc5-$cvc5_version)
VERSIONS
tar -C "$runtime" -cf - . | gzip -1 > "$bundle/components.tar.gz"
cp -- "$runtime/versions.txt" "$bundle/VERSIONS.txt"
destination="$root/release"
mkdir -p "$destination"
# Only complete release files are published. The staging directory contains no
# expanded toolchain and stays on the same filesystem as the final artifacts.
incoming=$(mktemp -d "$destination/.publish.XXXXXXXX")
trap 'rm -r -- "$work"; if [[ -d $incoming ]]; then rm -r -- "$incoming"; fi' EXIT
cp -- "$bundle/"* "$incoming/"
tar -C "$bundle" -czf "$incoming/crisp-linux-x86_64.tar.gz" .
for file in "$incoming/"*; do mv -fT -- "$file" "$destination/${file##*/}"; done
rmdir -- "$incoming"
echo "$destination/crisp"
