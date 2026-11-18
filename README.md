# Crisp

Crisp (Compositional RTL Invariant Synthesis and Proof) is a toolkit for checking RTL properties and mining invariants. veval lowers SystemVerilog assertions into module-level obligations and checks them. smartVerilog provides SMART assertion mining and MAPminer parallel mining. SMART uses EBMC, the third-party model checker from the hw-cbmc project. [Component provenance](PROVENANCE.md)

Crisp's command-line and verification code lives in `src/`. The tools in `components/` build as separate executables. Run `./crisp mine` to mine assertions, `./crisp lower` to generate module obligations, or `./crisp check` to verify them.

## Verification prototype

`./crisp verify` calls the component executables to check a property and try mined lemmas. `--top` names the specification wrapper. The switches `--split on|off` and `--lemmas on|off` work independently and both default to `on`.

With splitting enabled, veval selects module-level proof obligations. With splitting off, it checks the intact design under test. If the induction step stays open on an eligible single-clock safety property and lemma synthesis is enabled, SMART mines candidate invariants. Crisp independently proves each supported one-bit, single-frame predicate on the same module model under the original premises. It retries the property using only the predicates it proved.

Each report associates a module instance and a source assertion with the verdict, premises and proof evidence. [Token example report](examples/token/out/split-lemmas/report.json)

## Quick start

On Ubuntu 24.04 x86-64, install the host build tools and development headers.

```sh
sudo apt-get update
sudo apt-get install build-essential cmake flex bison git curl patch ca-certificates python3-pip libsqlite3-dev libssl-dev
```

From the Crisp root, fetch the pinned dependencies once and build both components.

```sh
make deps && make build
```

The repository ships one small token example. Two registers start with one token between them and swap values on an enabled step. The assertion checks that they are never both high when sampled. To run it, put Spot's `ltl2tgba` on `PATH`. [Spot installation](https://spot.lre.epita.fr/install.html) · [Token RTL](examples/token/rtl/token.sv) · [Specification wrapper](examples/token/spec.sv)

```sh
./crisp verify examples/token/rtl/token.sv --top token_formal --spec examples/token/spec.sv
```

The command writes `crisp-out/run.*/report.json`. To run all four switch combinations and compare them in `crisp-out/token.*/summary.json`, use the example target.

```sh
make example
```
