#pragma once

namespace veval::cli {
struct CommandLineOptions;
int runContracts(const CommandLineOptions&);
int runConnections(const CommandLineOptions&);
int runInspectSva(const CommandLineOptions&);
int runReachability(const CommandLineOptions&);
int runUndrivenSignals(const CommandLineOptions&);
int runLowering(const CommandLineOptions&);
} // namespace veval::cli
