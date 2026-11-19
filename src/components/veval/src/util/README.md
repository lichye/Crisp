# Shared support

Each subdirectory contains one kind of support code. The two small shared
headers below measure generation and read generated SVA definitions.

| Directory | Responsibility |
|---|---|
| [process/](process/) | Launch, supervise, cancel and clean up external processes. |
| [task/](task/) | Shared task context, generation metadata, journal and execution helpers. |
| [files/](files/) | Atomic output and content hashing. |
| [cli/](cli/) | Command options, task input and entry-point declarations. |
| [database/](database/) | JSON/SQLite support, lowering record persistence and saved verdict annotations. |
| [verilog/](verilog/) | Typed Verilog expression and helper-function output shared by lowering and checking. |
| [sby/](sby/) | SBY inputs, external engine calls, verdict interpretation and trace readers. |
| [yosys/](yosys/) | Yosys integration passes and identifier conversion. |

Generation and checking policies remain in [lowering](../lowering/) and
[check](../check/). Separate campaign utilities live in
[tools/evaluation](../../tools/evaluation/).

| Files | Responsibility |
|---|---|
| [ReadGeneratedSva.h](ReadGeneratedSva.h) | Read generated property declarations and combinational helper-function definitions. |
| [MeasureGeneration.h](MeasureGeneration.h) | Measure generation phase time, process memory and counters. |
