#include "LocalVariableMonitor.h"
#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <temporal-logic/temporal_logic.h>
#include <trans-word-level/next_symbol.h>
#include <util/arith_tools.h>
#include <util/bitvector_expr.h>
#include <util/bitvector_types.h>
#include <util/expr_util.h>
#include <util/mathematical_types.h>
#include <util/replace_expr.h>
#include <verilog/sva_expr.h>

namespace veval::sva {
namespace {
constexpr const char *localsId = "#veval_sva_locals";
constexpr const char *matchId = "#veval_sva_match";
constexpr const char *assignId = "#veval_sva_assign";
[[noreturn]] void refuse(const std::string &why) { throw std::runtime_error("SVA-LOCALS: " + why); }
exprt peel(exprt e) {
    while ((e.id() == ID_sva_boolean || e.id() == ID_sva_sequence_property ||
            e.id() == ID_sva_implicit_weak || e.id() == ID_sva_weak) &&
           e.operands().size() == 1)
        e = e.operands()[0];
    return e;
}
std::optional<exprt> predicate(exprt e) {
    e = peel(std::move(e));
    if ((e.id() == ID_sva_overlapped_implication || e.id() == ID_sva_implies) &&
        e.operands().size() == 2) {
        auto a = predicate(e.operands()[0]), b = predicate(e.operands()[1]);
        if (a && b)
            return implies_exprt{*a, *b};
    }
    bool captures = false;
    e.visit_pre([&](const exprt &n) {
        captures |= n.id() == matchId || n.id() == localsId || n.id() == assignId;
    });
    if (e.type().id() == ID_bool && !has_temporal_operator(e) && !captures)
        return e;
    return {};
}
unsigned width(const typet &t) {
    if (t.id() == ID_bool)
        return 1;
    if (t.id() == ID_unsignedbv || t.id() == ID_signedbv)
        return to_bitvector_type(t).get_width();
    refuse("local variable must have a packed two-state type");
}
unsigned bitsFor(std::size_t n) {
    unsigned bits = 1;
    while ((std::size_t{1} << bits) < n)
        ++bits;
    return bits;
}
unsigned constant(const exprt &e) {
    auto n = numeric_cast<mp_integer>(e);
    if (!n || *n < 0 || *n > 1024)
        refuse("delay must be a constant in [0,1024]");
    return numeric_cast_v<unsigned>(*n);
}
struct RepetitionBounds {
    unsigned lo, hi;
    bool unbounded;
};
RepetitionBounds repetitionBounds(const exprt &e) {
    if (!predicate(e.operands().at(0)))
        refuse("antecedent repetition requires a single-cycle Boolean operand without assignments");
    const auto &lo = e.operands().at(1), &hi = e.operands().at(2);
    const bool unbounded = lo.is_nil() || hi.id() == ID_infinity;
    const unsigned lower = lo.is_nil() ? 0 : constant(lo);
    const unsigned upper = unbounded || hi.is_nil() ? lower : constant(hi);
    if (!unbounded && upper < lower)
        refuse("descending repetition interval");
    return {lower, upper, unbounded};
}
bool containsRepetition(const exprt &e) {
    if (e.id() == ID_sva_sequence_repetition_star)
        return true;
    return std::any_of(e.operands().begin(), e.operands().end(), containsRepetition);
}
// Empty matches end one sample BEFORE their start. An epsilon edge at the
// current sample would therefore be off by one. Split off empty matches using
// IEEE 1800 16.9.2.1 before building the ordinary, nonempty sequence graph.
struct NonemptySequence {
    exprt body;
    bool empty = false;
};
exprt sequenceOr(std::vector<exprt> alternatives) {
    exprt result = false_exprt{};
    for (auto &part : alternatives) {
        if (part.is_false()) continue;
        result = result.is_false() ? std::move(part)
                                  : exprt{sva_or_exprt{result, part, verilog_sva_sequence_typet{}}};
    }
    return result;
}
NonemptySequence nonemptySequence(const exprt &raw) {
    auto e = peel(raw);
    if (!containsRepetition(e))
        return {e};
    if (e.id() == ID_sva_sequence_repetition_star) {
        const auto bounds = repetitionBounds(e);
        if (!bounds.unbounded && bounds.hi == 0)
            return {false_exprt{}, true};
        e.operands()[1] = from_integer(std::max(1u, bounds.lo), integer_typet{});
        if (bounds.unbounded)
            e.operands()[2] = infinity_exprt{integer_typet{}};
        return {std::move(e), bounds.lo == 0};
    }
    if (e.id() == ID_sva_or) {
        auto a = nonemptySequence(e.operands()[0]), b = nonemptySequence(e.operands()[1]);
        return {sequenceOr({a.body, b.body}), a.empty || b.empty};
    }
    if (e.id() == matchId) {
        auto inner = nonemptySequence(e.operands()[0]);
        if (inner.empty)
            refuse("match assignment on an empty-admitting sequence is not supported");
        e.operands()[0] = std::move(inner.body);
        return {std::move(e)};
    }
    if (e.id() != ID_sva_cycle_delay)
        refuse("repetition nested in unsupported antecedent operator: " + e.id_string());
    const auto &lhs = e.operands()[0], &rhs = e.operands()[3];
    auto right = nonemptySequence(rhs);
    const auto lo = constant(e.operands()[1]);
    const bool unbounded = e.operands()[2].id() == ID_infinity;
    const auto hi = unbounded || e.operands()[2].is_nil() ? lo : constant(e.operands()[2]);
    if (!unbounded && hi < lo) refuse("descending delay interval");
    auto delayed = [&](exprt a, unsigned lower, exprt b, unsigned shift = 0) -> exprt {
        if (b.is_false() || a.is_false() || (!unbounded && hi < lower))
            return false_exprt{};
        const auto first = from_integer(lower - shift, integer_typet{});
        exprt last = unbounded ? exprt{infinity_exprt{integer_typet{}}}
                              : lower == hi ? exprt{nil_exprt{}}
                                            : exprt{from_integer(hi - shift, integer_typet{})};
        return sva_cycle_delay_exprt{std::move(a), first, last, std::move(b)};
    };
    if (lhs.is_nil()) {
        // A leading ##n starts from an implicit one-sample true sequence.
        std::vector<exprt> parts{delayed(nil_exprt{}, lo, right.body)};
        if (right.empty)
            parts.push_back(delayed(nil_exprt{}, std::max(1u, lo), true_exprt{}, 1));
        return {sequenceOr(std::move(parts))};
    }
    auto left = nonemptySequence(lhs);
    std::vector<exprt> parts{delayed(left.body, lo, right.body)};
    if (left.empty)
        parts.push_back(delayed(nil_exprt{}, std::max(1u, lo), right.body, 1));
    if (right.empty)
        parts.push_back(delayed(left.body, std::max(1u, lo), true_exprt{}, 1));
    if (left.empty && right.empty)
        parts.push_back(delayed(nil_exprt{}, std::max(2u, lo), true_exprt{}, 2));
    const bool empty = left.empty && right.empty && lo <= 1 && (unbounded || hi >= 1);
    return {sequenceOr(std::move(parts)), empty};
}
struct Parsed {
    exprt body, abort = false_exprt{};
    std::vector<exprt> declarations;
};
Parsed parse(const exprt &original) {
    if (original.id() == ID_sva_assume)
        refuse("assume with local variables is not supported");
    // Initializers belong to the declaration's attempt, not necessarily to
    // the outer assertion launch. Hoisting a delayed sequence initializer
    // would sample its RHS too early. Only outer property initializers are
    // currently encoded; inner declarations still support match assignments.
    std::function<void(const exprt &, bool)> initializers = [&](const exprt &e, bool outer) {
        if (e.id() == localsId) {
            if (!outer)
                for (const auto &d : e.operands()[1].operands())
                    if (d.operands()[1].is_not_nil())
                        refuse("initializer in a nested sequence/property is not supported");
            initializers(e.operands()[0], outer);
            return;
        }
        const bool shell = e.id() == ID_sva_always || e.id() == ID_sva_disable_iff ||
                           e.id() == ID_sva_sequence_property || e.id() == ID_sva_implicit_weak ||
                           e.id() == ID_sva_weak;
        for (const auto &op : e.operands())
            initializers(op, outer && shell);
    };
    initializers(original, true);
    Parsed p;
    p.declarations = localDeclarations(original);
    p.body = withoutLocalDeclarations(original);
    if (p.body.id() == ID_sva_always && p.body.operands().size() == 1)
        p.body = p.body.operands()[0];
    if (p.body.id() == ID_sva_disable_iff) {
        p.abort = p.body.operands()[0];
        p.body = p.body.operands()[1];
    }
    bool nested = false;
    p.body.visit_pre([&](const exprt &e) { nested |= e.id() == ID_sva_disable_iff; });
    if (nested || !predicate(p.abort))
        refuse("requires one outer Boolean disable iff");
    p.body = peel(p.body);
    return p;
}
// The only abort-compatible consequent forms are a single point requirement
// (possibly after capture-only stages) and Boolean weak-until. This retains
// the existing refusal of two separate requirements under an abort.
unsigned requirements(const exprt &raw) {
    auto e = peel(raw);
    if (auto p = predicate(e))
        return p->is_true() ? 0 : 1;
    if (e.id() == matchId)
        return requirements(e.operands()[0]);
    if (e.id() == ID_sva_cycle_delay) {
        const auto lo = constant(e.operands()[1]);
        const auto &hi = e.operands()[2];
        if (hi.is_not_nil() && (hi.id() == ID_infinity || constant(hi) != lo))
            return 2;
        return (e.operands()[0].is_nil() ? 0 : requirements(e.operands()[0])) +
               requirements(e.operands()[3]);
    }
    if ((e.id() == ID_sva_until || e.id() == ID_sva_until_with) && predicate(e.operands()[0]) &&
        predicate(e.operands()[1]))
        return 1;
    return 2;
}
struct Target {
    irep_idt id;
    unsigned low, size;
};
struct Variable {
    symbol_exprt symbol;
    unsigned offset, size;
};
struct Edge {
    exprt guard = true_exprt{};
    std::vector<exprt> writes;
    std::size_t next = 0;
    bool tick = false, fail = false;
};
class Builder {
  public:
    std::map<irep_idt, Variable> vars;
    std::vector<std::vector<Edge>> nodes{3}; // idle, successful end, failed end
    unsigned dataWidth = 0;
    std::size_t node(std::vector<Edge> edges = {}) {
        if (nodes.size() >= 4096)
            refuse("monitor exceeds 4096 control locations");
        nodes.push_back(std::move(edges));
        return nodes.size() - 1;
    }
    std::optional<Target> target(const exprt &e) const {
        if (e.id() == ID_symbol) {
            auto f = vars.find(to_symbol_expr(e).identifier());
            if (f != vars.end())
                return Target{f->first, 0, f->second.size};
        }
        if ((e.id() == ID_extractbits || e.id() == ID_extractbit) && e.operands().size() == 2) {
            auto base = target(e.operands()[0]);
            auto offset = numeric_cast<mp_integer>(e.operands()[1]);
            if (base && offset && *offset >= 0 && *offset + width(e.type()) <= base->size)
                return Target{base->id, base->low + numeric_cast_v<unsigned>(*offset),
                              width(e.type())};
        }
        return {};
    }
    using Defined = std::set<std::pair<irep_idt, unsigned>>;
    void reads(const exprt &e, const Defined &known) const {
        if (e.id() == ID_verilog_past) {
            bool local = false;
            e.visit_pre([&](const exprt &n) { local |= n.get_bool("#veval_sva_local"); });
            if (local)
                refuse("sampled-value functions of local variables are not supported");
        }
        if (auto t = target(e)) {
            for (unsigned b = 0; b < t->size; ++b)
                if (!known.count({t->id, t->low + b}))
                    refuse("read before assignment: " + id2string(t->id));
            return;
        }
        if (e.id() == ID_symbol && e.get_bool("#veval_sva_local"))
            refuse("unknown local declaration");
        for (const auto &op : e.operands())
            reads(op, known);
    }
    void write(const exprt &a, Defined &known) const {
        if (a.id() != assignId || a.operands().size() != 2)
            refuse("unsupported match assignment");
        auto t = target(a.operands()[0]);
        if (!t)
            refuse("match assignment must write a local variable or fixed local bit range");
        reads(a.operands()[1], known);
        for (unsigned b = 0; b < t->size; ++b)
            known.insert({t->id, t->low + b});
    }
    Defined check(const exprt &raw, Defined known) const {
        auto e = peel(raw);
        if (e.id() == matchId) {
            known = check(e.operands()[0], std::move(known));
            for (const auto &a : e.operands()[1].operands())
                write(a, known);
        } else if (e.id() == ID_sva_or) {
            auto a = check(e.operands()[0], known), b = check(e.operands()[1], known);
            known.clear();
            std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                                  std::inserter(known, known.end()));
        } else if (e.id() == ID_sva_cycle_delay) {
            if (!e.operands()[0].is_nil())
                known = check(e.operands()[0], std::move(known));
            known = check(e.operands()[3], std::move(known));
        } else {
            reads(e, known);
        }
        return known;
    }
    std::size_t delay(unsigned n, std::size_t next) {
        while (n--)
            next = node({Edge{true_exprt{}, {}, next, true}});
        return next;
    }
    std::size_t sequence(const exprt &raw, std::size_t end, bool assertion) {
        auto e = peel(raw);
        if (auto p = predicate(e)) {
            std::vector<Edge> edges{Edge{*p, {}, end}};
            if (assertion)
                edges.push_back(Edge{not_exprt{*p}, {}, 2, false, true});
            return node(std::move(edges));
        }
        if (e.id() == matchId) {
            const auto assignments = node({Edge{true_exprt{}, e.operands()[1].operands(), end}});
            return sequence(e.operands()[0], assignments, assertion);
        }
        if (e.id() == ID_sva_or && !assertion)
            return node({Edge{true_exprt{}, {}, sequence(e.operands()[0], end, false)},
                         Edge{true_exprt{}, {}, sequence(e.operands()[1], end, false)}});
        if (e.id() == ID_sva_sequence_repetition_star) {
            if (assertion)
                refuse("repetition in a local-variable consequent is not supported");
            const auto bounds = repetitionBounds(e);
            if (bounds.lo == 0)
                refuse("internal error: empty repetition was not normalized");
            const auto condition = *predicate(e.operands()[0]);
            if (bounds.unbounded) {
                auto at = node();
                nodes[at] = {Edge{condition, {}, end}, Edge{condition, {}, at, true}};
                for (unsigned n = 1; n < bounds.lo; ++n)
                    at = node({Edge{condition, {}, at, true}});
                return at;
            }
            auto at = node({Edge{condition, {}, end}});
            for (unsigned n = bounds.hi; n > 1; --n) {
                std::vector<Edge> edges{Edge{condition, {}, at, true}};
                if (n - 1 >= bounds.lo)
                    edges.push_back(Edge{condition, {}, end});
                at = node(std::move(edges));
            }
            return at;
        }
        if (e.id() == ID_sva_cycle_delay) {
            const auto lo = constant(e.operands()[1]);
            const auto &rawHi = e.operands()[2];
            const bool unbounded = rawHi.id() == ID_infinity;
            const auto hi = rawHi.is_nil() || unbounded ? lo : constant(rawHi);
            if (hi < lo)
                refuse("descending delay interval");
            if (assertion && (unbounded || hi != lo))
                refuse("alternative consequent matches require universal failure tracking");
            const auto right = sequence(e.operands()[3], end, assertion);
            std::size_t wait;
            if (unbounded) {
                wait = node();
                nodes[wait] = {Edge{true_exprt{}, {}, right}, Edge{true_exprt{}, {}, wait, true}};
                wait = delay(lo, wait);
            } else {
                std::vector<Edge> alternatives;
                for (unsigned n = lo; n <= hi; ++n)
                    alternatives.push_back(Edge{true_exprt{}, {}, delay(n, right)});
                wait =
                    alternatives.size() == 1 ? alternatives[0].next : node(std::move(alternatives));
            }
            return e.operands()[0].is_nil() ? wait : sequence(e.operands()[0], wait, assertion);
        }
        if (assertion && (e.id() == ID_sva_until || e.id() == ID_sva_until_with)) {
            auto p = predicate(e.operands()[0]), q = predicate(e.operands()[1]);
            if (!p || !q)
                refuse("until with local variables requires Boolean operands");
            const auto at = node();
            exprt bad = e.id() == ID_sva_until ? exprt{and_exprt{not_exprt{*p}, not_exprt{*q}}}
                                               : exprt{not_exprt{*p}};
            nodes[at] = {Edge{and_exprt{*p, not_exprt{*q}}, {}, at, true},
                         Edge{e.id() == ID_sva_until ? *q : and_exprt{*p, *q}, {}, end},
                         Edge{bad, {}, 2, false, true}};
            return at;
        }
        refuse("unsupported local-variable " +
               std::string(assertion ? "consequent: " : "antecedent: ") + e.id_string());
    }
};
struct Branch {
    exprt guard;
    replace_mapt values;
    std::size_t next;
    bool fail;
};
} // namespace

bool hasLocalVariables(const exprt &e) {
    if (e.id() == localsId)
        return true;
    return std::any_of(e.operands().begin(), e.operands().end(), hasLocalVariables);
}
std::vector<exprt> localDeclarations(const exprt &e) {
    std::vector<exprt> result;
    std::function<void(const exprt &)> walk = [&](const exprt &n) {
        if (n.id() == localsId) {
            for (const auto &d : n.operands().at(1).operands())
                result.push_back(d);
            walk(n.operands()[0]);
        } else
            for (const auto &op : n.operands())
                walk(op);
    };
    walk(e);
    return result;
}
exprt withoutLocalDeclarations(exprt e) {
    if (e.id() == localsId)
        e = e.operands().at(0);
    for (auto &op : e.operands())
        op = withoutLocalDeclarations(std::move(op));
    return e;
}
exprt canonicalLocalVariables(exprt e, bool keepDisplayNames) {
    std::map<irep_idt, symbol_exprt> renames;
    std::set<std::string> displayNames;
    for (const auto &d : localDeclarations(e)) {
        const auto &old = to_symbol_expr(d.operands()[0]);
        auto name = "__cf_local_" + std::to_string(renames.size());
        auto value = symbol_exprt{name, old.type()};
        value.set("#veval_sva_local", true);
        auto display = keepDisplayNames ? id2string(old.get("#veval_sva_local_name")) : name;
        if (display.empty())
            display = name;
        while (!displayNames.insert(display).second)
            display += "_";
        value.set("#veval_sva_local_name", display);
        renames.emplace(old.identifier(), value);
    }
    e.visit_pre([&](exprt &n) {
        if (n.id() == ID_symbol) {
            auto f = renames.find(to_symbol_expr(n).identifier());
            if (f != renames.end())
                n = f->second;
        }
    });
    return e;
}
bool localAbortWindowExact(const exprt &original) {
    try {
        auto p = parse(original);
        auto body = p.body;
        if (body.id() == ID_sva_overlapped_implication ||
            body.id() == ID_sva_non_overlapped_implication)
            body = body.operands()[1];
        return requirements(body) <= 1;
    } catch (...) {
        return false;
    }
}
LocalEncoding localViolationMonitor(const exprt &raw, const std::string &name) {
    const auto original = canonicalLocalVariables(raw, true);
    auto p = parse(original);
    if (p.declarations.empty())
        refuse("no local declarations");
    const bool implication = p.body.id() == ID_sva_overlapped_implication ||
                             p.body.id() == ID_sva_non_overlapped_implication;
    if (containsRepetition(implication ? p.body.operands()[1] : p.body))
        refuse("repetition in a local-variable consequent is not supported");
    if (!p.abort.is_false() && !localAbortWindowExact(original))
        refuse("DISABLE-IFF-WINDOW: multiple or inexact consequent requirements");
    Builder b;
    std::vector<exprt> initializers;
    Builder::Defined known;
    for (const auto &d : p.declarations) {
        const auto &s = to_symbol_expr(d.operands()[0]);
        const auto w = width(s.type());
        if (!b.vars.emplace(s.identifier(), Variable{s, b.dataWidth, w}).second)
            refuse("duplicate local declaration");
        b.dataWidth += w;
        if (b.dataWidth > 16384)
            refuse("more than 16384 local data bits");
        if (d.operands()[1].is_not_nil()) {
            exprt a{assignId, typet{}, {s, d.operands()[1]}};
            b.write(a, known);
            initializers.push_back(std::move(a));
        }
    }
    std::size_t start;
    if (p.body.id() == ID_sva_overlapped_implication ||
        p.body.id() == ID_sva_non_overlapped_implication) {
        auto after = b.check(p.body.operands()[0], known);
        b.check(p.body.operands()[1], std::move(after));
        auto consequent = b.sequence(p.body.operands()[1], 1, true);
        const auto emptyConsequent = consequent;
        if (p.body.id() == ID_sva_non_overlapped_implication)
            consequent = b.delay(1, consequent);
        const auto antecedent = nonemptySequence(p.body.operands()[0]);
        start = b.sequence(antecedent.body, consequent, false);
        // Empty antecedents do not start an overlapped consequent. For |=>,
        // their end is launch-1, so the consequent starts at launch itself.
        if (antecedent.empty && p.body.id() == ID_sva_non_overlapped_implication)
            start = b.node({Edge{true_exprt{}, {}, start}, Edge{true_exprt{}, {}, emptyConsequent}});
    } else {
        b.check(p.body, known);
        start = b.sequence(p.body, 1, true);
    }
    b.nodes[0] = {Edge{true_exprt{}, {}, 0, true}, Edge{true_exprt{}, initializers, start}};
    b.nodes[1] = {Edge{true_exprt{}, {}, 0, true}};
    b.nodes[2] = {Edge{true_exprt{}, {}, 0, true}};
    const auto pcWidth = bitsFor(b.nodes.size());
    const auto payloadWidth = pcWidth + b.dataWidth;
    // First collapse epsilon edges symbolically. A match assignment affects
    // all subsequent tests in the same cycle, in source order.
    const symbol_exprt symbolicState{"veval::local-state::" + name, unsignedbv_typet{payloadWidth}};
    replace_mapt initialValues;
    for (const auto &[id, v] : b.vars)
        initialValues.emplace(
            v.symbol,
            typecast_exprt{extractbits_exprt{symbolicState,
                                             from_integer(pcWidth + v.offset, integer_typet{}),
                                             unsignedbv_typet{v.size}},
                           v.symbol.type()});
    std::vector<std::vector<Branch>> branches(b.nodes.size());
    std::size_t maxBranches = 1;
    for (std::size_t at = 0; at < b.nodes.size(); ++at) {
        std::set<std::size_t> active;
        std::function<void(std::size_t, exprt, replace_mapt)> close = [&](std::size_t n,
                                                                          exprt guard,
                                                                          replace_mapt values) {
            if (!active.insert(n).second)
                refuse("zero-cycle sequence loop");
            for (const auto &e : b.nodes[n]) {
                auto edgeGuard = e.guard;
                replace_expr(values, edgeGuard);
                auto combined = and_exprt{guard, edgeGuard};
                auto nextValues = values;
                for (const auto &assignment : e.writes) {
                    const auto t = b.target(assignment.operands()[0]);
                    if (!t)
                        refuse("assignment outside a local variable");
                    const auto &v = b.vars.at(t->id);
                    auto value = assignment.operands()[1];
                    replace_expr(nextValues, value);
                    if (t->low == 0 && t->size == v.size)
                        nextValues[v.symbol] = typecast_exprt{value, v.symbol.type()};
                    else {
                        const auto type = unsignedbv_typet{v.size};
                        const mp_integer mask = ((mp_integer{1} << t->size) - 1) << t->low;
                        auto cleared = bitand_exprt{
                            typecast_exprt{nextValues.at(v.symbol), type},
                            from_integer(((mp_integer{1} << v.size) - 1) - mask, type)};
                        auto inserted = shl_exprt{
                            typecast_exprt{typecast_exprt{value, unsignedbv_typet{t->size}}, type},
                            from_integer(t->low, unsignedbv_typet{32})};
                        nextValues[v.symbol] =
                            typecast_exprt{bitor_exprt{cleared, inserted}, v.symbol.type()};
                    }
                }
                if (e.tick || e.fail) {
                    if (branches[at].size() >= 4096)
                        refuse("too many same-cycle sequence paths");
                    branches[at].push_back({combined, std::move(nextValues), e.next, e.fail});
                } else
                    close(e.next, combined, std::move(nextValues));
            }
            active.erase(n);
        };
        close(at, true_exprt{}, initialValues);
        maxBranches = std::max(maxBranches, branches[at].size());
    }
    LocalEncoding out;
    out.payloadWidth = payloadWidth;
    out.choiceWidth = bitsFor(maxBranches);
    auto state =
        symbol_exprt{symbolicState.identifier(), unsignedbv_typet{payloadWidth + out.choiceWidth}};
    auto payload =
        extractbits_exprt{state, from_integer(0, integer_typet{}), unsignedbv_typet{payloadWidth}};
    auto pc = extractbits_exprt{state, from_integer(0, integer_typet{}), unsignedbv_typet{pcWidth}};
    auto choice = extractbits_exprt{state, from_integer(payloadWidth, integer_typet{}),
                                    unsignedbv_typet{out.choiceWidth}};
    replace_mapt widen{{symbolicState, payload}};
    exprt next = from_integer(0, unsignedbv_typet{payloadWidth});
    std::vector<exprt> failures;
    for (std::size_t n = 0; n < branches.size(); ++n)
        for (std::size_t k = 0; k < branches[n].size(); ++k) {
            const auto &branch = branches[n][k];
            auto guard = branch.guard;
            replace_expr(widen, guard);
            auto selected = and_exprt{equal_exprt{pc, from_integer(n, pc.type())},
                                      equal_exprt{choice, from_integer(k, choice.type())}, guard};
            exprt value = from_integer(branch.next, unsignedbv_typet{payloadWidth});
            for (const auto &[id, v] : b.vars) {
                auto data = branch.values.at(v.symbol);
                replace_expr(widen, data);
                // Narrow before widening so signed captures cannot leak into
                // neighbouring registers in the packed monitor state.
                auto narrow = typecast_exprt{data, unsignedbv_typet{v.size}};
                value = bitor_exprt{
                    value, shl_exprt{typecast_exprt{narrow, unsignedbv_typet{payloadWidth}},
                                     from_integer(pcWidth + v.offset, unsignedbv_typet{32})}};
            }
            next = if_exprt{selected, value, next};
            if (branch.fail)
                failures.push_back(selected);
        }
    out.initialPayload = from_integer(0, unsignedbv_typet{payloadWidth});
    out.nextPayload = next;
    out.failure = disjunction(failures);
    out.abort = p.abort;
    auto &monitor = out.monitor;
    monitor.property = name;
    monitor.originalProperty = original;
    monitor.hasLocalVariables = true;
    exprt declarations{"#veval_sva_declarations", typet{}, std::move(p.declarations)};
    exprt localBody{localsId, p.body.type(), {p.body, declarations}};
    monitor.sourceProperty = sva_always_exprt{localBody};
    monitor.stateSymbol = state;
    monitor.init = equal_exprt{payload, out.initialPayload};
    auto nextState = next_symbol_exprt{state.identifier(), state.type()};
    auto nextPayload = extractbits_exprt{nextState, from_integer(0, integer_typet{}),
                                         unsignedbv_typet{payloadWidth}};
    monitor.trans = equal_exprt{nextPayload, next};
    monitor.errorSignal = out.failure;
    monitor.livenessSignal = false_exprt{};
    if (!p.abort.is_false()) {
        monitor.sourceProperty = sva_disable_iff_exprt{p.abort, monitor.sourceProperty};
        monitor.trans =
            if_exprt{p.abort, equal_exprt{nextPayload, out.initialPayload}, monitor.trans};
        monitor.errorSignal = and_exprt{monitor.errorSignal, not_exprt{p.abort}};
        std::set<irep_idt> seen;
        p.abort.visit_pre([&](const exprt &e) {
            if (e.id() == ID_symbol && seen.insert(to_symbol_expr(e).identifier()).second)
                monitor.abortAtoms.push_back(to_symbol_expr(e));
        });
    }
    return out;
}
} // namespace veval::sva
