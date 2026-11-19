# Module checking

Both entry points report the engine verdict at the checked module. They do not
lift a local proof or counterexample to a source-level conclusion.

| Directory | Responsibility |
|---|---|
| [prepare/](prepare/) | Read generated specifications, prepare Jobs and annotate missing environment dependencies. |
| [run/](run/) | Select checks, execute them, resume saved attempts and enforce resource limits. |
| [results/](results/) | Report per-module Results and check progress. |

| Files | Responsibility |
|---|---|
| [CheckSpecCommand.cpp](CheckSpecCommand.cpp) | Parse `veval check DESIGN.sby` and start lowering/checking. |
| [CheckSavedLowering.cpp](CheckSavedLowering.cpp) | Check an existing lowering bundle without generating it again. |

`auto` and `deepest` select the deepest available landing on every generated
branch of each source. A deeper landing on one branch does not suppress a
shallower sibling. After a completed counterexample, `auto` executes the next
available ancestor check for the same source, stopping at a proof, an
inconclusive result or the last landing below Top. These are independent
solver runs; their per-module results are retained without a source verdict.
`auto` includes Top-only cases. `deepest` and `all-depth` exclude Top and do
not change their selection after a result. `top` explicitly checks the starting
DUT. Resume retains completed checks, including the ancestor checks.

The former correspondence, proof-reuse and trace-replay implementation has
been removed, together with its executable targets. Shared local-specification
reading is in `prepare/`. SBY input generation, external engine calls and trace
readers live in [util/sby](../util/sby/).
