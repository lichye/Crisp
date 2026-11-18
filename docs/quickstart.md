# Run the token example

Two registers hold a token. They start as `1` and `0` and exchange values on an enabled step. The top-level property says that sampling the registers can never signal a conflict. [RTL](../examples/token/rtl/token.sv) · [Property](../examples/token/spec.sv) · [Proof task](../examples/token/token.sby)

## Build

Run the following from the Crisp repository root. On Ubuntu 24.04, these packages provide the host compiler, build tools and development headers.

```sh
sudo apt-get update
sudo apt-get install build-essential cmake flex bison git curl patch ca-certificates python3-pip libsqlite3-dev libssl-dev
```

On a fresh Linux x86-64 checkout, install the project dependencies as your regular user and then compile. The first command downloads and builds the pinned hw-cbmc (including EBMC) and cvc5, and installs the OSS CAD Suite used by both components.

```sh
make deps && make build
```

If a previous build stopped with `hw-cbmc is not installed`, run the same commands in your existing checkout. Later builds only need `make build`. Both steps accept `JOBS=N` to control build parallelism. Crisp installs OSS CAD Suite 2026-09-01 under `components/smartVerilog/.deps/`. This release can load plugins built with Ubuntu 24.04's GCC 13. To reuse a compatible installation, export its absolute bin path (for example, `export OSS_CAD_BIN=/opt/oss-cad-suite/bin`) before setup and use the same setting at runtime.

For `verify`, `lower`, `check` and `make example`, install Spot and put `ltl2tgba` on `PATH`. The saved M1 run used Spot 2.15.1 and OSS CAD Suite 2025-06-04. [Spot installation](https://spot.lre.epita.fr/install.html) · [Component credits](../PROVENANCE.md)

If you already have complete SMART dependencies in another checkout, you can point the build at them directly. `SMART_DEPS_ROOT` contains the built `hw-cbmc/` tree and `include/nlohmann/json.hpp`. `CVC5_ROOT` contains the cvc5 headers and libraries. With matching Yosys development headers available, the build command is

```sh
make build SMART_DEPS_ROOT=../smartVerilog/.deps CVC5_ROOT=../smartVerilog/otherTools/cvc5
```

`make veval` builds the veval component independently.

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
