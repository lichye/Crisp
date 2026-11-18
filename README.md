# Crisp

Crisp (Compositional RTL Invariant Synthesis and Proof) checks RTL against named properties in a SystemVerilog specification wrapper. It combines veval's property decomposition and module checking with SMART assertion mining and MAPminer parallel mining from smartVerilog. SMART uses EBMC, the third-party model checker from the hw-cbmc project. [Component provenance](PROVENANCE.md)

## Verification flow

`--top` names the specification wrapper. The switches `--split on|off` and `--lemmas on|off` work independently and both default to `on`.

With splitting enabled, veval selects module-level proof obligations. With splitting off, it checks the intact design under test. If the induction step stays open on an eligible single-clock safety property and lemma synthesis is enabled, SMART mines candidate invariants. Crisp independently proves each supported one-bit, single-frame predicate on the same module model under the original premises. It retries the property using only the predicates it proved.

Each report associates a module instance and a source assertion with the verdict, premises and proof evidence. [Token proof walkthrough](docs/report-m1.md)

## Quick start

Install the required dependencies, then build Crisp from the repository root. [Dependency and setup guide](docs/quickstart.md)

```sh
make build
```

The repository ships one small token example. Two registers start with one token between them and swap values on an enabled step. The assertion checks that they are never both high when sampled. [Token RTL](examples/token/rtl/token.sv) · [Specification wrapper](examples/token/spec.sv)

```sh
./crisp verify examples/token/rtl/token.sv --top token_formal --spec examples/token/spec.sv
```

The command writes `crisp-out/run.*/report.json`. To run all four switch combinations and compare them in `crisp-out/token.*/summary.json`, use the example target.

```sh
make example
```

You can also run `./crisp mine` to mine and check assertions, `./crisp lower` to generate module obligations, or `./crisp check` to verify them independently. [Standalone command examples](docs/quickstart.md#run-the-components-independently) · [Project plan](docs/project-plan.md)
