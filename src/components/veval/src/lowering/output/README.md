# Generated local specifications

Print module-local SVA and coordinate product/log export. Persistent generation
records live in [util/database](../../util/database/), configuration metadata in
[util/task](../../util/task/), and human-readable graph/verdict summaries in
[reports/](../reports/).

| Files | Responsibility |
|---|---|
| [ExportLoweringResults.cpp](ExportLoweringResults.cpp) / [ExportLoweringResults.h](ExportLoweringResults.h) | Write lowering summaries, generated specifications and manifests. |
| [WriteLocalSpecs.cpp](WriteLocalSpecs.cpp) / [WriteLocalSpecs.h](WriteLocalSpecs.h) | Print module-local SVA wrappers, environment comments and optional explanation files. |

In `WriteLocalSpecs.cpp`, start at `writeModuleSpecs` for the output flow.
`classifyProductEntries` selects entries and counts their coverage;
`printedExpression` prepares a property once for its local declarations,
function definitions and SVA text. `writeRouteComment` handles route metadata.
