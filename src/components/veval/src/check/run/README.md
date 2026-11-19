# Check execution and scheduling

Run individual or batched Jobs, coordinate resources and sequence the complete
verification flow. The external SBY adapter is in [util/sby](../../util/sby/), while
OS process supervision is in [../../util/process/](../../util/process/).

| Files | Responsibility |
|---|---|
| [RunPropertyChecks.cpp](RunPropertyChecks.cpp) / [RunPropertyChecks.h](RunPropertyChecks.h) | Select and sequence local/root checks and collect their outcomes. |
| [CheckMemoryAdmission.cpp](CheckMemoryAdmission.cpp) / [CheckMemoryAdmission.h](CheckMemoryAdmission.h) | Admit batch checks using the shared memory budget, worker count and observed usage. |
| [RunBatch.cpp](RunBatch.cpp) | Run a collection of independent Jobs. |
| [RunJob.cpp](RunJob.cpp) | Dispatch one Job to a supported external engine and normalize its Result. |

Within `RunPropertyChecks`, `selectChecks` orders the queue and selects its
landings before any results are read. `importAttempts` handles saved, interrupted
and newly completed attempts. `workflow` sequences preparation, execution and
publication. In `auto`, a completed counterexample admits the next executable
ancestor from the same source via `AncestorRecheck.h`. A proof, bounded pass,
unknown result or error stops that branch. This ascent excludes Top; `auto`
continues to check Top-only properties directly, as before.
The scheduler does not lift module results to source conclusions.

`RunBatch` separates queue preparation and resource overrides from worker
startup and saved-result recovery. Cancellation retains completed work and
requeues interrupted Jobs only after their processes have been reclaimed.

`RunJob` validates inputs, dispatches to SBY or external EBMC, then publishes
the Result before compacting scratch files. Its engine-specific status readers
keep bounded checks distinct from proofs and require a premise audit for
counterexamples.
