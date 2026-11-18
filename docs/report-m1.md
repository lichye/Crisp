# M1 — proving the token property with mined lemmas

`crisp verify` closed the module obligation for `token_formal.p_no_conflict` after SMART mined the relationship between the two token registers. Unassisted k-induction left the same obligation open. The only design shipped with Crisp is this token example. [Design](../examples/token/rtl/token.sv) · [Specification](../examples/token/spec.sv)

## Results

Every row uses the same RTL, wrapper, SMTBMC/Boolector engine and k-induction depth. The budget is wall time for the whole invocation, including decomposition, mining, independent lemma proofs and the final retry. A source assertion counts once if it has at least one selected check and all its selected checks were proved. The results describe the listed module instance. Chip-level composition remains an open project decision. [Recorded comparison](../examples/token/out/summary.json)

| Split | Lemmas | Selected instance | Depth | Budget | Sources with all selected checks proved | Module verdict |
|---|---|---|---:|---:|---:|---|
| Off | Off | `token_formal.dut` | 5 | 60 s | 0/1 | [Unknown](../examples/token/out/whole/report.json) |
| On | Off | `token_formal.dut.state` | 5 | 60 s | 0/1 | [Unknown](../examples/token/out/split/report.json) |
| On | On | `token_formal.dut.state` | 5 | 60 s | 1/1 | [Proved](../examples/token/out/split-lemmas/report.json) |
| Off | On | `token_formal.dut` | 5 | 60 s | 1/1 | [Proved](../examples/token/out/whole-lemmas/report.json) |

## Why the proof changes

`left` starts high and `right` starts low. An enabled update swaps the registers. The wrapper asserts that `conflict`, defined as `sample && left && right`, is always false. The induction base case passes. In the induction step, both registers can start high in an unreachable state. Keeping `sample` low satisfies the induction hypothesis for arbitrarily many cycles. Raising it on the next cycle violates the assertion. This accounts for the `unknown` result. [Base case](../examples/token/out/split/evidence/check-001/initial-basecase.log) · [Induction step](../examples/token/out/split/evidence/check-001/initial-induction.log)

At the `token` instance, SMART generated `left |-> !right` and `right || left`. The first predicate excludes the unreachable state. SMART checked its output with third-party EBMC. Crisp then proved each predicate independently on the exact RTLIL model from veval's original attempt, with the same clock, initialization and premises. The saved model for this instance has no external premises. [Mining record](../examples/token/out/split-lemmas/evidence/check-001/mining.json) · [Initial attempt](../examples/token/out/split-lemmas/evidence/check-001/initial.json)

For each lemma proof, Crisp removes the target assertion and checks the candidate as the sole assertion. The retry restores the target and assumes only the independently proved predicates. Each proof record identifies the model by SHA-256 and includes the SMT constraint, replay task and both solver logs. [First lemma](../examples/token/out/split-lemmas/evidence/check-001/lemma-001/proof.json) · [Second lemma](../examples/token/out/split-lemmas/evidence/check-001/lemma-002/proof.json) · [Retry](../examples/token/out/split-lemmas/evidence/check-001/retry/proof.json)

This comparison fixes k-induction as the proof method in all four modes. The separate `crisp check` example uses ABC PDR, which proves this property without helper lemmas. The result demonstrates automatic lemma reuse on one design. [Standalone task](../examples/token/token.sby)

## Reproduce

From the repository root, run `make example` after building the two local components. It creates a fresh `crisp-out/token.*/` with the full work directories, individual reports and `summary.json`. The snapshot in `examples/token/out/` came from a clean local checkout of source revision `75993d7`. Every saved SymbiYosys task reproduced its recorded status when replayed in a fresh output directory. [Build instructions](quickstart.md) · [Comparison script](../examples/token/reproduce.py)

To replay an individual proof with the same OSS CAD Suite on `PATH`, run the following from the repository root. The temporary directory keeps the snapshot clean.

```sh
out=$(mktemp -d)
(cd examples/token/out/split-lemmas/evidence/check-001/lemma-001 && sby -d "$out/lemma" proof.sby)
(cd examples/token/out/split-lemmas/evidence/check-001/retry && sby -d "$out/retry" proof.sby)
```

The regression tests inject a false candidate and verify that the base-case counterexample prevents it from entering the retry. A second test adds an assumption and a false assertion over a wrapper input, and checks the whole-DUT verdict, premises and retained waveform. Run `python3 -m unittest discover -s tests` with the components built. [Tests](../tests/test_verify.py)
