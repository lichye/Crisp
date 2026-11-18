/*******************************************************************\

Module:

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

#include "expr2verilog.h"
#include "expr2verilog_class.h"

#include <util/arith_tools.h>
#include <util/bitvector_types.h>
#include <util/ieee_float.h>
#include <util/lispexpr.h>
#include <util/lispirep.h>
#include <util/mp_arith.h>
#include <util/namespace.h>
#include <util/prefix.h>
#include <util/std_expr.h>
#include <util/symbol.h>

#include "sva_expr.h"
#include "verilog_expr.h"
#include "verilog_typecheck_base.h"
#include "verilog_types.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>

std::string verilog_string_literal(const std::string &src)
{
  std::string dest;

  dest = '"';

  for(auto &ch : src)
  {
    // Follows Table Table 5-1 in 1800-2017.
    switch(ch)
    {
    case '\n':
      dest += "\\n";
      break;
    case '\t':
      dest += "\\t";
      break;
    case '\\':
      dest += "\\\\";
      break;
    case '"':
      dest += "\\\"";
      break;
    case '\v':
      dest += "\\v";
      break;
    case '\f':
      dest += "\\f";
      break;
    case '\a':
      dest += "\\a";
      break;
    default:
      if(
        (unsigned(ch) >= ' ' && unsigned(ch) <= 126) ||
        (unsigned(ch) >= 128 && unsigned(ch) <= 254))
      {
        dest += ch;
      }
      else
      {
        std::ostringstream oss;
        oss << "\\x" << std::setw(2) << std::setfill('0') << std::hex << ch;
        dest += oss.str();
      }
    }
  }

  dest += '"';

  return dest;
}

/*******************************************************************\

Function: expr2verilogt::convert_if

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_if(const if_exprt &src, verilog_precedencet precedence)
{
  if(src.operands().size()!=3)
    return convert_norep(src);

  std::string dest;

  auto op0 = convert_rec(src.op0());
  auto op1 = convert_rec(src.op1());
  auto op2 = convert_rec(src.op2());

  // veval: each operand is bracketed by its OWN precedence. ?: groups to
  // the right, so a condition that is itself a ?: needs brackets at equal
  // precedence: `((c ? x : y) ? a : b)' printed as `c ? x : y ? a : b',
  // which reads `c ? x : (y ? a : b)'.
  const bool bracket0 = precedence >= op0.p;
  const bool bracket1 = precedence > op1.p;
  const bool bracket2 = precedence > op2.p;

  if(bracket0)
    dest += '(';
  dest += op0.s;
  if(bracket0)
    dest += ')';

  dest+=" ? ";

  if(bracket1)
    dest += '(';
  dest += op1.s;
  if(bracket1)
    dest += ')';

  dest+=" : ";

  if(bracket2)
    dest += '(';
  dest += op2.s;
  if(bracket2)
    dest += ')';

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_cycle_delay

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_sva_cycle_delay(const sva_cycle_delay_exprt &src)
{
  std::string dest;

  if(src.lhs().is_not_nil())
  {
    auto lhs = convert_rec(src.lhs());
    if(
      lhs.p == verilog_precedencet::MIN &&
      src.lhs().id() != ID_sva_cycle_delay &&
      src.lhs().id() != ID_sva_cycle_delay_plus &&
      src.lhs().id() != ID_sva_cycle_delay_star)
    {
      dest += "(" + lhs.s + ")";
    }
    else
      dest += lhs.s;
    dest += ' ';
  }

  dest += "##";

  auto from = convert_rec(src.from());

  if(!src.is_range())
    dest += from.s;
  else if(src.is_unbounded())
    dest += '[' + from.s + ':' + '$' + ']';
  else
  {
    auto to = convert_rec(src.to());
    dest += '[' + from.s + ':' + to.s + ']';
  }

  dest+=' ';

  auto rhs = convert_rec(src.rhs());

  if(rhs.p == verilog_precedencet::MIN)
    dest += '(';
  dest += rhs.s;
  if(rhs.p == verilog_precedencet::MIN)
    dest += ')';

  return {verilog_precedencet::MIN, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_cycle_delay

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_cycle_delay(
  const std::string &name,
  const binary_exprt &expr)
{
  std::string dest;

  if(expr.lhs().is_not_nil())
  {
    auto lhs = convert_rec(expr.lhs());
    if(lhs.p == verilog_precedencet::MIN)
      dest += "(" + lhs.s + ")";
    else
      dest += lhs.s;
    dest += ' ';
  }

  dest += name;
  dest += ' ';

  auto rhs = convert_rec(expr.rhs());
  if(rhs.p == verilog_precedencet::MIN)
    dest += "(" + rhs.s + ")";
  else
    dest += rhs.s;

  return {verilog_precedencet::MIN, std::move(dest)};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_sequence_first_match

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_sequence_first_match(
  const sva_sequence_first_match_exprt &src)
{
  std::string dest = "first_match(";

  dest += convert_rec(src.lhs()).s;

  if(src.rhs().is_not_nil())
  {
    dest += ", ";
    dest += convert_rec(src.rhs()).s;
  }

  dest += ')';

  return {verilog_precedencet::MAX, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_binary

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_binary(
  const multi_ary_exprt &src,
  const std::string &symbol,
  verilog_precedencet precedence)
{
  if(src.operands().size()<2)
    return convert_norep(src);

  bool first=true;
  std::string dest;

  // Every binary operator here groups to the left (1800-2017 table 11-2),
  // so a RIGHT operand at the same precedence needs brackets too:
  // `7 - (1 - 1)' printed without them is `7 - 1 - 1', which reads as
  // `(7 - 1) - 1'. lower() builds exactly that shape for a select on an
  // ascending `[lo:hi]' vector. The one exception is the same associative
  // operator, where both groupings mean the same value.
  const auto &id = src.id();
  const bool associative = id == ID_plus || id == ID_mult ||
                           id == ID_bitand || id == ID_bitor ||
                           id == ID_bitxor || id == ID_xor || id == ID_and ||
                           id == ID_or;

  forall_operands(it, src)
  {
    const bool right = !first;
    if(first)
      first=false;
    else
    {
      dest+=' ';
      dest+=symbol;
      dest+=' ';
    }

    auto op = convert_rec(*it);

    const bool bracket =
      precedence > op.p ||
      (right && precedence == op.p && !(associative && it->id() == id));

    if(bracket)
      dest += '(';
    dest += op.s;
    if(bracket)
      dest += ')';
  }

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_with

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_with(
  const with_exprt &src,
  verilog_precedencet precedence)
{
  if(src.operands().size()<1)
    return convert_norep(src);

  std::string dest = "(" + convert_rec(src.old()).s;

  for(unsigned i=1; i<src.operands().size(); i+=2)
  {
    dest+=" WITH ";
    dest += convert_rec(src.operands()[i]).s;
    dest+=":=";
    dest += convert_rec(src.operands()[i + 1]).s;
  }

  dest+=")";

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_concatenation

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_concatenation(
  const concatenation_exprt &src,
  verilog_precedencet precedence)
{
  if(src.operands().size()<1)
    return convert_norep(src);

  bool first=true;
  std::string dest="{ ";

  forall_operands(it, src)
  {
    if(first)
      first=false;
    else
      dest+=", ";

    auto op = convert_rec(*it);

    if(precedence > op.p)
      dest += '(';
    dest += op.s;
    if(precedence > op.p)
      dest += ')';
  }

  dest+=" }";

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_function

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_function(const std::string &name, const exprt &src)
{
  bool first=true;
  std::string dest=name;
  dest+="(";

  forall_operands(it, src)
  {
    if(first)
      first=false;
    else
      dest+=", ";

    dest += convert_rec(*it).s;
  }

  dest+=")";

  return {verilog_precedencet::MAX, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_function_call

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_function_call(const function_call_exprt &src)
{
  if(src.operands().size()!=2)
    return convert_norep(src);

  auto fkt = convert_rec(src.op0());

  std::string dest = fkt.s;
  bool first=true;
  dest+="(";

  for(const auto & op : src.op1().operands())
  {
    if(first)
      first=false;
    else
      dest+=", ";

    if(op.id() == ID_type)
      dest += convert(op.type());
    else
      dest += convert_rec(op).s;
  }

  dest+=")";

  return {verilog_precedencet::MEMBER, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_ranged_predicate(
  const std::string &name,
  const sva_ranged_predicate_exprt &src)
{
  std::string range_str;

  range_str = "[" + convert(src.from()) + ':';
  if(src.is_unbounded())
    range_str += "$";
  else
    range_str += convert(src.to());
  range_str += "] ";

  auto &src_op = src.op();
  auto op = convert_rec(src_op);
  if(op.p == verilog_precedencet::MIN && src_op.operands().size() >= 2)
    op.s = "(" + op.s + ")";

  return {verilog_precedencet::MIN, name + " " + range_str + op.s};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_case

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_sva_case(const sva_case_exprt &src)
{
  std::string cases;

  for(auto &case_item : src.case_items())
  {
    bool first = true;
    for(auto &pattern : case_item.patterns())
    {
      if(first)
        first = false;
      else
        cases += ", ";
      cases += convert(pattern);
    }

    cases += ": ";
    cases += convert(case_item.result());
    cases += "; ";
  }

  return {
    verilog_precedencet::MIN,
    "case(" + convert(src.case_op()) + ") " + cases + "endcase"};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_unary

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_unary(
  const std::string &name,
  const unary_exprt &src)
{
  auto &op = src.op();

  std::size_t op_operands = 0;

  if(op.id() == ID_typecast)
    op_operands = to_typecast_expr(op).op().operands().size();
  else if(
    src.op().id() == ID_sva_sequence_property ||
    src.op().id() == ID_sva_implicit_weak ||
    src.op().id() == ID_sva_implicit_strong)
  {
    op_operands =
      to_sva_sequence_property_expr_base(op).sequence().operands().size();
  }
  else
    op_operands = op.operands().size();

  auto op_rec = convert_rec(op);

  if(op_rec.p == verilog_precedencet::MIN && op_operands >= 2)
  {
    op_rec.s = "(" + op_rec.s + ")";
  }

  return {verilog_precedencet::MIN, name + " " + op_rec.s};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_unary

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_unary(
  const unary_exprt &src,
  const std::string &name)
{
  auto op = convert_rec(src.op());
  if(op.p == verilog_precedencet::MIN && src.op().operands().size() >= 2)
    op.s = "(" + op.s + ")";
  return {verilog_precedencet::MIN, op.s + name};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_binary

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_binary(
  const std::string &name,
  const binary_exprt &expr)
{
  auto op0 = convert_rec(expr.lhs());
  if(op0.p == verilog_precedencet::MIN)
    op0.s = "(" + op0.s + ")";

  auto op1 = convert_rec(expr.rhs());
  if(op1.p == verilog_precedencet::MIN)
    op1.s = "(" + op1.s + ")";

  return {verilog_precedencet::MIN, op0.s + " " + name + " " + op1.s};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_sequence_repetition

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_sequence_repetition(
  const std::string &name,
  const sva_sequence_repetition_exprt &expr)
{
  auto op_rec = convert_rec(expr.op());

  if(op_rec.p == verilog_precedencet::MIN)
    op_rec.s = "(" + op_rec.s + ")";

  std::string dest = op_rec.s + " [" + name;

  if(expr.repetitions_given())
  {
    if(expr.is_range())
    {
      dest += convert_rec(expr.from()).s;
      dest += ':';

      if(expr.is_unbounded())
        dest += "$";
      else
        dest += convert_rec(expr.to()).s;
    }
    else
      dest += convert_rec(expr.repetitions()).s;
  }

  dest += ']';

  return {verilog_precedencet::MIN, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_abort

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_abort(
  const std::string &text,
  const binary_exprt &expr)
{
  auto op0 = convert_rec(expr.op0());
  auto op1 = convert_rec(expr.op1());

  return {verilog_precedencet::MIN, text + " (" + op0.s + ") " + op1.s};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_indexed_binary

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_indexed_binary(
  const std::string &name,
  const binary_exprt &expr)
{
  std::string op0;

  if(expr.op0().is_not_nil())
    op0 = '[' + convert_rec(expr.lhs()).s + ']';

  auto op1 = convert_rec(expr.rhs());
  if(op1.p == verilog_precedencet::MIN)
    op1.s = "(" + op1.s + ")";

  return {verilog_precedencet::MIN, name + op0 + ' ' + op1.s};
}

/*******************************************************************\

Function: expr2verilogt::convert_sva_if

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sva_if(const sva_if_exprt &src)
{
  if(src.false_case().is_nil())
    return {
      verilog_precedencet::MIN,
      "if(" + convert_rec(src.cond()).s + ") " +
        convert_rec(src.true_case()).s};
  else
    return {
      verilog_precedencet::MIN,
      "if(" + convert_rec(src.cond()).s + ") " +
        convert_rec(src.true_case()).s + " else " +
        convert_rec(src.false_case()).s};
}

/*******************************************************************\

Function: expr2verilogt::convert_replication

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_replication(
  const replication_exprt &src,
  verilog_precedencet precedence)
{
  if(src.operands().size()!=2)
    return convert_norep(src);

  std::string dest="{ ";

  dest += convert_rec(src.op0()).s;
  dest+=" { ";
  dest += convert_rec(src.op1()).s;
  dest+=" } }";

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_unary

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_unary(
  const unary_exprt &src,
  const std::string &symbol,
  verilog_precedencet precedence)
{
  if(src.operands().size()!=1)
    return convert_norep(src);

  auto op = convert_rec(src.op());

  // A unary operator takes a *primary* in the Verilog grammar
  // (1800-2023 A.8.3: `unary_operator { attribute_instance } primary'), so an
  // operand that is itself at unary precedence needs brackets. Hence >= and
  // not >: the operand is bracketed when it binds no tighter than this
  // operator, which for two nested unaries is the equal case.
  //
  // The old `>' went wrong in two different ways, and the second one is the
  // dangerous one.
  //
  //   `!', `~' and the reductions. `!(!x)' was printed `!!x', which the
  //   vendored parser.y really does refuse -- `unary_operator
  //   attribute_instance_brace primary', line 5196 -- so the file did not
  //   parse and the damage announced itself. This is the shape that made
  //   three coral products unreadable.
  //
  //   Unary minus. `-(-x)' was printed `--x', with no space between the two
  //   operators, and that is NOT a syntax error: the lexer reads it as one
  //   TOK_MINUSMINUS and parser.y accepts `TOK_MINUSMINUS
  //   attribute_instance_brace variable_lvalue' as ID_predecrement (line
  //   5155). A double negation was printed as a decrement in place -- legal,
  //   readable, and a different expression. The old code changed the meaning
  //   of the property silently. (Unary minus is the one operator this grammar
  //   takes an `expression' under -- line 5209, `%prec UNARY_MINUS' -- so
  //   `- -x' with a space would have been correct; the printer emitted no
  //   space.)
  //
  // Bracketing is semantics-preserving at every width: `!!x' on a multi-bit x
  // is a coercion to bool and `!(!x)' is that same coercion, whereas dropping
  // the pair would not be.
  //
  // A second class of operand is newly bracketed as well. The CTL operators
  // AG/EG/AX/EX come through this function at MIN precedence, and MIN is the
  // bottom of the enum, so `precedence > op.p' could never hold for them and
  // their operands were never parenthesised; `precedence >= op.p' now
  // brackets an operand that is itself at MIN (another CTL operator, or SVA).
  // veval's printing paths do not reach the CTL operators, so no product
  // changes because of it, but it is a behaviour change of this function and
  // is recorded rather than discovered later.
  // Keep the unary sign outside a sized primary. In particular, Yosys reads
  // -32'(x) as a cast with a negative width, rather than negation of the cast.
  const auto size_end = op.s.find_first_not_of("0123456789_");
  const bool sized_operand = !op.s.empty() && op.s.front() >= '0' &&
    op.s.front() <= '9' && size_end != std::string::npos &&
    op.s[size_end] == '\'';
  const bool brackets = precedence >= op.p || (symbol == "-" && sized_operand);
  std::string dest=symbol;
  if(brackets)
    dest += '(';
  dest += op.s;
  if(brackets)
    dest += ')';

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_typecast

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_typecast(const typecast_exprt &src)
{
  if(src.operands().size()==1)
  {
    //const typet &from=src.op0().type();
    //const typet &to=src.type();

    // just ignore them for now
    return convert_rec(src.op());
  }

  return convert_norep(src);
}

/*******************************************************************\

Function: expr2verilogt::convert_explicit_const_cast

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_explicit_const_cast(
  const verilog_explicit_const_cast_exprt &src)
{
  return {verilog_precedencet::MAX, "const'(" + convert_rec(src.op()).s + ')'};
}

/*******************************************************************\

Function: expr2verilogt::convert_explicit_signing_cast

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_explicit_signing_cast(
  const verilog_explicit_signing_cast_exprt &src)
{
  std::string signing = src.is_signed() ? "signed" : "unsigned";

  return {
    verilog_precedencet::MAX, signing + "'(" + convert_rec(src.op()).s + ')'};
}

/*******************************************************************\

Function: expr2verilogt::convert_explicit_type_cast

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_explicit_type_cast(
  const verilog_explicit_type_cast_exprt &src)
{
  return {
    verilog_precedencet::MAX,
    convert(src.type()) + "'(" + convert_rec(src.op()).s + ')'};
}

/*******************************************************************\

Function: expr2verilogt::convert_explicit_size_cast

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_explicit_size_cast(
  const verilog_explicit_size_cast_exprt &src)
{
  return {
    verilog_precedencet::MAX,
    convert_rec(src.size()).s + "'(" + convert_rec(src.op()).s + ')'};
}

/*******************************************************************\

Function: expr2verilogt::convert_index

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_index(
  const index_exprt &src,
  verilog_precedencet precedence)
{
  if(src.operands().size()!=2)
    return convert_norep(src);

  auto op = convert_rec(src.op0());

  std::string dest;
  if(precedence > op.p)
    dest += '(';
  dest += op.s;
  if(precedence > op.p)
    dest += ')';

  dest+='[';
  dest += convert_rec(src.op1()).s;
  dest+=']';

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_non_indexed_part_select

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_non_indexed_part_select(
  const verilog_non_indexed_part_select_exprt &src,
  verilog_precedencet precedence)
{
  auto op = convert_rec(src.src());

  std::string dest;
  if(precedence > op.p)
    dest += '(';
  dest += op.s;
  if(precedence > op.p)
    dest += ')';

  dest += '[';
  dest += convert_rec(src.msb()).s;
  dest += ':';
  dest += convert_rec(src.lsb()).s;
  dest += ']';

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_indexed_part_select

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_indexed_part_select(
  const verilog_indexed_part_select_plus_or_minus_exprt &src,
  verilog_precedencet precedence)
{
  auto op = convert_rec(src.src());

  std::string dest;
  if(precedence > op.p)
    dest += '(';
  dest += op.s;
  if(precedence > op.p)
    dest += ')';

  dest += '[';
  dest += convert_rec(src.index()).s;

  if(src.id() == ID_verilog_indexed_part_select_plus)
    dest += '+';
  else
    dest += '-';

  dest += ':';

  dest += convert_rec(src.width()).s;
  dest += ']';

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_bit_select

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_bit_select(
  const verilog_bit_select_exprt &src,
  verilog_precedencet precedence)
{
  auto op = convert_rec(src.src());

  std::string dest;
  if(precedence > op.p)
    dest += '(';
  dest += op.s;
  if(precedence > op.p)
    dest += ')';

  dest += '[';
  dest += convert_rec(src.index()).s;
  dest += ']';

  return {precedence, dest};
}

/*******************************************************************\

Function: is_verilog_select_target

  Inputs: an expression an extraction is being taken from

 Outputs: true when a `[msb:lsb]' suffix on it is legal Verilog

 Purpose: 1800-2017 A.8.4 allows a select on a hierarchical identifier and
          on nothing else -- not on a parenthesised expression, not on a
          concatenation, not on a function call. parser.y implements exactly
          that (`hierarchical_identifier_bit_select_brace '[' ... ']'').

\*******************************************************************/

static bool is_verilog_select_target(const exprt &expr)
{
  // The identifier forms, as expr2verilogt prints them.
  if(
    expr.id() == ID_symbol || expr.id() == ID_verilog_identifier ||
    expr.id() == ID_hierarchical_identifier)
  {
    return true;
  }

  // A select already applied to one: `x[3][2:0]' is a select on
  // hierarchical_identifier_bit_select_plus, and `x.f' likewise.
  if(
    expr.id() == ID_index || expr.id() == ID_verilog_bit_select ||
    expr.id() == ID_extractbit)
  {
    return is_verilog_select_target(to_binary_expr(expr).op0());
  }

  if(expr.id() == ID_member)
    return is_verilog_select_target(to_member_expr(expr).compound());

  return false;
}

/*******************************************************************\

Function: verilog_fold_constant

  Inputs: an index or shift-distance expression

 Outputs: its value, when this narrow fold can settle it

 Purpose: Elaboration folds `$bits(x)' to a constant but leaves the
          arithmetic written round it standing, so the index of
          `x[$bits(x)-1 -: 3]' reaches the printer as `181 - 1' rather than
          as 180. Only the operators an index of that shape is built from
          are folded here. The result is used solely where a bounds check
          follows, so declining to fold costs a less readable form and never
          a wrong one.

\*******************************************************************/

static std::optional<mp_integer> verilog_fold_constant(const exprt &expr)
{
  if(expr.is_constant())
    return numeric_cast<mp_integer>(to_constant_expr(expr));

  if(expr.id() == ID_plus || expr.id() == ID_minus || expr.id() == ID_mult)
  {
    if(expr.operands().empty())
      return {};

    auto result = verilog_fold_constant(expr.operands().front());
    if(!result.has_value())
      return {};

    for(std::size_t i = 1; i < expr.operands().size(); i++)
    {
      auto operand = verilog_fold_constant(expr.operands()[i]);
      if(!operand.has_value())
        return {};

      if(expr.id() == ID_plus)
        *result += *operand;
      else if(expr.id() == ID_minus)
        *result -= *operand;
      else
        *result *= *operand;
    }

    return result;
  }

  if(expr.id() == ID_unary_minus)
  {
    auto operand = verilog_fold_constant(to_unary_expr(expr).op());
    if(!operand.has_value())
      return {};
    return -*operand;
  }

  // Deliberately not folded through a typecast: a cast to a narrower type
  // truncates, and this fold does not model that.
  return {};
}

/*******************************************************************\

Function: verilog_bit_width

  Inputs: a type

 Outputs: its width in bits, when it has one

 Purpose:

\*******************************************************************/

static std::optional<mp_integer> verilog_bit_width(const typet &type)
{
  if(type.id() == ID_bool)
    return mp_integer{1};

  if(
    type.id() == ID_unsignedbv || type.id() == ID_signedbv ||
    type.id() == ID_verilog_unsignedbv || type.id() == ID_verilog_signedbv)
  {
    return mp_integer{to_bitvector_type(type).get_width()};
  }

  return {};
}

/*******************************************************************\

Function: verilog_type_is_signed

  Inputs: a type

 Outputs: true when a value of it is signed

 Purpose:

\*******************************************************************/

static bool verilog_type_is_signed(const typet &type)
{
  return type.id() == ID_signedbv || type.id() == ID_verilog_signedbv;
}

/*******************************************************************\

Function: expr2verilogt::convert_extractbit

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_extractbit(
  const extractbit_exprt &src,
  verilog_precedencet precedence)
{
  if(src.operands().size()!=2)
    return convert_norep(src);

  auto op = convert_rec(src.op0());

  // A bit select is a select: 1800-2017 A.8.4 allows one on a hierarchical
  // identifier and on nothing else. The operand here is post-lowering and
  // need not be one -- `$rose(x)' normalises to `!$past(x, 1)[0] && x[0]',
  // which puts a select on a function call:
  //
  //     Result/PLENA_fullSpec/plena_spec.sv line 400:
  //     $past(system_break, 1)[0]
  //
  // Bracketing it, which is what the old code did when the precedences
  // asked for it, does not make it legal: there is no production for
  // `(expr)[i]' either.
  auto index = verilog_fold_constant(src.op1());
  if (is_verilog_select_target(src.op0())) {
    // A constant select needs its index value, not the data literal's type.
    auto position = index ? integer2string(*index) : convert_rec(src.op1()).s;
    return {precedence, op.s + '[' + position + ']'};
  }

  // The general form, as in convert_extractbits below: one bit of the
  // operand, taken with a shift and a size cast.
  std::string inner =
    op.p < verilog_precedencet::SHIFT ? '(' + op.s + ')' : op.s;

  if(!index.has_value() || *index != 0)
  {
    auto index_printed = convert_rec(src.op1());
    inner += " >> ";
    inner += index_printed.p <= verilog_precedencet::SHIFT
               ? '(' + index_printed.s + ')'
               : index_printed.s;
  }

  if(verilog_type_is_signed(src.op0().type()))
    inner = "$unsigned(" + inner + ')';

  // Keep the size cast a parenthesised primary. Under a unary operator,
  // bare !1'(x) can be parsed as a cast whose size is !1 (zero).
  return {verilog_precedencet::MEMBER, "(1'(" + inner + "))"};
}

/*******************************************************************\

Function: expr2verilogt::convert_extractbits

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_extractbits(
  const extractbits_exprt &src,
  verilog_precedencet precedence)
{
  auto width = to_bitvector_type(src.type()).get_width();

  // A DIRECT PART SELECT, when the grammar allows one.
  //
  // 1800-2017 A.8.4 hangs a select off a hierarchical identifier and off
  // nothing else, and parser.y follows the standard here
  // (`hierarchical_identifier_bit_select_brace '[' part_select_range ']'',
  // line 5337). So `[msb:lsb]' may only be appended to something
  // is_verilog_select_target() accepts; the brackets the old code put round
  // any other operand did not make it legal, they only made the syntax error
  // land one token later.
  //
  // The shift is peeled because that is the shape this printer is handed for
  // a part select whose index elaboration did not fold: lower() (verilog_expr
  // .cpp) turns `x[i -: w]' into extractbits(lshr(x, i-w+1), 0, w), and an
  // index written `$bits(x)-1' arrives as arithmetic over a constant rather
  // than as a constant, so that branch is taken for an index that IS known.
  // Folding it back is what turns
  //
  //     (host_a_o >> 181 - 1 - 2)[2:0]         -- not Verilog
  //
  // into `host_a_o[180:178]', which is the select the specification wrote.
  {
    const exprt *base = &src.src();
    auto index = verilog_fold_constant(src.index());

    if(index.has_value() && base->id() == ID_lshr)
    {
      // Taking bits [i+w-1:i] of `x >> d' is taking bits [i+d+w-1:i+d] of x.
      auto &shift = to_binary_expr(*base);
      auto distance = verilog_fold_constant(shift.op1());
      if(distance.has_value() && *distance >= 0)
      {
        index = *index + *distance;
        base = &shift.op0();
      }
    }

    if(index.has_value() && *index >= 0 && is_verilog_select_target(*base))
    {
      auto base_width = verilog_bit_width(base->type());

      // A select that runs past the end of its operand is legal Verilog with
      // a meaning of its own (1800-2017 11.5.1, and patch 0023), but it is
      // NOT the meaning of this expression: lower() zero-pads, so the bits
      // above the operand read 0 here and would read x there. Such a select
      // is printed in the general form below instead.
      if(base_width.has_value() && *index + width <= *base_width)
      {
        // ZERO-BASED, and deliberately so. lower() has already subtracted
        // the operand's `C_offset' (verilog_expr.cpp lines 200 and 271), so
        // the index arriving here counts from bit 0 of the bit-vector, not
        // from the operand's declared numbering. Adding the offset back
        // would be right only if the signal this text is re-parsed against
        // carried the same offset -- and it does not: veval declares every
        // port of a product `[W-1:0]' (ModuleSpec.cpp:114). A boundary
        // property that proves, `b[4 -: 3] == 3'b010' over `logic [8:1] b',
        // would be printed `b_o[4:2]' and refute, naming three bits one
        // place above the ones it was written about.
        auto lsb = *index;
        auto msb = lsb + width - 1;

        return {
          precedence,
          convert_rec(*base).s + '[' + integer2string(msb) + ':' +
            integer2string(lsb) + ']'};
      }
    }
  }

  // THE GENERAL FORM: a size cast over a shift.
  //
  // `w'(x >> i)' is the grammar's way to say "w bits of x from bit i up"
  // when x is not something a select may be written on, or when i is not
  // known until run time (1800-2017 A.8.4 cast, 11.4.10 shift, 6.24.1 size
  // cast). The zero-fill of `>>' is the same answer lower() padded for, so
  // this holds for a select that runs off the top as well.
  //
  // What it replaces did not parse in one branch and did not typecheck in
  // the other: `(x >> i)[2:0]' has no production, and `x[i + 2 : i]' is a
  // non-indexed part select, whose bounds 1800-2017 11.5.1 requires to be
  // constant.
  auto op = convert_rec(src.src());

  // A part select is unsigned (11.5.1) while a size cast keeps the
  // signedness of its operand (6.24.1), so a signed operand is made
  // unsigned first -- otherwise the printed expression sign-extends where
  // the expression it came from zero-extends.
  bool is_signed = verilog_type_is_signed(src.src().type());

  std::string inner = op.p < verilog_precedencet::SHIFT ? '(' + op.s + ')'
                                                        : op.s;

  auto index_constant = verilog_fold_constant(src.index());
  if(!index_constant.has_value() || *index_constant != 0)
  {
    auto index = convert_rec(src.index());
    inner += " >> ";
    inner += index.p <= verilog_precedencet::SHIFT ? '(' + index.s + ')'
                                                   : index.s;
  }

  if(is_signed)
    inner = "$unsigned(" + inner + ')';

  return {
    verilog_precedencet::MEMBER,
    std::to_string(width) + "'(" + inner + ')'};
}

/*******************************************************************\

Function: expr2verilogt::convert_member

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_member(
  const member_exprt &src,
  verilog_precedencet precedence)
{
  if(src.operands().size()!=1)
    return convert_norep(src);

  auto op = convert_rec(src.compound());

  std::string dest;
  if(precedence > op.p)
    dest += '(';
  dest += op.s;
  if(precedence > op.p)
    dest += ')';

  dest+='.';
  dest += id2string(src.get_component_name());

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_norep

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_norep(const exprt &src)
{
  return {verilog_precedencet::MAX, src.pretty()};
}

/*******************************************************************\

Function: expr2verilogt::convert_symbol

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_symbol(const exprt &src)
{
  if(src.get_bool("#veval_sva_local"))
    return {verilog_precedencet::MAX,
            id2string(src.get("#veval_sva_local_name"))};
  std::string dest=src.get_string(ID_identifier);
 
  if(std::string(dest, 0, 9)=="Verilog::")
    dest.erase(0, 9);

  // We special-case the pretty name for identifiers under $root:
  // it is customary to use the name of the module only as root
  // of the hierarchical identifier.
  const static std::string root_module_prefix =
    id2string(verilog_root_module_name()) + '.';

  if(has_prefix(dest, root_module_prefix))
    dest.erase(0, root_module_prefix.size());

  return {verilog_precedencet::MAX, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_verilog_identifier

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_verilog_identifier(const verilog_identifier_exprt &src)
{
  return {verilog_precedencet::MAX, id2string(src.base_name())};
}

/*******************************************************************\

Function: expr2verilogt::convert_nondet_symbol

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_nondet_symbol(const exprt &src)
{
  return {verilog_precedencet::MAX, "nondet(" + convert_symbol(src).s + ")"};
}

/*******************************************************************\

Function: expr2verilogt::convert_next_symbol

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_next_symbol(const exprt &src)
{
  return {verilog_precedencet::MAX, "next(" + convert_symbol(src).s + ")"};
}

/*******************************************************************\

Function: expr2verilogt::convert_hierarchical_identifier

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_hierarchical_identifier(
  const hierarchical_identifier_exprt &src)
{
  return {
    verilog_precedencet::MAX,
    convert_rec(src.module_instance()).s + '.' +
      id2string(src.item().base_name())};
}

/*******************************************************************\

Function: expr2verilogt::convert_constant

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_constant(const constant_exprt &src)
{
  const auto precedence = verilog_precedencet::MAX;

  const typet &type=src.type();
  std::string dest;

  if(type.get(ID_C_verilog_type) == ID_verilog_enum)
  {
    // Get the numerical value.
    auto int_value = numeric_cast_v<mp_integer>(src);

    // Look up the enum type symbol.
    auto enum_type_identifier = type.get(ID_C_identifier);
    auto &symbol = ns.lookup(enum_type_identifier);
    auto &enum_type = to_verilog_enum_type(symbol.type);

    // Find the enum name with the matching value.
    for(auto &enum_name : enum_type.enum_names())
    {
      const auto identifier = enum_name.identifier();
      auto &enum_symbol = ns.lookup(identifier);
      DATA_INVARIANT(
        enum_symbol.value.id() == ID_constant,
        "enum name value must be constant");
      auto enum_int_value =
        numeric_cast_v<mp_integer>(to_constant_expr(enum_symbol.value));
      if(enum_int_value == int_value)
        return {precedence, id2string(enum_symbol.display_name())};
    }

    return {precedence, integer2string(int_value)};
  }
  else if(type.id() == ID_bool)
  {
    if(src.is_true())
      dest+='1';
    else
      dest+='0';
  }
  else if(type.id()==ID_unsignedbv ||
          type.id()==ID_signedbv)
  {
    auto width = to_bitvector_type(type).get_width();
    auto i = numeric_cast_v<mp_integer>(src);

    if(i>=256)
    {
      // 1800-2017 5.7.1: size ' [s|S] base_char value -- the signedness
      // marker comes BEFORE the base letter. Written the other way round,
      // `32'hs121' is not a number at all: the lexer takes `32'h' and then
      // reads `s121' as an identifier, which is the syntax error six PLENA
      // products carried. The binary branch below already had the order
      // right, which is why only values of 256 and up were affected.
      dest = std::to_string(width);
      dest += '\'';
      if(type.id() == ID_signedbv)
        dest += 's';
      dest += 'h';
      dest += integer2string(i, 16);
    } else {
      // Unsized decimal integers change both width and signing. In particular,
      // {10'h000, imm[21:0]} must not become the 54-bit {0, imm[21:0]}.
      // Use a sized literal even for small values in wide bit vectors.
      dest = std::to_string(width);
      dest += "'";
      if (type.id() == ID_signedbv)
        dest += 's';
      dest += 'b';
      dest += integer2binary(i, width);
    }
  }
  else if(type.id()==ID_verilog_signedbv)
  {
    // these have a binary representation
    const irep_idt &value = src.get_value();
    unsigned width=to_verilog_signedbv_type(src.type()).get_width();
    return {precedence, std::to_string(width) + "'sb" + id2string(value)};
  }
  else if(type.id()==ID_verilog_unsignedbv)
  {
    // these have a binary representation
    const irep_idt &value = src.get_value();
    unsigned width=to_verilog_unsignedbv_type(src.type()).get_width();
    return {precedence, std::to_string(width) + "'b" + id2string(value)};
  }
  else if(type.id()==ID_integer || type.id()==ID_natural ||
          type.id()==ID_range)
  {
    // these have a decimal representation
    const irep_idt &value = src.get_value();
    dest=id2string(value);
  }
  else if(type.id() == ID_verilog_real)
  {
    constant_exprt tmp = src;
    tmp.type() = ieee_float_spect::double_precision().to_type();
    auto ieee_float = ieee_float_valuet{tmp};
    return {precedence, ieee_float.to_ansi_c_string()};
  }
  else if(type.id() == ID_string)
  {
    dest = verilog_string_literal(id2string(src.get_value()));
  }
  else if(type.id() == ID_verilog_chandle || type.id() == ID_verilog_event)
  {
    if(src.get_value() == ID_NULL)
    {
      dest = "null";
    }
    else
      return convert_norep(src);
  }
  else if(type.id() == ID_verilog_null)
  {
    dest = "null";
  }
  else
    return convert_norep(src);

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_array

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_array(const exprt &src, verilog_precedencet precedence)
{
  std::string dest="{ ";

  forall_operands(it, src)
  {
    auto tmp = convert_rec(*it).s;

    exprt::operandst::const_iterator next_it=it;
    next_it++;

    if(next_it!=src.operands().end())
    {
      tmp+=", ";
      if(tmp.size()>40) tmp+="\n    ";
    }

    dest+=tmp;
  }

  dest+=" }";

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_streaming_concatenation

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_streaming_concatenation(
  const std::string &name,
  const verilog_streaming_concatenation_exprt &src)
{
  std::string dest = "{";

  dest += name;

  // slice_size?
  if(src.has_slice_size())
  {
    auto tmp = convert_rec(src.slice_size());
    dest += tmp.s;
  }

  dest += "{";
  bool first = true;
  for(auto &op : src.stream_expressions())
  {
    if(first)
      first = false;
    else
      dest += ", ";
    dest += convert_rec(op).s;
  }
  dest += "}";

  dest += "}";

  return {verilog_precedencet::CONCAT, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_inside

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_inside(const verilog_inside_exprt &src)
{
  std::string dest;
  auto precedence = verilog_precedencet::RELATION;

  auto op = convert_rec(src.op());

  if(precedence > op.p)
    dest += '(';
  dest += op.s;
  if(precedence > op.p)
    dest += ')';

  dest += " inside {";
  bool first = true;
  for(auto &op : src.range_list())
  {
    if(first)
      first = false;
    else
      dest += ", ";
    dest += convert_rec(op).s;
  }
  dest += "}";

  return {precedence, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_sequence_property_instance

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_sequence_property_instance(
  const sva_sequence_property_instance_exprt &src)
{
  if(src.arguments().empty())
    return convert_rec(src.symbol());

  auto fkt = convert_rec(src.symbol());

  std::string dest = fkt.s;
  bool first = true;
  dest += "(";

  for(const auto &op : src.arguments())
  {
    if(first)
      first = false;
    else
      dest += ", ";

    dest += convert_rec(op).s;
  }

  dest += ")";

  return {verilog_precedencet::MEMBER, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_struct

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_struct(const struct_exprt &src)
{
  std::string dest = "'{";

  auto &type = to_struct_type(src.type());
  DATA_INVARIANT(
    type.components().size() == src.operands().size(),
    "number of compontents must match");

  for(std::size_t index = 0; index < src.operands().size(); index++)
  {
    auto &op = src.operands()[index];
    if(index != 0)
      dest += ',';
    dest += ' ';
    dest += id2string(type.components()[index].get_name());
    dest += ": ";
    dest += convert_rec(op).s;
  }

  dest += " }";

  return {verilog_precedencet::MAX, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_union

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_union(const union_exprt &src)
{
  std::string dest = "'{ " + id2string(src.get_component_name()) + ": " +
                     convert_rec(src.op()).s + " }";

  return {verilog_precedencet::MAX, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_unbased_unsized_literal

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_unbased_unsized_literal(
  const verilog_unbased_unsized_literal_exprt &src)
{
  return {
    verilog_precedencet::MAX, std::string(1, '\'') + id2string(src.value())};
}

expr2verilogt::resultt expr2verilogt::convert_based_unsized_literal(
  const verilog_based_unsized_literal_exprt &src)
{
  return convert_constant(src.lower());
}

/*******************************************************************\

Function: expr2verilogt::convert_value_range

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt
expr2verilogt::convert_value_range(const verilog_value_range_exprt &src)
{
  std::string dest =
    '[' + convert_rec(src.lhs()).s + ':' + convert_rec(src.rhs()).s + ']';

  return {verilog_precedencet::MEMBER, dest};
}

/*******************************************************************\

Function: expr2verilogt::convert_rec

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

expr2verilogt::resultt expr2verilogt::convert_rec(const exprt &src)
{
  // The ordinary EBMC description printer has no module declaration context.
  // ConsFlow's typed product/monitor printer renders this lexical DAG as an
  // automatic function plus a call. Keep descriptions bounded and visibly
  // non-executable, rather than dumping a quadratically indented let tree.
  if(src.id() == ID_let && src.get_bool("#veval_sva_compact_function"))
    return {verilog_precedencet::MAX, "<pure-function-expression>"};
  // Property normalization can introduce the generic implication node.
  // Printing its whole irep would recursively expand compact function DAGs
  // hidden in the consequent instead of visiting their bounded renderer.
  if(src.id() == ID_implies)
    return convert_sva_binary("implies", to_binary_expr(src));
  if(src.id() == "#veval_sva_match")
  {
    std::string text = "(" + convert(src.operands()[0]);
    for(const auto &assignment : src.operands()[1].operands())
      text += ", " + convert(assignment);
    return {verilog_precedencet::MAX, text + ")"};
  }
  if(src.id() == "#veval_sva_assign")
    return {verilog_precedencet::MAX,
            convert(src.operands()[0]) + " = " + convert(src.operands()[1])};
  static const std::map<irep_idt, std::function<resultt(expr2verilogt &, const exprt &)>> action_table =
  {
    { ID_plus, [](expr2verilogt &expr2verilog, const exprt &src) { return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "+", verilog_precedencet::ADD); } },

    { ID_if, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_if(to_if_expr(src), verilog_precedencet::IF); } },

    { ID_concatenation, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_concatenation(
      to_concatenation_expr(src), verilog_precedencet::CONCAT); } },

    { ID_with, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_with(to_with_expr(src), verilog_precedencet::MAX); } },

    { ID_replication, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_replication(
      to_replication_expr(src), verilog_precedencet::CONCAT); } },

    { ID_array, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_array(src, verilog_precedencet::MAX); } },

    { ID_minus, [](expr2verilogt &expr2verilog, const exprt &src) { 
    if(src.operands().size()!=2)
      return expr2verilog.convert_norep(src);
    else
      return expr2verilog.convert_binary(
        to_multi_ary_expr(src), "-", verilog_precedencet::ADD); } },

    { ID_shl, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "<<", verilog_precedencet::SHIFT); } },

    { ID_lshr, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), ">>", verilog_precedencet::SHIFT); } },

    { ID_ashr, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), ">>>", verilog_precedencet::SHIFT); } },

    { ID_unary_minus, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_unary_minus_expr(src), "-", verilog_precedencet::NOT); } },

    { ID_unary_plus, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_unary_plus_expr(src), "+", verilog_precedencet::NOT); } },

    { ID_index, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_index(to_index_expr(src), verilog_precedencet::MEMBER); } },

    { ID_verilog_indexed_part_select_plus, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_indexed_part_select(
      to_verilog_indexed_part_select_plus_or_minus_expr(src),
      verilog_precedencet::MEMBER); } },

    { ID_verilog_indexed_part_select_minus, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_indexed_part_select(
      to_verilog_indexed_part_select_plus_or_minus_expr(src),
      verilog_precedencet::MEMBER); } },

    { ID_verilog_non_indexed_part_select, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_non_indexed_part_select(
      to_verilog_non_indexed_part_select_expr(src),
      verilog_precedencet::MEMBER); } },

    { ID_extractbit, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_extractbit(
      to_extractbit_expr(src), verilog_precedencet::MEMBER); } },

    { ID_verilog_bit_select, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_bit_select(
      to_verilog_bit_select_expr(src), verilog_precedencet::MEMBER); } },

    { ID_extractbits, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_extractbits(
      to_extractbits_expr(src), verilog_precedencet::MEMBER); } },

    { ID_member, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_member(to_member_expr(src), verilog_precedencet::MEMBER); } },

    { ID_mult, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "*", verilog_precedencet::MULT); } },

    { ID_div, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "/", verilog_precedencet::MULT); } },

    { ID_lt, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src),
      id2string(src.id()),
      verilog_precedencet::RELATION); } },

    { ID_gt, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src),
      id2string(src.id()),
      verilog_precedencet::RELATION); } },

    { ID_le, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src),
      id2string(src.id()),
      verilog_precedencet::RELATION); } },

    { ID_ge, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src),
      id2string(src.id()),
      verilog_precedencet::RELATION); } },

    { ID_equal, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "==", verilog_precedencet::EQUALITY); } },

    { ID_verilog_logical_equality, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "==", verilog_precedencet::EQUALITY); } },

    { ID_notequal, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "!=", verilog_precedencet::EQUALITY); } },

    { ID_verilog_logical_inequality, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "!=", verilog_precedencet::EQUALITY); } },

    { ID_verilog_case_equality, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "===", verilog_precedencet::EQUALITY); } },

    { ID_verilog_case_inequality, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "!==", verilog_precedencet::EQUALITY); } },

    { ID_verilog_wildcard_equality, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "==?", verilog_precedencet::EQUALITY); } },

    { ID_verilog_wildcard_inequality, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "!=?", verilog_precedencet::EQUALITY); } },

    { ID_not, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(to_not_expr(src), "!", verilog_precedencet::NOT); } },

    { ID_sva_not, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary("not", to_sva_not_expr(src)); } },

    { ID_bitnot, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(to_bitnot_expr(src), "~", verilog_precedencet::NOT); } },

    { ID_verilog_explicit_const_cast, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_explicit_const_cast(
      to_verilog_explicit_const_cast_expr(src)); } },

    { ID_verilog_explicit_size_cast, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_explicit_size_cast(to_verilog_explicit_size_cast_expr(src)); } },

    { ID_verilog_explicit_signing_cast, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_explicit_signing_cast(
      to_verilog_explicit_signing_cast_expr(src)); } },

    { ID_verilog_explicit_type_cast, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_explicit_type_cast(to_verilog_explicit_type_cast_expr(src)); } },

    { ID_typecast, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_typecast(to_typecast_expr(src)); } },

    { ID_and, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "&&", verilog_precedencet::AND); } },

    { ID_sva_and, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("and", to_sva_and_expr(src)); } },

    { ID_sva_iff, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("iff", to_sva_iff_expr(src)); } },

    { ID_sva_implies, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("implies", to_sva_implies_expr(src)); } },

    { ID_power, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "**", verilog_precedencet::POWER); } },

    { ID_bitand, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "&", verilog_precedencet::BITAND); } },

    { ID_bitxor, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "^", verilog_precedencet::XOR); } },

    { ID_bitxnor, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "~^", verilog_precedencet::XOR); } },

    { ID_mod, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "%", verilog_precedencet::MULT); } },

    { ID_or, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "||", verilog_precedencet::OR); } },

    { ID_sva_or, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("or", to_sva_or_expr(src)); } },

    { ID_bitor, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "|", verilog_precedencet::BITOR); } },

    { ID_verilog_implies, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "->", verilog_precedencet::IMPLIES); } },

    { ID_verilog_iff, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "<->", verilog_precedencet::IMPLIES); } },

    { ID_reduction_or, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_reduction_or_expr(src), "|", verilog_precedencet::NOT); } },

    { ID_reduction_and, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_reduction_and_expr(src), "&", verilog_precedencet::NOT); } },

    { ID_reduction_nor, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_reduction_nor_expr(src), "~|", verilog_precedencet::NOT); } },

    { ID_reduction_nand, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_reduction_nand_expr(src), "~&", verilog_precedencet::NOT); } },

    { ID_reduction_xor, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_reduction_xor_expr(src), "^", verilog_precedencet::NOT); } },

    { ID_reduction_xnor, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unary(
      to_reduction_xnor_expr(src), "~^", verilog_precedencet::NOT); } },

    { ID_AG, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_unary(
      to_unary_expr(src), src.id_string() + " ", verilog_precedencet::MIN); } },
    
    { ID_EG, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_unary(
      to_unary_expr(src), src.id_string() + " ", verilog_precedencet::MIN); } },
    
    { ID_AX, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_unary(
      to_unary_expr(src), src.id_string() + " ", verilog_precedencet::MIN); } },
    
    { ID_EX, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_unary(
      to_unary_expr(src), src.id_string() + " ", verilog_precedencet::MIN); } },

    { ID_symbol, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_symbol(src); } },

    { ID_verilog_identifier, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_verilog_identifier(to_verilog_identifier_expr(src)); } },

    { ID_nondet_symbol, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_nondet_symbol(src); } },

    { ID_next_symbol, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_next_symbol(src); } },

    { ID_hierarchical_identifier, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_hierarchical_identifier(
      to_hierarchical_identifier_expr(src)); } },

    { ID_constant, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_constant(to_constant_expr(src)); } },

    { ID_constraint_select_one, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function("$ND", src); } },

    { ID_verilog_past, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function("$past", src); } },

    { ID_onehot, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function("$onehot", src); } },
    
    { ID_onehot0, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function("$onehot0", src); } },

    { ID_sva_overlapped_implication, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("|->", to_binary_expr(src)); } },

    { ID_sva_non_overlapped_implication, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("|=>", to_binary_expr(src)); } },

    { ID_sva_cycle_delay_star, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_cycle_delay("##[*]", to_sva_cycle_delay_star_expr(src)); } },

    { ID_sva_cycle_delay_plus, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_cycle_delay("##[+]", to_sva_cycle_delay_plus_expr(src)); } },

    { ID_sva_overlapped_followed_by, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("#-#", to_binary_expr(src)); } },

    { ID_sva_nonoverlapped_followed_by, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("#=#", to_binary_expr(src)); } },

    { ID_sva_cycle_delay, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_cycle_delay(to_sva_cycle_delay_expr(src)); } },

    { ID_sva_strong, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function("strong", src); } },

    { ID_sva_weak, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function("weak", src); } },

    { ID_sva_sequence_property, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_rec(to_sva_sequence_property_expr_base(src).sequence()); } },

    { ID_sva_implicit_strong, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_rec(to_sva_sequence_property_expr_base(src).sequence()); } },

    { ID_sva_implicit_weak, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_rec(to_sva_sequence_property_expr_base(src).sequence()); } },

    // These are invisible
    { ID_sva_boolean, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_rec(to_sva_boolean_expr(src).op()); } },

    { ID_sva_sequence_first_match, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_sequence_first_match(
      to_sva_sequence_first_match_expr(src)); } },

    { ID_sva_sequence_intersect, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("intersect", to_binary_expr(src)); } },
    // not sure about precedence

    { ID_sva_sequence_throughout, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("throughout", to_binary_expr(src)); } },
    // not sure about precedence

    { ID_sva_sequence_within, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("within", to_binary_expr(src)); } },
    // not sure about precedence

    { ID_sva_always, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary("always", to_sva_always_expr(src)); } },

    { ID_sva_sequence_repetition_plus, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_sequence_repetition(
      "+", to_sva_sequence_repetition_plus_expr(src)); } },

    { ID_sva_sequence_non_consecutive_repetition, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_sequence_repetition(
      "=", to_sva_sequence_non_consecutive_repetition_expr(src)); } },

    { ID_sva_sequence_repetition_star, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_sequence_repetition(
      "*", to_sva_sequence_repetition_star_expr(src)); } },

    { ID_sva_sequence_goto_repetition, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_sequence_repetition(
      "->", to_sva_sequence_goto_repetition_expr(src)); } },

    { ID_sva_ranged_always, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_ranged_predicate(
      "always", to_sva_ranged_always_expr(src)); } },

    { ID_sva_s_always, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_ranged_predicate("s_always", to_sva_s_always_expr(src)); } },

    { ID_sva_cover, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary("cover", to_sva_cover_expr(src)); } },

    { ID_sva_assume, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary("assume", to_sva_assume_expr(src)); } },

    { ID_sva_accept_on, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_abort("accept_on", to_sva_abort_expr(src)); } },

    { ID_sva_reject_on, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_abort("reject_on", to_sva_abort_expr(src)); } },

    { ID_sva_sync_accept_on, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_abort("sync_accept_on", to_sva_abort_expr(src)); } },

    { ID_sva_sync_reject_on, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_abort("sync_reject_on", to_sva_abort_expr(src)); } },

    { ID_sva_nexttime, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary("nexttime", to_sva_nexttime_expr(src)); } },

    { ID_sva_indexed_nexttime, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_indexed_binary(
      "nexttime", to_sva_indexed_nexttime_expr(src)); } },

    { ID_sva_s_nexttime, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary("s_nexttime", to_sva_s_nexttime_expr(src)); } },

    { ID_sva_indexed_s_nexttime, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_indexed_binary(
      "s_nexttime", to_sva_indexed_s_nexttime_expr(src)); } },

    { ID_sva_disable_iff, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_abort("disable iff", to_sva_abort_expr(src)); } },

    { ID_sva_sequence_disable_iff, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_abort(
      "disable iff", to_sva_sequence_disable_iff_expr(src)); } },

    { ID_sva_eventually, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_ranged_predicate(
      "eventually", to_sva_eventually_expr(src)); } },

    { ID_sva_s_eventually, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary("s_eventually", to_sva_s_eventually_expr(src)); } },

    { ID_sva_ranged_s_eventually, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_ranged_predicate(
      "s_eventually", to_sva_ranged_s_eventually_expr(src)); } },

    { ID_sva_until, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("until", to_sva_until_expr(src)); } },

    { ID_sva_s_until, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("s_until", to_sva_s_until_expr(src)); } },

    { ID_sva_until_with, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("until_with", to_sva_until_with_expr(src)); } },

    { ID_sva_s_until_with, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_binary("s_until_with", to_sva_s_until_with_expr(src)); } },

    { ID_sva_if, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_if(to_sva_if_expr(src)); } },

    { ID_sva_case, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_case(to_sva_case_expr(src)); } },

    { ID_function_call, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function_call(to_function_call_expr(src)); } },

    { ID_clog2, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_function("$clog2", src); } },

    { ID_verilog_streaming_concatenation_left_to_right, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_streaming_concatenation(
      ">>", to_verilog_streaming_concatenation_expr(src)); } },

    { ID_verilog_streaming_concatenation_right_to_left, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_streaming_concatenation(
      "<<", to_verilog_streaming_concatenation_expr(src)); } },

    { ID_verilog_inside, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_inside(to_verilog_inside_expr(src)); } },

    { ID_verilog_value_range, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_value_range(to_verilog_value_range_expr(src)); } },

    { ID_postincrement, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sva_unary(to_unary_expr(src), "++"); } },

    { ID_nand, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_function(src.id_string(), src); } },

    { ID_nor, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_function(src.id_string(), src); } },

    // veval: Boolean xor/xnor printed as `xor(a, b)', which is not
    // SystemVerilog; on one-bit operands the bitwise operators mean the same.
    { ID_xnor, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "~^", verilog_precedencet::XOR); } },

    { ID_xor, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_binary(
      to_multi_ary_expr(src), "^", verilog_precedencet::XOR); } },

    // veval: `$countones' is lowered to popcount, which printed as an irep
    // dump; print the SystemVerilog function it came from.
    { ID_popcount, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_function("$countones", src); } },

    { ID_zero_extend, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_rec(to_zero_extend_expr(src).op()); } },

    { ID_sva_sequence_property_instance, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_sequence_property_instance(
      to_sva_sequence_property_instance_expr(src)); } },

    { ID_struct, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_struct(to_struct_expr(src)); } },

    { ID_verilog_unbased_unsized_literal, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_unbased_unsized_literal(to_verilog_unbased_unsized_literal_expr(src)); } },

    { ID_verilog_based_unsized_literal, [](expr2verilogt &expr2verilog, const exprt &src) { 
    return expr2verilog.convert_based_unsized_literal(to_verilog_based_unsized_literal_expr(src)); } },

    { ID_union, [](expr2verilogt &expr2verilog, const exprt &src) {
    return expr2verilog.convert_union(to_union_expr(src)); } }
  };

  auto action = action_table.find(src.id());

  if(action != action_table.end())
  {
    return action->second(*this, src);
  }
  else
  {
    // no VERILOG language expression for internal representation
    return convert_norep(src);
  }
}

/*******************************************************************\

Function: expr2verilogt::convert

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

std::string expr2verilogt::convert(const exprt &src)
{
  return convert_rec(src).s;
}

/*******************************************************************\

Function: expr2verilogt::convert

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

std::string expr2verilogt::convert(const typet &type)
{
  if(type.id() == ID_verilog_shortint)
    return "shortint";
  else if(type.id() == ID_verilog_int)
    return "int";
  else if(type.id() == ID_verilog_longint)
    return "longint";
  else if(type.id() == ID_verilog_byte)
    return "byte";
  else if(type.id() == ID_verilog_bit)
    return "bit";
  else if(type.id() == ID_verilog_logic)
    return "logic";
  else if(type.id() == ID_verilog_integer)
    return "integer";
  else if(type.id() == ID_verilog_string)
    return "string";
  else if(type.id() == ID_verilog_reg)
    return "reg";
  else if(type.id() == ID_verilog_time)
    return "time";
  else if(type.id() == ID_bool)
    return "bool";
  else if(type.id()==ID_unsignedbv || type.id()==ID_signedbv)
  {
    unsigned width=to_bitvector_type(type).get_width();
    bool increasing = type.get_bool(ID_C_increasing);
    unsigned offset=atoi(type.get(ID_C_offset).c_str());

    if(width!=0)
    {
      std::string dest;
      if(type.id()==ID_unsignedbv)
        dest="";
      else if(type.id()==ID_signedbv)
        dest="signed ";

      dest+='[';

      if(increasing)
      {
        dest+=std::to_string(offset);
        dest += ":";
        dest += std::to_string(offset + width - 1);
      }
      else
      {
        dest+=std::to_string(offset+width-1);
        dest += ":";
        dest += std::to_string(offset);
      }

      dest+="]";
      return dest;
    }
  }
  else if(type.id()==ID_array)
  {
    auto &array_type = to_array_type(type);

    std::string dest;

    if(type.get(ID_C_verilog_type) == ID_verilog_unpacked_array)
      dest += "unpacked ";
    else if(type.get(ID_C_verilog_type) == ID_verilog_packed_array)
      dest += "packed ";

    dest += "array [";

    dest += convert(array_type.size());

    dest+="] of ";
    dest += convert(array_type.element_type());

    return dest;
  }
  else if(type.id() == ID_verilog_chandle)
    return "chandle";
  else if(type.id() == ID_verilog_class_type)
    return "class";
  else if(type.id() == ID_verilog_event)
    return "event";
  else if(type.id() == ID_verilog_genvar)
    return "genvar";
  else if(type.id() == ID_verilog_new)
    return "new";
  else if(type.id()==ID_integer)
    return "integer";
  else if(type.id()==ID_verilog_shortreal)
    return "shortreal";
  else if(type.id()==ID_verilog_real)
    return "real";
  else if(type.id()==ID_verilog_realtime)
    return "realtime";
  else if(type.id() == ID_verilog_null)
    return "null";
  else if(type.id() == ID_verilog_enum)
  {
    return "enum";
  }
  else if(type.id() == ID_verilog_queue)
  {
    std::string dest = "queue of ";
    dest += convert(to_type_with_subtype(type).subtype());
    return dest;
  }
  else if(type.id() == ID_struct)
  {
    if(type.get_bool(ID_packed))
      return "struct packed";
    else
      return "struct";
  }
  else if(type.id() == ID_union)
  {
    return "union";
  }
  else if(type.id() == ID_verilog_type_reference)
  {
    auto &type_reference = to_verilog_type_reference(type);
    if(type_reference.expression_op().is_not_nil())
      return "type(" + convert(type_reference.expression_op()) + ")";
    else
      return "type(" + convert(type_reference.type_op()) + ")";
  }
  else if(type.id() == ID_verilog_void)
    return "void";

  return "IREP("+type.pretty()+")";
}

/*******************************************************************\

Function: expr2verilog

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

std::string expr2verilog(const exprt &expr, const namespacet &ns)
{
  expr2verilogt expr2verilog(ns);
  return expr2verilog.convert(expr);
}

/*******************************************************************\

Function: type2verilog

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

std::string type2verilog(const typet &type, const namespacet &ns)
{
  expr2verilogt expr2verilog(ns);
  return expr2verilog.convert(type);
}
