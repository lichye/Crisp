# Lowering reports

Export hierarchy views, reachability summaries and existing verdict annotations.
Graph export presents the existing graph; it does not create RTL semantics.

| Files | Responsibility |
|---|---|
| [ExportHierarchyGraph.cpp](ExportHierarchyGraph.cpp) / [ExportHierarchyGraph.h](ExportHierarchyGraph.h) | Export the hierarchy and its property routes. |
| [ReachabilityReport.cpp](ReachabilityReport.cpp) / [ReachabilityReport.h](ReachabilityReport.h) | Format reachability explanations and coverage summaries. |
| [VerdictReport.cpp](VerdictReport.cpp) / [VerdictReport.h](VerdictReport.h) | Write result views grouped by verdict from existing records. |
