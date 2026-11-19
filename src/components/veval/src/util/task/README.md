# Shared task context and journals

Task settings, path/identity helpers, per-stage records and restart bookkeeping
used by generation and checking. Existing C++ namespaces and helper interfaces
are retained.

| Files | Responsibility |
|---|---|
| [RunProgressLog.cpp](RunProgressLog.cpp) / [RunProgressLog.h](RunProgressLog.h) | Persist completed steps and restart state. |
| [TaskInputAndExecution.cpp](TaskInputAndExecution.cpp) / [TaskInputAndExecution.h](TaskInputAndExecution.h) | Define shared task input and stage-execution interfaces, with file and identity helpers. |
| [CommandArgumentsAndFiles.cpp](CommandArgumentsAndFiles.cpp) / [CommandArgumentsAndFiles.h](CommandArgumentsAndFiles.h) | Parse helper-command arguments and share file, content-identity and serialization operations. |
| [WriteGenerationMetadata.cpp](WriteGenerationMetadata.cpp) / [WriteGenerationMetadata.h](WriteGenerationMetadata.h) | Save effective generation options, input identities and result metadata. |
