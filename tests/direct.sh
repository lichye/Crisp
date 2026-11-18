#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
scratch=$(mktemp -d /tmp/crisp-single-file.XXXXXXXX)
trap 'rm -r -- "$scratch"' EXIT
cd "$scratch"
cp -- "$root/build/crisp" .
cat "$root/examples/token/rtl/token.sv" "$root/examples/token/spec.sv" > 'good design.sv'
sed 's/@(posedge clk) !conflict/@(posedge clk) conflict/' 'good design.sv' > bad.sv
echo 'module rtl_only(input wire clk); endmodule' > no_assert.sv
cat > cover.sv <<'SV'
module cover_only(input wire clk, input wire a);
    hit: cover property (@(posedge clk) a);
endmodule
SV
cat > mixed.sv <<'SV'
module mixed(input wire clk, input wire a);
    premise: assume property (@(posedge clk) a);
    check_a: assert property (@(posedge clk) a);
    missing: cover property (@(posedge clk) !a);
endmodule
SV
./crisp 'good design.sv' > good.stdout
grep -Fq '1/1 assertions proved, 0 refuted' good.stdout
code=0
./crisp bad.sv > bad.stdout || code=$?
[[ $code == 1 ]]
grep -Fq 'Counterexample:' bad.stdout
for input in no_assert.sv cover.sv; do
    code=0
    ./crisp "$input" > none.stdout || code=$?
    [[ $code == 3 ]]
    grep -Fq 'No assertions found' none.stdout
done
./crisp mixed.sv > mixed.stdout
grep -Fq '1/1 assertions proved, 0 refuted' mixed.stdout
logs=(log/*)
[[ ${#logs[@]} == 5 ]]
for log in "${logs[@]}"; do [[ -f $log && $log == *.log ]]; done
echo 'Standalone binary tests passed.'
