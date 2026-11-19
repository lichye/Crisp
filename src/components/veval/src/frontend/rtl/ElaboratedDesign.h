// Own the design produced by hw-cbmc elaboration: resolved symbols, typed
// port bindings and transition expressions after parameter/generate expansion.
// The Verilog frontend performs parsing, type checking and synthesis;
// this class does not invoke an EBMC model checker. Frontend failures are
// reported as exceptions rather than replaced with an abstract design.

#ifndef VEVAL_FRONTEND_ELABORATEDDESIGN_H
#define VEVAL_FRONTEND_ELABORATEDDESIGN_H

#include <memory>
#include <string>
#include <vector>

class cmdlinet;
class message_handlert;
class transition_systemt;

namespace veval {
namespace frontend {

struct ElaborateOptions {
    // Design files passed to the frontend in the supplied order. The frontend
    // resolves module dependencies during elaboration.
    std::vector<std::string> files;
    // Required top module: selects the design root.
    std::string top;

    // `-I` paths for the Verilog preprocessor, and extra `-D` defines
    // ("NAME" or "NAME=VALUE").
    std::vector<std::string> includePaths;
    std::vector<std::string> defines;
    bool systemVerilog = true;

    // 0 (silent) to 10 (everything). The frontend is chatty at 6 and above.
    unsigned verbosity = 1;
};

// An elaborated design. Holds the symbol table and transition system, and
// owns the machinery that produced them, so the caller can keep expressions
// that point into it.
class ElaboratedDesign {
public:
    // Elaborate `options`. Throws std::runtime_error with the frontend's own
    // diagnostic when the design does not elaborate.
    static ElaboratedDesign elaborate(const ElaborateOptions& options);

    ElaboratedDesign(ElaboratedDesign&&) noexcept;
    ElaboratedDesign& operator=(ElaboratedDesign&&) noexcept;
    ~ElaboratedDesign();

    const transition_systemt& transitionSystem() const;

    // Writable, for a pass that adds to the system -- splicing a monitor in,
    // above all. Doing so consumes the design: elaborate again for another.
    transition_systemt& transitionSystem();

    // The handler the frontend reported through. Later passes reuse it so
    // that all diagnostics come out the same way.
    message_handlert& messageHandler() const;

    // Identifier of the design root, which is `$root' itself -- NOT the top
    // module. Signals live one level below it.
    std::string top() const;

    // Identifier of the top module's instance under $root, which is where
    // its signals actually live: `Verilog::$root.child'. This is the prefix
    // to build a signal name from, and mistaking top() for it silently
    // produces names that refer to nothing.
    std::string topInstance() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    explicit ElaboratedDesign(std::unique_ptr<Impl>);
};

// Strips the language prefix and the implicit `$root` scope from every name
// in `text`, so it works on a printed expression as well as on a single
// identifier. Nothing else about the string is changed.
std::string displayName(const std::string& text);

}  // namespace frontend
}  // namespace veval

#endif  // VEVAL_FRONTEND_ELABORATEDDESIGN_H
