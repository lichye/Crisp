# Check preparation

Create and validate Jobs from lowering products, including their assumptions,
clock/reset context and design constants. Running them is handled by [../run/](../run/).

| Files | Responsibility |
|---|---|
| [ResolveEnumConstants.cpp](ResolveEnumConstants.cpp) | Resolve compilation-unit enum constants using the same frontend and source context as lowering. |
| [GenerateJobs.cpp](GenerateJobs.cpp) | Parse engine settings, build Jobs with shared inputs, and export selected checks to files or SQLite. |
| [AuditJobEnvironment.cpp](AuditJobEnvironment.cpp) / [AuditJobEnvironment.h](AuditJobEnvironment.h) | Annotate missing local environment dependencies after Job preparation; do not change products or verdicts. |
| [ProjectEnvironment.cpp](ProjectEnvironment.cpp) / [ProjectEnvironment.h](ProjectEnvironment.h) | Build a small private view of an elaborated netlist for the environment audit; preserve full queried subtrees and ancestor logic, and retain adjacent module interfaces. |
| [JobSchema.cpp](JobSchema.cpp) / [JobSchema.h](JobSchema.h) / [JobSchema.inc](JobSchema.inc) | Define and validate the Job/Result interchange contracts. |
| [ReadLocalSpec.cpp](ReadLocalSpec.cpp) / [ReadLocalSpec.h](ReadLocalSpec.h) | Read generated assertions, assumptions and reset metadata for a local check. |
| [ResolveSourcePaths.h](ResolveSourcePaths.h) | Resolve the original input paths used by enum-constant extraction. |

Environment annotations describe structural dependencies. Matching cached targets
can be reused for a smaller selection; a changed target requires a fresh audit.
The audit does not alter property text, Job identity or solver verdicts.
Input projection applies only when module names match standalone Yosys-generated
source definitions. Parameterized or preprocessed source keeps the original
input route. If Yosys rejects the projected view, the original inputs are retried.
