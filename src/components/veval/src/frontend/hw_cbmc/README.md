# Imported hw-cbmc frontend

This is the subset of hw-cbmc used to parse, elaborate and print SystemVerilog
and translate SVA for Veval. It is third-party code, with local patches.

| Location | Purpose |
|---|---|
| `verilog/` | Parser, type checker, elaboration and expression printing |
| `temporal-logic/` | Property normalization and Spot translation |
| `ebmc/` | Frontend language interface, design/property metadata and `$past` instrumentation; no solver |
| `trans-word-level/` | Expression types and hierarchy printing |
| `../rtl/HwCbmcVerilogSynthesis*` | Imported RTL symbolic evaluator, compiled directly |
| `../../util/cbmc/` | CBMC expressions, symbols, simplification and supporting utilities |

`Sources.cmake` lists the imported implementation files. CMake regenerates the
parser/scanners inside the build directory. The former upstream build, EBMC
executable, BMC/PDR engines, VHDL/SMV/AIGER/BTOR frontends and unused libraries
are not included. Safety-monitor construction uses Spot's determinism metadata
or subset construction; it does not issue a SAT query.

The semantic tests have their own reference encoder in
`test/support/cbmc_solver`. That library is never linked into Veval or its
runtime helpers. Comparing against an external EBMC is an optional test.

## Origin and licence

hw-cbmc revision: `1a4ffdbffae877b7a7c1bf908de82ed07eafc858`,
from <https://github.com/diffblue/hw-cbmc>, BSD-3-Clause ([LICENSE](LICENSE)).
CBMC revision: `7483d0de40b2f39850f4f5ba5dd9c6e38f959e31`,
BSD-4-Clause ([licence](../../util/cbmc/LICENSE)). Original author headers remain.
The original paths within each imported tree are retained, except for the
three explicitly named `HwCbmcVerilogSynthesis*` files.

[PATCH-HISTORY.md](PATCH-HISTORY.md) and `patches/` preserve the earlier import
record; their paths describe the old tree. This relocation also removes unused
solver/trace fields and includes from the property metadata, and records Spot's
HOA determinism flag for monitor construction. It does not add an RTL prover.

This product includes software developed by Daniel Kroening, Edmund Clarke,
Computer Science Department, University of Oxford, Computer Science Department,
Carnegie Mellon University.
