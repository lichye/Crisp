# SymbiYosys adapter

Prepare RTL/monitor inputs, run SBY stages and interpret their results.
Job selection and batch policy live in [check/](../../check/); process supervision
lives in [process/](../process/). This directory does not implement a model checker.

| Files | Responsibility |
|---|---|
| [SbyCommand.cpp](SbyCommand.cpp) | Expose the SBY adapter command. |
| [SbyHarness.cpp](SbyHarness.cpp) / [SbyHarness.h](SbyHarness.h) | Prepare SBY-compatible RTL and monitor harnesses. |
| [SbyRunner.cpp](SbyRunner.cpp) / [SbyRunner.h](SbyRunner.h) | Run SBY stages and distinguish proof, base-case counterexample and unclosed induction. |
| [ReadArrayTrace.cpp](ReadArrayTrace.cpp) | Prepare exact array observation bindings for the local SBY model. |
| [ReadLivenessTrace.cpp](ReadLivenessTrace.cpp) | Check the local liveness reduction's concrete lasso trace. |

`SbyRunner` reads compiler receipts, launches each ladder stage and audits
counterexample premises or liveness lassos. `SbyHarness` selects the sampling
clock, prepares reset/derived premises and encodes each selected property.
Encoding fallback restores the previous register state before invoking the
native SVA compiler; unsupported assumptions still stop the check.
