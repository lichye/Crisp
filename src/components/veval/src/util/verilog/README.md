# Verilog expression output

[ExtractModuleSubtree.cpp](ExtractModuleSubtree.cpp) builds the standalone
`veval_extract_subtree SOURCE TOP OUTPUT` command. It indexes module byte ranges
in one buffered pass, reads instance declarations only in the requested subtree,
and copies those module bytes unchanged. It accepts preprocessed, elaborated
netlists with generated module/instance headers, not general RTL source.
It does not elaborate a design or run a solver.

[ExpressionPrinter.cpp](ExpressionPrinter.cpp) and its
[header](ExpressionPrinter.h) provide shared output preparation for local
specifications and SBY monitors. They preserve bit widths and signedness,
translate array indices back to their declared ranges, and emit shared pure
expressions as Verilog helper functions.

The existing C++ namespace remains `veval::frontend`; this move changes file
organization, not the API or expression semantics.
