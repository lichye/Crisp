#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
# The CLI checks run tasks in the default rtl-mode strict, which needs slang (spec 23).
slang=${CRISP_SLANG:-$root/src/components/slang/build/bin/slang}
if [[ ! -x $slang ]]; then
    echo "tests/front/run.sh: slang not found at $slang; run 'make slang' or set CRISP_SLANG" >&2
    exit 1
fi
if [[ $slang != /* ]]; then slang="$PWD/$slang"; fi
tmp_base=${TMPDIR:-$root/build}
mkdir -p -- "$tmp_base"
scratch=$(mktemp -d "$tmp_base/crisp-front.XXXXXXXX")
trap 'rm -r -- "$scratch"' EXIT
mkdir -p "$scratch"/{work,capture,cases/rtl,cases/props,tmp}
export TMPDIR="$scratch/tmp"
cp -- "$root/tests/front/fifo.salt" "$scratch/cases/"
sed 's/$/\r/' "$root/tests/front/fifo.salt" > "$scratch/cases/fifo-crlf.salt"
touch "$scratch/cases/rtl/fifo.sv" "$scratch/cases/props/fifo_props.sv" "$scratch/cases/exists.txt"
cd -- "$scratch/work"
capture="$scratch/capture"

cpus=
if [[ -r /proc/self/status ]]; then
    cpus=$(awk '/^Cpus_allowed_list:/ {
        n = split($2, ranges, ",")
        for (i = 1; i <= n; i++) {
            count = split(ranges[i], endpoints, "-")
            total += count == 2 ? endpoints[2] - endpoints[1] + 1 : 1
        }
        print total
    }' /proc/self/status)
fi
"$root/build/tests/test-front" ../cases "$capture/errors" "$cpus"
# The parser suite above covers explicit SMART selection. The standalone CLI
# checks below exercise EBMC without requiring the optional mining tools.
sed -i 's/^smart on/smart off/' "$scratch/cases/"fifo*.salt
cat > "$scratch/cases/rtl/fifo.sv" <<'SV'
module fifo(input wire clk, input wire rst_n);
    p_front: assert property (@(posedge clk) 1'b1);
endmodule
SV

# Successful task runs now produce a progress TSV in their output directories.
mkdir -p "$scratch/work/crisp-out"/{fifo,fifo-crlf}_{task1,task2}
snapshot() {
    (cd -- "$scratch" && find work cases "$@" -print0 | LC_ALL=C sort -z)
}

cli() {
    local expected=$1 status=0
    shift
    local -a filter=()
    if [[ $expected == 0 && ${1:-} == ../cases/*.salt ]]; then
        filter=(! -path 'work/crisp-out/*/progress-??????.tsv')
    fi
    snapshot "${filter[@]}" > "$capture/before"
    "$root/build/crisp" "$@" </dev/null > "$capture/out" 2> "$capture/err" || status=$?
    snapshot "${filter[@]}" > "$capture/after"
    cmp "$capture/before" "$capture/after"
    if [[ $status != "$expected" ]]; then
        cat "$capture/out" "$capture/err" >&2
        echo "crisp $*: expected exit $expected, got $status" >&2
        exit 1
    fi
    if [[ $expected == 0 ]]; then
        [[ ! -s $capture/err ]]
    else
        [[ ! -s $capture/out && $(wc -l < "$capture/err") == 1 ]]
        grep -q '^crisp: ' "$capture/err"
    fi
}

for file in fifo.salt fifo-crlf.salt; do
    cli 0 "../cases/$file"
    grep -Fxq task1 "$capture/out"
    grep -Fxq task2 "$capture/out"
    [[ $(grep -c 'fifo.p_front  proved' "$capture/out") == 2 ]]
    [[ $(tail -n 1 "$capture/out") == "2 tasks, 2 proved, 0 passed, 0 refuted, 0 unknown; vacuity: 0 triggered, 0 not triggered, 0 never triggered, 2 unchecked" ]]
done
cli 0 ../cases/fifo.salt task2
grep -Fxq task2 "$capture/out"
if grep -Fq task1 "$capture/out"; then exit 1; fi
cli 2 ../cases/fifo.salt task3
grep -Fq task1 "$capture/err"
grep -Fq task2 "$capture/err"

while IFS=$'\t' read -r file line mention; do
    cli 2 "$file"
    grep -Fq -- "crisp: $file:$line:" "$capture/err"
    grep -Fq -- "$mention" "$capture/err"
done < "$capture/errors"

cli 2 ../cases/missing-sources.salt task2
grep -Fq 'no source files' "$capture/err"
grep -Fq task2 "$capture/err"
if grep -Eq ':[0-9]+:' "$capture/err"; then exit 1; fi

cli 2
cli 2 fifo.sv
cli 2 --foo
cli 2 absent.salt
cli 0 --help
grep -Fq 'FILE.salt [TASK...]' "$capture/out"
cli 0 --version
[[ $(cat "$capture/out") == 'crisp 0.1.0' ]]

# Install a copy of Crisp with a controlled source tree and sibling directory.
# Distinct diagnostics identify which executable was actually invoked.
layout="$scratch/lookup"
mkdir -p "$layout/build" overrides
cp -- "$root/build/crisp" "$layout/build/crisp"
sed 's/^smart off/smart on/' ../cases/fifo.salt > ../cases/mining.salt

stub() {
    mkdir -p -- "$(dirname -- "$1")"
    printf '#!/bin/sh\ncase "$0" in /*) ;; *) exit 1 ;; esac\necho "error: selected %s"\nexit 1\n' "$2" > "$1"
    chmod +x "$1"
}

selected() {
    local expected=$1 status=0
    shift
    env -u CRISP_SLANG -u CRISP_SMART "$@" "$layout/build/crisp" "$salt" "$task" \
        > "$capture/out" 2> "$capture/err" || status=$?
    if [[ $status != 2 ]] || ! grep -Fxq -- "error: selected $expected" "$capture/out" "$capture/err"; then
        cat "$capture/out" "$capture/err" >&2
        echo "executable lookup did not reach $expected (exit $status)" >&2
        exit 1
    fi
}

for tool in slang smart; do
    settings=()
    salt=../cases/fifo.salt
    task=task2
    fallback=src/components/slang/build/bin/slang
    variable=CRISP_SLANG
    if [[ $tool == smart ]]; then
        settings=("CRISP_SLANG=$slang")
        salt=../cases/mining.salt
        task=task1
        fallback=src/components/smartVerilog/build/smart
        variable=CRISP_SMART
    fi
    stub "overrides/$tool" "$tool override"
    stub "$layout/build/$tool" "$tool sibling"
    stub "$layout/$fallback" "$tool source tree"
    selected "$tool override" "${settings[@]}" "$variable=overrides/$tool"
    selected "$tool sibling" "${settings[@]}"
    mv -- "$layout/build/$tool" "$layout/$tool"
    selected "$tool source tree" "${settings[@]}"
    mkdir -- "$layout/build/$tool"
    selected "$tool source tree" "${settings[@]}"
done
echo 'Front CLI tests passed (spec 9.1–9.13).'
