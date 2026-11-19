# SVA frontend

SVA temporal semantics and monitor construction. Lowering reuses this
representation and rewrites design observations in
[lowering/Symbolic/](../../lowering/Symbolic/).

| Files | Responsibility |
|---|---|
| [ClockSampling.cpp](ClockSampling.cpp) / [ClockSampling.h](ClockSampling.h) | Check clock events and identify their sampling ports. |
| [LocalVariableMonitor.cpp](LocalVariableMonitor.cpp) / [LocalVariableMonitor.h](LocalVariableMonitor.h) | Compile assertions that capture values in property-local variables. |
| [PropertyMonitor.cpp](PropertyMonitor.cpp) / [PropertyMonitor.h](PropertyMonitor.h) | Represent source properties and compile their violation monitors, including the Spot translation. |
| [SafetyMonitor.cpp](SafetyMonitor.cpp) / [SafetyMonitor.h](SafetyMonitor.h) | Determinize supported safety monitors so assumptions cannot evade violations. |

[CompileSvaMonitor.cpp](CompileSvaMonitor.cpp) is the command entry point that
emits the Verilog monitors used by the external checking backend.

`PropertyMonitor` records unsupported entries with their assertion/assumption
role at the point of refusal. Abort-guard observations are collected in one
traversal, in first-occurrence order. Selection keeps the existing order and
sampling metadata. The accepted temporal shapes and monitor encoding are unchanged.
