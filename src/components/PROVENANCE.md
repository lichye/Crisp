# Source provenance

Crisp imports the production source from two local Git revisions. Each snapshot was read from the named commit. Work in progress in the source checkouts stays with those projects.

| Component in Crisp | Source revision | Contribution |
|---|---|---|
| `src/components/smartVerilog/` | `smartVerilog` · `81739ba7394e9a65d47427970287e61185c12d9b` | SMART assertion mining and MAPminer's parallel block synthesis and MSA selection |
| `src/components/veval/` | `veval` · `355d529a43913cdf94ab694050fb1cf438b43a2d` | SpecLadder property lowering, module-local proof obligations and checking |

The import selects the runtime sources and build inputs. Crisp's CMake files build the component binaries and runtime helpers. SMART can link a prebuilt hw-cbmc installation read-only and locate its EBMC executable. The source repositories retain their research corpora, evaluation scripts and experiment history.

SMART's upstream citation is Leiqi Ye, Yixuan Li, Guy Frankel, Jianyi Cheng and Elizabeth Polgreen, “Unlocking Hardware Verification with Oracle Guided Synthesis,” FMCAD 2025. The smartVerilog revision has no top-level licence file. veval retains its BSD 3-clause licence. [smartVerilog](https://github.com/lichye/smartVerilog) · [veval licence](veval/LICENSE)

## Third-party software

SMART links the Verilog frontend and runs the EBMC executable from Crisp's
`src/components/hw-cbmc` gitlink. The fork already contains the attribute
retention fix used by SMART. [EBMC fork](https://github.com/lichye/ebmc)

SMART uses cvc5 1.4.2. Crisp's installer pins the official Linux x86-64 static
package and its SHA-256 hash. The package supplies the C++ API, parser, CLI
and MPFR. [cvc5 release](https://github.com/cvc5/cvc5/releases/tag/cvc5-1.4.2)

veval carries selected frontend and symbolic-evaluation sources from hw-cbmc `1a4ffdbffae877b7a7c1bf908de82ed07eafc858` and expression libraries from CBMC `7483d0de40b2f39850f4f5ba5dd9c6e38f959e31`. The imported files retain their author notices and licence texts. [Import history](veval/src/frontend/rtl/HwCbmcProvenance.md) · [hw-cbmc licence](veval/src/frontend/hw_cbmc/LICENSE) · [CBMC licence](veval/src/util/cbmc/LICENSE)

This product includes software developed by Daniel Kroening,
Edmund Clarke,
Computer Science Department, University of Oxford
Computer Science Department, Carnegie Mellon University.

Other external tools used in this example are cvc5 for synthesis, Icarus Verilog for simulation, Spot for property translation, and Yosys, SymbiYosys and ABC for module checking.

The single-file Crisp executable links the property-checking and SystemVerilog frontend APIs from the maintained EBMC fork at `90b8c17890052551f36480e5a2301b62a91e8dc4` and its CBMC submodule at `f17df18eaf4a84e7ab7b4e8f60bac1685ad05045`. Its compiled-in engine checks properties with the BDD method. The optional workflow runtime uses the SBY fork at `b21f778333efc1e7c33fe22b56d43f87e4f759d4` and the shared EBMC executable used by SMART. The release records the Yosys and solver versions used to build the bundle. [EBMC fork](https://github.com/lichye/ebmc) · [SBY fork](https://github.com/lichye/sby)
