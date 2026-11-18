# Source provenance

Crisp imports the production source from two local Git revisions. Each snapshot was read from the named commit. Work in progress in the source checkouts stays with those projects.

| Component in Crisp | Source revision | Contribution |
|---|---|---|
| `components/smartVerilog/` | `smartVerilog` · `81739ba7394e9a65d47427970287e61185c12d9b` | SMART assertion mining and MAPminer's parallel block synthesis and MSA selection |
| `components/veval/` | `veval` · `355d529a43913cdf94ab694050fb1cf438b43a2d` | SpecLadder property lowering, module-local proof obligations and checking |

The import selects the runtime sources and build inputs. Crisp's CMake files build the component binaries and runtime helpers. SMART can link a prebuilt hw-cbmc installation read-only and locate its EBMC executable. The source repositories retain their research corpora, evaluation scripts and experiment history.

SMART's upstream citation is Leiqi Ye, Yixuan Li, Guy Frankel, Jianyi Cheng and Elizabeth Polgreen, “Unlocking Hardware Verification with Oracle Guided Synthesis,” FMCAD 2025. The smartVerilog revision has no top-level licence file. veval retains its BSD 3-clause licence. [smartVerilog](https://github.com/lichye/smartVerilog) · [veval licence](components/veval/LICENSE)

## Third-party software

EBMC is the hardware model checker from **hw-cbmc**. SMART invokes it as an external executable and links the Verilog frontend from hw-cbmc revision `9b402aa7cfb1157b0399f9e684bdfc1b505ed50d` with CBMC revision `3c915ebe35448a20555c1ef55d51540b52c5c34a`. SMART's local frontend compatibility patch is retained with its build scripts. [Upstream hw-cbmc](https://github.com/diffblue/hw-cbmc) · [Patch notes](components/smartVerilog/tools/patches/hw-cbmc/README.md)

veval carries selected frontend and symbolic-evaluation sources from hw-cbmc `1a4ffdbffae877b7a7c1bf908de82ed07eafc858` and expression libraries from CBMC `7483d0de40b2f39850f4f5ba5dd9c6e38f959e31`. The imported files retain their author notices and licence texts. [Import history](components/veval/src/frontend/rtl/HwCbmcProvenance.md) · [hw-cbmc licence](components/veval/src/frontend/hw_cbmc/LICENSE) · [CBMC licence](components/veval/src/util/cbmc/LICENSE)

This product includes software developed by Daniel Kroening,
Edmund Clarke,
Computer Science Department, University of Oxford
Computer Science Department, Carnegie Mellon University.

Other external tools used in this example are cvc5 for synthesis, Icarus Verilog for simulation, Spot for property translation, and Yosys, SymbiYosys and ABC for module checking.
