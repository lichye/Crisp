# Provenance of `third_party/hw-cbmc`

This directory is a **verbatim vendored copy** of third-party source, plus a
small, separately-recorded set of local patches. Nothing here is original
work of this project except the files listed under "Local modifications".

## Upstream

| what | where | pinned commit |
|---|---|---|
| hw-cbmc / EBMC | https://github.com/diffblue/hw-cbmc | `1a4ffdbffae877b7a7c1bf908de82ed07eafc858` |
| CBMC (upstream's `lib/cbmc` submodule) | https://github.com/diffblue/cbmc | `7483d0de40b2f39850f4f5ba5dd9c6e38f959e31` |
| MiniSat 2.2.1 (at `lib/cbmc/minisat-2.2.1/`) | http://ftp.debian.org/debian/pool/main/m/minisat2/minisat2_2.2.1.orig.tar.gz | release 2.2.1, with CBMC's `lib/cbmc/scripts/minisat-2.2.1-patch` applied |

MiniSat was added on 2026-11-13. It is exactly what CBMC's own `make -C lib/cbmc/src minisat2-download`
produces; it is kept in the tree so that the build needs no download. MiniSat is under the MIT licence
(`lib/cbmc/minisat-2.2.1/LICENSE`).

Vendored on 2026-07-27. The CBMC tree is placed at `lib/cbmc/`, which is the
path upstream's own build system expects (`src/Makefile` sets
`CPROVER_DIR := ../lib/cbmc/src`), so **no upstream build file was modified**.

## Licence

hw-cbmc is under the **BSD 3-clause licence**. The bundled CBMC revision is
under a **BSD 4-clause licence**, including an advertising acknowledgement
clause; the earlier statement that these licences were the same was incorrect.
Patch `0014` touches CBMC and remains subject to its own licence.
The original licence texts are kept unmodified at:

- `LICENSE` (hw-cbmc)
- `lib/cbmc/LICENSE` (CBMC)

The RTL synthesis implementation and its two headers were relocated on
2026-11-14 to `src/frontend/rtl/HwCbmcVerilogSynthesis*` in the Veval repository.
Their original locations forward to those files, so the vendor archive and
Veval use a single implementation. The algorithms and original author notices
are unchanged; file locations and include paths changed. A complete hw-cbmc
licence accompanies the relocated files; see
[their provenance](../../src/frontend/rtl/HwCbmcProvenance.md).

Every source file retains its original `Module:` / `Author:` copyright header.
Redistribution of this directory must retain those notices, and neither the
names of the copyright holders nor of their contributors may be used to
endorse or promote products derived from this software without specific prior
written permission.

## Local modifications

Patches to the SystemVerilog frontend, to the SVA-to-automaton path and to the
expression printer, applied to the vendored tree and also kept as standalone
diffs under `patches/` so that what was changed is visible without a diff
against upstream. They are candidates for upstream submission.

**The table below is the whole of it: every local modification to this
directory is enumerated here, and nothing outside these patches was touched.**
That is a checkable claim, not a promise — reverse-applying every patch in
`patches/` to this tree returns it to the vendored commit, file by file. It
was not true between 2026-07-31 and 2026-08-02, when three edits (`0024`,
`0025`, `0026`) lived in the tree with no row and no diff; they are recorded
now, `0024` and `0025` after the fact.

The numbering has no `0010`: a regeneration of `0005` as a whole-file diff
absorbed the change that would have been `0010`, and renumbering the rest
would break references from the commit log — and, since `0024`, from comments
in the patched source itself.

| patch | file | change |
|---|---|---|
| `0041-synthesis-source-relocation.patch` | `verilog_synthesis.cpp`, `verilog_synthesis.h`, `verilog_synthesis_class.h` | Replace the original locations with forwarding includes to Veval's explicitly attributed `src/frontend/rtl/HwCbmcVerilogSynthesis*` files. No algorithm changes; include spellings and original author/licence notices are retained in the relocated implementation. Apply after earlier synthesis patches. |
| `0001-crlf-macro-line-continuation.patch` | `verilog_preprocessor_tokenizer.cpp` | In `yy_input`, drop a carriage return that immediately precedes a newline, so sources with CRLF line endings behave like sources with Unix line endings. Without this a macro line continuation is a backslash followed by CR and IEEE 1800-2017 22.5.1 does not apply. |
| `0002-preprocessor-macros-and-compilation-unit.patch` | `verilog_preprocessor.{h,cpp}` | Two changes. (a) In `parse_define_arguments`, resolve each captured argument token through `context().define_arguments`: a macro argument may itself be a parameter of the macro being expanded (`` `define OUTER(NAME) `INNER(NAME, _o) ``), and previously the inner macro received the literal parameter name. (b) Carry the macro table across the files of one compilation unit, per 1800-2017 22.5.1. A preprocessor is constructed per file, so without this an include guard cannot work: a header included from two files is expanded twice and whatever it declares -- typically a package -- is declared twice. |
| `0003-name-implicit-port-connection.patch` | `parser.y` | Add a `'.' any_identifier` production to `named_port_connection`, implementing the implicit `.name` port connection of 1800-2017 23.3.2.3 as shorthand for `.name(name)`. Bison reports no new conflicts. |
| `0004-wildcard-port-connection.patch` | `verilog_typecheck.cpp` | Expand `.*` (1800-2017 23.3.2.3) into ordinary named connections at typecheck time, connecting every port not named explicitly to a same-named net or variable in the instantiating scope. Previously it was parsed, ignored by the typechecker, and rejected by synthesis. |
| `0005-typecheck-expr-widths-and-generate-refs.patch` | `verilog_typecheck_expr.cpp` | Four fixes, kept in one diff because they touch one file. First, three for integral types that carry no bit-vector width. (a) `$clog2` is typed 32-bit signed rather than mathematical integer, per 1800-2017 20.8.1; without this `localparam int W = $clog2(N)` cannot be used as a width, because constant folding hands back an untyped constant that overwrites the declared type. (b) A size cast widens an `integer` operand before deciding its result type, so `WIDTH'($clog2(N))` is accepted. (c) A conditional expression whose branches are the same enum keeps that enum type, so `cond ? A : B` remains assignable to a variable of that enum, which 6.19.3 requires. And fourth, `g[0].w` -- a reference into a named generate block -- resolves by settling the index and looking the member up under the unrolled block's name, guarded so that an ordinary indexed expression of the same shape falls through untouched. |
| `0006-genvar-loop-local.patch` | `verilog_generate.cpp` | A genvar declared in a loop's initialisation is local to that loop (1800-2017 27.4), so two sibling `for(genvar i = ...)` loops each have their own `i`. Elaboration runs the loops one after another into a single scope, so an existing genvar of that name is reused rather than redeclared; the loop initialises it before every run. Erasing it after the loop is wrong -- the generated items are typechecked later and still refer to it. |
| `0007-indexed-port-binding.patch` | `verilog_synthesis.cpp` | Synthesis could not find the symbol behind a port bound to one element of a signal, `.o(arr[i])`: its identifier walk knew about member access and typecast but not indexing. |
| `0008-restore-genvars-through-set-genvars.patch` | `verilog_elaborate_module_instances.cpp`, `verilog_interfaces.cpp` | Three passes run over a module's items before its statements are typechecked, and all three descended through the `ID_set_genvars` wrapper without restoring the genvar values it carries. The interface pass is the one that mattered: it converts a port connection expression while deciding whether an implicit net is needed, and did so against whatever the loop counter had finished on. A port bound to `a[i]` inside a generate loop was therefore wired to `a[<final i>]`, and to a constant when that was out of range -- silently, with no diagnostic. |
| `0009-demoted-wire-vs-declared-net.patch` | `verilog_synthesis.cpp`, `verilog_synthesis_class.h` | Synthesis turns a variable into a wire as soon as something drives it combinationally. It now records which signals it demoted, so a later procedural assignment to one of them is permitted: the signal was declared assignable and both drivers are combinational. A signal declared as a `wire` is still rejected. Without the distinction, whichever of two drivers was synthesised second was refused. |
| `0011-parameter-argument-scope.patch` | `verilog_instantiate_module.cpp` | A type parameter's argument names a typedef in the instantiating module, but was carried across as written and looked up inside the instantiated copy, where that name was never declared. The scope in which it was found is now recorded while that scope is still current. It records the location rather than elaborating the type there: elaboration lowers a `logic [3:0]` typedef to a plain unsignedbv, and the instantiated module's declarations are written against the Verilog-level type. |
| `0012-constant-function-return.patch` | `verilog_interpreter.cpp`, `verilog_typecheck.cpp` | A function's return value is read back out of the variable named after the function, so `return expr;` has to assign to that variable. Without it a constant function that returns rather than assigning to its own name cannot be evaluated during elaboration, which is how the usual `localparam W = f(N)` idiom is written. Execution is not cut short at the return; that would only matter for a function with control flow after it, and this interpreter supports neither `if` nor `case`, so such a function fails loudly rather than silently. |
| `0013-aval-bval-packed-aggregate.patch` | `aval_bval_encoding.cpp` | The four-valued encoding accepts a packed aggregate, which is a bit-vector underneath. An *unpacked* array is deliberately still refused: it is not a bit-vector, and treating it as one would produce a nonsense value instead of stopping. |
| `0014-simplify-power-negative-exponent.patch` | `lib/cbmc/src/util/simplify_expr_int.cpp` | The only patch that touches vendored CBMC rather than hw-cbmc. The generic simplifier had no answer for `**` with a negative exponent and left the expression alone, which then failed a constant check. |
| `0015-constant-conditional-and-negative-power.patch` | `verilog_lowering.h`, `verilog_simplifier.cpp` | Three folds in the Verilog constant simplifier. (a) A conditional with a constant condition takes only one branch: the untaken branch is routinely something that could not be evaluated at all -- a shift by a negative amount, `2 ** -2` -- guarded precisely by that condition, so folding the operands first would reject the whole expression over a branch the language says never runs. (b) `**` with a negative exponent per 1800-2017 11.4.4. (c) A packed aggregate whose members are all constants IS a constant expression, but `is_constant()` is false for a `struct_exprt` however it was written; it is flattened into the bit-vector it is underneath. Without (c), `parameter cfg_t C = '0` is rejected and every package declaring a zeroed configuration is unusable. |
| `0016-post-elaboration-symbol-stub.patch` | `verilog_typecheck_expr.h` | The expression typechecker's base class is used on its own in contexts that run after elaboration -- constant-folding a system function call during synthesis, for one -- where every symbol is already elaborated. Aborting on a precondition there turned a legal design into an invariant violation with a backtrace; it is now a no-op that the derived typechecker overrides where elaboration genuinely still has work to do. |
| `0017-extract-from-packed-aggregate.patch` | `verilog_lowering.cpp` | A part select from a packed aggregate widens it into the bit-vector it is underneath first, exactly as the `bool` case above it already did. Without this the preconditions below fail, which is an invariant violation with a backtrace rather than a diagnostic. |
| `0018-compound-genvar-iteration.patch` | `parser.y`, `verilog_generate.cpp` | The parser dropped the operator of a compound genvar iteration, so `h *= 2` became `h = 2`: the loop variable never advanced and elaboration did not terminate. It presented as a duplicate-declaration error three levels down. |
| `0023-out-of-range-indexed-part-select.patch` | `verilog_expr.cpp` | An indexed part select wider than its operand, or starting below it, ran the extract off the end and CBMC's flattener aborted on an invariant -- no diagnostic, no location, the whole run lost. 1800-2017 11.5.1 makes such a select legal: it reads x for a 4-state value and 0 for a 2-state one. The NON-indexed lowering in the same file already pads the operand for exactly this and says so in a comment; the indexed one did not. It now does, in both the constant-index and the variable-index branch -- for a variable index the shift brings zeros in from above, which is the same answer, so widening the operand to the width being taken is enough however large the index turns out to be. PLENA writes `buffered_data[i][latched_acc_waddr * N +: N]`, in range only while the design's own invariant holds, and Yosys emits part selects wider than their operand as a matter of course. |
| `0022-name-the-conflicting-assignment.patch` | `verilog_synthesis.cpp` | Diagnostic only. "conflict with previous assignment" named neither the symbol nor which parts of it collided, and the source location is empty when the input is a generated netlist. On a million-line file that is not a diagnostic. It now reads `conflict with previous assignment to `...mem_slice[0]`, already assigned at [0]`, which is what identified PLENA's vector SRAM as a dual-write-port memory rather than something small. |
| `0021-unpacked-array-conflict-diagnostic.patch` | `verilog_synthesis.cpp` | Diagnostic only; no behaviour changes. "conflicting assignment types" now says, for an unpacked array, that 1800-2017 6.5 makes each element its own variable -- so `assign a[0] = x` alongside an `always_ff` over `a[1:N]` is legal, and the refusal is this model's limitation rather than a defect in the design. slang accepts exactly that pattern and rejects the genuinely overlapping ones, which is what says the distinction is real. Fixing it properly means modelling part of a symbol as an invariant and the rest as state, which the per-symbol assignment map cannot express; `scripts/yosys_reemit.sh` is the way through in the meantime. |
| `0020-directives-inert-in-a-skipped-conditional.patch` | `verilog_preprocessor.cpp` | `` `__FILE__ ``, `` `__LINE__ `` and `` `default_nettype `` produced output even inside a conditional that was not taken; every other directive that writes to the output stream already checked. The visible effect needs two files: a header with an include guard, included by both. On the second file the guard fires and the body is skipped -- except for the `` `__FILE__ `` and `` `__LINE__ `` inside the macro bodies, which emit a string literal and a number into the middle of whatever the parser was doing. lowRISC's `prim_assert.sv` is exactly that header, included by every SRAM wrapper, and the result was a syntax error reported at the line of its own include guard, in a file the parser had no business being in. This is what blocked the whole PLENA top. |
| `0019-sva-throughout-within-and-zero-range-delay.patch` | `ltl_sva_to_string.cpp` | Three SVA sequence forms that did not become automata. (a) `exp throughout seq` (1800-2017 16.9.9) is the length-matching intersection `(exp)[*] && (seq)`: `&&` pins both operands to one interval, so `exp[*]` contributes one copy of `exp` per tick and constrains the length not at all. (b) `seq1 within seq2` (16.9.10) is `(1[*] ; seq1 ; 1[*]) && seq2`, which is the standard's own definition written in Spot's SERE syntax. (c) `f ##[0:to] g` was refused outright; the zero end of the range is a fusion rather than a delay, so it splits as `(f : g) \| (f ; 1[*0..to-1] ; g)`. An lhs admitting an empty match is still refused, because 16.9.2.1 says `empty ##0 g` does not match and the fusion would not respect that. `scripts/sva_crosscheck.sh` checks the results against EBMC's independent word-level sequence semantics. |
| `0024-xz-sensitive-mark-and-isunknown-guard.patch` | `hw_cbmc_irep_ids.h`, `verilog_expr.h`, `verilog_lowering.cpp`, `verilog_typecheck_expr.cpp` | Two changes, recorded after the fact: the patch was applied in commit `d50e455` and referenced by number from five places (three of them in the patched source) before it had a row here. (a) Lowering erases the x/z-sensitive comparisons: `a === b` and `a == b` become the same expression, and `==?` / `!=?` become their `aval_bval` expansion, so a consumer downstream cannot tell that the author was distinguishing x and z. Each lowering now leaves the operator it came from on the result as `C_verilog_xz_sensitive`; the id is declared in `hw_cbmc_irep_ids.h`. veval refuses to build a monitor from such a comparison (`src/automaton/BuechiMonitor.cpp`), which it could not do while the mark was absent -- it would have checked `==` and said nothing. (b) `$isunknown` folds its argument only when that argument really is a constant expression. Handing a variable to `elaborate_constant_expression` reaches `var_value`, whose precondition is `false` in this typechecker, so `$isunknown(some_signal)` aborted the process instead of reporting anything; giving up is what the neighbouring branches already do for every other non-constant case. |
| `0025-extractbits-msb-off-by-one.patch` | `expr2verilog.cpp` | Recorded after the fact: applied in commit `a628651`. The msb of a width-`w` select starting at bit `i` is `i + w - 1`, and the printer wrote `i + w` -- a width-6 extraction at index 5 printed as `[11:5]`, a seven-bit select. Both branches were wrong, the constant-index one and the variable-index one. The product is a compilable obligation a reader hands to a prover, so a printed property that is not the property transported is the worst kind of defect this printer can have: it is legal Verilog and it says something else. |
| `0026-nested-unary-operand-brackets.patch` | `expr2verilog.cpp` | `convert_unary` bracketed its operand on `precedence > op.p`, which is never true when both are unary, so two nested unary operators printed with nothing between them. Two consequences, and the second is a silent one. `!(!x)` printed `!!x`, which the grammar refuses -- `unary_operator ... primary` -- and that is how three coral products came out unreadable; the frontend normalises `!rstn \|=> X` into a not over a not, so the shape is routine rather than exotic. But `-(-x)` printed `--x`, which the lexer reads as one `--` token and the parser accepts as `ID_predecrement`: legal, parseable, and a different expression. The condition is now `>=`. It also brackets a second class that was never bracketed before -- the CTL operators `AG`/`EG`/`AX`/`EX` sit at the bottom precedence, where `>` could not fire -- which changes no veval product, since veval's printing paths do not reach them, but is a behaviour change of the function and is stated here rather than found later. Known answers: `test/tools/unary_print_unit.sv`, checked by `test/tools/product_check.py` section B, which parses every generated `.sv` with the vendored `ebmc` itself. |
| `0027-select-printed-on-a-non-identifier.patch` | `expr2verilog.cpp` | Three printed forms, one grammar rule. 1800-2017 A.8.4 hangs a select off a hierarchical identifier and off nothing else -- `hierarchical_identifier_bit_select_brace '[' part_select_range ']'`, `parser.y` line 5337 -- so a select the printer put on anything else is unreadable however it is bracketed, and bracketing is all the old code did. (a) `convert_extractbits` printed `(host_a_o >> 181 - 1 - 2)[2:0]`. The shift is not something anyone wrote: `verilog_expr.cpp` lowers `x[i -: 3]` to `extractbits(lshr(x, i-2), 0, 3)`, and elaboration folds `$bits(x)` while leaving the arithmetic round it standing, so an index that IS constant took the non-constant branch. The printer now folds that arithmetic itself (`verilog_fold_constant`, deliberately narrow: constants under `+`, `-`, `*`, unary minus, and not through a typecast) and re-forms the direct part select `host_a_o[180:178]`, bounds-checked against the operand's width. The bounds are ZERO-BASED, which is what the old constant branch already did and is correct for this path rather than an oversight in it: `lower()` has subtracted the operand's `C_offset` before the printer sees the index (`verilog_expr.cpp` lines 200 and 271), and the text is re-parsed against a product whose every port veval declares `[W-1:0]` (`ModuleSpec.cpp:114`). A draft of this patch added the offset back and was caught in review by the case that shows it: `b[4 -: 3] == 3'b010` over `logic [8:1] b` proves at the boundary and printed `b_o[4:2]`, which refutes -- three bits one place above the ones the property was written about. `test/tools/select_print_unit.sv` now carries that case as a known answer, decided by an engine rather than by eye. When the fold declines, the general form is a size cast over a shift, `3'(host_a_o >> lane_sel * 3)`, with `$unsigned` round a signed operand because a part select is unsigned (11.5.1) and a size cast is not (6.24.1). That branch also replaces `x[i + 2 : i]`, which parses and then fails to typecheck: 11.5.1 requires the bounds of a non-indexed part select to be constant. (b) `convert_extractbit` had the same defect one production over. `$rose(x)` normalises to `!$past(x, 1)[0] && x[0]`, so a bit select landed on a function call; it is now `1'($past(x, 1))` unless the operand is an identifier. (c) `convert_constant` wrote the signedness marker of a sized literal after the base letter -- `32'hs121` where 5.7.1 spells `32'sh121` -- so the lexer took `32'h` and read `s121` as an identifier. Only values of 256 and up were affected, which is where the printer switches to hex; the binary branch below it already had the order right. `$bits(host_element_a_o)` is 289. All three were live in `Result/PLENA_fullSpec` on 2026-08-03, in eight products, and they hid each other: the parse stopped at the `32'hs121` on line 220 of `plena_spec.sv`, and the `$past(...)[0]` on line 400 only appeared once that was fixed. Known answers: `test/tools/select_print_unit.sv`, whose four shapes are copied off `benchmark/PLENA_full/spec/plena_spec.sv` (`fetch_is_read`, `isa_break_only_on_break`) and off the RTL idiom patch `0023` cites; checked by `test/tools/product_check.py` section B, which parses every generated `.sv` with the vendored `ebmc` and now also names the two illegal forms directly, over every corpus. |
| `0028-packed-select-declaration-coordinates.patch` | `verilog_expr.cpp`, `verilog_typecheck_expr.cpp` | Existing T17 packed-select correction, recorded without a new semantic change. Lower fixed and indexed `+:`/`-:` selects from declaration coordinates into zero-based physical bits using `C_offset` and `C_increasing`; ascending indexed selects preserve order and dynamic indices use the same width transform. Typechecking rejects a fixed part-select whose direction disagrees with its declaration and retains the original bounds for width calculation. Known answers: `testPackedDirections` in `src/transport/test_connection_map.cpp`, the six declaration/direction cases and independent original/printed assertion values in T17 `u2-pyslang-values.json`, and permanent `test/tools/field_lowering_check.py` (157 checks). See `docs/report/REPORT-generation-field-lowering.md` U0/U2 evidence and final T17 freeze; this patch records those already accepted bytes rather than adding a new vendor behavior. |
| `0032-compilation-unit-defines-source-text-only.patch` | `verilog_preprocessor.{h,cpp}` | Refines `0002` (b). The shared `compilation_unit_defines` table was assigned the whole working table when a file's preprocessing ended, so a file's command-line initial defines were handed on to the next file along with its source-text macros. Within EBMC's own flow every file of the command line carries the same initial defines, so nothing changed there; but a caller that preprocesses files under different per-file options -- veval's `check` source bridge, which replays an SBY `[script]` with one `read_verilog` command's `-D`/`-formal` per file -- either leaked one command's `-D`/`FORMAL` into the next command or, to avoid that, reset the table per file and lost the source-text `define`s Yosys keeps (1800-2017 22.5.1; Yosys `read_verilog` keeps `define`/`undef` across files and commands and re-applies `-D` per command). The table is now maintained by the `define`, `undef` and `undefineall` handlers themselves and the end-of-file assignment is gone: it carries exactly the source-text macros, the constructor still applies a file's initial defines without overriding a persisted source-text macro of the same name, which is also Yosys's precedence. Known answers: `macroSemantics` and `macroBridgeControl` in `test/check/Check.cpp` (the latter compares the bridge's elaborated `generate` branch with Yosys `-ppdump` on the `generation-field` include-config corpus read in the order that exposed the defect, `docs/review/REVIEW-codebase-20261024.md` P1). |
| `0033-right-operand-same-precedence-brackets.patch` | `expr2verilog.cpp` | `convert_binary` bracketed an operand only when it bound more loosely than the operator (`precedence > op.p`), so a RIGHT operand at the same precedence printed bare. Every binary operator groups to the left (1800-2017 table 11-2), so `7 - (1 - 1)` printed `7 - 1 - 1`, which reads `(7 - 1) - 1`: legal, parseable, and a different number. `lower()` builds exactly that shape for a bit select on an ascending `[lo:hi]` vector (`(W-1) - (i - lo)`, patch `0028`), so `w[1] == x[7]` over `logic [1:8] w` printed a select of bit 5 and refuted a property that holds; `a - (b + c)`, `a / (b * c)` and `a == (b == c)` were wrong the same way. A right operand at equal precedence is now bracketed, except under the same associative operator (`+`, `*`, `&`, `\|`, `^`, `&&`, `\|\|`), where both groupings are one value -- so products that only nest those print byte-identical text. Found by the `srcProof` review (2026-10-26); known answers in `srcProof/Transport/repro/ascending-range/` (four properties over a `[1:8]` internal vector, all decided `verified` by the engine after the fix, `q_msb` refuted before it). |
| `0034-veval-review-front-end-and-printer-repairs.patch` | `expr2verilog.cpp`, `parser.y`, `sva_expr.cpp`, `verilog_elaborate.cpp`, `verilog_synthesis.cpp`, `verilog_synthesis_class.h`, `verilog_typecheck_expr.cpp`, `temporal-logic/ltl_to_buechi.cpp` | Ten repairs from the `srcProof` differential review (2026-10-27), each with a reproducer under `srcProof/Differential/`: (1) `convert_if` bracketed all three operands by the condition's precedence with `>`, so `((c ? x : y) ? a : b)` printed `c ? x : y ? a : b`, which reads `c ? x : (y ? a : b)`; each operand is now bracketed by its own precedence, the condition also at equal precedence. (2) Boolean `xor`/`xnor` printed as `xor(a, b)`, and `popcount` (from `$countones`) as an irep dump; they print as `^`, `~^` and `$countones(...)`. (3) `dist` had no parser action, so `x dist {0:=1, 1:=2}` became the Boolean `x != 0`; it is now `x inside {...}` (weights dropped; 1800-2017 16.14.2). (4) `>>>` in an unsigned context stayed an arithmetic shift; it becomes a logical one (11.4.10). (5) An index into a packed array whose outer range has an offset or ascends, and any part select of a packed array, were lowered as if the array were a flat zero-based vector; they are now refused with an error instead. (6) A gated or explicitly clocked `$past`/`$stable`/`$rose`/`$fell`/`$changed` lost its gate or clock in synthesis; the result is marked `#veval_dropped_sampling_arguments` so veval refuses the property. (7) Localparams are marked `is_file_local` (as enum names already are), so a tool restating an instance can pass only the parameters that may be overridden. (8) A variable written both by a continuous assignment and by a procedural block (`assign v[3:0] = ...` beside `always_comb v[7:4] = ...`, the coral `barrel_shifter.sv` shape on a packed array) lost the procedural writes; it is now an elaboration error. (9) A `default:` item of a property `case` was built with its result as operand 0, so the typechecker read past the end and crashed (exit 139); and `sva_case_exprt::lower` turned its empty pattern list into `false`, so the default arm never applied. The item now carries a `default` marker and the lowering places its result as the final else. (10) `ltl_to_buechi` read every acceptance mark of the HOA output as Buechi acceptance without checking the `Acceptance:` header; it now requires `1 Inf(0)`. |

| `0039-readonly-expression-walks.patch` | `verilog_lowering.cpp`, `verilog_synthesis.cpp` | Preserve unchanged operand DAG nodes and typed leaves; detach only when a recursive rewrite changes node identity. No expression rule or metadata is removed. |
| `0040-release-typed-source-forest.patch` | `verilog_ebmc_language.{h,cpp}` | Allow Veval's single-top consumer to release parser trees and module-source templates after complete typechecking and before synthesis. Default EBMC diagnostics and multiple-top calls retain source trees. |


Patch `0035-function-locality-prefix-index.patch` adds a shared lexical symbol
index during Verilog synthesis. `function_locality` retains its exact
`function-name + '.'` prefix rule; recursive module synthesis shares the
index and `replace_by_wire`, the synthesis-time insertion path, updates it.
This removes a whole-chip scan for every generated mux-function call.
The NVDLA-derived `benchmark/NVDLA/rtl/check_function_index.sv` exercises
repeated calls and distinct instances; its connection and transport outputs
are byte-identical before and after the change. This is a preparation-time
optimization, with no new proof assumption or RTL transformation.

Measured effect on the SystemVerilog benchmark used to develop them
(213 non-empty `.sv`/`.v` files, parse + typecheck):

| stage | files accepted (parse + typecheck) |
|---|---|
| upstream, unmodified | 167 / 213 (78%) |
| + `0001`, `0002` | 179 / 213 (84%) |
| + `0003` | 194 / 213 (91%) |
| + package headers in the compilation unit | 200 / 213 (94%) |

Patches `0004` and `0005` are about elaboration rather than parsing, and are
measured on modules instead of files: they take a design whose elaboration
stopped four modules below the top down to one that reaches deep into the
compute core, and they made a hand-written abstraction for one module
unnecessary by fixing the frontend instead.

## What was removed

The vendored copy was taken from a built tree, so it carried build products:
the `cbmc`, `goto-instrument` and `smt2_solver` binaries, and the two
`version.cpp` files that the build generates from `git describe`. They are
untracked here. Nothing under version control that upstream's *source* tree
contains has been removed.

## Not vendored

**Spot** (https://spot.lre.epita.fr) provides `ltl2tgba`, which EBMC invokes as
an external process to translate LTL into a Büchi automaton. Spot is
**GPL-3.0**. It is deliberately *not* included in this repository: EBMC calls
it as a separate program rather than linking against it, so no GPL obligation
attaches to this source tree. Install it separately if you need the automaton
path (`--buechi`) or the unbounded engines that depend on it; bounded model
checking of the SVA fragment does not require it.

## Local patch 0029: extracted-bit cast parentheses

`0029-parenthesise-extractbit-size-cast.patch` retains the typed one-bit cast but
prints it as a parenthesized primary. The frozen Coral field-lowering premise
`!1'(uop_lsu_valid_lsu2rvv[0:0])` otherwise makes Yosys parse `!1` as the cast
width. The fix is carried from isolated commit 0595eab; the CMake
`coral_cast_print` regression checks the original two lanes and direct selects.
No RTL or source specification is changed. Rebuild the Verilog archive before
linking consumers; the evaluation's frozen archive is not modified in place.

## Local patch 0030: completed `within` windows

`0030-within-completed-window.patch` fixes `trans-word-level/sequence.cpp`.
An inner match pending beyond the BMC bound cannot fit inside an outer match
that already ends within that bound. It must not make that completed interval
pending. A pending outer match still propagates, preserving upstream issue
1763. The unchanged `sequence_ops.sv` corpus previously refuted `bad_within`
only at bound 3 despite its outer interval ending at tick 2; it now refutes at
bound 2. `sequence_boundary` checks the match representation, and
`sva_crosscheck` checks the actual EBMC and transported-monitor verdicts.
Rebuild the trans-word-level archive and EBMC before linking veval.

## Local patch 0031: ranged `always`

`0031-ranged-always-globally.patch` prints `sva_ranged_always` as `G`, not
`F`, including finite, singleton and unbounded ranges. Singleton ranges
have the same truth value under either operator. The unchanged
`test/tools/sva/property_ops.sv` assertion `always [1:3] a` distinguishes
them on alternating input: always refutes, whereas eventually holds.
The `safety_monitors` CTest checks that known answer through the native
monitor engine and checks the SBY helper's formula. This was a latent
defect; no current benchmark specification is claimed to have triggered it.
Rebuild the temporal-logic archive and EBMC before linking veval.

## Local patch 0037: unary negation of sized primaries

`0037-unary-sized-primary.patch` brackets sized casts and constants under unary
minus. PLENA FP12 exponent arithmetic printed `-32'(x)`, which Yosys interprets
as a cast with a negative width. The printer now emits `-(32'(x))` in both
generated products and native monitors. Typed value/sign/width round trips and
the `unary_cast` public-path regression cover the change.

## Local patch 0036: pure assertion functions and SVA local variables

`0036-sva-functions-and-local-variables.patch` adds named property/sequence
local declarations and match assignments, types each instance with fresh local
identifiers, and preserves standard SVA when printing. Automatic functions used
in assertion expressions are checked for argument-only access, no side effects,
no recursion or static state, then inlined through the existing synthesis
interpreter. Nonconstant loop conditions and unsupported statements are refused
with the function name. Assignment-width annotations preserve truncation after
temporary variables disappear. Function ANSI arguments also inherit the
preceding declared type for grouped names, such as `input [3:0] a, b`.
Named properties pre-resolve only the lexical left side of hierarchical
identifiers. A member such as `u_dut.addr` is resolved in the DUT's scope,
not as a bare `addr` in the wrapper; missing members still fail typechecking.

Pure calls use isolated value maps, argument DAGs are not resynthesized at call
boundaries, and an earlier executed `return` takes priority over later returns.
Memoized simplification retains exact nonconstant casts instead of recursively
distributing them through conditionals. Shared numerical expressions become
lexical `let` bindings (no design wires or temporal state). The ConsFlow product
and native monitor printers emit the same automatic-function representation;
the ordinary EBMC description printer labels the compact expression instead of
expanding it into an exponentially large diagnostic. Native regressions include
nested early returns, truncation, real port transport/root correspondence and
Icarus differential evaluation of the frozen PLENA FP12 reference functions.
Packed return/local values written by constant bit slices discard their initial
backing value only after every bit is overwritten, with the last write winning.
Partial initialization and reads of the old value still fail validation; tests
also compare three eight-element vector and two nine-element reduction FP12
operations against Icarus. Nonconstant concatenations retain their DAG rather
than triggering recursive cast distribution in the generic simplifier.
Normalized implications print through the property renderer, so a compact
function in their consequent is not expanded by the fallback irep printer.

The vendor patch provides typed expressions; ConsFlow's shared `LocalMonitor`
compiler provides assertion violation tracking. It does not claim general EBMC
support for proving arbitrary local-variable properties. Assumptions with local
variables remain unsupported. Tests and limitations are documented in
`srcProof/Differential/LocalMonitor/README.md` at the repository root. No CBMC
library source was modified by this patch.

## Local patch 0038: retain shared prefixes of fresh auxiliary wires

`0038-fresh-aux-wire-sharing.patch` skips the self-reference rewrite only for
the names created by `replace_by_wire` in the current synthesis object. Their
right-hand sides predate those fresh names and cannot contain a direct reference
to them. Subsequent procedural assignments update source variables, not these
saved auxiliary definitions. The previous mutable traversal detached shared
array-update prefixes even when it found nothing to replace.

User wires, including incomplete combinational assignments, retain the original
self-reference-to-nondeterminism handling. This is a storage and traversal
optimization; it does not simplify expressions or change initial-state,
array-write, or assignment-priority semantics. The regression `frontend_sharing`
covers genuine self-reference, collisions with auxiliary-looking user names,
and shared conditional array updates. No CBMC library source is modified.


## Local patch 0039: read before detaching expression nodes

`0039-readonly-expression-walks.patch` uses const operand access in lowering and
synthesis. It detaches a parent only when a child's storage identity changes.
Unchanged two-state constants and symbols retain their exact types, annotations
and shared nodes. Mutating a returned value still uses the existing copy-on-write
semantics. `frontend_sharing` checks full annotated-IR equality and isolation of
a subsequently edited copy; transport regressions compare generated products.

## Local patch 0040: release the source forest before synthesis

`0040-release-typed-source-forest.patch` is enabled by Veval's internal
`veval-release-source-trees` option. With one explicit top, typechecking has
completed every parameterized instance before the source forest is released.
The parse-tree containers and standards remain alive; only their item index,
parser payload and `ID_module_source` symbols are removed. Elaborated module
values, interfaces, parameters, package constants and property symbols remain.
Synthesis consumes those elaborated values. Ordinary EBMC invocations and
multiple-top processing keep their original source-retention behavior.

Rebuild the Verilog archive and EBMC before relinking Veval for both patches.
No source under `lib/cbmc/` is modified.

## Direct parsing of files without preprocessing syntax

`verilog_ebmc_language.cpp` scans a file for backticks before preprocessing.
For plain source with only standalone, terminated comment lines, it rewinds the
same input stream and feeds the Verilog parser directly. The file cannot invoke
a macro or change compilation-unit macro state;
comments, strings and source locations are handled by the existing parser.
Any backtick, inline comment, continuation or non-newline EOF conservatively
retains preprocessing, including malformed-comment diagnostics. Missing
files and preprocessing errors retain their existing diagnostics. The
`frontend_direct_parsing` regression compares complete annotated symbols and
transition systems against the preprocessing path for LF and CRLF input.

## Literal range evaluation reuse

`verilog_typecheck_expr.cpp` retains successful integer conversions of raw,
untyped literals in a bounded per-thread cache. Repeated vector widths and index
bounds use the same numeric value. Parameters, compound expressions, typed
constants and failures follow the original conversion path, so scope and error
locations remain specific to each occurrence. Constant-type inspection also
uses const access to avoid detaching a shared expression merely to read it.
`frontend_literal_ranges` checks signed and wide literals, eviction, typed
constants and repeated failures at distinct source locations.
