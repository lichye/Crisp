# Command-line interfaces and task input

Parse command options and SBY tasks, read generation configuration, expose
inspection commands, and dispatch application entry points. The generation
body is [lowering/RunLowering.cpp](../../lowering/RunLowering.cpp);
check selection and execution live in [check/](../../check/).

| Files | Responsibility |
|---|---|
| [ParseArguments.cpp](ParseArguments.cpp) / [ParseArguments.h](ParseArguments.h) | Parse generator/inspection arguments; reject retired embedded-solver options. |
| [SpecSelectionAndDiagnostics.cpp](SpecSelectionAndDiagnostics.cpp) / [SpecSelectionAndDiagnostics.h](SpecSelectionAndDiagnostics.h) | Select propagated assumptions, identify the source wrapper and report rejected properties or reserved labels. |
| [Commands.h](Commands.h) | Declare command entry points. |
| [PrintHelp.cpp](PrintHelp.cpp) | Print CLI help. |
| [InspectionCommands.cpp](InspectionCommands.cpp) | Inspect connections, reachable signals and SVA; normalize undriven values. |
| [CheckCommands.h](CheckCommands.h) | Declare the lower/check command interfaces. |
| [ReadSbyTask.cpp](ReadSbyTask.cpp) | Read task options, file lists and properties from SBY input. |
| [ProcessSupervisorCli.cpp](ProcessSupervisorCli.cpp) | Expose process supervision as a standalone command. |
| [GenerationConfig.cpp](GenerationConfig.cpp) / [GenerationConfig.h](GenerationConfig.h) | Read generation settings shared by command-line options. |
