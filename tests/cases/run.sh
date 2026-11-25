#!/usr/bin/env bash
set -Eeuo pipefail
ulimit -c 0
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
# level word runs EBMC with yices (spec 19); the tests need it.
if ! command -v yices-smt2 >/dev/null; then
    echo 'tests/cases/run.sh: yices-smt2 not found in PATH; install yices or add its directory to PATH' >&2
    exit 1
fi
# veval and the kind/PDR Büchi conversion need Spot's ltl2tgba.
if ! command -v ltl2tgba >/dev/null; then
    echo "tests/cases/run.sh: ltl2tgba not found in PATH; install Spot or add its bin directory to PATH" >&2
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

report_failure() {
    local status=$1 line=$2 task=
    # Let the parent report a failed assignment or pipeline once.
    if (( BASH_SUBSHELL != 0 )); then return 0; fi
    if [[ -r out ]]; then
        task=$(sed -n 's|^  progress  crisp-out/\([^/]*\)/progress-[^/]*\.tsv$|\1|p' out) || task=
        if [[ $task == *$'\n'* ]]; then task=; fi
    fi
    printf '%s:%s: check failed (exit %s)%s\n' \
        "$root/tests/cases/run.sh" "$line" "$status" "${task:+ after task $task}" >&2 || :
}
trap 'report_failure "$?" "$LINENO"' ERR

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

progress_file() {
    sed -n 's/^  progress  //p' out
}

has_vacuity() {
    local property=$1 verdict=$2 state=$3
    if ! awk -v property="$property" -v verdict="$verdict" -v state="$state" '
        $1 == property && $2 == verdict && substr($0, length($0) - length(state) - 1) == "  " state { found++ }
        END { exit found != 1 }
    ' out || ! awk -F '\t' -v property="$property" -v verdict="$verdict" -v state="$state" '
        $1 == "final" && $2 == property && $4 == verdict && $6 == state { found++ }
        END { exit found != 1 }
    ' "$(progress_file)"; then
        cat out err "$(progress_file)" >&2
        echo "missing verdict/vacuity for $property: $verdict / $state" >&2
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
journal=$(progress_file)
[[ $journal == crisp-out/token_prove/progress-*.tsv ]]
grep -Fxq $'final\ttoken_formal.p_no_conflict\t\tproved\t\tunchecked' "$journal"
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
journal=$(progress_file)
grep -Fxq $'final\tcounter.p_range\t\trefuted\tcrisp-out/counter_ic3/counter.p_range.vcd\ttriggered' "$journal"
grep -Fxq $'final\tcounter.p_stop\t\tproved\t\tunchecked' "$journal"
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
    certificate=1
    if [[ $mode == bmc* ]]; then certificate=0; fi
    awk -v fixture=reset-low -v require_certificate="$certificate" -f "$root/tests/cases/check-witness.awk" "$wave"
done
# A real witness with the opposite reset polarity must fail the same validator.
printf '[files]\n%s\n[design]\nreset rst_n high\n[options]\nmode kind\ndepth 3\n' \
    "$root/tests/cases/reset-low.sv" > reversed-reset.salt
status=0
timeout 11 "$root/build/crisp" reversed-reset.salt >out 2>err || status=$?
[[ $status == 1 ]]
has 'top.p  refuted'
wave="crisp-out/reversed-reset/top.p.vcd"
valid_vcd "$wave"
if awk -v fixture=reset-low -v require_certificate=1 -f "$root/tests/cases/check-witness.awk" "$wave" 2>reversed-reset.err; then
    echo 'reset-low validator accepted a witness with the opposite reset polarity' >&2
    exit 1
fi
has 'expected active-low reset at #0' reversed-reset.err
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
wave=crisp-out/constant/empty.p_false.vcd
valid_vcd "$wave"
# This marker is emitted by the backend's empty-assignments renderer.
has '$scope module ebmc $end' "$wave"
has '$comment finite witness with proved total extension $end' "$wave"
has '1!' "$wave"
run 1 bdd-witness
has '2 refuted, 0 unknown'
wave=crisp-out/bdd-witness/ctl_witness.eventually_finishes.vcd
valid_vcd "$wave"
has '$comment witness loop ' "$wave"
has $'final\tctl_witness.eventually_finishes\t\trefuted\t'"$wave" "$(progress_file)"
has $'final\tctl_witness.finishes_in_phase\t\trefuted\texact BDD verdict (no certified waveform)' "$(progress_file)"
[[ ! -e crisp-out/bdd-witness/ctl_witness.finishes_in_phase.vcd ]]
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
selected literal top.u_leaf.q_leaf 'top.g[1].p_gen'
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

# Preserve temporal assumptions, target names, and independent PDR witnesses.
for mode in kind pdr; do
    run 0 buechi "$mode"
    has 'buechi.p_quiet  proved'
    run 1 buechi "free_$mode"
    has 'buechi.p_quiet  refuted'
    valid_vcd "crisp-out/buechi_free_$mode/buechi.p_quiet.vcd"
done
run 1 buechi multi_pdr
has 'buechi.p_quiet  proved'
has 'buechi.p_next  refuted'
has 'buechi.p_later  refuted'
has '1 task, 1 proved, 0 passed, 2 refuted, 0 unknown'
for case in 'p_next 1' 'p_later 0'; do
    read -r target choose_next <<< "$case"
    wave="crisp-out/buechi_multi_pdr/buechi.$target.vcd"
    valid_vcd "$wave"
    awk -v fixture=buechi -v require_certificate=1 -v choose_next="$choose_next" -f "$root/tests/cases/check-witness.awk" "$wave"
done
if awk -v fixture=buechi -v require_certificate=1 -v choose_next=1 -f "$root/tests/cases/check-witness.awk" \
    crisp-out/buechi_multi_pdr/buechi.p_later.vcd 2>wrong-target.err; then
    echo 'PDR witness validator accepted the other target waveform' >&2
    exit 1
fi
has 'wrong initial reset or target choice' wrong-target.err
run 2 buechi unsupported_contract
has 'unsupported Buechi assumption buechi.live' err

# An outstanding assumption cannot justify a refutation with no legal continuation.
for window in one eight; do
    delay=1
    if [[ $window == eight ]]; then delay=8; fi
    for mode in kind pdr; do
        locked="${window}_locked_$mode"
        allowed="${window}_allowed_$mode"
        run 3 terminal-obligation "$locked"
        has 'tail.unreachable  unknown'
        [[ ! -e "crisp-out/terminal-obligation_$locked/tail.unreachable.vcd" ]]
        run 1 terminal-obligation "$allowed"
        has 'tail.unreachable  refuted'
        wave="crisp-out/terminal-obligation_$allowed/tail.unreachable.vcd"
        valid_vcd "$wave"
        awk -v fixture=terminal -v require_certificate=1 -v delay="$delay" -f "$root/tests/cases/check-witness.awk" "$wave"
    done
done

# The same bounded task also checks a finite antecedent. Its completion
# cannot authorize publishing another goal's uncertified prefix.
run 3 terminal-obligation one_locked_word_bmc
has 'tail.unreachable  unknown'
has 'tail.match_trip  passed'
journal=$(progress_file)
grep -Fxq $'final\ttail.unreachable\t\tunknown\tEBMC: INCONCLUSIVE\tunchecked' "$journal"
grep -Fxq $'final\ttail.match_trip\t\tpassed\tbmc depth 10\ttriggered' "$journal"
[[ ! -e crisp-out/terminal-obligation_one_locked_word_bmc/tail.unreachable.vcd ]]
run 1 terminal-obligation one_allowed_word_bmc
has 'tail.unreachable  refuted'
has 'tail.match_trip  passed'
wave=crisp-out/terminal-obligation_one_allowed_word_bmc/tail.unreachable.vcd
valid_vcd "$wave"
awk -v fixture=terminal -v require_certificate=1 -v delay=1 -v bound=10 -f "$root/tests/cases/check-witness.awk" "$wave"
journal=$(progress_file)
grep -Fxq $'final\ttail.unreachable\t\trefuted\t'"$wave"$'\ttriggered' "$journal"
grep -Fxq $'final\ttail.match_trip\t\tpassed\tbmc depth 10\ttriggered' "$journal"

# Keep the same assumption through both queries, including at a veval landing.
for mode in kind pdr local_kind local_pdr; do
    run 0 vacuity-buechi "$mode"
    has_vacuity vacuity_monitor.leaf.blocked proved 'never triggered'
    has_vacuity vacuity_monitor.leaf.captured proved 'never triggered'
    has_vacuity vacuity_monitor.leaf.allowed proved triggered
    has '1 task, 3 proved, 0 passed, 0 refuted, 0 unknown; vacuity: 1 triggered, 0 not triggered, 2 never triggered, 0 unchecked'
    run 0 vacuity-buechi "free_$mode"
    has '1 task, 3 proved, 0 passed, 0 refuted, 0 unknown; vacuity: 3 triggered, 0 not triggered, 0 never triggered, 0 unchecked'
done
run 1 vacuity-buechi multi
has '1 task, 3 proved, 0 passed, 2 refuted, 0 unknown; vacuity: 3 triggered, 0 not triggered, 2 never triggered, 0 unchecked'
for case in 'next_bad 1' 'later_bad 2'; do
    read -r target delay <<< "$case"
    wave="crisp-out/vacuity-buechi_multi/vacuity_monitor.$target.vcd"
    valid_vcd "$wave"
    awk -v fixture=vacuity-buechi -v require_certificate=1 -v delay="$delay" -f "$root/tests/cases/check-witness.awk" "$wave"
done

properties="$root/tests/cases/properties.salt"
for unmatched in 'nomatch 34 top.absent*' 'dot 35 top?p_one' 'literal_miss 39 top.u_leaf.q_leag'; do
    read -r task line pattern <<< "$unmatched"
    reported 2 "$task" -
    has "$properties:$line: [options] properties: pattern '$pattern' matches no assertion" err
    [[ $(wc -l < err) == 1 ]]
    has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
done

while read -r task expected; do
    run 0 trailing-properties "$task"
    actual=$(awk -F '\t' '$1 == "final" && $4 == "passed" {print $2}' "$(progress_file)" | LC_ALL=C sort | paste -sd ' ')
    [[ $actual == "$expected" ]]
done <<'MATCHES'
empty top.p_one
prefix top.p_one top.p_still top.p_two
recursive top.u_leaf.q_leaf
brackets top.g[1].p_gen
middle top.u_leaf.q_leaf
MATCHES
for unmatched in 'dot top.u_*' 'case Top.*'; do
    read -r task pattern <<< "$unmatched"
    run 2 trailing-properties "$task"
    salt="$root/tests/cases/trailing-properties.salt"
    line=$(grep -nF "$task: properties $pattern" "$salt" | cut -d: -f1)
    has "$salt:$line: [options] properties: pattern '$pattern' matches no assertion" err
    [[ $(wc -l < err) == 1 ]]
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
    has 'bmc depth 10'
    has 'ram_word.p_untouched  refuted'
    wave="crisp-out/level-word_$task/ram_word.p_untouched.vcd"
    valid_vcd "$wave"
    # The two solver selections have the same deterministic prefix. Each
    # witness may then close at its own valid loop frame.
    awk -v fixture=ram -v bound=10 -v require_certificate=1 -f "$root/tests/cases/check-witness.awk" "$wave"
done
# The solver uses a private directory inside the run's output directory.
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
# The CLI accounts for a timeout; EBMC did not deliver a property verdict.
[[ $(wc -l < "$(progress_file)") == 1 ]]
# oss-cad-suite starts yices through its loader, so match the command line.
for pid in $(pgrep -f yices-smt2 || true); do
    if [[ $(readlink "/proc/$pid/cwd" 2>/dev/null) == "$work"* ]]; then
        echo "yices-smt2 $pid left after the timeout" >&2
        exit 1
    fi
done

# Inspect the live SMT input and leave extra files that require parent cleanup.
# Keep the solver alive on timeout and after killing its EBMC parent.
mkdir -p cleanup-solver crisp-out/level-word_factor/tmp
touch "$TMPDIR/keep" crisp-out/level-word_factor/tmp/smt2_dec_problem_keep
cat > cleanup-solver/yices-smt2 <<'SOLVER'
#!/usr/bin/env bash
set -eu
[[ $1 == "$TMPDIR"/smt2_dec_problem_* && -s $1 && $PWD == "$TMPDIR" ]]
touch "$TMPDIR/smt2_dec_stdout_leftover" relative-dump.smt2
printf '%s\n' "$TMPDIR" >> "$CLEANUP_TRACE"
case $CLEANUP_MODE in
    normal) exec "$CLEANUP_REAL_YICES" "$@" ;;
    killed) kill -KILL "$PPID" ;;
esac
exec sleep 15
SOLVER
chmod +x cleanup-solver/yices-smt2
export CLEANUP_REAL_YICES=$(command -v yices-smt2) CLEANUP_TRACE="$work/cleanup-trace"
rm crisp-out/level-word_ram_word/ram_word.p_untouched.vcd
for cleanup_mode in normal timeout killed; do
    case $cleanup_mode in
        normal) task=ram_word; expected=1 ;;
        timeout) task=factor; expected=3 ;;
        killed) task=factor; expected=2 ;;
    esac
    : > "$CLEANUP_TRACE"
    CLEANUP_MODE=$cleanup_mode PATH="$work/cleanup-solver:$PATH" run "$expected" level-word "$task"
    case $cleanup_mode in
        normal) valid_vcd crisp-out/level-word_ram_word/ram_word.p_untouched.vcd ;;
        timeout) has 'timeout after 2 s' ;;
        killed) has 'EBMC terminated by signal 9' err ;;
    esac
    [[ -s $CLEANUP_TRACE && -s $(progress_file) ]]
    while IFS= read -r solver_tmp; do
        [[ $solver_tmp == "$work/crisp-out"/.crisp-ebmc.*/tmp.* ]]
        [[ ! -e $solver_tmp && ! -e ${solver_tmp%/*} ]]
    done < "$CLEANUP_TRACE"
done
[[ -f $TMPDIR/keep && -f crisp-out/level-word_factor/tmp/smt2_dec_problem_keep ]]

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
# Spec 23 section 8: a compile-time out-of-range read. Under strict, slang reports
# it first; with a slang that accepts the design, EBMC's range-oob stops the task.
# Under relaxed the bit reads as x, the same as writing x explicitly.
oob="$root/tests/cases/rtl-oob.salt"
no_verdict() {
    if grep -Eq '  (proved|passed|refuted|unknown)  ' out; then
        cat out err >&2
        echo "rtl-oob $1: the task was solved" >&2
        exit 1
    fi
    has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
}
run 2 rtl-oob strict
[[ $(head -n 1 err) == "crisp: $oob:2: strict RTL check failed (slang); write rtl-mode relaxed to skip it" ]]
has "rtl-oob.sv:4:22: error: cannot select range of [3:1] from 'logic[2:0]' [-Wrange-oob]" err
no_verdict strict
printf '#!/bin/sh\nexit 0\n' > accept-slang
chmod +x accept-slang
status=0
CRISP_SLANG="$work/accept-slang" timeout 11 "$root/build/crisp" "$oob" strict >out 2>err || status=$?
[[ $status == 2 ]]
[[ $(head -n 1 err) == "crisp: $oob:2: constant out-of-range read (EBMC range-oob); write rtl-mode relaxed to read the extra bits as x" ]]
has 'range-oob: constant packed read [3:1] is outside declared range [2:0]' err
no_verdict strict
run 1 rtl-oob relaxed
has 'rtl-mode relaxed'
has 'oob.p_high_zero  refuted'
run 1 rtl-oob explicit_x
has 'oob_x.p_high_zero  refuted'
run 1 rtl-oob relaxed_veval
has 'oob.p_high_zero  refuted'
# Spec 21. The original verdict and the antecedent diagnostic are independent.
for mode in bounded word bit induction pdr bdd auto; do
    run 0 vacuity "$mode"
    case "$mode" in
        bounded|word|bit) success=passed; state='not triggered within 3' ;;
        *) success=proved; state='never triggered' ;;
    esac
    for name in never disabled assumed_out bool_dead; do
        has_vacuity "vacuity.basic_$name" "$success" "$state"
    done
    has_vacuity vacuity.basic_constant_false "$success" 'never triggered'
    has_vacuity vacuity.basic_ordinary "$success" unchecked
    for name in live bool_live; do has_vacuity "vacuity.basic_$name" "$success" triggered; done
    if [[ $mode == bounded || $mode == word ]]; then
        has_vacuity vacuity.next_dead passed 'not triggered within 3'
        has_vacuity vacuity.next_live passed triggered
        has_vacuity vacuity.chained passed triggered
        has '1 task, 0 proved, 11 passed, 0 refuted, 0 unknown; vacuity: 4 triggered, 5 not triggered, 1 never triggered, 1 unchecked'
    fi
done
run 0 vacuity late3
has_vacuity vacuity.late passed 'not triggered within 3'
run 0 vacuity late8
has_vacuity vacuity.late passed triggered
run 0 vacuity late_pdr
has_vacuity vacuity.late proved triggered
run 0 vacuity pdr30
has_vacuity sampled.after30 proved triggered
run 1 vacuity refute
has_vacuity vacuity.wrong refuted triggered
valid_vcd crisp-out/vacuity_refute/vacuity.wrong.vcd
run 0 vacuity sequence
has_vacuity vacuity.double_reset passed 'not triggered within 3'
has_vacuity vacuity.last_sample passed triggered
for mode in local also_top; do
    run 0 vacuity-local "$mode"
    for name in overlap next_cycle boolean_arrow captured; do
        has_vacuity "guarded_top.child.$name" passed 'not triggered within 3'
    done
    journal=$(progress_file)
    grep -Fxq $'local\tguarded_top.child.overlap\tguarded_top.child\tpassed\tpassed in guarded_top.child (veval), bmc depth 3\tunchecked' "$journal"
done

# Inject an error or a timeout after the primary assertion has passed.
mkdir -p vacuity-solver
export REAL_YICES=$(command -v yices-smt2) VACUITY_MARKER="$work/vacuity-called"
cat > vacuity-solver/yices-smt2 <<'SOLVER'
#!/usr/bin/env bash
case "$PWD" in
    */veval/check*/vacuity/tmp.*)
        if [[ -n ${LOCAL_PRIMARY_PROGRESS:-} && ! -e $VACUITY_MARKER ]]; then
            awk -F '\t' '$1 == "local" && $4 == "passed" {print $2}' \
                "$LOCAL_PRIMARY_PROGRESS"/progress-*.tsv > "$VACUITY_MARKER"
        else
            touch "$VACUITY_MARKER"
        fi
        ;;
    */veval/check*/tmp.*) exec "$REAL_YICES" "$@" ;;
    *) if [[ ! -f $VACUITY_MARKER ]]; then
           touch "$VACUITY_MARKER"
           exec "$REAL_YICES" "$@"
       fi ;;
esac
if [[ $VACUITY_FAULT == timeout ]]; then sleep 8; fi
exit 1
SOLVER
chmod +x vacuity-solver/yices-smt2
printf '[files]\n%s\n[design]\ntop vacuity\nreset rst high\n[options]\nmode bmc\nlevel word\ndepth 3\nproperties vacuity.late\n[resource]\ntimeout 2\n' \
    "$root/tests/cases/vacuity.sv" > vacuity-fault.salt
for fault in error timeout; do
    rm -f "$VACUITY_MARKER"
    VACUITY_FAULT=$fault PATH="$work/vacuity-solver:$PATH" timeout 11 \
        "$root/build/crisp" vacuity-fault.salt >out 2>err
    has_vacuity vacuity.late passed unchecked
    [[ $(wc -l < "$(progress_file)") == 2 ]]
done

# Check ordering at the first auxiliary call. The top-level fixture above
# already exercises deadline expiry.
for mode in only on; do
    task="local-fault-$mode"
    sed -e 's@/vacuity.sv$@/vacuity-local.sv@' -e 's/top vacuity/top guarded_top/' \
        -e '/reset rst high/d' -e "s/properties vacuity.late/veval $mode/" \
        -e '/^\[resource\]$/,$d' vacuity-fault.salt > "$task.salt"
    rm -f "$VACUITY_MARKER"
    VACUITY_FAULT=error LOCAL_PRIMARY_PROGRESS="$work/crisp-out/$task" \
        PATH="$work/vacuity-solver:$PATH" timeout 11 "$root/build/crisp" "$task.salt" >out 2>err
    [[ -f $VACUITY_MARKER && $(wc -l < "$VACUITY_MARKER") == 4 ]]
    for name in overlap next_cycle boolean_arrow captured; do
        grep -Fxq "guarded_top.child.$name" "$VACUITY_MARKER"
        has_vacuity "guarded_top.child.$name" passed unchecked
    done
    has '1 task, 0 proved, 4 passed, 0 refuted, 0 unknown; vacuity: 0 triggered, 0 not triggered, 0 never triggered, 4 unchecked'
done

# A complete match at the end of a finite prefix does not need an extension.
for mode in kind pdr; do
    run 0 vacuity-tail1 "$mode"
    has_vacuity tail.match_trip proved triggered
    run 0 vacuity-tail1 "open_$mode"
    has_vacuity tail.match_trip proved triggered
done
run 0 vacuity-tail1 bounded
has_vacuity tail.match_trip passed triggered
has 'bmc depth 10'

# Share the finite unwind with the antecedent of match_trip. The original
# assertion still needs EBMC's strict certificate whether checked alone or
# in that same batch. Repeat with the equivalent RTL monitor.
for level in word bit; do
    for monitor in '' 'define RTL_MONITOR'; do
        for alarm in '' 'define ALLOW_ALARM'; do
            cat > mixed.salt <<SALT
[tasks]
solo
mixed
[files]
$root/tests/cases/vacuity-tail1.sv
[design]
top tail
$monitor
$alarm
[options]
mode bmc
depth 10
level $level
properties tail.ordinary
mixed: properties tail.match_trip
SALT
            for batch in solo mixed; do
                status=0
                timeout 11 "$root/build/crisp" mixed.salt "$batch" >out 2>err || status=$?
                if [[ -n $alarm ]]; then
                    [[ $status == 1 ]]; has 'tail.ordinary  refuted'
                else
                    [[ $status == 3 ]]; has 'tail.ordinary  unknown'
                fi
                if [[ $batch == mixed ]]; then has 'tail.match_trip  passed'; fi
            done
        done
    done
done

# The finite prefix can match trip before the later obligation is due.
for mode in kind pdr; do
    run 0 vacuity-shallow "closed_$mode"
    has_vacuity shallow.outstanding proved triggered
    run 0 vacuity-shallow "open_$mode"
    has_vacuity shallow.outstanding proved triggered
done
run 0 vacuity-shallow finite
has_vacuity shallow.outstanding passed triggered

# A four-cycle witness classifies early. At cycle 30 the same design still
# needs the unbounded auxiliary query.
run 0 vacuity-shallow multi
has '1 task, 3 proved, 0 passed, 0 refuted, 0 unknown; vacuity: 2 triggered, 0 not triggered, 1 never triggered, 0 unchecked'
has_vacuity shallow.early proved triggered
has_vacuity shallow.late proved triggered
has_vacuity shallow.excluded proved 'never triggered'
run 0 vacuity-shallow pdr30
has_vacuity shallow.late proved triggered
run 0 vacuity-shallow bounded4
has_vacuity shallow.late passed 'not triggered within 4'
for mode in kind pdr; do
    run 0 vacuity-shallow "long_$mode"
    has_vacuity longloop.hit proved triggered
done

run 0 vacuity-total open
has_vacuity total.early proved triggered
has_vacuity total.never proved 'never triggered'
run 0 vacuity-total locked
has_vacuity total.early proved 'never triggered'
has_vacuity total.never proved 'never triggered'

# kind uses its native solver. The shallow witness search uses Yices.
# Let Yices answer the first shallow query, then hang the second. In the second
# run also break the fallback to distinguish a retained SAT hit from a recheck.
mkdir shallow-tools
export REAL_SPOT=$(command -v ltl2tgba) SHALLOW_MARKER="$work/shallow"
cat > shallow-tools/yices-smt2 <<'SOLVER'
#!/usr/bin/env bash
if [[ $FALLBACK == window ]]; then touch "$SHALLOW_MARKER.first"; exec "$REAL_YICES" "$@"; fi
if [[ $FALLBACK == unsupported && $PPID == "$(cat "$SHALLOW_MARKER.primary")" ]]; then
    exec "$REAL_YICES" "$@"
fi
if [[ -e $SHALLOW_MARKER.stalled ]]; then exec "$REAL_YICES" "$@"; fi
if [[ -e $SHALLOW_MARKER.first ]]; then
    echo "$$" > "$SHALLOW_MARKER.stalled"
    exec sleep 15
fi
touch "$SHALLOW_MARKER.first"
exec "$REAL_YICES" "$@"
SOLVER
cat > shallow-tools/ltl2tgba <<'SPOT'
#!/usr/bin/env bash
if [[ $FALLBACK == window && -e $SHALLOW_MARKER.first ]]; then exit 1; fi
if [[ $FALLBACK == unsupported ]]; then
    if [[ ! -e $SHALLOW_MARKER.primary ]]; then echo "$PPID" > "$SHALLOW_MARKER.primary"; fi
    if [[ $PPID != "$(cat "$SHALLOW_MARKER.primary")" ]]; then
        touch "$SHALLOW_MARKER.unsupported"
        exit 1
    fi
fi
if [[ -e $SHALLOW_MARKER.stalled && $FALLBACK == error ]]; then exit 1; fi
if [[ -e $SHALLOW_MARKER.stalled && $FALLBACK == timeout ]]; then
    # The parent announced this run's TSV before starting the worker.
    base=${SHALLOW_MARKER%/*}
    journal=$(sed -n 's/^  progress  //p' "$base/out")
    cp "$base/$journal" "$SHALLOW_MARKER.inflight"
    exec sleep 15
fi
exec "$REAL_SPOT" "$@"
SPOT
chmod +x shallow-tools/{ltl2tgba,yices-smt2}
for fallback in normal error timeout unsupported; do
    rm -f "$SHALLOW_MARKER".{first,stalled,primary,unsupported,inflight}
    FALLBACK=$fallback PATH="$work/shallow-tools:$PATH" run 0 vacuity-shallow multi
    if [[ $fallback == unsupported ]]; then
        [[ -e $SHALLOW_MARKER.unsupported && ! -e $SHALLOW_MARKER.first ]]
    else
        [[ -s $SHALLOW_MARKER.stalled && ! -e /proc/$(cat "$SHALLOW_MARKER.stalled")/exe ]]
    fi
    has_vacuity shallow.early proved triggered
    if [[ $fallback == normal || $fallback == unsupported ]]; then
        has_vacuity shallow.late proved triggered
        has_vacuity shallow.excluded proved 'never triggered'
    else
        has_vacuity shallow.late proved unchecked
        has_vacuity shallow.excluded proved unchecked
        has '1 task, 3 proved, 0 passed, 0 refuted, 0 unknown; vacuity: 1 triggered, 0 not triggered, 0 never triggered, 2 unchecked'
    fi
    if [[ $fallback == timeout ]]; then
        [[ $(wc -l < "$SHALLOW_MARKER.inflight") == 1 ]]
        [[ $(wc -l < "$(progress_file)") == 4 ]]
    fi
done

# The finite shallow query cannot see a match that starts after its bound.
rm -f "$SHALLOW_MARKER.first"
FALLBACK=window PATH="$work/shallow-tools:$PATH" run 0 vacuity-shallow window
has_vacuity periodic.on_time proved triggered
has_vacuity periodic.later proved unchecked

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
# Private generated specs must survive a failing filesystem-wide flush, and
# a failed fsync of their own data must still abort lowering.
if command -v strace >/dev/null && strace -qq -o /dev/null -e inject=syncfs:error=EIO true 2>/dev/null; then
    for call in syncfs fsync; do
        status=0
        timeout 11 strace -f -qq -e trace=syncfs,fsync -o "$work/$call.trace" -e "inject=$call:error=EIO" \
            "$root/build/crisp" "$root/tests/cases/veval.salt" unassumed >out 2>err || status=$?
        if [[ $call == syncfs ]]; then
            [[ $status == 3 ]]
            has 'local counterexample in assumed_top.u_child'
        else
            [[ $status == 2 ]]
            has 'flush output' err
        fi
    done
fi
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
grep '^local' "$(progress_file)" | grep -Fq 'checked without assumption missing_top.sum_small (arrives partial)'
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
# A compact helper has an unpacked array input. A second property whose
# helper cannot be written must leave the first local check usable.
run 0 veval array_function
has 'passed in array_function_top.u_child (veval), bmc depth 2'
run 3 veval array_function_unprintable
has 'passed in array_function_top.u_child (veval), bmc depth 2'
has 'array_function_top.p_update  unknown'
has 'veval: no landing'
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
run 0 veval reset_agree
has 'reset_top.u_child.p_small  proved'
journal=$(progress_file)
grep -Fxq $'local\treset_top.u_child.p_small\treset_top.u_child\tproved\tproved in reset_top.u_child (veval)\tunchecked' "$journal"
grep -Fxq $'final\treset_top.u_child.p_small\t\tproved\tproved in reset_top.u_child (veval)\tunchecked' "$journal"
run 3 veval reset_conflict
has 'reset_top.u_child.p_small  unknown'
reason='veval: reset_top.u_child: no local conclusion, Crisp cannot state its two different resets (rst and rst)'
has "$reason"
journal=$(progress_file)
[[ $(wc -l < "$journal") == 2 ]]
grep -Fxq $'final\treset_top.u_child.p_small\t\tunknown\t'"$reason"$'\tunchecked' "$journal"
run 3 veval no_reset
has 'local counterexample in reset_top.u_child'
run 0 veval aliases
for property in alias_top.p_parent alias_top.u_mid.u_leaf.p_small; do
    has $'local\t'"$property"$'\talias_top.u_mid.u_leaf\tproved\t' "$(progress_file)"
done
run 3 veval aliases_no_reset
has '1 task, 0 proved, 0 passed, 0 refuted, 2 unknown'
[[ $(grep -c 'local counterexample in alias_top.u_mid.u_leaf ' out) == 2 ]]
run 0 veval unconnected
has 'proved in unreset_top.u_child (veval); checked without reset: no input of unreset_top.u_child is connected to rst'
# Without ltl2tgba on PATH, a veval task stops before any check.
mkdir -p no-spot
status=0
timeout 11 env PATH="$work/no-spot" "$root/build/crisp" "$root/tests/cases/veval.salt" assumed >out 2>err || status=$?
[[ $status == 2 ]]
veval_only_line=$(grep -nx 'veval only' "$root/tests/cases/veval.salt" | cut -d: -f1)
has "crisp: $root/tests/cases/veval.salt:$veval_only_line: [options] veval only: ltl2tgba not found in PATH; install Spot (https://spot.lre.epita.fr/install.html) or add its bin directory to PATH" err
[[ $(wc -l < err) == 1 ]]
has '1 task, 0 proved, 0 passed, 0 refuted, 0 unknown'
# The direct engine also reports its dependency before starting a check.
status=0
timeout 11 env PATH="$work/no-spot" "$root/build/crisp" "$root/tests/cases/buechi.salt" pdr >out 2>err || status=$?
[[ $status == 2 ]]
has '[options] mode pdr: ltl2tgba not found in PATH' err
# Without a veval line the task runs as before.
run 0 token prove
if grep -Fq 'not connected' out; then
    cat out >&2
    exit 1
fi
python3 "$root/tests/cases/progress.py" "$root"
echo 'EBMC cases passed (Specs 3, 8, 11, 18, 19, 20, 21, 23, 25, 29, and 30).'
