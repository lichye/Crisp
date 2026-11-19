# Imported hw-cbmc symbolic evaluation

These files are third-party code, including the already recorded Veval frontend
patches. Moving them into `src/` does not make the upstream implementation an
original contribution of Veval.

| Current file | Original file in hw-cbmc |
|---|---|
| `HwCbmcVerilogSynthesis.cpp` | `src/verilog/verilog_synthesis.cpp` |
| `HwCbmcVerilogSynthesis.h` | `src/verilog/verilog_synthesis.h` |
| `HwCbmcVerilogSynthesisClass.h` | `src/verilog/verilog_synthesis_class.h` |

Upstream: [diffblue/hw-cbmc](https://github.com/diffblue/hw-cbmc), commit
`1a4ffdbffae877b7a7c1bf908de82ed07eafc858`.
The existing local patches are listed in
[the vendor provenance](../hw_cbmc/PATCH-HISTORY.md).

The original author headers are retained. The applicable hw-cbmc licence is
BSD-3-Clause, reproduced without changes in [LICENSE.hw-cbmc](LICENSE.hw-cbmc).
Source redistributions retain the copyright notice, conditions and disclaimer;
binary redistributions reproduce them in accompanying documentation/materials.
The copyright holders' and contributors' names cannot be used for endorsement
without permission. A filename attribution alone does not replace these terms.

## Build arrangement

CMake compiles this implementation directly, together with the selected parser,
type checker and SVA sources in `../hw_cbmc/`. The expression libraries are in
`../../util/cbmc/`. No prebuilt upstream archive or embedded EBMC executable
is required. See the [import inventory](../hw_cbmc/README.md).

## Parallel module synthesis

`SynthesizeModules.cpp` partitions the typed design into private writable
module scopes. Completed child transition relations are shared read-only;
workers publish symbol updates only after the dependency wave joins.
The original recursive synthesis remains the `-j1` path and the fallback for
interface ports and external package functions. Macros and type binding keep
their original compilation-unit order.

The imported synthesis now reuses an already completed child's transition
relation instead of entering that child's mutable synthesis context again.
Serial/parallel tests compare complete symbol tables and transition systems,
as well as generated properties and Jobs.

## Separate CBMC dependency

The bundled CBMC revision `7483d0de40b2f39850f4f5ba5dd9c6e38f959e31`
has a [four-clause BSD licence](../../util/cbmc/LICENSE),
including its advertising acknowledgement clause. Its licence is not the same
as hw-cbmc's three-clause licence. Both original licence files remain intact.

This product includes software developed by Daniel Kroening,
Edmund Clarke,
Computer Science Department, University of Oxford
Computer Science Department, Carnegie Mellon University
