# Crisp

Crisp is a model checker for SystemVerilog designs. Run the Linux x86-64 release binary on a file containing an assertion.

```sh
./crisp design.sv
```

Crisp finds the top-level modules and checks the assertions in the elaborated design using the EBMC engine linked into the executable. Use `--top MODULE` to select a particular root. The terminal and a plain-text file in `log/` show each verdict and any counterexample. The default time limit is 60 seconds. Use `--budget SECONDS` to change it, `--out DIRECTORY` to select a log directory, or `--log FILE.log` to name a new log.

The release includes one self-contained example. Running `./crisp token.sv` proves that the two registers never hold the token simultaneously when sampled. A failed assertion exits with 1, an input or engine error with 2, and an inconclusive check, timeout or design without assertions with 3. A completed proof of all assertions exits with 0.

## Component workflows

The optional `components.tar.gz` beside the binary supplies SMART, veval, SBY, the matching Yosys tools and their runtime dependencies. Crisp opens it in a temporary system directory when a component command runs.

| Command | Result in the text log |
| --- | --- |
| `./crisp mine design.sv` | Assertions mined by SMART |
| `./crisp lower task.sby` | Lowered properties and the module instance for each |
| `./crisp check task.sby` | veval module verdicts and counterexamples |
| `./crisp verify design.sv --top wrapper --spec spec.sv` | Selected module checks and independent proofs of the SMART lemmas used in a retry |

For `verify`, `--split on|off` controls module decomposition and `--lemmas on|off` controls mining. Both default to `on` for this command. A reported proof is scoped to the selected module and its recorded premises.

### SMART with a .salt task

Run `make smart` to build SMART against Crisp's hw-cbmc submodule and install
the pinned cvc5 package. Put Verilator on `PATH`, or run `make deps` to install
the external simulator suite. `src/components/smartVerilog/build/smart --check-env`
prints the tools it will use.

Writing `smart on` under `[options]` runs the miner before checking
the original properties. SMART writes the checked assertions to
`crisp-out/<salt-name>[_<task>]/smart_assertion.sv`. Crisp then checks the
original design with its original assumptions. When `top` is omitted, SMART
uses EBMC's sole uninstantiated module. Set `top` explicitly if several
modules qualify. Tasks that use `[design] include` or `define` can continue
to run with `smart off`.

Run `make test-smart` with Verilator, Icarus, and Python 3 on `PATH` to check
the SMART integration.

## Build from source

To build only the standalone checker, install a C++17 compiler, GNU Make, Flex, Bison, Git, curl and patch. The build fetches pinned engine sources into a private build cache.

```sh
make binary
./build/crisp examples/token/token.sv
```

For the component workflows on Ubuntu 24.04 x86-64, install the host development packages and put Spot's `ltl2tgba` on `PATH`. [Spot installation](https://spot.lre.epita.fr/install.html)

Run `make slang` before `make test`: by default (`rtl-mode strict`) each `.salt` task first checks the RTL with slang, built from the pinned `src/components/slang` submodule; set `CRISP_SLANG` to use another slang, or write `rtl-mode relaxed` under `[options]` to skip the check.

Writing `veval on` or `veval only` under `[options]` lowers the assertions with veval and checks each landing instance with Crisp's EBMC; `veval on` then checks what veval did not prove on the whole design. SMART and veval are off unless written. veval, `mode kind`, and `mode pdr` need Spot's `ltl2tgba` on `PATH`. Running `make test` requires it too.

Tasks with `level word` run EBMC's word-level SMT and need Yices's `yices-smt2` on `PATH`, for example from the OSS CAD Suite. `make test` requires it.

```sh
sudo apt-get install build-essential cmake flex bison git curl patch ca-certificates python3-pip libsqlite3-dev libssl-dev
make deps && make build
make test
make example
make release
```

`make release` creates `release/crisp` for single-file checking and `release/crisp-linux-x86_64.tar.gz` with the optional component workflows and token example. `./crisp --licenses` prints the notices for the standalone binary.
