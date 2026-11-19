#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
scratch=$(mktemp -d /tmp/crisp-test.XXXXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
suite="$root/src/components/smartVerilog/.deps/oss-cad-suite/bin"
if [[ ! -x $suite/yosys-config ]]; then suite="$HOME/.local/oss-cad-suite/bin"; fi
export OSS_CAD_BIN=${OSS_CAD_BIN:-$suite}
export PATH="$OSS_CAD_BIN:$PATH:$HOME/.local/spot/bin"
export TMPDIR="$scratch" PYTHONDONTWRITEBYTECODE=1
mkdir "$scratch/native" "$scratch/log"
"$root/src/components/veval/build/crisp/crisp_verify_tests" "$root" "$scratch/native"

# The intact DUT must retain authored assumptions and distinguish a real
# counterexample from an inconclusive induction step.
sed '/endmodule/i\
    a_step: assume property (@(posedge clk) step);\
    p_no_sample: assert property (@(posedge clk) !sample);' \
    "$root/examples/token/spec.sv" > "$scratch/spec.sv"
code=0
"$root/src/cli.sh" verify "$root/examples/token/rtl/token.sv" --top token_formal \
    --spec "$scratch/spec.sv" --split off --lemmas off --out "$scratch/log" > /dev/null || code=$?
logs=("$scratch/log/"*)
[[ $code == 1 && ${#logs[@]} == 1 && ${logs[0]} == *.log ]]
log=${logs[0]}
grep -Fxq 'token_formal.p_no_conflict @ token_formal.dut: unknown' "$log"
grep -Fxq 'token_formal.p_no_sample @ token_formal.dut: refuted' "$log"
[[ $(grep -Fxc 'Premise token_formal.a_step' "$log") == 2 ]]
grep -Fq '$enddefinitions' "$log"
grep -Fxq 'Sources with all selected module checks proved: 0/2' "$log"
echo 'Verification regression tests passed.'
