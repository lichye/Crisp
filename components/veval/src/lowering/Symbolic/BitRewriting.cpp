#include "BitRewriting.h"
#include "PackedBits.h"

#include <util/arith_tools.h>
#include <util/bitvector_expr.h>
#include <util/mathematical_types.h>
#include <trans-word-level/next_symbol.h>

#include <optional>

namespace veval {
namespace lowering {
namespace symbolic {

// How many bits a type is, or zero when the answer is not a plain width.
mp_integer bitWidth(const typet& type) {
    if (type.id() == ID_bool) return 1;
    if (can_cast_type<bitvector_typet>(type))
        return to_bitvector_type(type).get_width();
    return 0;
}

// The complement of an expression, in the operator its type takes.
exprt complementOf(const exprt& expression) {
    if (expression.type().id() == ID_bool) return not_exprt{expression};
    return bitnot_exprt{expression};
}

bool namesSignal(const exprt& expr, const irep_idt& name) {
    if (expr.id() == ID_symbol && to_symbol_expr(expr).get_identifier() == name) return true;
    for (const auto& op : expr.operands()) if (namesSignal(op, name)) return true;
    return false;
}

// Reconstruct exactly the bits an expression reads. Choose the lowest port
// position for a duplicated source bit. Adjacent chosen bits coalesce into
// selects; nonadjacent runs become a concatenation in original bit order.
// Missing bits and signed whole reads without a printable signing cast fail.
bool rebase(exprt& expr, const irep_idt& name, const symbol_exprt& port,
            const std::vector<Segment>& segments, bool wholeSignal,
            bool* wholeRead, bool printing) {
    auto reads = [&name](const exprt& src) {
        return src.id() == ID_symbol &&
               to_symbol_expr(src).get_identifier() == name;
    };
    // The product writer now renders two-state bit-vector casts explicitly
    // through prepareTypedPrint. Keep those casts in the rebased tree instead
    // of discarding an otherwise exact landing. Other conversion domains are
    // still refused here; a fixed layout does not authorize a new conversion.
    if (printing && expr.id() == ID_typecast && namesSignal(expr, name)) {
        const auto& operand = to_typecast_expr(expr).op();
        const auto explicitBitVector = [](const typet& type) {
            return type.id() == ID_unsignedbv || type.id() == ID_signedbv;
        };
        if (bitWidth(expr.type()) != bitWidth(operand.type()) ||
            (expr.type().id() == ID_signedbv) != (operand.type().id() == ID_signedbv)) {
          if (!(explicitBitVector(expr.type()) &&
                (explicitBitVector(operand.type()) || operand.type().id() == ID_bool)))
            return false;
        }
    }
    auto reconstruct = [&](const mp_integer& from, const mp_integer& taken,
                           const typet& type, const typet& indexType) -> std::optional<exprt> {
        if (taken <= 0) return {};
        std::vector<SelectedBit> chosen;
        for (mp_integer bit = from; bit < from + taken; ++bit) {
            std::optional<mp_integer> at;
            for (const auto& segment : segments)
                if (bit >= segment.sigLo && bit < segment.sigLo + segment.width) {
                    const mp_integer candidate = segment.portLo + bit - segment.sigLo;
                    if (!at.has_value() || candidate < *at) at = candidate;
                }
            if (!at.has_value()) return {};
            chosen.push_back({port, *at});
        }
        auto pieces = packedSlices(chosen, indexType, type.id() == ID_bool);
        if (pieces.size() == 1) {
            pieces.front().type() = type;
            return pieces.front();
        }
        if (type.id() != ID_unsignedbv) return {};
        return concatenation_exprt{std::move(pieces), type};
    };
    if (expr.id() == ID_extractbits || expr.id() == ID_extractbit) {
        const auto& source = expr.operands()[0];
        if (reads(source)) {
            auto index = numeric_cast<mp_integer>(expr.operands()[1]);
            if (!index.has_value()) return false;
            // SV selects are unsigned, including selects of signed ports.
            if (expr.type().id() == ID_signedbv) return false;
            auto rebuilt = reconstruct(*index, bitWidth(expr.type()), expr.type(), expr.operands()[1].type());
            if (!rebuilt.has_value()) return false;
            expr = std::move(*rebuilt); return true;
        }
    }
    if (reads(expr)) {
        // expr2verilog's implicit typecast printer drops signing casts. Do
        // not silently turn a signed whole value into an unsigned select.
        if (!wholeSignal || expr.type().id() == ID_signedbv) return false;
        auto rebuilt = reconstruct(0, bitWidth(expr.type()), expr.type(), integer_typet{});
        if (!rebuilt.has_value()) return false;
        expr = std::move(*rebuilt);
        if (wholeRead != nullptr) *wholeRead = true;
        return true;
    }
    for (auto& op : expr.operands())
        if (!rebase(op, name, port, segments, wholeSignal, wholeRead, printing)) return false;
    return true;
}

// Identify an available value whose signing cannot be preserved by the
// current printer, instead of misreporting that value as missing source bits.
bool signedWholeRead(const exprt& expr, const irep_idt& name) {
    if ((expr.id() == ID_extractbits || expr.id() == ID_extractbit) &&
        expr.operands()[0].id() == ID_symbol &&
        to_symbol_expr(expr.operands()[0]).get_identifier() == name) return false;
    if (expr.id() == ID_symbol &&
        to_symbol_expr(expr).get_identifier() == name)
        return expr.type().id() == ID_signedbv;
    for (const auto& op : expr.operands())
        if (signedWholeRead(op, name)) return true;
    return false;
}

// Put back what `rebase' took away: a read of `port' at bit k becomes a read
// of the signal the run says bit k came from.
//
// Only the runs are needed, not the direction the transport went, because a
// segment is an equality between two bit ranges.
//
// ONE pass over ALL the rebases, rather than one pass each. A whole-member
// concatenation puts SEVERAL signals on the SAME port, and undoing them a
// signal at a time each pass meets the other members' selects of that port,
// finds no run of its own that covers them, and reports the round trip failed
// on a transport that is exactly right.
//
// A read of a rebased port that NO run covers is still a failure, as it was:
// it is a read of bits nothing claims to have carried. Ports that were never
// rebased are left alone, also as before -- nothing here claims anything about
// them.
bool unrebase(exprt& expr, const RebasesByPort& byPort) {
    if (expr.id() == ID_extractbits || expr.id() == ID_extractbit) {
        auto& extract = expr.id() == ID_extractbits
                            ? static_cast<exprt&>(to_extractbits_expr(expr))
                            : static_cast<exprt&>(to_extractbit_expr(expr));
        auto& src = extract.operands()[0];
        auto& index = extract.operands()[1];

        auto rebases = src.id() == ID_symbol
                           ? byPort.find(to_symbol_expr(src).get_identifier())
                           : byPort.end();

        if (rebases != byPort.end()) {
            auto at = numeric_cast<mp_integer>(index);
            if (!at.has_value()) return false;

            mp_integer taken =
                expr.id() == ID_extractbit || expr.type().id() == ID_bool
                    ? 1
                    : to_bitvector_type(expr.type()).get_width();

            for (const auto* rebased : rebases->second)
                for (const auto& segment : rebased->segments) {
                    if (*at < segment.portLo ||
                        *at + taken > segment.portLo + segment.width)
                        continue;
                    const auto back = segment.sigLo + (*at - segment.portLo);

                    // `rebase' turned a BARE read of the signal into this
                    // select, so the bare read is what has to come back. A
                    // full-width select of the signal is a different
                    // expression, and putting that back instead would fail
                    // the comparison on a landing that is right.
                    if (rebased->wholeRead && back == 0 &&
                        taken == bitWidth(rebased->name.type()))
                        expr = rebased->name;
                    else {
                        src = rebased->name;
                        index = from_integer(back, index.type());
                    }
                    return true;
                }

            return false;
        }
    }

    for (auto& op : expr.operands())
        if (!unrebase(op, byPort)) return false;

    return true;
}

// Canonicalize only exact contiguous reads after undoing a split rebase.
// {s[h:m],s[m-1:l]} and s[h:l] have identical unsigned select semantics.
// Signed whole values are never identified with unsigned full-width selects.
void normalizeContiguousReads(exprt& expr) {
    for (auto& op : expr.operands()) normalizeContiguousReads(op);
    if (expr.id() == ID_concatenation && expr.type().id() == ID_unsignedbv) {
        exprt source;
        mp_integer low = 0, taken = 0;
        bool contiguous = true;
        for (auto it = expr.operands().rbegin(); it != expr.operands().rend(); ++it) {
            if ((it->id() != ID_extractbits && it->id() != ID_extractbit) ||
                it->operands()[0].id() != ID_symbol) { contiguous = false; break; }
            auto at = numeric_cast<mp_integer>(it->operands()[1]);
            if (!at.has_value()) { contiguous = false; break; }
            if (taken == 0) { source = it->operands()[0]; low = *at; }
            else if (source != it->operands()[0] || *at != low + taken) {
                contiguous = false; break;
            }
            taken += bitWidth(it->type());
        }
        if (contiguous && taken == bitWidth(expr.type()))
            expr = extractbits_exprt{source, from_integer(low, integer_typet{}), expr.type()};
    }
    if (expr.id() == ID_extractbits && expr.type().id() == ID_unsignedbv) {
        const auto& source = expr.operands()[0];
        auto at = numeric_cast<mp_integer>(expr.operands()[1]);
        if (at.has_value() && *at == 0 && source.type() == expr.type()) expr = source;
    }
}

// A type conversion that keeps every bit is the same wires under another
// declared shape -- a packed struct read as the vector it is laid out as, a
// signed port fed from an unsigned net of the same width, a one-bit vector
// where the property said bool. Undoing a transport leaves such a conversion
// around the parent's symbol, and comparing with the original must look
// through it. A conversion that changes the width is NOT looked through: it
// is exactly the error the comparison exists to catch.
void dropWidthPreservingCasts(exprt& expr) {
    for (auto& op : expr.operands()) dropWidthPreservingCasts(op);
    if (expr.id() != ID_typecast) return;
    const auto& op = to_typecast_expr(expr).op();
    if (op.type() == expr.type()) { exprt inner = op; expr = inner; return; }
    const auto width = bitWidth(expr.type());
    if (width <= 0 || width != bitWidth(op.type())) return;
    if (op.id() == ID_symbol)
        expr = symbol_exprt{to_symbol_expr(op).get_identifier(), expr.type()};
    else if (op.id() == ID_next_symbol)
        expr = next_symbol_exprt{to_next_symbol_expr(op).identifier(), expr.type()};
}

// Push complements into bit selects and cancel double complements:
// `(~e)[i]' becomes `!e[i]', `(~e)[h:l]' becomes `~e[h:l]', and `~~e' and
// `!!e' become `e'. Each step keeps the value. A signal carried as the
// complement of a port arrives as `~port'; without this its bit reads would
// print as `(~port)[i]', which is not SystemVerilog, and the round trip would
// compare `~~x' with `x'.
void simplifyComplements(exprt& expr) {
    for (auto& op : expr.operands()) simplifyComplements(op);
    auto isComplement = [](const exprt& e) {
        return e.id() == ID_bitnot || e.id() == ID_not;
    };
    if (isComplement(expr) && isComplement(to_unary_expr(expr).op())) {
        const auto& inner = to_unary_expr(to_unary_expr(expr).op()).op();
        if (inner.type() == expr.type()) {
            exprt kept = inner;
            expr = std::move(kept);
        }
        return;
    }
    if (expr.id() == ID_extractbit) {
        const auto& extract = to_extractbit_expr(expr);
        if (extract.src().id() != ID_bitnot) return;
        exprt bit = extractbit_exprt{to_unary_expr(extract.src()).op(),
                                     extract.index()};
        expr = not_exprt{std::move(bit)};
        return;
    }
    if (expr.id() == ID_extractbits) {
        const auto& extract = to_extractbits_expr(expr);
        if (extract.src().id() != ID_bitnot) return;
        exprt bits = extractbits_exprt{to_unary_expr(extract.src()).op(),
                                       extract.index(), expr.type()};
        expr = bitnot_exprt{std::move(bits)};
        return;
    }
}

// Rename atoms by identifier, keeping each occurrence's own type. Undoing an
// edge must not retype anything: the next edge up keys its own reversal on
// the symbols exactly as the forward substitution left them, casts and all,
// and the casts are judged once, at the final comparison. (The first version
// normalized between edges and so could not undo a route that changed shape
// twice -- a false rejection the 2026-10-22 review reproduced on the Coral
// concat corpus.)
void renameAtoms(exprt& expr, const std::map<irep_idt, irep_idt>& rename) {
    for (auto& op : expr.operands()) renameAtoms(op, rename);
    if (expr.id() == ID_symbol) {
        const auto it = rename.find(to_symbol_expr(expr).get_identifier());
        if (it != rename.end()) expr = symbol_exprt{it->second, expr.type()};
    } else if (expr.id() == ID_next_symbol) {
        const auto it = rename.find(to_next_symbol_expr(expr).identifier());
        if (it != rename.end()) expr = next_symbol_exprt{it->second, expr.type()};
    }
}

// Replace each listed port, current or next, by the complement of the parent
// signal it stood for, keeping the occurrence's type.
void complementAtoms(exprt& expr, const std::map<irep_idt, irep_idt>& complemented) {
    for (auto& op : expr.operands()) complementAtoms(op, complemented);
    if (expr.id() == ID_symbol) {
        const auto it = complemented.find(to_symbol_expr(expr).get_identifier());
        if (it != complemented.end())
            expr = complementOf(symbol_exprt{it->second, expr.type()});
    } else if (expr.id() == ID_next_symbol) {
        const auto it = complemented.find(to_next_symbol_expr(expr).identifier());
        if (it != complemented.end())
            expr = complementOf(next_symbol_exprt{it->second, expr.type()});
    }
}

}  // namespace symbolic
}  // namespace lowering
}  // namespace veval
