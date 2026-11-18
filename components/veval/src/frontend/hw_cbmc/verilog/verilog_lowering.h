/*******************************************************************\

Module: Verilog Lowering

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

#ifndef CPROVER_VERILOG_LOWERING_H
#define CPROVER_VERILOG_LOWERING_H

class exprt;
class typecast_exprt;
class typet;

exprt verilog_lowering(exprt);
typet verilog_lowering(typet);

exprt verilog_lowering_cast(typecast_exprt);

/// Flatten a packed aggregate into the bit-vector it is underneath.
exprt to_bitvector(const exprt &);

#endif
