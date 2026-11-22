# Crisp

Crisp checks SystemVerilog assertions with the EBMC engine linked into the executable.

## Quick start from source

Install GNU Make, a C and C++ toolchain with C++20 support, Flex, Bison, CMake 3.20
or later, Python 3, Git, curl, tar, and patch. The build fetches pinned sources
and dependencies.

From the Crisp checkout, run the included token proof.

```sh
make crisp
make slang
./build/crisp tests/cases/token.salt prove
```

The proof exits with status 0 and reports `token_formal.p_no_conflict` as
`proved`. Crisp prints the path of the per-run TSV under
`crisp-out/token_prove/`. Counterexample waveforms, when available, are VCDs
in the corresponding task output directory.

Crisp takes a `.salt` file and optional task names. Use the `[files]` section
for sources, `[design]` for the top module, `[options]` for the verification
mode, and `[resource]` for the timeout. The default timeout is 60 seconds.
Run `./build/crisp --help` for the command syntax.

The default `rtl-mode strict` checks the design with slang first. `make slang`
places the executable at `src/components/slang/build/bin/slang`. Set
`CRISP_SLANG` to use a different executable or select `rtl-mode relaxed` in
`[options]` to skip the check.

Exit status 1 means an assertion was refuted, 2 indicates an input or engine
error, and 3 indicates an incomplete result, including a timeout, a vacuous
result, or a task with no assertions.

## Optional components and tests

Run `make test` with Spot's `ltl2tgba` and Yices's `yices-smt2` on `PATH`.
Word-level SMT tasks (`level word`) use Yices.

### SMART

Run `make smart` to build SMART against Crisp's hw-cbmc submodule and install the
pinned cvc5 package. Put Verilator on `PATH` or run `make deps` to install the
external simulator suite. Run
`src/components/smartVerilog/build/smart --check-env` to see which tools it uses.

Set `smart on` under `[options]` to mine assertions before verifying the original
design. SMART writes its checked assertions to
`crisp-out/<salt-name>[_<task>]/smart_assertion.sv`. Set `top` explicitly if
several modules are uninstantiated. Use `smart off` for tasks that require
`[design] include` or `define`. Run `make test-smart` with Verilator, Icarus,
and Python 3 to check the SMART integration.

### veval

veval, `mode kind` and `mode pdr` require Spot's `ltl2tgba` on `PATH`. Set
`veval only` under `[options]` to lower
assertions and check their landing instances. `veval on` also checks remaining
assertions on the whole design. Crisp links its veval support into the checker.
Run `make veval` to build the standalone veval program.

## Optional lasso certificates

Set `CRISP_LASSO_JSON=1` to request an independently replayable JSON sidecar
for an explicit lasso found by EBMC on a whole-design safety check.

```sh
CRISP_LASSO_JSON=1 ./build/crisp tests/cases/lean-lasso.salt
```

Crisp prints a separate `witness` path under the task output directory. The name
identifies the progress file for that run. The sidecar contains the compiled
initial predicate, invariants, transitions, active assumptions and assertion,
plus the sampled scalar values, including RTL and monitor state. Its final
frame repeats the indexed loop entry. The verifier evaluates the back edge
against the transition expression and confirms the assertion fails at the
stated tick.

Version `crisp-safety-lasso-v1` supports Boolean and finite integer scalar
values and a restricted set of expressions. Unsupported expressions or absent
sample values produce a warning. VCD and verdict reporting continue normally.
Total-prefix and exact BDD verdicts have no lasso sidecar. Check that the
sidecar came from the desired Crisp run. Replay establishes correctness
relative to the exported compiled model.
