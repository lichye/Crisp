// SPDX-License-Identifier: BSD-3-Clause
// Imported from diffblue/hw-cbmc, src/verilog/verilog_synthesis.h.
// Upstream: 1a4ffdbffae877b7a7c1bf908de82ed07eafc858, with Veval frontend patches.
// Original author notice follows; see LICENSE.hw-cbmc and HwCbmcProvenance.md.

/*******************************************************************\

Module: Verilog Synthesis

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

#ifndef CPROVER_VERILOG_SYNTHESIS_H
#define CPROVER_VERILOG_SYNTHESIS_H

#include <util/message.h>
#include <util/options.h>
#include <util/symbol_table_base.h>

#include <verilog/verilog_standard.h>

class transt;

// Synthesize independent, already typed modules in dependency order.
// Returns false for interface designs that require a shared writable scope.
bool verilog_synthesis_parallel(
  symbol_tablet &, const irep_idt &, verilog_standardt,
  bool ignore_initial, bool initial_zero, message_handlert &, std::size_t threads);

transt verilog_synthesis(
  symbol_table_baset &,
  const irep_idt &module_identifier,
  verilog_standardt,
  bool ignore_initial,
  bool initial_zero,
  message_handlert &);

bool verilog_synthesis(
  exprt &,
  const irep_idt &module_identifier,
  verilog_standardt,
  message_handlert &,
  const namespacet &);

#endif
