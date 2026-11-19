# Shared storage access

Read and write persistent lowering records and saved verdict annotations, with
shared JSON primitives and SQLite access. These routines serialize the supplied
results; they do not select property locations or run a solver. Check reporting
lives under [check/results](../../check/results/), and run configuration metadata
lives in [util/task](../task/).

Fresh lowering exports insert rows before building non-unique query indexes.
Primary/unique constraints stay active, and an aborted transaction restores the
original schema. The SQLite writer sorts indexes on its own thread and uses a
64 MiB page-cache target for the private export. Journal and sync
settings are unchanged. Existing result databases are not
bulk-rebuilt.

| Files | Responsibility |
|---|---|
| [JsonEscape.h](JsonEscape.h) | Escape strings written to JSON records. |
| [Json.cpp](Json.cpp) / [Json.h](Json.h) | Shared JSON primitives for existing interchange records; persistent generation data use SQLite. |
| [LoweringDatabase.cpp](LoweringDatabase.cpp) / [LoweringDatabase.h](LoweringDatabase.h) | Read/query the SQLite lowering record. |
| [LoweringDatabaseWriter.cpp](LoweringDatabaseWriter.cpp) / [LoweringDatabaseWriter.h](LoweringDatabaseWriter.h) | Write lowering observations, routes and environment records to SQLite. |
| [SavedVerdicts.cpp](SavedVerdicts.cpp) / [SavedVerdicts.h](SavedVerdicts.h) | Import/export saved verdict annotations in the existing JSON format without invoking a solver. |
