# Symbolic relations and expression rewriting

| Files | Responsibility |
|---|---|
| `SignalAliases.*` | Read typed same-net equalities and definitions, including width-preserving views. |
| `CombinationalDefinitions.*` | Expand unavailable signals using same-cycle definitions in ancestor modules, retaining typed arithmetic, conditions and fixed bit updates. |
| `BitRewriting.*` | Rebuild reads through fixed bit layouts, handle complements and signedness, and reverse rebases. |
| `PackedBits.h` | Pack selected bits into ordered slices and concatenations. |
| `PropertySubstitution.*` | Substitute design signals in both source SVA and its monitor, and compare a reversed substitution with the original. |

The supporting rewriting operations were extracted from `PropertyLowering.cpp`
and `PropertyMonitor.cpp`; signal aliases came from `ConnectionMap.cpp`.
`CombinationalDefinitions` adds expansion of ancestor RTL definitions.
Propagation location and stopping policy remain in `propagation/`.
SVA language semantics and monitor construction remain in `frontend/sva/`.

## Relation to the paper

The paper's symbolic dependency analysis spans frontend synthesis and the
relations consumed here. RTL assignment evaluation, conditional merges and
register boundaries are handled by the imported
[HwCbmcVerilogSynthesis.cpp](../../frontend/rtl/HwCbmcVerilogSynthesis.cpp)
through [ElaboratedDesign.cpp](../../frontend/rtl/ElaboratedDesign.cpp).
The implementation and headers were relocated without algorithm changes;
[provenance and licence](../../frontend/rtl/HwCbmcProvenance.md) identify the source.
When direct bindings cannot carry a property, lowering can now expand missing
signals using unconditional combinational definitions from the elaborated RTL.
It stops at available child observations and at registers, refuses cycles and
conflicting definitions, and never imports a sibling or the target's own logic.
It does not invert arithmetic input connections. Expanded expressions keep the
frontend's bit widths, signedness and conditional priority; fixed bit updates
are emitted as masks and inserts. Reverse checks compare against the expanded
source before moving back through the earlier port bindings.

Covered expression families include arithmetic, comparisons, Boolean and
bitwise operators, reductions, fixed slices and concatenations, replication,
and packed-struct fields. Frontend-expanded pure functions, fixed loops and
case/conditional assignments use the same typed expressions. Field reads do
not create reverse drivers. Unsupported or ambiguous expressions leave the
original incomplete landing intact.

The separate CVC5 mapping-inference prototype remains in the ignored
`research-extras/` archive and is not enabled by this reorganization.
