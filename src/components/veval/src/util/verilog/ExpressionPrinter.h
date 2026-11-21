#ifndef VEVAL_FRONTEND_TYPEDPRINT_H
#define VEVAL_FRONTEND_TYPEDPRINT_H

#include <map>
#include <optional>
#include <stdexcept>
#include <vector>
#include <util/expr.h>
#include <util/mp_arith.h>

class symbolt;

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
// Emit executable SystemVerilog from a prepared expression.
std::string productExpression(const exprt& expression, const namespacet& ns);
// Describe shared function bodies without recursively expanding their definition.
std::string describeExpression(const exprt& expression, const namespacet& ns);
struct VerilogTypeDeclaration {
    std::string prefix, suffix;
    std::string scope, name, declarationFile;
    std::string declare(const std::string& identifier) const {
        return prefix + " " + identifier + suffix;
    }
};

// Render a sized declaration while keeping nominal aggregate types in their
// original compilation unit or package. The typedef index is built on demand.
class VerilogTypePrinter {
public:
    explicit VerilogTypePrinter(const namespacet& ns) : ns(ns) {}
    std::optional<VerilogTypeDeclaration> spelling(const typet&);
private:
    const namespacet& ns;
    bool indexed = false;
    std::map<typet, std::vector<const symbolt*>> aggregateTypedefs;
    std::map<irep_idt, std::vector<const symbolt*>> enumTypedefs;
    void indexTypes();
};

class FunctionPrintError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FunctionPrint {
    exprt expression;
    std::map<std::string, std::string> definitions;
    std::vector<VerilogTypeDeclaration> prerequisites;
};
// Render lexical sharing in pure function expressions as automatic SV
// functions. Both product output and the measurement helper use this path.
FunctionPrint prepareFunctionPrint(exprt expression, const namespacet &ns);
} // namespace frontend
} // namespace veval
#endif
