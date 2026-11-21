#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
tmp_base=${TMPDIR:-$root/build}
mkdir -p -- "$tmp_base"
work=$(mktemp -d "$tmp_base/crisp-cases.XXXXXXXX")
trap 'rm -r -- "$work"' EXIT
mkdir -p "$work/tmp"
export TMPDIR="$work/tmp"
cd -- "$work"

run() {
    local expected=$1 design=$2 status=0
    shift 2
    timeout 11 "$root/build/crisp" "$root/tests/cases/$design.salt" "$@" >out 2>err || status=$?
    if [[ $status != "$expected" ]]; then
        cat out err >&2
        echo "$design $*: expected exit $expected, got $status" >&2
        exit 1
    fi
}

has() {
    if ! grep -Fq -- "$1" "${2:-out}"; then
        cat out err >&2
        echo "missing: $1" >&2
        exit 1
    fi
}

valid_vcd() {
    awk '
        $1 == "$scope" {depth++}
        $1 == "$upscope" {if (--depth < 0) invalid=1}
        $1 == "$var" {ids[$4]=1}
        $1 == "$enddefinitions" && $2 == "$end" && depth == 0 {header=1}
        /^#0$/ && header {started=1}
        started && /^[01xz]/ && (substr($1,2) in ids) {values++}
        started && /^b[01xz]+ / && ($2 in ids) {values++}
        END {exit !(header && started && values && !invalid)}
    ' "$1"
}

run 0 token prove
has 'token_formal.p_no_conflict  proved'
run 3 token induction
has 'token_formal.p_no_conflict  unknown'
has 'k-induction inconclusive at depth 5'
run 0 token bounded
has 'token_formal.p_no_conflict  passed'
has 'bmc depth 10'
has '1 task, 0 proved, 1 passed, 0 refuted, 0 unknown'

wave=crisp-out/token_bug/token_formal.p_no_conflict.vcd
mkdir -p "$(dirname "$wave")"
echo stale > "$wave"
echo keep > crisp-out/token_bug/unrelated
run 1 token bug
has 'token_formal.p_no_conflict  refuted'
has "$wave"
valid_vcd "$wave"
[[ $(cat crisp-out/token_bug/unrelated) == keep ]]

run 0 counter correct
has 'counter.p_range  proved'
run 1 counter bug
has 'counter.p_range  refuted'
valid_vcd crisp-out/counter_bug/counter.p_range.vcd
run 1 counter ic3
has 'counter.p_range  refuted'
has 'counter.p_stop  proved'
valid_vcd crisp-out/counter_ic3/counter.p_range.vcd

run 0 reset with_reset
has 'reset_counter.p_range  proved'
run 1 reset without_reset
has 'reset_counter.p_range  refuted'
valid_vcd crisp-out/reset_without_reset/reset_counter.p_range.vcd

for mode in bmc kind pdr; do
    run 1 reset-low "$mode"
    has 'top.p  refuted'
    wave="crisp-out/reset-low_$mode/top.p.vcd"
    valid_vcd "$wave"
    grep -Fxq '#2' "$wave"
    if grep -Fxq '#3' "$wave"; then
        echo "reset-low $mode: expected the first failure at cycle 2" >&2
        cat "$wave" >&2
        exit 1
    fi
done
run 0 reset-high bmc
has 'top.p  passed'
for mode in kind pdr; do
    run 0 reset-high "$mode"
    has 'top.p  proved'
done

invalid="$root/tests/cases/reset-invalid.salt"
rejected_reset() {
    local task=$1 line=$2 message=$3
    run 2 reset-invalid "$task"
    has "$invalid:$line: $message" err
    [[ $(wc -l < err) == 1 ]]
    has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
}
rejected_reset register 15 "reset 'busy' is a register, not an input of top"
rejected_reset wire 18 "reset 'internal' is an internal signal, not an input of top"
rejected_reset output 20 "reset 'outgoing' is an output port, not an input of top"
rejected_reset inout 22 "reset 'bidir' is an inout port, not an input of top"
rejected_reset wide 24 "reset 'wide' is a 2-bit input of top; expected a 1-bit input"
rejected_reset missing 26 "reset 'absent' does not exist in top"
rejected_reset missing_default 27 "reset 'absent' does not exist in any top-level module"
run 2 reset-invalid register valid
has "$invalid:15: reset 'busy' is a register" err
has '2 tasks, 0 proved, 0 passed, 1 refuted, 0 unknown'

# An explicit SMART run also gets the source-located error before it can
# consume the invalid reset, even if no SMART executable is installed.
printf '[files]\n%s\n[design]\nreset busy low\n[options]\nsmart on\n' "$root/tests/cases/reset-invalid.sv" > smart-reset.salt
status=0
CRISP_SMART="$work/missing-smart" "$root/build/crisp" smart-reset.salt >out 2>err || status=$?
[[ $status == 2 && $(wc -l < err) == 1 ]]
has "smart-reset.salt:4: reset 'busy' is a register" err
has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'

for name in 'u_core.rst' 'rst==0'; do
    printf '[files]\n%s\n[design]\nreset %s low\n' "$root/tests/cases/reset-high.sv" "$name" > invalid-name.salt
    status=0
    "$root/build/crisp" invalid-name.salt >out 2>err || status=$?
    [[ $status == 2 && $(wc -l < err) == 1 && ! -s out ]]
    has 'invalid-name.salt:4:' err
    has "invalid name '$name'" err
done

run 3 token prove induction
has 'token_formal.p_no_conflict  proved'
has 'token_formal.p_no_conflict  unknown'
has '2 tasks, 1 proved, 0 passed, 0 refuted, 1 unknown'
run 0 token prove bounded
has 'token_formal.p_no_conflict  proved'
has 'token_formal.p_no_conflict  passed'
has '2 tasks, 1 proved, 1 passed, 0 refuted, 0 unknown'

run 3 empty
has 'no assertions'
run 1 constant
has 'empty.p_false  refuted'
valid_vcd crisp-out/constant/empty.p_false.vcd
has '1!' crisp-out/constant/empty.p_false.vcd
run 2 syntax
has 'syntax.sv' err
grep -Eq '(line |:)2([^0-9]|$)' err
run 3 timeout
has 'multiply.p_commutes  unknown'
has 'timeout after 1 s'
run 2 param
has 'param WIDTH is not supported' err
run 0 bmc-passed bmc
has 'bmc_passed.p_below_limit  passed'
has 'bmc depth 10'
has '1 task, 0 proved, 1 passed, 0 refuted, 0 unknown'
run 3 bmc-passed kind
has 'bmc_passed.p_below_limit  unknown'
has 'k-induction inconclusive at depth 5'
has '1 task, 0 proved, 0 passed, 0 refuted, 1 unknown'
run 3 bmc-passed
has '2 tasks, 0 proved, 1 passed, 0 refuted, 1 unknown'

for task in hold sequence; do
    run 0 undriven-value "$task"
    has 'undriven_value.p_nonzero  proved'
    has 'crisp: warning: undriven undriven_value.en at ' err
    has 'undriven-value.sv:2' err
done
run 2 undriven-value reject
has "crisp: error: undriven undriven_value.en at $root/tests/cases/undriven-value.sv:2" err
has "crisp: error: undriven undriven_value.bus[3] at $root/tests/cases/undriven-value.sv:3" err
[[ $(grep -c '^crisp: error: undriven ' err) == 2 ]]
if grep -Fq 'crisp: warning:' err; then
    cat out err >&2
    echo 'undriven error emitted a warning' >&2
    exit 1
fi
has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
if grep -Fq 'undriven_value.p_nonzero' out; then
    cat out err >&2
    echo 'undriven error started verification' >&2
    exit 1
fi

run 2 undriven-value hold reject
has '2 tasks, 1 proved, 0 passed, 0 refuted, 0 unknown'
has 'crisp: warning: [hold] undriven undriven_value.en at ' err
has 'crisp: error: [reject] undriven undriven_value.en at ' err
[[ $(grep -c '^crisp: warning: \[hold\] undriven ' err) == 2 ]]
[[ $(grep -c '^crisp: error: \[reject\] undriven ' err) == 2 ]]

run 0 undriven-temporal hold
has 'undriven_temporal.p_stable  proved'
has 'crisp: warning: undriven undriven_temporal.en at ' err
run 1 undriven-temporal default
has 'undriven seq'
has 'undriven_temporal.p_stable  refuted'
has 'crisp: warning: undriven undriven_temporal.en at ' err

run 0 undriven-clean
has '3 tasks, 3 proved, 0 passed, 0 refuted, 0 unknown'
[[ ! -s err ]]

# Spec 20: properties PATTERN selects the assertions that are checked.
reported() {
    local status=$1 task=$2 verdict=$3
    shift 3
    run "$status" properties "$task"
    for name in "$@"; do has "  $name  $verdict"; done
    if [[ $(grep -c '^  top\.' out) != $# ]]; then
        cat out err >&2
        echo "properties $task: expected exactly $# reported assertions" >&2
        exit 1
    fi
}
selected() {
    local task=$1
    shift
    reported 0 "$task" passed "$@"
    has "1 task, 0 proved, $# passed, 0 refuted, 0 unknown"
}
selected one top.p_one
has '  properties  top.p_one'
selected union top.p_one top.p_two
has '  properties  top.p_one  top.p_two'
selected top_only top.p_one top.p_two top.p_still
selected deep top.u_leaf.q_leaf top.p_one top.p_two top.p_still 'top.g[0].p_gen' 'top.g[1].p_gen'
selected sub top.u_leaf.q_leaf
selected generate 'top.g[0].p_gen'
selected single top.p_one
# p_still holds only under the assumption, which stays in effect.
selected assumed top.p_still
sed '/en_low: assume/d' "$root/tests/cases/properties.sv" > unassumed.sv
printf '[files]\n%s\n[design]\ntop top\n[options]\nmode bmc\nproperties top.p_still\n' "$work/unassumed.sv" > unassumed.salt
status=0
timeout 11 "$root/build/crisp" unassumed.salt >out 2>err || status=$?
[[ $status == 1 ]]
has 'top.p_still  refuted'
reported 0 pdr proved top.p_one top.p_two top.p_still
has '1 task, 3 proved, 0 passed, 0 refuted, 0 unknown'

properties="$root/tests/cases/properties.salt"
for unmatched in 'nomatch 32 top.absent*' 'dot 33 top?p_one'; do
    read -r task line pattern <<< "$unmatched"
    reported 2 "$task" -
    has "$properties:$line: [options] properties: pattern '$pattern' matches no assertion" err
    [[ $(wc -l < err) == 1 ]]
    has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
done

# The patterns for all tasks and for one task merge in file order.
printf '[tasks]\nwith\nwithout\n[files]\n%s\n[design]\ntop top\n[options]\nmode bmc\nwith: properties top.p_two\nproperties top.p_one\n' \
    "$root/tests/cases/properties.sv" > merged.salt
status=0
timeout 11 "$root/build/crisp" merged.salt >out 2>err || status=$?
[[ $status == 0 ]]
has '  properties  top.p_two  top.p_one'
has '2 tasks, 0 proved, 3 passed, 0 refuted, 0 unknown'
echo 'EBMC cases passed (Specs 3, 8, 11, 18, and 20).'
