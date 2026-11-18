# Imported CBMC expression support

Selected CBMC utilities for typed expressions, symbols, simplification, parsing
and arbitrary-precision integers. CMake builds the explicit subset listed in
`src/frontend/hw_cbmc/Sources.cmake`; there is no CBMC executable or solver here.

Revision: `7483d0de40b2f39850f4f5ba5dd9c6e38f959e31`. Author headers and the
original [BSD-4-Clause licence](LICENSE) are retained. See the
[frontend provenance](../../frontend/hw_cbmc/README.md).

Veval's parallel lowering uses atomic shared-expression reference counts and
hash caches. The string interner serializes insertion and publishes stable
segments for concurrent reads. Each worker mutates only its own expression
handles; sharing a mutable handle between workers is still unsupported.

The parser reads scanner bytes directly from the existing stream buffer instead
of issuing a one-byte `istream::read` for each character. Line tracking, CRLF
handling and EOF state remain in the existing parser/scanner code.

This product includes software developed by Daniel Kroening, Edmund Clarke,
Computer Science Department, University of Oxford, Computer Science Department,
Carnegie Mellon University.
