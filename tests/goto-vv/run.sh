#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
base=${TMPDIR:-$root/build}
mkdir -p -- "$base"
work=$(mktemp -d "$base/crisp-goto-vv.XXXXXXXX")
trap 'rm -r -- "$work"' EXIT
cd -- "$work"
mkdir -p headers tmp
export TMPDIR="$PWD/tmp"

run() {
    local expected=$1 file=$2 status=0
    "$root/build/crisp" "$file" </dev/null >out 2>err || status=$?
    if [[ $status != "$expected" ]]; then
        cat out err >&2
        printf '%s: expected exit %s, got %s\n' "$file" "$expected" "$status" >&2
        exit 1
    fi
    # A normal exit must clean both the object and the task's scratch directory.
    [[ -z $(find . \( -name '*.vv' -o -name '*.f' -o -name '.crisp-ebmc.*' \) -print -quit) ]]
}

cat > headers/values.svh <<'SV'
`define HEADER_VALUE 1'b1
SV
cat > helper.sv <<'SV'
`include "values.svh"
module helper(output logic value);
    assign value = `FROM_SALT & `HEADER_VALUE;
endmodule
SV
cat > top.sv <<'SV'
module top(input logic clk, input logic rst);
    logic value;
    helper u_helper(value);
    logic first;
    initial first = 1'b1;
    always @(posedge clk) first <= 1'b0;
    p_value: assert property (@(posedge clk) value == `FROM_SALT && value == `HEADER_VALUE);
    p_reset: assert property (@(posedge clk) first == rst);
endmodule
SV
cat > reset.salt <<'SALT'
[options]
frontend goto-vv
mode bmc
depth 2
[files]
helper.sv
top.sv
[design]
top top
clock clk
reset rst high
include headers
define FROM_SALT 1'b1
SALT
run 0 reset.salt
grep -Fq 'frontend goto-vv' out
grep -Eq 'top.p_reset  passed.*bmc depth 2' out
grep -Eq 'top.p_value  (passed|proved)' out

sed 's/frontend goto-vv/frontend wrong/' reset.salt > wrong.salt
run 2 wrong.salt
grep -Fq ':2: [options]' err
grep -Fq 'expected native, goto-vv' err
for component in smart veval; do
    for option in on only; do
        sed "/^frontend/a$component $option" reset.salt > conflict.salt
        run 2 conflict.salt
        grep -Fq ":3: [options] frontend goto-vv conflicts with $component $option" err
    done
done

cat > enum.sv <<'SV'
module top(input logic clk);
    typedef enum logic [1:0] {A = 1'b0, B = 1'b1} choice_t;
    choice_t value;
    always_ff @(posedge clk) value <= A;
    p: assert property (@(posedge clk) value == value);
endmodule
SV
cat > forward.sv <<'SV'
module top(input logic clk);
    wire value = later;
    wire later = 1'b1;
    p: assert property (@(posedge clk) value);
endmodule
SV
cat > driver.sv <<'SV'
module top(input logic clk);
    logic value;
    initial value = 1'b0;
    always_ff @(posedge clk) value <= 1'b1;
    p: assert property (@(posedge clk) value == value);
endmodule
SV
for rule in enum forward driver; do
    cat > rule.salt <<SALT
[options]
frontend goto-vv
rtl-mode strict
mode bmc
depth 2
[files]
$rule.sv
[design]
top top
SALT
    run 2 rule.salt
    grep -Fxq "crisp: $work/rule.salt:2: goto-vv failed" <(head -1 err)
    grep -Eq "$rule\.sv:[0-9]+:[0-9]+: error:" err
    ! grep -q '^  top.p ' out
    sed 's/rtl-mode strict/rtl-mode relaxed/' rule.salt > relaxed.salt
    run 0 relaxed.salt
done

# Interrupt the compiler after it has written the output. Neither timeout nor
# a killed compiler should leave the object or start a check.
cat > interrupted-compiler <<'SH'
#!/bin/sh
set -eu
printf partial > "$2"
case "$CRISP_TEST_INTERRUPT" in
    timeout) exec sleep 30 ;;
    kill) kill -KILL "$$" ;;
esac
SH
chmod +x interrupted-compiler
printf '\n[resource]\ntimeout 1\n' >> reset.salt
CRISP_GOTO_VV="$work/interrupted-compiler" CRISP_TEST_INTERRUPT=timeout run 2 reset.salt
grep -Fq 'goto-vv exceeded the task timeout' err
! grep -q '^  top.p_' out
CRISP_GOTO_VV="$work/interrupted-compiler" CRISP_TEST_INTERRUPT=kill run 2 reset.salt
grep -Fq 'goto-vv terminated by signal 9' err
! grep -q '^  top.p_' out
printf '%s\n' 'goto-vv frontend checks passed'
