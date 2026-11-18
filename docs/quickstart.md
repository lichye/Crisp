# Run the token example

Two registers hold a token. They start as `1` and `0` and exchange values on an enabled step. The top-level property says that sampling the registers can never signal a conflict. [RTL](../examples/token/rtl/token.sv) · [Property](../examples/token/spec.sv) · [Proof task](../examples/token/token.sby)

## Build

Run these commands from the Crisp root. The local build can reuse the dependencies already installed beside the original smartVerilog checkout.

```sh
make build SMART_DEPS_ROOT=../smartVerilog/.deps CVC5_ROOT=../smartVerilog/otherTools/cvc5
```

On a fresh Linux host, install a C++17 compiler, CMake, Make, Flex, Bison, the SQLite3 and OpenSSL development headers, and the tools used by SMART's installer (Git, curl and Python with pip). Install Spot and an OSS CAD Suite with matching Yosys development headers. Put their `bin/` directories on `PATH` and set `OSS_CAD_BIN` to the suite's `bin/` directory. The local run used OSS CAD Suite 2025-06-04 and Spot 2.15.1. SMART's installer sets up its hw-cbmc and cvc5 dependencies.

```sh
(cd components/smartVerilog && ./install.sh)
make build
```

`make veval` builds lowering and checking independently. EBMC is required for SMART's default final proof. [Component credits](../PROVENANCE.md)

## Verify

Run the comparison from the repository root after building both components.

```sh
make example
```

The command writes a fresh `crisp-out/token.*/summary.json` and one report per mode. To run just the split proof with automatic lemma generation, use the public command directly.

```sh
./crisp verify examples/token/rtl/token.sv --top token_formal --spec examples/token/spec.sv
```

`--top` names the SystemVerilog **specification wrapper** declared in `--spec`. Supply the design files in compilation order. Include any required `.vh` or `.svh` headers explicitly under their include basenames. The switches `--split on|off` and `--lemmas on|off` both default to `on`. The default `--depth 5` applies to each k-induction attempt. The default `--budget 60` covers the entire invocation, including mining and retries. Use `--out DIR` to choose a new output directory.

With splitting off, use a wrapper that instantiates one DUT and connects distinct wrapper inputs directly to its inputs. Crisp keeps the full hierarchy of that DUT and preserves the authored assertion and assumption roles. With splitting on, each module is checked under the premises that veval placed at that instance. The report groups selected checks under their source assertion. Each `initial.json` lists the actual premises and reset conditions. A source with no selected check receives no proof credit.

`report.json` links to the original attempt, the candidates that SMART produced and a proof record for each lemma used. An inconclusive single-clock safety check can use one-bit, single-frame Boolean lemmas over visible DUT signals. A reachable module failure includes its `trace.vcd`. Exit codes are 0 for complete coverage of the selected checks for every source, 1 for a module counterexample, 2 for an input or tool error, and 3 for an inconclusive run. [Saved comparison and replay instructions](report-m1.md)

## Run the components independently

These commands run SMART, export the lowered properties and invoke veval's standalone checker on the same example.

```sh
mkdir -p crisp-out
out=$(mktemp -d crisp-out/token.XXXXXX)
./crisp mine examples/token/rtl/token.sv --top token --msa --simulator iverilog -j 4 \
    --keep-work --workdir "$out/mining" --output "$out/token_assertions.sv"
./crisp lower examples/token/token.sby --out "$out/lowering"
./crisp check examples/token/token.sby --lowering "$out/lowering" --out "$out/check"
cat "$out/mining/invariants.txt" "$out/check/check.txt"
```

SMART writes the assertions that passed its EBMC k-induction check to `mining/invariants.txt`. The standalone `token.sby` requests ABC PDR. veval lowers `p_no_conflict` to `token_formal.dut.state` and proves it there with that engine. The module verdict appears in `check/check.txt`.
