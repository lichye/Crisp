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

## Run

Use a fresh output directory for each run. All three commands operate on the same example.

```sh
mkdir -p crisp-out
out=$(mktemp -d crisp-out/token.XXXXXX)
./crisp mine examples/token/rtl/token.sv --top token --msa --simulator iverilog -j 4 \
    --keep-work --workdir "$out/mining" --output "$out/token_assertions.sv"
./crisp lower examples/token/token.sby --out "$out/lowering"
./crisp check examples/token/token.sby --lowering "$out/lowering" --out "$out/check"
cat "$out/mining/invariants.txt" "$out/check/check.txt"
```

SMART writes the assertions that passed its EBMC k-induction check to `mining/invariants.txt`. veval lowers `p_no_conflict` to `token_formal.dut.state` and proves it there with SymbiYosys and ABC PDR. The module verdict appears in `check/check.txt`. Mining and checking currently run independently. Feeding a mined lemma into a module proof is part of M1.
