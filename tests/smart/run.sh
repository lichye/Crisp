#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
export TMPDIR="${TMPDIR:-$root/build}"
mkdir -p -- "$TMPDIR"
ulimit -c 0
work=$(mktemp -d "$TMPDIR/crisp-smart.XXXXXXXX")
trap 'rm -r -- "$work"' EXIT
cd -- "$work"

smart="$root/src/components/smartVerilog/build/smart"
crisp="$root/build/crisp"
export CRISP_SMART="$smart"
unset SMART_EBMC

run() {
    local expected=$1 log=$2 status=0
    shift 2
    "$@" >"$log" 2>&1 || status=$?
    if [[ "$status" != "$expected" ]]; then
        cat "$log" >&2
        echo "Expected exit $expected, got $status: $*" >&2
        exit 1
    fi
}

# Verilator encodes generate indices in member names. The two registers
# have different elaborated widths and share a source declaration.
cat > gen_top.sv <<'SV'
module lane_child #(parameter W = 40)(input [W-1:0] a, output [W-1:0] b);
  (* anyseq *) reg [W-1:0] r;
  assign b = a ^ r;
endmodule

module gen_top(input a, output x, output y);
  for (genvar i = 0; i < 2; i++) begin : lane
    localparam W = 39 + i;
    wire [W-1:0] out;
    lane_child #(.W(W)) u({W{a}}, out);
    if (i == 0) assign x = ^out;
    else assign y = ^out;
  end
endmodule
SV
run 0 infer.log "$smart" --dump-frontend --no-discover-siblings gen_top.sv
grep -Fq '"top": "gen_top"' infer.log
run 0 generate.log "$smart" --top gen_top --no-discover-siblings gen_top.sv \
    --cycles 2 --traces 1 --no-blockified -j 2 --keep-work --workdir generate -o generated.sv
grep -Fq 'lane__BRA__0__KET____DOT__u__DOT__r = (randomWord() & ((QData{1} << 39)' generate/sim_main.cpp
grep -Fq 'lane__BRA__1__KET____DOT__u__DOT__r = (randomWord() & ((QData{1} << 40)' generate/sim_main.cpp
awk '$1=="$var" && $5=="r" && $3==39 {a=1} $1=="$var" && $5=="r" && $3==40 {b=1} END {exit !(a&&b)}' \
    generate/runtime/sim_results/sim1.vcd

# The narrow instance satisfies candidate. The other instance and an
# unrelated property both fail, but only the named candidate can refine.
cat > scopes.sv <<'SV'
module leaf(input sig);
  candidate: assert property (!sig);
  unrelated: assert property (sig);
endmodule
module wrapper #(parameter V = 0)();
  leaf u(.sig(V != 0));
endmodule
module scope_probe;
  wrapper #(.V(0)) narrow();
  wrapper #(.V(1)) wide();
endmodule
SV
"$root/src/components/smartVerilog/build/test_smart_ebmc" "$work/scopes.sv" "$work/wide.vcd"

mkdir sample
cat > sample/rtl.sv <<'SV'
module review_xor(input clk, a, b, output y);
  assign y = a ^ b;
  p_xor: assert property (@(posedge clk) y == (a ^ b));
endmodule
SV
cat > sample/defs.sv <<'SV'
package unrelated_package;
  localparam int W = 1;
endpackage
SV
cat > sample/mining.salt <<'SALT'
[files]
defs.sv
rtl.sv
[options]
smart on
veval off
mode bmc
depth 5
[resource]
thread 2
timeout 60
SALT
sed '/^defs.sv$/d' sample/mining.salt > sample/one-file.salt

cat > ebmc-stub <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
    --version) echo "EBMC test stub"; exit 0 ;;
esac
case "$EBMC_TEST_MODE" in
    error) echo 'backend failure' >&2; exit 6 ;;
    unknown|final-error)
        for arg; do
            case "$arg" in
                *_check*.sv) echo 'final check failed' >&2; exit 6 ;;
                */reachability_*.sv)
                    printf '<result property="Verilog::$root.review_xor.__crisp_smart_property" status="PROVED up to bound 10"/>\n'
                    exit 0 ;;
                */safety_*.sv)
                    if [[ "$EBMC_TEST_MODE" == unknown ]]; then
                        printf '<result property="Verilog::$root.review_xor.__crisp_smart_property" status="INCONCLUSIVE"/>\n'
                        exit 10
                    fi
                    printf '<result property="Verilog::$root.review_xor.__crisp_smart_property" status="PROVED up to bound 10"/>\n'
                    exit 0 ;;
            esac
        done ;;
esac
echo 'unexpected EBMC test arguments' >&2
exit 6
STUB
chmod +x ebmc-stub
touch non-executable
run 2 bad-mode.log env SMART_EBMC="$work/non-executable" "$smart" --check-env
grep -Fq '[MISSING] ebmc' bad-mode.log
chmod +x non-executable
printf '#!/bin/sh\necho fake version\nexit 6\n' > non-executable
run 2 bad-version.log env SMART_EBMC="$work/non-executable" "$smart" --check-env
grep -Fq '(version command failed)' bad-version.log

smart_args=(--no-discover-siblings --top review_xor sample/rtl.sv --simulator iverilog
            --cycles 2 --traces 1 --seed 1 --no-blockified -j 2 --keep-work)
run 3 tool-error.log env SMART_EBMC="$work/ebmc-stub" EBMC_TEST_MODE=error \
    "$smart" "${smart_args[@]}" --workdir tool-error -o tool-error.sv
grep -Fq 'EBMC exited with status 6' tool-error.log
grep -Fq '"reason":"error"' tool-error/run-log.jsonl
[[ ! -e tool-error.sv ]]
run 2 salt-error.log env SMART_EBMC="$work/ebmc-stub" EBMC_TEST_MODE=error "$crisp" sample/mining.salt
grep -Fq 'SMART mining failed' salt-error.log
[[ ! -e crisp-out/mining/smart_assertion.sv ]]

run 0 unknown.log env SMART_EBMC="$work/ebmc-stub" EBMC_TEST_MODE=unknown \
    "$smart" "${smart_args[@]}" --unbounded --workdir unknown -o unknown.sv
grep -Fq '"reason":"unknown"' unknown/run-log.jsonl
if grep -Fq '"reason":"refuted"' unknown/run-log.jsonl; then exit 1; fi
if grep -Fq 'using EBMC counterexample' unknown/runtime/logs/core_*.log; then exit 1; fi

run 3 final-error.log env SMART_EBMC="$work/ebmc-stub" EBMC_TEST_MODE=final-error \
    "$smart" "${smart_args[@]}" --workdir final-error -o final-error.sv
grep -Fq 'EBMC final check failed' final-error.log
grep -Fq 'EBMC exited with status 6' final-error.log
[[ ! -e final-error.sv ]]

# Exercise the actual final check and validate every record it leaves behind.
cat > sample/comb_xor.sv <<'SV'
module comb_xor(input a, b, output y);
  assign y = a ^ b;
endmodule
SV
run 0 final-check.log "$smart" --no-discover-siblings sample/comb_xor.sv --simulator iverilog \
    --cycles 2 --traces 1 --seed 1 --refine 5 --no-blockified -j 1 --keep-work \
    --workdir final-check -o final-check.sv
python3 -I - final-check/run-log.jsonl <<'PYTHON'
import json
import sys

path = sys.argv[1]
checks = []
with open(path, encoding="utf-8") as log:
    for number, line in enumerate(log, 1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise SystemExit(f"{path}:{number}: {error}") from error
        if not isinstance(record, dict) or not {"run", "t"} <= record.keys():
            raise SystemExit(f"{path}:{number}: expected a run log record")
        if record.get("stage") == "check":
            checks.append(record)

if len(checks) != 1:
    raise SystemExit(f"{path}: expected one final check record, got {len(checks)}")
check = checks[0]
outcomes = ("verified", "refuted", "unknown", "timed_out", "errors")
for field in ("checked", *outcomes):
    value = check.get(field)
    if type(value) is not int or value < 0:
        raise SystemExit(f"{path}: expected a nonnegative integer for {field}, got {value!r}")
if check["mode"] != "k-induction" or check["bound"] is not None:
    raise SystemExit(f"{path}: expected an unbounded k-induction check")
if check["verified"] == 0 or check["checked"] != sum(check[field] for field in outcomes):
    raise SystemExit(f"{path}: inconsistent final check counts: {check}")
PYTHON

# Omitted top follows EBMC resolution, even when the first input is a
# package and the file that declares the module has a different name.
for name in one-file mining; do
    run 0 "$name.log" "$crisp" "sample/$name.salt"
    grep -Fq 'review_xor.p_xor  passed' "$name.log"
    grep -Fq 'added to module review_xor.' "crisp-out/$name/smart_assertion.sv"
done
cat > sample/second.sv <<'SV'
module second(input a);
  p: assert property (a == a);
endmodule
SV
sed '/^rtl.sv$/a second.sv' sample/mining.salt > sample/ambiguous.salt
run 2 ambiguous.log "$crisp" sample/ambiguous.salt
grep -Fq 'multiple top-level modules review_xor second; specify top explicitly' ambiguous.log

echo 'SMART generate, counterexample scope, backend status, and .salt top regressions passed.'
