#ifndef VEVAL_FRONTEND_TYPEDPRINT_H
#define VEVAL_FRONTEND_TYPEDPRINT_H

#include <map>
#include <optional>
#include <util/expr.h>
#include <util/mp_arith.h>

namespace veval {
namespace frontend {
struct StaticArrayElement {
    mp_integer internalIndex, declaredIndex, offset, size;
    bool increasing;
};
// Decode a constant, in-bounds unpacked-array element through its typed layout.
std::optional<StaticArrayElement> staticArrayElement(const exprt &expression);

// Make implicit bit-vector conversions visible to the existing SV printer.
// Resize using the operand's signing first, then change the interpretation.
exprt prepareTypedPrint(exprt expression);
struct FunctionPrint {
    exprt expression;
    std::map<std::string, std::string> definitions;
};
// Render lexical sharing in pure function expressions as automatic SV
// functions. Both product output and the measurement helper use this path.
FunctionPrint prepareFunctionPrint(exprt expression, const namespacet &ns);
} // namespace frontend
} // namespace veval
#endif
