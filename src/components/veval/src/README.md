# Source guide

The source is grouped by function: [frontend](frontend/),
[lowering](lowering/), [check](check/) and shared [util](util/) support.
There is no separate `core/` or `workflow/` directory.

Start at [main.cpp](main.cpp). For generation, read
[RunLowering.cpp](lowering/RunLowering.cpp) and
[LoweringStore.cpp](lowering/LoweringStore.cpp). For checking, read
[RunPropertyChecks.cpp](check/run/RunPropertyChecks.cpp) or
[CheckSavedLowering.cpp](check/CheckSavedLowering.cpp) for an existing lowering bundle.

| Directory | Responsibility |
|---|---|
| [frontend/](frontend/) | Input preparation, RTL elaboration and SVA monitor compilation, grouped in `input/`, `rtl/` and `sva/`. |
| [lowering/](lowering/) | Connectivity, symbolic substitutions, propagation, local environments and products. |
| [check/](check/) | Jobs, external module checks and local result reports. |
| [util/](util/) | CLI/input adaptation, process/file support, shared storage, Verilog expression output, SBY adaptation and Yosys plugins. |
| [../tools/evaluation/](../tools/evaluation/) | Separate experiment scheduling and measurement programs. |

CMake compiles the Verilog/SVA frontend and CBMC expression libraries from
Crisp's pinned [hw-cbmc](../../hw-cbmc/) and [CBMC](../../hw-cbmc/lib/cbmc/).

The largest former flat directories are grouped into concrete responsibilities
(such as check preparation, SBY, processes and storage). Each directory README lists its
files and responsibilities. Unit tests remain
in `test/`. Separate research prototypes are outside this build. The imported
hw-cbmc files retain their [licence and origin](frontend/rtl/HwCbmcProvenance.md).

## Correspondence with the paper

| Paper step | Source location |
|---|---|
| Connectivity relation | [ConnectionMap](lowering/connectivity/ConnectionMap.cpp) and [SignalReachability](lowering/connectivity/SignalReachability.cpp). |
| Symbolic dependency map | Imported [RTL synthesis](frontend/rtl/HwCbmcVerilogSynthesis.cpp) and [Symbolic](lowering/Symbolic/) substitutions. |
| Dependency graph and lowering | Elaborated hierarchy, connections and relations consumed by [PropertyLowering](lowering/propagation/PropertyLowering.cpp); [HierarchyGraph](lowering/reports/ExportHierarchyGraph.cpp) exports the view. |
| Module-level assumptions | [Local environment](lowering/environment/LocalEnvironment.cpp) and [AssumptionDependencies](lowering/environment/AssumptionDependencies.cpp). |
| Module checks | [Check workflow](check/run/RunPropertyChecks.cpp) and [SBY adapter](util/sby/). |

## Current cleanup status (2026-11-14)

File relocation and renaming are followed by removal of the old lifting path:
source correspondence checks, proved-child guarantees, source qualification
and ancestor replay. Shared product reading remains in `check/prepare`; local
trace handling remains in `util/sby`. The `check` report now contains local
verdicts rather than source conclusions. `--reuse`/`veval_reuse` are removed.
Lowering and `consflow_check` remain available, including stored-bundle reuse.
Some internal namespaces retain their historical names; they do not imply
that the removed functionality is still built.

Frontend files are grouped into input preparation, RTL and SVA. Shared
expression output lives in `util/verilog`; the unused `BuildModuleHarness`
implementation has been removed. The explicit `freex` rewrite is named
`input/ReplaceDontCareConstants` and remains opt-in.

The former `lowering/storage` files now live in `util/database` for lowering
records and saved annotations, and `util/task` for generation metadata.
This move preserves the existing storage formats and interfaces.

The directory cleanup built on Linux with `make build BUILD_JOBS=4`.
`make test BUILD_JOBS=4` passed all 54 CTest tests and eight CLI regression
suites, including actual lowering and local SBY checks. Frozen experiment
binaries and data have not been replaced; no paper benchmark was rerun.
The macOS build branches remain, but this run only tested Linux.

The subsequent readability pass separates classification, rendering, premise
collection, queue selection and result import within the existing files.
It removes repeated expression rewrites, name scans and index construction,
unused state and historical commentary. Public interfaces and output formats
are preserved; CLI help now correctly describes local results.

SBY input preparation separates clock selection, reset contracts and encoding
fallback. The runner separates stage execution, premise checks and liveness
trace checks. Batch scheduling separates budget overrides, worker startup and
saved-result recovery. Cancellation, process ownership and proof semantics
retain their existing behavior.

Saved-bundle checks separate template selection, runtime identification,
execution and result storage. Single-Job execution separates SBY requests,
EBMC status interpretation and scratch cleanup. Graph reporting collects
usable premises and routes in one pass; the coverage report retains its
partition check.

Job preparation separates engine options, per-target construction and export.
Schema validation reuses array-item rules and reads shared values by reference.
Environment annotation separates cache matching, query construction and output;
cache matching no longer copies all target sentences.

Each step passed its related unit and integration checks. Comparisons cover
88 lowering runs, 36 SVA inspections, 24 connection/reach inspections,
21 real solver cases, 13 adapter result cases, 21 saved-bundle selections
and seven queue preparations. Generated inputs and
stable records match their saved baselines; solver timing is not a golden
output. The newly added cases cover dependency cycles, encoding fallback
state, reset ambiguity and unsupported names. This is separate from the
earlier 54-test directory-refactor run above. Job preparation also has 26 option
comparisons, 25 validation cases that launch no engine, and cached/environment
failure comparisons. Generation timestamps are normalized only in comparisons
whose Job identity includes the generated configuration digest.

After all fourteen cleanup steps, `make test BUILD_JOBS=4` passed the complete
56-test CTest set and all eight CLI suites on Linux. The changes remain on
`clean`, uncommitted and unpushed.
