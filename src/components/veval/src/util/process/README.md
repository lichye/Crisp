# Process execution and cleanup

Shared process ownership, command launching, resource supervision, cancellation
and Linux/macOS session handling. Existing launchers keep their implementations
and interfaces; this grouping does not merge their behavior.

| Files | Responsibility |
|---|---|
| [SuperviseStage.cpp](SuperviseStage.cpp) | Launch workflow stages with cancellation and resource ownership. |
| [RunWithResourceLimits.cpp](RunWithResourceLimits.cpp) / [RunWithResourceLimits.h](RunWithResourceLimits.h) | Supervise subprocesses, time/memory limits, cancellation and cleanup. |
| [ChildProcess.h](ChildProcess.h) | Own and reap child processes. |
| [CurrentExecutablePath.h](CurrentExecutablePath.h) | Locate the currently running executable independently of argv[0] and the working directory. |
| [ProcessPipeCompatibility.h](ProcessPipeCompatibility.h) | Create close-on-exec pipes/socket pairs with Linux and macOS compatibility. |
| [ProcessGroup.h](ProcessGroup.h) | Own and clean process groups. |
| [ProcessSession.h](ProcessSession.h) | Own and clean process sessions. |
| [ProcessSessionMac.h](ProcessSessionMac.h) | Implement process-session inspection and cleanup on macOS. |
| [RunExternalCommand.cpp](RunExternalCommand.cpp) / [RunExternalCommand.h](RunExternalCommand.h) | Launch external commands and handle cancellation; no solving algorithm. |
