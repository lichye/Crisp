#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
# level word runs EBMC with yices (spec 19); the tests need it.
if ! command -v yices-smt2 >/dev/null; then
    echo 'tests/cases/run.sh: yices-smt2 not found in PATH; install yices or add its directory to PATH' >&2
    exit 1
fi
# rtl-mode strict, the default, runs slang before EBMC (spec 23); the tests need it.
slang=${CRISP_SLANG:-$root/src/components/slang/build/bin/slang}
if [[ ! -x $slang ]]; then
    echo "tests/cases/run.sh: slang not found at $slang; run 'make slang' or set CRISP_SLANG" >&2
    exit 1
fi
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

# Two waveforms are the same apart from the $date block, which EBMC fills with the
# current time to the second.
same_vcd() {
    if ! cmp -s <(sed '/^\$date/,/^\$end/d' "$1") <(sed '/^\$date/,/^\$end/d' "$2"); then
        echo "waveforms differ: $1 $2" >&2
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

for mode in bmc kind pdr bmc_word kind_word; do
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
# level word (spec 19) agrees with the bit level, including the waveform.
for mode in bmc kind; do
    same_vcd "crisp-out/reset-low_$mode/top.p.vcd" "crisp-out/reset-low_${mode}_word/top.p.vcd"
done
for mode in bmc bmc_word; do
    run 0 reset-high "$mode"
    has 'top.p  passed'
done
for mode in kind pdr kind_word; do
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

# Spec 19: level word uses EBMC's word-level SMT with yices.
marker=$(mktemp "$work/marker.XXXXXXXX")
mkdir -p empty-tmp
for task in ram_word ram_auto; do
    TMPDIR="$work/empty-tmp" run 1 level-word "$task"
    has 'ram_word.p_written  passed'
    has 'ram_word.p_untouched  refuted'
    valid_vcd "crisp-out/level-word_$task/ram_word.p_untouched.vcd"
    # The memory element 5 is written in cycle 5 and read as nonzero in cycle 6.
    [[ $(grep '^#' "crisp-out/level-word_$task/ram_word.p_untouched.vcd" | tail -n 1) == '#6' ]]
done
same_vcd crisp-out/level-word_ram_word/ram_word.p_untouched.vcd crisp-out/level-word_ram_auto/ram_word.p_untouched.vcd
# The SMT formulas go to the task's scratch directory, not to TMPDIR or /tmp.
[[ -z $(ls -A empty-tmp) ]]
if [[ -n $(find /tmp -maxdepth 1 -newer "$marker" -name 'smt2_dec_*' 2>/dev/null) ]]; then
    echo 'level word left SMT files in /tmp' >&2
    exit 1
fi
run 1 level-word counter_bmc
has 'level word'
has 'counter.p_range  refuted'
has 'counter.p_stop  passed'
[[ $(grep '^#' crisp-out/level-word_counter_bmc/counter.p_range.vcd | tail -n 1) == '#10' ]]
run 1 level-word counter_kind
has 'counter.p_range  refuted'
has 'counter.p_stop  proved'

# A timeout ends yices with the task.
run 3 level-word factor
has 'factor.p_no_factors  unknown'
has 'timeout after 2 s'
# oss-cad-suite starts yices through its loader, so match the command line.
for pid in $(pgrep -f yices-smt2 || true); do
    if [[ $(readlink "/proc/$pid/cwd" 2>/dev/null) == "$work"* ]]; then
        echo "yices-smt2 $pid left after the timeout" >&2
        exit 1
    fi
done

# Designs that Yices 2.7 rejects (spec 19, 3.6): EBMC reports FAILURE at
# level word, and no verdict is given; level auto still decides them. When
# the solver handles these, check instead that level word agrees with auto.
for case in 'bool bool_memory.p_written bool_memory.p_equal' 'grid grid.p_kept grid.p_zero'; do
    read -r task holds fails <<< "$case"
    run 2 yices-unsupported "${task}_word"
    has "crisp: $holds: " err
    has 'FAILURE' err
    if grep -Eq '  (proved|passed|refuted)  ' out; then
        cat out err >&2
        echo "yices-unsupported ${task}_word: expected no verdict" >&2
        exit 1
    fi
    has '1 task, 0 proved, 0 passed, 0 refuted, 2 unknown'
    run 1 yices-unsupported "${task}_auto"
    has "$holds  passed"
    has "$fails  refuted"
done

# Without yices-smt2 on PATH, the task is not solved.
mkdir -p no-yices
status=0
timeout 11 env PATH="$work/no-yices" "$root/build/crisp" "$root/tests/cases/level-word.salt" ram_word >out 2>err || status=$?
[[ $status == 2 ]]
has "crisp: $root/tests/cases/level-word.salt:26: [options] level word: yices-smt2 not found in PATH; install yices or add its directory to PATH" err
[[ $(wc -l < err) == 1 ]]
has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
# Spec 23: rtl-mode strict runs slang first; its errors stop the task, relaxed skips it.
rtl="$root/tests/cases/rtl-mode.salt"
rejected_rtl() {
    local task=$1 line=$2 diagnostic=$3
    run 2 rtl-mode "$task"
    [[ $(head -n 1 err) == "crisp: $rtl:$line: strict RTL check failed (slang); write rtl-mode relaxed to skip it" ]]
    has "$diagnostic" err
    if grep -Eq '  (proved|passed|refuted|unknown)  ' out; then
        cat out err >&2
        echo "rtl-mode $task: the strict check did not stop the task" >&2
        exit 1
    fi
    has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
}
rejected_rtl before 2 "rtl-before.sv:3:30: error: identifier 'q' used before its declaration"
rejected_rtl enum 4 'rtl-enum.sv:3:39: error: expression width of 4 does not exactly match declared enum type width of 3'
rejected_rtl initial 27 "rtl-initial.sv:8:30: error: variable 'mem' driven by always_ff procedure cannot be written to"
for case in 'before use_before_declaration.p_follows' 'enum enum_width.p_known' 'initial initial_and_always_ff.p_written'; do
    read -r task name <<< "$case"
    run 0 rtl-mode "${task}_relaxed"
    has 'rtl-mode relaxed'
    has "$name  passed"
done
# slang 11.0 reports $fatal("...") only as a warning; warnings are not printed.
run 0 rtl-mode fatal
has 'rtl-mode strict'
has 'fatal_string.p_true  passed'
[[ ! -s err ]]
run 0 rtl-mode legal
has 'legal.p_follows  passed'
has 'legal.p_known  passed'
[[ ! -s err ]]
# A strict and a relaxed task in one run.
run 2 rtl-mode before before_relaxed
has "crisp: $rtl:2: strict RTL check failed (slang)" err
has 'use_before_declaration.p_follows  passed'
has '2 tasks, 0 proved, 1 passed, 0 refuted, 0 unknown'
# Without slang, strict tasks stop and relaxed tasks run.
status=0
CRISP_SLANG="$work/no-slang" timeout 11 "$root/build/crisp" "$rtl" legal before_relaxed >out 2>err || status=$?
[[ $status == 2 ]]
has "crisp: $rtl:9: rtl-mode strict: slang not found at $work/no-slang. Run 'make slang' or set CRISP_SLANG" err
[[ $(wc -l < err) == 1 ]]
has 'use_before_declaration.p_follows  passed'
has '2 tasks, 0 proved, 1 passed, 0 refuted, 0 unknown'
# Spec 25: veval lowers the assertions; Crisp's EBMC checks each landing.
run 0 veval assumed
has 'assumed_top.u_child.p_child  proved'
has 'proved in assumed_top.u_child (veval)'
# Without the parent's assumption the same landing has a local counterexample.
run 3 veval unassumed
wave=crisp-out/veval_unassumed/veval/assumed_top.u_child.p_child/assumed_top.u_child.vcd
has "assumed_top.u_child.p_child  unknown"
has "local counterexample in assumed_top.u_child $wave"
valid_vcd "$wave"
run 0 veval assumed_bmc
has 'assumed_top.u_child.p_child  passed'
has 'passed in assumed_top.u_child (veval), bmc depth 5'
# A local counterexample the parent never allows: unknown with veval only,
# proved by the whole design with veval on.
run 3 veval spurious_only
has 'even_top.u_child.p_even  unknown'
valid_vcd crisp-out/veval_spurious_only/veval/even_top.u_child.p_even/even_top.u_child.vcd
run 0 veval spurious_on
has 'even_top.u_child.p_even  proved'
if grep -Fq '(veval)' out; then
    cat out err >&2
    echo 'veval on: the whole design should decide p_even' >&2
    exit 1
fi
run 1 veval bug_on
has 'bug_top.u_child.p_small  refuted  '
has 'crisp-out/veval_bug_on/bug_top.u_child.p_small.vcd'
valid_vcd crisp-out/veval_bug_on/bug_top.u_child.p_small.vcd
# The leaf has a counterexample; one level up the property holds.
run 0 veval up
has 'proved in up_top.u_mid (veval)'
run 3 veval missing
has 'checked without assumption missing_top.sum_small (arrives partial)'
run 0 veval selected
has 'two_top.u_child.p_one  proved'
[[ $(grep -c '^  two_top\.' out) == 1 ]]
run 2 veval clocks
has 'crisp: veval could not lower the design' err
has 'more than one clock (two_clocks.clk_a, two_clocks.clk_b)' err
run 2 veval unreadable
has 'crisp: veval could not lower the design' err
has 'veval-unreadable.sv line 3' err
# A struct port read field by field: the product keeps the port's type.
run 0 veval struct
has 'proved in struct_top.u_child (veval)'
# veval's local environment (spec 25, 2.4): an observed internal register,
# a reset asserted at the initial instant, and a clamp on an input.
run 0 veval observe
has 'proved in observe_top.u_child (veval)'
run 0 veval reset
has 'proved in reset_top.u_child (veval)'
run 0 veval clamp
has 'proved in clamp_top.u_child (veval)'
# [design] reset (spec 25, 2.4.5) resets the landing's input on the same net.
run 0 veval salt_reset
has 'proved in reset_top.u_child (veval)'
run 3 veval no_reset
has 'local counterexample in reset_top.u_child'
run 0 veval unconnected
has 'proved in unreset_top.u_child (veval); checked without reset: no input of unreset_top.u_child is connected to rst'
# Without a veval line the task runs as before.
run 0 token prove
if grep -Fq 'not connected' out; then
    cat out >&2
    exit 1
fi
echo 'EBMC cases passed (Specs 3, 8, 11, 18, 19, 20, 23, and 25).'
