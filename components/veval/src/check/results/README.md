# Local check reports

| Files | Responsibility |
|---|---|
| [CheckReport.cpp](CheckReport.cpp) / [CheckReport.h](CheckReport.h) | Report each module's engine verdict, assumptions, environment annotations and progress. |
| [ExportResultTables.cpp](ExportResultTables.cpp) | Export tables from lowering and recorded local engine results. |

The `veval-check-report/2` report groups checks by source label for navigation.
A property's `status` is progress (`checked`, `partial`, `pending`, etc.). Logical
verdicts belong only to its `attempts`/`landings`, each naming the checked instance.
`check_counts` counts those local checks. There is no source qualification,
proof dependency, proof reuse or ancestor replay stage.
