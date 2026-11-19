#pragma once

// IdString moved from a global string table to a per-design Twine pool in
// newer Yosys releases. Every pass scopes the design while resolving names.
namespace {
namespace veval_yosys {
#ifdef VEVAL_YOSYS_TWINE
static thread_local RTLIL::Design *activeDesign = nullptr;
struct Scope {
    RTLIL::Design *previous;
    explicit Scope(RTLIL::Design *design) : previous(activeDesign) { activeDesign = design; }
    ~Scope() { activeDesign = previous; }
};
inline RTLIL::IdString identifier(const std::string &text) {
    return activeDesign->twines().add(RTLIL::escape_id(text));
}
inline std::string unescaped(RTLIL::IdString id) {
    return activeDesign->twines().unescaped_str(id);
}
#else
struct Scope { explicit Scope(RTLIL::Design *) {} };
inline RTLIL::IdString identifier(const std::string &text) {
    return RTLIL::IdString(RTLIL::escape_id(text));
}
inline std::string unescaped(RTLIL::IdString id) { return RTLIL::unescape_id(id); }
#endif
} // namespace veval_yosys
} // namespace
