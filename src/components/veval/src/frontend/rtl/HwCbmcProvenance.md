# hw-cbmc symbolic evaluator

veval uses the recursive symbolic evaluator from hw-cbmc commit
`1a4ffdbffae877b7a7c1bf908de82ed07eafc858` to generate module-local
proof obligations. [Original source](https://github.com/diffblue/hw-cbmc/tree/1a4ffdbffae877b7a7c1bf908de82ed07eafc858/src/verilog)

| File in veval | Original file in hw-cbmc |
|---|---|
| `HwCbmcVerilogSynthesis.cpp` | `src/verilog/verilog_synthesis.cpp` |
| `HwCbmcVerilogSynthesis.h` | `src/verilog/verilog_synthesis.h` |
| `HwCbmcVerilogSynthesisClass.h` | `src/verilog/verilog_synthesis_class.h` |

These files retain their original author notices and BSD-3-Clause licence.
[Evaluator licence](LICENSE.hw-cbmc)

The evaluator runs on one thread and uses the parser, type checker and CBMC
expression library from Crisp's pinned submodules. [hw-cbmc](../../../../hw-cbmc) · [CBMC licence](../../../../hw-cbmc/lib/cbmc/LICENSE)

This product includes software developed by Daniel Kroening,
Edmund Clarke,
Computer Science Department, University of Oxford
Computer Science Department, Carnegie Mellon University
