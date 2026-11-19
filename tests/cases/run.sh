#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
work=$(mktemp -d /tmp/crisp-cases.XXXXXXXX)
trap 'rm -r -- "$work"' EXIT
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
run 3 token bounded
has 'token_formal.p_no_conflict  unknown'
has 'no counterexample through depth 10'

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

run 3 token prove induction
has 'token_formal.p_no_conflict  proved'
has 'token_formal.p_no_conflict  unknown'
has '2 tasks, 1 proved, 0 refuted, 1 unknown'

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
has '0 proved, 0 refuted, 0 unknown'
if grep -Fq 'undriven_value.p_nonzero' out; then
    cat out err >&2
    echo 'undriven error started verification' >&2
    exit 1
fi

run 2 undriven-value hold reject
has '2 tasks, 1 proved, 0 refuted, 0 unknown'
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
has '3 tasks, 3 proved, 0 refuted, 0 unknown'
[[ ! -s err ]]
echo 'EBMC cases passed (Specs 3 and 8).'
