# RTL elaboration

Read SystemVerilog through the bundled frontend and expose resolved symbols,
instance connections and typed expressions to lowering.

| Files | Responsibility |
|---|---|
| [ElaboratedDesign.cpp](ElaboratedDesign.cpp) / [header](ElaboratedDesign.h) | Set up elaboration, own the resulting symbol table and transition representation, and report frontend errors. |
| [HwCbmcVerilogSynthesis.cpp](HwCbmcVerilogSynthesis.cpp) | Imported RTL synthesis: symbolic assignments, branch merges, function expansion and register transition expressions. |
| [HwCbmcVerilogSynthesis.h](HwCbmcVerilogSynthesis.h) | Public synthesis entry point used by the bundled frontend. |
| [HwCbmcVerilogSynthesisClass.h](HwCbmcVerilogSynthesisClass.h) | Synthesis implementation state and helper declarations. |

The synthesis code retains its [upstream provenance](HwCbmcProvenance.md) and
[licence](LICENSE.hw-cbmc). Vendor forwarding files compile this implementation
into the existing archive; it is not compiled a second time by Veval.
These routines construct the RTL representation and do not invoke a model checker.
