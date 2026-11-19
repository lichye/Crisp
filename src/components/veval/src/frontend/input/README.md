# Source preparation

Prepare input text before RTL elaboration. Both `veval lower` and `veval check`
use source preparation.

| Files | Responsibility |
|---|---|
| [PrepareSpecSources.cpp](PrepareSpecSources.cpp) | Preprocess macros and includes, inventory labelled properties, and separate the specification wrapper from design modules while preserving source text. |
| [ReplaceDontCareConstants.cpp](ReplaceDontCareConstants.cpp) / [header](ReplaceDontCareConstants.h) | Rewrite sized X/Z/don't-care literal bits into independent free inputs in a Yosys netlist. This is the explicit `freex` command, not an automatic lowering step. |

Elaboration is in [rtl/](../rtl/); preparing a check from an already generated
local specification belongs to [check/prepare](../../check/prepare/).
