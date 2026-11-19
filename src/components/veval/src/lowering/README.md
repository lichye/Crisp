# Specification lowering

Build connectivity and symbolic relations, select legal property locations,
construct local environments, and save reusable lowering products.

[RunLowering.cpp](RunLowering.cpp) is the generation driver;
[LoweringStore.cpp](LoweringStore.cpp) stores and reopens its products.
Neither is a second implementation of property propagation.

| Directory | Responsibility |
|---|---|
| [connectivity/](connectivity/) | Instance ports, parent expressions and signal reachability. |
| [Symbolic/](Symbolic/) | Signal aliases, typed bit rewrites and property substitutions. |
| [propagation/](propagation/) | Traversal, landing selection and admissibility. |
| [environment/](environment/) | Local premises and assumption dependencies. |
| [output/](output/) | Generated local SVA and product export. |
| [reports/](reports/) | Hierarchy, reachability and verdict summaries. |

Database records and saved annotations live in [util/database](../util/database/).
Input/settings metadata is written by
[WriteGenerationMetadata](../util/task/WriteGenerationMetadata.cpp).

The symbolic dependency graph is represented by the elaborated hierarchy,
connection edges, relations and traversal indexes. `reports/ExportHierarchyGraph.cpp`
exports that view. RTL assignment synthesis belongs to the imported frontend.

| Files | Responsibility |
|---|---|
| [RunLowering.cpp](RunLowering.cpp) | Run elaboration, property lowering and output generation. |
| [LoweringStore.cpp](LoweringStore.cpp) / [LoweringStore.h](LoweringStore.h) | Create and reopen reusable lowering bundles without regenerating for each check. |
