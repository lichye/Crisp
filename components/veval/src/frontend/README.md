# RTL and SVA frontends

Read input sources, elaborate RTL and compile SVA semantics. Cross-module
placement belongs to [lowering](../lowering/).

| Directory | Responsibility |
|---|---|
| [input/](input/) | Preprocess sources and separate the specification wrapper from the design; provide the explicit `freex` input rewrite. |
| [rtl/](rtl/) | Expose the elaborated design and integrate the imported hw-cbmc synthesis implementation. |
| [sva/](sva/) | Handle sampling, temporal properties, property-local variables and monitor compilation. |

Shared Verilog expression output is in [util/verilog](../util/verilog/).
The imported RTL implementation retains its original author notices,
[licence and provenance](rtl/HwCbmcProvenance.md), and is compiled once through
the vendor archive. Parsing and type checking still use the bundled frontend.
