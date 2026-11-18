# Crisp

**C**ompositional **R**TL **I**nvariant **S**ynthesis and **P**roof.

The current checkout provides `./crisp mine`, `./crisp lower` and `./crisp check`.
The token example shows how to mine an invariant and how to lower and prove a property on the same RTL. [Build and run the example](docs/quickstart.md)

Sections 0–3 describe the research goal and first milestone. Automatic reuse of mined lemmas in module proofs is still open. Section 5 holds the project rules and section 6 lists decisions for Leiqi.

## 0. Read this first

**The idea in one sentence.**
Crisp is a full formal verification toolkit for Verilog that scales to specification verification of real chips: it splits each proof soundly and automatically, and it generates helper lemmas to close the proof.

**Why it exists.**
Hardware is now generated faster than experts can specify and verify it.
A tool that needs an expert to drive it falls behind.
Crisp runs with no expert in the loop: the user supplies a design and its chip-level specification, and Crisp does everything else.

**In Leiqi's words** (2026-11-18):

> 一个完整的 Verilog formal verification 工具组件，基于 Yosys 和我自己工作的扩展，总体来说是 sound & automation 的 decompose 问题和生成辅助 lemma 解决证明问题。
>
> Scale up to real chip.
>
> 任何把 expert 当主导的工具都很大程度上会被一脚踹死。

**Three questions guiding Crisp's design.**

1. *What does each module have to satisfy?* Crisp splits a chip-level property into module-level properties.
2. *What extra facts does each module proof need?* Crisp generates helper lemmas and proves every lemma before using it.
3. *What does a module-level proof say about the chip?* The report identifies the source property, module instance and assumptions. A chip-level verdict requires a sound composition rule (D5).

## 1. What Crisp is built from

Crisp brings together three lines of research from the smartVerilog and veval repositories.
EBMC is a third-party model checker from the hw-cbmc project. SMART uses it to check candidates and prove the assertions it emits. [Source revisions and credits](PROVENANCE.md)

| Line | Tool today | Location | What it gives Crisp |
|---|---|---|---|
| Specification mining | SMART (FMCAD 2025) | `/home/magna/Projects/research/smartVerilog`, public at `github.com/lichye/smartVerilog`, branch `master` | Helper lemmas. SMART mines SystemVerilog assertions from simulation traces with SyGuS (cvc5), refutes candidates with EBMC, and emits only the assertions it proves by k-induction. |
| Parallel mining | MAPminer (CAV 2026) | Same repository: the blockified mode (`-j`, `--msa`, `smart/src/mus/`) | Scale for lemma generation: many small synthesis problems solved in parallel. |
| Decomposition | veval | `/home/magna/Projects/research/veval`, private | The sound split. `veval lower` restates a chip-level property at the modules below; `veval check` checks each module alone with SymbiYosys. |

Read these first:

- `smartVerilog/README.md` §4 "How a run works", and `smartVerilog/AGENTS.md`.
- `veval/README.md`, then `veval/src/README.md` (file map and the table linking each step of the method to its source file), then `veval/Agent.md`.
- `veval/srcProof/` holds the Coq models of the decomposition method.

The local component builds live under `components/smartVerilog/build/` and `components/veval/build/`. Their respective checkers and frontends have different roles.

- SMART proves and refutes with EBMC. veval checks with SymbiYosys and Yosys. Crisp is Yosys-based. Decision D2 covers this gap.
- veval reports module-level verdicts. Its code for chip-level conclusions was removed on 2026-11-14. Decision D5 covers chip-level verdicts in Crisp.
- Both tools read Verilog through the hw-cbmc frontend (`smartVerilog/.deps/hw-cbmc`, `veval/src/frontend/hw_cbmc`). The shared frontend is the natural joint for the fusion.

## 2. Target behaviour

```
crisp verify <design files> --top <top> --spec <chip-level properties>
```

1. **Read** the design and the chip-level specification.
2. **Split** each chip-level property into module-level properties (the veval line).
3. **Prove** each module-level property. When a module proof needs more facts, generate helper lemmas for that module (the SMART and MAPminer line), prove each lemma, then use the proved lemmas in the module proof.
4. **Report**, for each chip-level property: the module-level properties it was split into, the verdict of each, and the helper lemmas each proof used.

Crisp is one tool, deeply fused (Leiqi, 2026-11-18).
Splitting and helper-lemma generation are capabilities of that one tool, and the user decides per run which capabilities are on:

```
crisp verify ... --split on|off --lemmas on|off
```

With both switches off, Crisp checks the whole design directly.
Each capability also runs alone: mining proved assertions for a design, or splitting a property and stopping there.

Soundness rules. A run that breaks one of them is a bug.

- **S1.** A helper lemma is used only after it is proved on the same module under the same assumptions.
- **S2.** Every verdict names the module and the property that was actually checked.
- **S3.** A failed module check is reported as a module-level counterexample, with its trace, for audit.

Automation rule.

- **A1.** No step asks a person for a template, an invariant, a module boundary or a tuning choice. Every option has a default.

Usage rule.

- **U1.** The user chooses which capabilities run. Every capability works alone and in every combination with the others.

## 3. First milestone (M1): one command, one small design, end to end

**Goal.** Show the whole idea working once, on a design small enough to read in one sitting.

**Deliver.**

- A `crisp` command in this repository that runs steps 1–4 on top of `veval` and `smart`, with the two switches `--split` and `--lemmas`. Both tools are Leiqi's own; change them as much as the milestone needs, in Crisp's own copies (rule 1).
- One small hierarchical design with one chip-level property, under `examples/`, where the property is split into module-level properties and at least one module proof fails or times out alone and goes through with helper lemmas.
- The saved output of that run under `examples/<name>/out/`.
- `docs/report-m1.md`.

**Acceptance.**

- One command reproduces the run from a clean checkout of Crisp, with the two source tools built.
- Every helper lemma in the report has its own proof record (S1).
- The report gives three rows with the same property count N and the same time budget: both switches off (the baseline: the whole design checked directly) proves x/N; split only proves y/N; split with lemmas proves z/N.

**A clear "this cannot be done" is a valid result.**
If a step stays impossible after changing the tools — for example a module proof that no generated lemma closes — stop, write down the cause in plain words, and attach a reproducer.
Name the cause (for example "smart rejects concurrent assumes"); a code such as "C3" is a cross-reference only.

**Out of scope for M1.** Performance work and real-chip benchmarks. M1 starts the fusion wherever the end-to-end run needs it; completing the fusion is M2.

## 4. After M1

An outline only. Leiqi rules on each step before work starts.

- **M2. Deep fusion.** One code base, one build, one command line: one frontend, one design representation and one proving back end shared by every capability, with the capabilities still switchable (U1). Settle the proving engine first (D2).
- **M3. Real chips.** Any evaluation uses designs maintained outside Crisp. The tool ships only the small token example.
- **M4. Release.** Public repository, licence, documentation (D3, D4).

## 5. Rules

1. **Change veval and SMART freely, in Crisp's own copies.** Both tools are Leiqi's own, and Crisp is free to rework them deeply (Leiqi, 2026-11-18). Bring a copy of each into this repository, record the source commit of each copy in `PROVENANCE.md`, keep the licence files of the imported hw-cbmc code, and make every change in the copies. The original working trees (`research/smartVerilog`, `research/smartVerilog-subsets`, `research/veval` and its worktrees) are in use by other agents: never edit, commit, stash, checkout or reset there.
2. **Everything stays on this machine.** veval implements a paper that is under double-blind review. Create no public remote, push nowhere, publish nothing, and send no code or results to outside services. Leiqi decides when Crisp becomes public.
3. **One example.** Ship one small Verilog example. Benchmark suites and their evaluation data stay in their source repositories.
4. **Numbers come from saved artifacts.** Every number in a report is read from a file that the run saved, and the report gives the path.
5. **Report format.** For each result give the baseline x/N and Crisp y/N with the same N, and put the budgets side by side.
6. **Code comments are in English.**
7. **Commit to this local repository as the work proceeds. Push only when Leiqi asks.**
8. **The decisions in §6 belong to Leiqi.** Bring one decision at a time, with the options and a recommendation.

## 6. Open decisions for Leiqi

- **D1. How the three lines join.** Ruled 2026-11-18: deep fusion. Crisp carries its own copies of veval and SMART, changes them freely, and fuses them into one tool whose capabilities the user switches on or off per run.
- **D2. The engine that proves helper lemmas inside Crisp.** SMART uses EBMC today; Crisp is Yosys-based.
- **D3. When Crisp becomes public, and under which licence.** veval carries a BSD-style licence text; smartVerilog has no licence file.
- **D4. Repository identity.** Rename the veval repository to Crisp, or keep Crisp as its own repository.
- **D5. Chip-level verdicts.** Whether Crisp reports a chip-level "proved" on top of the module-level verdicts, and under which conditions. Until Leiqi rules, Crisp reports module-level verdicts and lists which chip-level property each one belongs to.
