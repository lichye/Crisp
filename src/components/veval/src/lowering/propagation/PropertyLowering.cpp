#include "util/database/JsonEscape.h"
#include "PropertyLowering.h"
#include "util/MeasureGeneration.h"
#include "util/process/ParallelFor.h"
#include "lowering/Symbolic/PropertySubstitution.h"
#include "lowering/environment/CopyPortAssumptions.h"


#include "frontend/rtl/ElaboratedDesign.h"

#include <cstdint>
#include <ebmc/transition_system.h>

#include <util/arith_tools.h>
#include <util/bitvector_expr.h>
#include <util/format_expr.h>
#include <util/mathematical_types.h>
#include <util/std_expr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <numeric>

#include <trans-word-level/next_symbol.h>
#include <map>
#include <iostream>
#include <ostream>
#include <sstream>
#include <set>
#include <string_view>
#include <unordered_set>
#include <unordered_map>

namespace veval {
namespace lowering {

using symbolic::bitWidth;
using symbolic::complementOf;
using symbolic::rebase;
using symbolic::signedWholeRead;
using symbolic::simplifyComplements;
using symbolic::sameMonitor;
using symbolic::usesComplement;
using symbolic::undoHop;
using symbolic::roundTripsBack;

// Highest-precedence refusal first. Keep ComputedComb last so it cannot
// mask a more specific failed binding or timing condition.
const ReasonCode PRECEDENCE[] = {
    ReasonCode::BlackboxInterface,
    ReasonCode::DisableIffIncompatible,
    ReasonCode::Constant,
    ReasonCode::NoSameCycleWitness,
    ReasonCode::Registered,
    ReasonCode::CrossClock,
    ReasonCode::DynamicSelect,
    ReasonCode::MultiSignalAggregate,
    ReasonCode::WidthType,
    ReasonCode::ReadOutsideSlice,
    ReasonCode::ComputedComb,
};

std::size_t sequentialHops = maxSequentialHops;

std::string toString(ReasonCode code) {
    switch (code) {
        case ReasonCode::BlackboxInterface:
            return "BLACKBOX-INTERFACE";
        case ReasonCode::DisableIffIncompatible:
            return "DISABLE-IFF-INCOMPATIBLE";
        case ReasonCode::Constant:
            return "CONSTANT";
        case ReasonCode::NoSameCycleWitness:
            return "NO-SAME-CYCLE-WITNESS";
        case ReasonCode::Registered:
            return "REGISTERED";
        case ReasonCode::CrossClock:
            return "CROSS-CLOCK";
        case ReasonCode::DynamicSelect:
            return "DYNAMIC-SELECT";
        case ReasonCode::MultiSignalAggregate:
            return "MULTI-SIGNAL-AGGREGATE";
        case ReasonCode::WidthType:
            return "WIDTH-TYPE";
        case ReasonCode::ReadOutsideSlice:
            // Renders under WIDTH-TYPE until the closed-set extension is
            // adjudicated; see the note in 讨论.md, 2026-08-01.
            return "WIDTH-TYPE/read-outside";
        case ReasonCode::ComputedComb:
            return "COMPUTED-COMB";
    }
    return "?";
}

// Derive diagnostics from recorded binding, timing and role facts.
std::vector<ReasonCode> SignalFate::reasons() const {
    // A landing has no failed premise. Nothing about the target -- a
    // blackboxed module, a guard that is also read elsewhere -- turns a
    // carried observation into a refused one.
    if (reached) return {};

    std::vector<ReasonCode> codes;

    // Blackbox internals and missing abort guards are special cases of no binding.
    // When a binding exists, report its own failure instead.
    if (edge.kind == EdgeFact::Kind::None) {
        if (edge.blackboxTarget)
            codes.push_back(ReasonCode::BlackboxInterface);
        if (role == RoleFact::AbortAtom)
            codes.push_back(ReasonCode::DisableIffIncompatible);
    }
    if (edge.tiedOff) codes.push_back(ReasonCode::Constant);

    // The base code is the ABSENCE of same-cycle evidence, and the upgrade
    // stands alongside it rather than replacing it: the observation still has
    // no same-cycle witness, and that is what forbids the transport.
    if (fall == FallFact::Registered) {
        codes.push_back(ReasonCode::Registered);
        codes.push_back(ReasonCode::NoSameCycleWitness);
    } else if (fall == FallFact::CrossClock) {
        codes.push_back(ReasonCode::CrossClock);
        codes.push_back(ReasonCode::NoSameCycleWitness);
    }

    if (edge.parentComputes) codes.push_back(ReasonCode::ComputedComb);

    if (edge.kind == EdgeFact::Kind::SliceOutside)
        codes.push_back(ReasonCode::ReadOutsideSlice);

    if (edge.kind == EdgeFact::Kind::SliceRefused) {
        // Classified Partial with no recorded shape at all: the binding kind
        // is the only thing known about it.
        if (edge.shapeCodes.empty())
            codes.push_back(ReasonCode::ComputedComb);
        else
            for (auto shape : edge.shapeCodes) codes.push_back(shape);
    }

    return codes;
}

std::vector<std::string> SignalFate::path() const {
    std::vector<std::string> tags;

    switch (edge.kind) {
        case EdgeFact::Kind::AlreadyLocal:
            // Neither a refusal nor a mapping, and no branch routed it.
            return tags;
        case EdgeFact::Kind::Whole:
            tags.push_back("whole");
            if (edge.retyped) tags.push_back("typecast");
            if (edge.complemented) tags.push_back("complement");
            return tags;
        case EdgeFact::Kind::SliceCarried:
        case EdgeFact::Kind::SliceOutside:
            tags.push_back("partial");
            tags.push_back("slice-runs");
            if (edge.kind == EdgeFact::Kind::SliceOutside)
                tags.push_back("read-outside");
            return tags;
        case EdgeFact::Kind::SliceRefused:
            // No runs were recorded -- that is what makes it Refused rather
            // than Outside -- so there is no `slice-runs' step to report.
            tags.push_back("partial");
            tags.push_back("shape-refused");
            return tags;
        case EdgeFact::Kind::None:
            break;
    }

    // Sigma had nothing, so attribution ran. Its branches, in the order it
    // takes them.
    if (edge.blackboxTarget) tags.push_back("blackbox");
    if (role == RoleFact::AbortAtom) tags.push_back("abort-atom");
    if (edge.tiedOff) tags.push_back("constant");
    if (fall == FallFact::Registered) tags.push_back("seq-registered");
    if (fall == FallFact::CrossClock) tags.push_back("seq-crossclock");
    if (edge.parentComputes) tags.push_back("computed");
    if (reasons().empty()) tags.push_back("no-relation");

    return tags;
}

ReasonCode primaryReason(const std::vector<ReasonCode>& reasons) {
    for (auto candidate : PRECEDENCE)
        if (std::find(reasons.begin(), reasons.end(), candidate) != reasons.end())
            return candidate;
    return ReasonCode::ComputedComb;
}

std::string toString(LoweringCoverage result) {
    switch (result) {
        case LoweringCoverage::Full:
            return "full";
        case LoweringCoverage::Partial:
            return "partial";
        case LoweringCoverage::Blocked:
            return "blocked";
    }
    return "?";
}

std::string toString(AssumeGrade grade) {
    switch (grade) {
        case AssumeGrade::ClaimedSyntactic:
            return "claimed-syntactic";
        case AssumeGrade::SoundAsConstraint:
            return "sound-as-constraint";
        case AssumeGrade::Refused:
            return "refused";
    }
    return "?";
}

const char* const kNoPortBinding = "no-port-binding";

namespace {

std::string display(const irep_idt& identifier) {
    return frontend::displayName(id2string(identifier));
}

std::string escape(const std::string& s) {
    return veval::util::jsonEscape(s);
}

// One invertible run: port[portLo +: width] == signal[sigLo +: width].
// A concatenation can contribute several runs; rewriting must cover every read.
struct Slice {
    symbol_exprt port;
    std::vector<Segment> segments;
    // Coverage of the entire source value, including layouts with multiple
    // runs and repeated bits. Signing/printing restrictions are checked by
    // the expression rewriter, independently of physical coverage.
    bool wholeSignal = false;
};

struct Inverse {
    // parent signal -> the child port that equals it
    std::map<irep_idt, exprt> whole;
    // Parent signal -> port holding its same-type bitwise complement.
    std::map<irep_idt, exprt> complemented;
    // the ports the child DRIVES, by identifier
    std::set<irep_idt> driven;
    // Classify direction over the entire alias class, not the chosen port.
    // A net wired to both input and output is driven by the child.
    std::set<irep_idt> drivenKeys;
    // parent signal -> the runs of it a child port carries
    std::map<irep_idt, std::vector<Slice>> slices;
    // parent signal -> how it reaches the child, when not whole
    std::map<irep_idt, BindingKind> partial;
    // parent signal -> the port it feeds through parent-side computation
    std::map<irep_idt, std::string> computed;
    // Sorted union of the three binding maps consulted by attribution.
    // Built once per edge, so a small target need not scan a whole-design
    // sequential-reach set for every observation of every property.
    std::vector<irep_idt> sequentialKeys;
    // parent signal -> which side-conditions its refused Partial bindings
    // violate. Populated only for bindings asSlice turned down.
    std::map<irep_idt, std::vector<ReasonCode>> partialShapes;
    // Parent signal -> offending concatenation member, when there are at least two.
    std::map<irep_idt, std::string> partialWitness;
    // Parent signals whose only route into this child is a port tied to a
    // literal. Recorded so the refusal can name CONSTANT rather than fall to
    // the "nothing to do with this instance" default -- a tied-off port is a
    // fact about the design worth reporting, not an absence.
    std::set<irep_idt> constants;
};

// Record a == ~b in both directions. Require a bare signal of the same type
// so complementing is an exact inverse.
using Complements = std::map<irep_idt, symbol_exprt>;

Complements collectComplements(const std::vector<InstanceEdge>& edges) {
    Complements result;
    for (const auto& edge : edges)
        for (const auto& binding : edge.bindings) {
            const auto& rhs = binding.parentExpr;
            if (rhs.id() != ID_bitnot && rhs.id() != ID_not) continue;
            const auto& op = to_unary_expr(rhs).op();
            if (op.id() != ID_symbol) continue;
            const auto& source = to_symbol_expr(op);
            if (source.type() != binding.port.type()) continue;
            result.emplace(binding.port.get_identifier(), source);
            result.emplace(source.get_identifier(), binding.port);
        }
    return result;
}

// List every failed slice-binding side condition. This only explains a refusal;
// it does not admit a binding or choose just the first failure.
std::vector<ReasonCode> whySliceRefused(const PortBinding& binding) {
    std::vector<ReasonCode> reasons;
    bool dynamicIndex = false, secondSignal = false, notASelect = false;
    irep_idt seen;

    std::function<void(const exprt&)> look = [&](const exprt& expr) {
        const exprt* e = &expr;
        while (e->id() == ID_typecast) e = &to_typecast_expr(*e).op();

        const exprt* src = nullptr;
        if (e->id() == ID_extractbits) {
            const auto& extract = to_extractbits_expr(*e);
            if (!numeric_cast<mp_integer>(extract.index()).has_value())
                dynamicIndex = true;
            src = &extract.src();
        } else if (e->id() == ID_extractbit) {
            const auto& extract = to_extractbit_expr(*e);
            if (!numeric_cast<mp_integer>(extract.index()).has_value())
                dynamicIndex = true;
            src = &extract.src();
        } else if (e->id() == ID_symbol) {
            src = e;
        } else if (e->is_constant()) {
            // Constant fill inside a concatenation: the port is not a view of
            // one signal, it is a signal padded out.
            secondSignal = true;
            return;
        } else {
            notASelect = true;
            return;
        }

        if (src->id() != ID_symbol) {
            notASelect = true;
            return;
        }
        auto name = to_symbol_expr(*src).get_identifier();
        if (seen.empty())
            seen = name;
        else if (seen != name)
            secondSignal = true;
    };

    const exprt* top = &binding.parentExpr;
    while (top->id() == ID_typecast) top = &to_typecast_expr(*top).op();
    if (top->id() == ID_concatenation)
        for (const auto& op : top->operands()) look(op);
    else
        look(*top);

    if (dynamicIndex) reasons.push_back(ReasonCode::DynamicSelect);
    if (secondSignal) reasons.push_back(ReasonCode::MultiSignalAggregate);
    if (notASelect) reasons.push_back(ReasonCode::ComputedComb);
    if (reasons.empty()) reasons.push_back(ReasonCode::WidthType);
    return reasons;
}

bool transparentBitCast(const exprt& expression) {
    if (expression.id() != ID_typecast) return true;
    const auto& op = to_typecast_expr(expression).op();
    const auto width = bitWidth(expression.type());
    return width > 0 && width == bitWidth(op.type()) &&
           (expression.type().id() == ID_signedbv) == (op.type().id() == ID_signedbv);
}

// SV concatenations pack bare signed vectors as unsigned bit members. The
// frontend makes this packing explicit; unlike a cast on a source expression,
// it changes no member bits or the interpretation of a transported read.
bool concatSymbolPackingCast(const exprt& expression, bool concatMember) {
    if (!concatMember || expression.id() != ID_typecast) return false;
    const auto& op = to_typecast_expr(expression).op();
    return expression.type().id() == ID_unsignedbv && op.id() == ID_symbol &&
           op.type().id() == ID_signedbv && bitWidth(op.type()) > 0 &&
           bitWidth(expression.type()) == bitWidth(op.type());
}

const exprt* unsupportedLayoutCast(const exprt& expression,
                                  bool concatMember = false) {
    if (!transparentBitCast(expression) &&
        !concatSymbolPackingCast(expression, concatMember)) return &expression;
    // Casts in index/compute operands are not fixed-layout members. Leave
    // those unsupported shapes to the existing shape classifier.
    if (expression.id() != ID_typecast && expression.id() != ID_concatenation)
        return nullptr;
    for (const auto& op : expression.operands())
        if (const auto* found = unsupportedLayoutCast(
                op, expression.id() == ID_concatenation)) return found;
    return nullptr;
}

// A concat is unsigned in SV, even when it explicitly repeats a sign bit.
// The frontend casts that fixed-width value to the declared signed port at
// the assignment boundary. This one exact outer context conversion preserves
// all bits; it licenses no signed/unsigned source-expression equivalence.
const exprt& layoutExpression(const PortBinding& binding) {
    const auto& expression = binding.parentExpr;
    if (expression.id() == ID_typecast) {
        const auto& op = to_typecast_expr(expression).op();
        if (expression.type() == binding.port.type() &&
            op.id() == ID_concatenation && bitWidth(op.type()) > 0 &&
            bitWidth(op.type()) == bitWidth(binding.port.type())) return op;
    }
    return expression;
}

// Recover a fixed bit layout once per binding. Every member's typed width
// contributes to its position, including constants; only source-backed runs
// are recorded. Repeated source bits deliberately retain all their copies.
// Width-changing casts and unresolved indices have no guessed layout.
std::vector<std::pair<irep_idt, Slice>> fixedMemberLayout(
    const PortBinding& binding) {
    std::map<irep_idt, std::vector<Segment>> runs;
    std::map<irep_idt, mp_integer> sourceWidths;
    std::function<bool(const exprt&, const mp_integer&, bool)> member =
        [&](const exprt& expression, const mp_integer& portLo,
            bool concatMember) {
            const auto occupies = bitWidth(expression.type());
            if (occupies <= 0) return false;
            if (expression.id() == ID_typecast) {
                const auto& op = to_typecast_expr(expression).op();
                return (transparentBitCast(expression) ||
                        concatSymbolPackingCast(expression, concatMember)) &&
                       member(op, portLo, false);
            }
            if (expression.id() == ID_concatenation) {
                mp_integer at = portLo;
                for (auto it = expression.operands().rbegin();
                     it != expression.operands().rend(); ++it) {
                    if (!member(*it, at, true)) return false;
                    at += bitWidth(it->type());
                }
                return at - portLo == occupies;
            }
            if (expression.is_constant()) return true;
            const exprt* source = &expression;
            mp_integer low = 0;
            if (expression.id() == ID_extractbits ||
                expression.id() == ID_extractbit) {
                source = &expression.operands()[0];
                auto index = numeric_cast<mp_integer>(expression.operands()[1]);
                if (!index.has_value()) return false;
                low = *index;
            }
            if (source->id() != ID_symbol) return false;
            const auto sourceWidth = bitWidth(source->type());
            // New layouts use normalized, descending, zero-based IR vectors.
            // Nonzero declaration offsets/directions need a separately
            // established frontend convention before this path can use them.
            if (!source->type().get(ID_C_offset).empty() &&
                source->type().get(ID_C_offset) != "0") return false;
            if (source->type().get_bool(ID_C_increasing)) return false;
            if (sourceWidth <= 0 || low < 0 || low + occupies > sourceWidth)
                return false;
            const auto name = to_symbol_expr(*source).get_identifier();
            sourceWidths[name] = sourceWidth;
            runs[name].push_back({portLo, low, occupies});
            return true;
        };
    if (bitWidth(binding.port.type()) != bitWidth(binding.parentExpr.type()) ||
        !member(layoutExpression(binding), 0, false)) return {};
    std::vector<std::pair<irep_idt, Slice>> result;
    for (auto& entry : runs) {
        // Coverage, not the number of runs, decides whether a whole unsigned
        // read is available. This is not a claim of a bijective layout.
        mp_integer covered = 0;
        auto ordered = entry.second;
        std::sort(ordered.begin(), ordered.end(), [](const Segment& a,
                                                    const Segment& b) {
            return a.sigLo < b.sigLo;
        });
        for (const auto& run : ordered) {
            if (run.sigLo > covered) break;
            covered = std::max(covered, mp_integer(run.sigLo + run.width));
        }
        result.emplace_back(entry.first, Slice{binding.port, std::move(entry.second),
                                              covered == sourceWidths[entry.first]});
    }
    return result;
}

// The printed form of an expression, in the names a reader of the RTL uses.
std::string shown(const exprt& expr) {
    std::ostringstream out;
    out << format(expr);
    return frontend::displayName(out.str());
}

// Describe the first unsupported concatenation member, including its port bits.
// Return empty for non-concatenations or concatenations of whole signals.
std::string blockingMember(const PortBinding& binding) {
    const exprt* top = &binding.parentExpr;
    while (top->id() == ID_typecast) top = &to_typecast_expr(*top).op();
    if (top->id() != ID_concatenation || top->operands().size() < 2) return {};

    std::map<irep_idt, std::size_t> seen;
    mp_integer portLo = 0;
    std::size_t position = 0;

    for (auto it = top->operands().rbegin(); it != top->operands().rend();
         ++it, ++position) {
        const auto occupies = bitWidth(it->type());

        const exprt* member = &*it;
        while (member->id() == ID_typecast)
            member = &to_typecast_expr(*member).op();

        std::string what;
        if (member->is_constant()) {
            what = "a constant pad";
        } else if (member->id() == ID_extractbits ||
                   member->id() == ID_extractbit) {
            const auto& source = member->operands()[0];
            const bool dynamic =
                !numeric_cast<mp_integer>(member->operands()[1]).has_value();
            what = dynamic ? "a dynamically indexed select of `" +
                                 shown(source) + "'"
                           : "a slice of `" + shown(source) + "'";
        } else if (member->id() != ID_symbol) {
            what = "a term the parent computes";
        } else {
            const auto& symbol = to_symbol_expr(*member);
            const auto width = bitWidth(symbol.type());
            auto repeat = seen.find(symbol.get_identifier());
            if (repeat != seen.end())
                what = "a second copy of `" + shown(symbol) +
                       "', already carried by member " +
                       std::to_string(repeat->second);
            else if (width == 0)
                what = "a member whose width the frontend does not state";
            else if (occupies != width)
                what = "an implicit width change of `" + shown(symbol) + "'";
            else
                seen.emplace(symbol.get_identifier(), position);
        }

        if (!what.empty()) {
            std::ostringstream out;
            out << "member " << position << " of the concatenation, `"
                << shown(*it) << "'";
            if (occupies > 0)
                out << ", at port bits [" << (portLo + occupies - 1) << ":"
                    << portLo << "]";
            out << ", is " << what
                << "; this member has no supported fixed bit layout";
            return out.str();
        }

        if (occupies == 0) return {};  // no bit arithmetic past an unknown width
        portLo += occupies;
    }

    return {};
}

// Clock domains come from port bindings: trans() uses one global step.
// Clock-port names are "clock" or begin with "clk"; unlike globalClockOrReset,
// this convention includes domain-specific clocks and excludes reset names.
}  // namespace

std::map<std::string, std::string> clockNets(
    const std::vector<InstanceEdge>& edges,
    const std::map<irep_idt, irep_idt>& aliases) {
    std::map<std::string, std::string> clocks;
    for (const auto& edge : edges)
        for (const auto& binding : edge.bindings) {
            if (binding.isOutput) continue;
            auto port = display(binding.port.get_identifier());
            auto dot = port.rfind('.');
            auto base = dot == std::string::npos ? port : port.substr(dot + 1);
            if (base != "clk" && base != "clock" && base != "clk_i" &&
                base.compare(0, 3, "clk") != 0)
                continue;
            auto symbols = binding.parentSymbols();
            if (symbols.size() != 1) continue;

            // Use the alias representative so renames across hierarchy levels do not
            // look like clock-domain crossings. Record only whole-signal clock bindings.
            auto identifier = symbols.front().get_identifier();
            auto alias = aliases.find(identifier);
            clocks[edge.instance] =
                display(alias == aliases.end() ? identifier : alias->second);
        }
    return clocks;
}

namespace {

// These analyses collect sets of names, so a shared expression contributes
// once regardless of how many paths reach it. Keep the visited set local to
// one root: a second assignment must still get its own complete dependency
// set. An explicit stack also handles long shared array-update chains.
template <typename Visit>
void visitOperandDagUntil(const exprt& root, Visit visit) {
    std::unordered_set<const void*> seen;
    std::vector<const exprt*> pending{&root};
    while (!pending.empty()) {
        const auto& node = *pending.back();
        pending.pop_back();
        if (!seen.insert(&node.read()).second) continue;
        if (visit(node)) return;
        for (auto op = node.operands().rbegin(); op != node.operands().rend(); ++op)
            pending.push_back(&*op);
    }
}

// Successive array updates share a growing RHS prefix across assignments.
// A per-root visited set alone still scans that prefix for every assignment.
// Remember small complete name sets for shared interior nodes as well as
// roots. Bounds only control cache admission, never the answer.
class OperandSymbols {
    static constexpr std::size_t maxEntries = 65536;
    static constexpr std::size_t maxNames = 16;
    struct CachedNames {
        const void* key = nullptr;
        std::vector<irep_idt> names;
    };
    std::vector<CachedNames> cache;
    std::uint64_t hits = 0, visited = 0;
    bool report;
    CachedNames& slot(const void* key) {
        auto hash = std::hash<const void*>{}(key);
        hash ^= hash >> 17;
        hash *= std::size_t{0xed5ad4bbU};
        hash ^= hash >> 11;
        return cache[hash & (maxEntries - 1)];
    }
public:
    explicit OperandSymbols(bool report = true) : report(report) {}
    std::uint64_t cacheHits() const { return hits; }
    std::uint64_t visitedNodes() const { return visited; }
    ~OperandSymbols() {
        if (report) {
            profile::count("attribution.symbol-cache-hits", hits);
            profile::count("attribution.symbol-nodes", visited);
        }
    }
    std::vector<irep_idt> collect(const exprt& root) {
        if (cache.empty()) cache.resize(maxEntries);
        std::vector<irep_idt> names;
        std::unordered_set<const void*> seen;
        struct Frame {
            const exprt* node;
            std::size_t child, firstName;
            bool complete;
        };
        std::vector<Frame> pending;
        const auto enter = [&](const exprt& node) {
            // Leaves have no descendant traversal to memoize. Collect their
            // names contiguously and deduplicate once, avoiding one allocation
            // per name and per visited leaf.
            if (node.operands().empty()) {
                if (node.id() == ID_symbol || node.id() == ID_next_symbol)
                    names.push_back(node.get(ID_identifier));
                return;
            }
            const auto key = &node.read();
            if (node.read().ref_count > 1) {
                const auto& known = slot(key);
                if (known.key == key) {
                    ++hits;
                    names.insert(names.end(), known.names.begin(), known.names.end());
                    return;
                }
            }
            if (!seen.insert(key).second) {
                // Its names are already in the root's union, but not
                // necessarily in this parent's suffix. Never cache a partial
                // subtree summary and reuse it for a different assignment.
                if (!pending.empty()) pending.back().complete = false;
                return;
            }
            ++visited;
            pending.push_back({&node, 0, names.size(), true});
            if (node.id() == ID_symbol || node.id() == ID_next_symbol)
                names.push_back(node.get(ID_identifier));
        };
        enter(root);
        while (!pending.empty()) {
            auto& frame = pending.back();
            const auto& operands = frame.node->operands();
            if (frame.child < operands.size()) {
                const auto& child = operands[frame.child++];
                enter(child);
                continue;
            }
            auto finished = frame;
            pending.pop_back();
            if (finished.complete && finished.node->read().ref_count > 1) {
                const auto first = names.begin() + finished.firstName;
                std::sort(first, names.end());
                names.erase(std::unique(first, names.end()), names.end());
                if (names.size() - finished.firstName <= maxNames) {
                    const auto key = &finished.node->read();
                    // A collision replaces only a cached result, never an
                    // input dependency. Direct slots avoid a chained hash
                    // lookup on every node of the large expression DAG.
                    auto& saved = slot(key);
                    saved.key = key;
                    saved.names.assign(names.begin() + finished.firstName, names.end());
                } else {
                    // An ancestor's union cannot be smaller, so avoid sorting
                    // these same large sets at every level of a shared chain.
                    finished.complete = false;
                }
            }
            if (!finished.complete && !pending.empty())
                pending.back().complete = false;
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }
};

// Sort an index and apply its permutation once. A PropertyLanding contains two
// monitors and many containers; merge-sorting the records needs a large
// temporary array, and in-place comparison sorting moves them repeatedly.
// The original index is the tie-breaker, preserving stable-sort semantics.
template <typename Less>
void stableSortTransports(std::vector<PropertyLanding>& rows, std::size_t first,
                          Less less) {
    std::vector<std::size_t> order(rows.size() - first);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (less(rows[first + a], rows[first + b])) return true;
        if (less(rows[first + b], rows[first + a])) return false;
        return a < b;
    });
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == i) continue;
        auto saved = std::move(rows[first + i]);
        auto at = i;
        while (order[at] != i) {
            const auto from = order[at];
            rows[first + at] = std::move(rows[first + from]);
            order[at] = at;
            at = from;
        }
        rows[first + at] = std::move(saved);
        order[at] = at;
    }
}

// Boolean and word assignments both use ID_equal; delayed targets use
// ID_next_symbol. Collect all RHS dependencies, including enables and holds.
// These are possible paths, not a proof of a fixed-cycle value relation.
}  // namespace

namespace {
template <typename Emit>
void equalityDrives(const exprt& expr, bool delayed, OperandSymbols& symbols, Emit emit) {
    if (expr.id() != ID_equal || expr.operands().size() != 2) return;
    auto record = [&](const exprt& target, const exprt& source) {
        if (target.id() != (delayed ? ID_next_symbol : ID_symbol)) return;
        const auto name = target.get(ID_identifier);
        if (delayed && name.empty()) return;
        if (source.operands().empty()) {
            if (source.id() == ID_symbol || source.id() == ID_next_symbol) {
                const auto from = source.get(ID_identifier);
                if (delayed || from != name) emit(name, from);
            }
            return;
        }
        const auto names = symbols.collect(source);
        for (const auto& from : names)
            if (delayed || from != name) emit(name, from);
    };
    // A symbol on either side supplies a target. Delayed self-edges remain;
    // combinational ones do not.
    record(expr.operands()[0], expr.operands()[1]);
    record(expr.operands()[1], expr.operands()[0]);
}

template <typename Visit>
void forEachEquality(const exprt& relation, Visit visit) {
    std::vector<const exprt*> pending{&relation};
    while (!pending.empty()) {
        const auto& expr = *pending.back();
        pending.pop_back();
        if (expr.id() == ID_and) {
            for (auto op = expr.operands().rbegin(); op != expr.operands().rend(); ++op)
                pending.push_back(&*op);
        } else if (expr.id() == ID_equal && expr.operands().size() == 2) {
            visit(expr);
        }
    }
}

template <typename Emit>
void forEachDrive(const exprt& relation, bool delayed, Emit emit) {
    OperandSymbols symbols;
    forEachEquality(relation, [&](const exprt& expr) {
        equalityDrives(expr, delayed, symbols, emit);
    });
}

std::uint64_t normalizeDrives(ForwardDrives& forward) {
    std::uint64_t edges = 0;
    for (auto& entry : forward) {
        auto& targets = entry.second;
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        edges += targets.size();
    }
    return edges;
}

ForwardDrives forwardDrives(const exprt& relation, bool delayed, std::size_t) {
    profile::Phase timer(delayed ? "attribution.delayed-drives"
                                : "attribution.combinational-drives");
    ForwardDrives forward;
    profile::count("attribution.workers", 1);
    forEachDrive(relation, delayed, [&](const irep_idt& target, const irep_idt& source) {
        forward[source].push_back(target);
    });
    profile::count("attribution.dependency-edges", normalizeDrives(forward));
    return forward;
}
} // namespace

DelayedDrives delayedDrives(const transition_systemt& system) {
    DelayedDrives drives;
    forEachDrive(system.trans_expr.trans(), true,
                 [&](const irep_idt& target, const irep_idt& source) {
        drives[target].insert(source);
    });
    return drives;
}

DelayedDrives combinationalDrives(const transition_systemt& system) {
    DelayedDrives drives;
    forEachDrive(system.trans_expr.invar(), false,
                 [&](const irep_idt& target, const irep_idt& source) {
        drives[target].insert(source);
    });
    return drives;
}

namespace {

// Use the whole-design body census. Port bindings remain usable on blackboxes;
// only unavailable internal observations are refused.
bool looksBlackboxed(const InstanceEdge& edge,
                     const BlackboxSet& blackboxes) {
    return blackboxes.count(edge.instance) != 0;
}

// Explain a refused partial binding. Record all failed shape conditions,
// including overlapping cases such as dynamic indices in aggregates.
void attributeSlice(SignalFate& fate, const irep_idt& key,
                    const Inverse& inverse) {
    // The member that blocked a concatenation, when the binding is one. A
    // refusal that cannot say where it stopped is a refusal the reader has to
    // re-derive; naming the member is the difference between "this binding is
    // not certified" and "member 0 is a constant pad".
    auto member = inverse.partialWitness.find(key);
    if (member != inverse.partialWitness.end() && fate.witness.empty()) {
        const bool cast = member->second.find("nontransparent-cast ") == 0;
        fate.witnessKind = cast ? "unsupported-layout-cast" : "aggregate-member";
        if (cast) fate.blockedBy = "fixed layout contains a width/sign-changing member cast";
        fate.witness = member->second;
    }

    auto shapes = inverse.partialShapes.find(key);
    if (shapes == inverse.partialShapes.end()) return;
    // Left empty when nothing was recorded: reasons() renders the bare
    // SliceRefused kind as the fallback, which is what "the binding kind is
    // the only thing known about it" means.
    fate.edge.shapeCodes = shapes->second;
}

// Record every premise this observation fails.
//
// No early exit anywhere: exhaustiveness is what makes `all_reason_codes'
// stable under a revision of the precedence table, and it is what stopped
// the fallback from masking MULTI-SIGNAL-AGGREGATE.
void attribute(SignalFate& fate, const irep_idt& key, const Inverse& inverse,
               const SequentialReach& reach, bool blackboxTarget,
               bool isAbortAtom,
               const std::map<std::string, std::string>& clocks,
               const std::string& targetInstance) {
    fate.edge.blackboxTarget = blackboxTarget;

    // The `disable iff' guard could not be carried. The transported property
    // would then abort on a condition the child does not have, which is not
    // the property anybody wrote -- a different failure from an observation
    // that did not carry, and the reason this has its own premise.
    //
    // 32 pairs in the frozen corpus, in two shapes: the guard reaches the
    // instance only through a register (14), or does not reach it at all
    // (18, e.g. `partition_a', a combinational submodule with no reset port).
    // Both were reported as having no reason at all before this.
    fate.role = isAbortAtom ? RoleFact::AbortAtom : RoleFact::Observation;

    fate.edge.tiedOff = inverse.constants.count(key) != 0;

    // Find the fewest clocked stages reaching this instance. A register output
    // can still be a same-cycle binding; only a missing binding needs attribution.
    // Reuse per-signal reach instead of repeating the walk for each instance.
    irep_idt landed;
    std::size_t landedStages = 0;
    auto consider = [&](const auto& arrival) {
        auto stages = reach.stages.at(arrival.second);
        if (landed.empty() || stages < landedStages) {
            landed = arrival.second;
            landedStages = stages;
        }
    };
    // Both sides use the same irep_idt ordering. Visiting the smaller side
    // preserves the old first-in-map tie break as well as the minimum stage
    // count; the chosen route and clock-domain witness must not change.
    if (inverse.sequentialKeys.size() < reach.byClass.size()) {
        for (const auto& key : inverse.sequentialKeys) {
            const auto arrival = reach.byClass.find(key);
            if (arrival != reach.byClass.end()) consider(*arrival);
        }
    } else {
        for (const auto& arrival : reach.byClass) {
            // Keyed by alias class throughout: sigma is keyed by the class
            // representative, and a register's output reaches a port under
            // whatever name the instantiation used. Comparing raw names finds
            // nothing, and finding nothing looks exactly like "there is no
            // sequential path".
            if (!inverse.whole.count(arrival.first) &&
                !inverse.slices.count(arrival.first) &&
                !inverse.computed.count(arrival.first))
                continue;
            consider(arrival);
        }
    }

    const bool sequential = !landed.empty();
    if (sequential) {
        const std::pair<const irep_idt, std::size_t> arrival{landed,
                                                             landedStages};

        // Which upgrade? The register lives in some instance; if that
        // instance is clocked by a different net from the one being
        // transported into, the path crosses domains and "a cycle later" is
        // not even a well-formed statement.
        auto ownerOf = [](const std::string& signal) {
            auto dot = signal.rfind('.');
            return dot == std::string::npos ? std::string{}
                                            : signal.substr(0, dot);
        };
        auto here = clocks.find(targetInstance);
        auto there = clocks.find(
            ownerOf(display(reach.lastRegister.at(arrival.first))));
        bool crosses = here != clocks.end() && there != clocks.end() &&
                       here->second != there->second;

        fate.fall = crosses ? FallFact::CrossClock : FallFact::Registered;
        fate.witnessKind = "delayed-path";

        // The route from the observation, so a reader can check the claim
        // against the RTL rather than take it.
        std::ostringstream path;
        path << "reaches this instance after " << arrival.second
             << " clocked stage(s), via ";
        const auto& route = reach.route.at(arrival.first);
        for (std::size_t i = 0; i < route.size(); i++) {
            if (i) path << " -> ";
            path << display(route[i]);
        }
        // Never "corresponds k cycles later": a mux, stall, flush or reset
        // between the stages breaks that reading and nothing here has looked
        // for one.
        if (crosses)
            path << ", which is clocked by " << there->second
                 << " while this instance is clocked by " << here->second;
        path << ". A potential sequential dependency; no same-cycle transport "
                "certificate";
        fate.witness = path.str();
    }

    auto computed = inverse.computed.find(key);
    if (computed != inverse.computed.end()) {
        fate.through = BindingKind::Computed;
        fate.blockedBy =
            "the parent computes what it drives into " + computed->second;
        fate.witnessKind = "computed-term";
        fate.witness = computed->second;
        fate.edge.parentComputes = true;
    }

    // No premise fired at all. Nothing about this instance mentions the
    // signal, this cycle or later: not a refusal of anything -- the property
    // simply has nothing to say here, and this pair is `irrelevant' rather
    // than `noticed'. Axis 2 says so explicitly rather than by the absence of
    // everything else.
    const auto codes = fate.reasons();
    if (codes.empty()) {
        if (fate.fall == FallFact::NotAsked) fate.fall = FallFact::NoRelation;
        fate.blockedBy = "does not reach this instance";
        return;
    }

    if (fate.blockedBy.empty()) {
        // The primary code already leads the printed line, so the prose must
        // add something rather than repeat it. The witness carries the
        // detail; when there is none, name the premise in words.
        switch (primaryReason(codes)) {
            case ReasonCode::NoSameCycleWitness:
            case ReasonCode::Registered:
                fate.blockedBy = "no same-cycle bit-source for this "
                                 "observation";
                break;
            case ReasonCode::Constant:
                fate.blockedBy = "the port is tied to a literal";
                break;
            case ReasonCode::BlackboxInterface:
                fate.blockedBy = "the property reads inside a module with no "
                                 "body";
                break;
            default:
                fate.blockedBy = "the admitted fragment does not certify this "
                                 "binding";
                break;
        }
    }
}

std::array<Inverse, 2> invert(const InstanceEdge& edge,
               const std::map<irep_idt, irep_idt>& aliases,
               const Complements& complements = Complements{}) {
    std::array<Inverse, 2> indexes;

    for (const auto& binding : edge.bindings) {
        // Computed bindings only populate a name -> port map. Repeating
        // a symbol cannot change that map, so walk their shared DAG once.
        // Whole/Partial bindings retain occurrence multiplicity for layouts.
        auto symbols = binding.kind == BindingKind::Computed
            ? std::vector<symbol_exprt>{} : binding.parentSymbols();
        // Both name views use the same layout and refusal analysis. Preserve
        // source occurrences: repeated bits still produce repeated slices.
        const auto members = binding.kind == BindingKind::Partial
            ? fixedMemberLayout(binding) : std::vector<std::pair<irep_idt, Slice>>{};
        const auto computed = binding.kind == BindingKind::Computed
            ? binding.parentSymbolNames() : std::vector<irep_idt>{};
        const auto portName = computed.empty() ? std::string{} : display(binding.port.get_identifier());
        bool refusalReady = false;
        std::vector<ReasonCode> refusal;
        std::string witness;

        for (std::size_t view = 0; view < indexes.size(); ++view) {
            auto& inverse = indexes[view];
            const auto canonical = [&](const irep_idt& name) {
                if (view == 1) return name;
                const auto it = aliases.find(name);
                return it == aliases.end() ? name : it->second;
            };

            switch (binding.kind) {
                case BindingKind::Whole:
                    // Exactly one symbol, and it is the whole of it.
                    if (symbols.size() == 1) {
                        inverse.whole.emplace(
                            canonical(symbols.front().get_identifier()),
                            binding.port);
                        if (binding.isOutput) {
                            inverse.driven.insert(binding.port.get_identifier());
                            inverse.drivenKeys.insert(
                                canonical(symbols.front().get_identifier()));
                        }
                        // The net is the complement of another signal: that
                        // signal equals the complement of this port.
                        const auto complement =
                            complements.find(symbols.front().get_identifier());
                        if (complement != complements.end() &&
                            complement->second.type() == binding.port.type()) {
                            const auto key =
                                canonical(complement->second.get_identifier());
                            inverse.complemented.emplace(key, binding.port);
                            if (binding.isOutput) inverse.drivenKeys.insert(key);
                        }
                    }
                    break;

                case BindingKind::Partial: {
                    // One typed fixed layout handles whole and sliced members,
                    // padding and duplicates. Runs remain indexed per source;
                    // the property's actual reads decide complete transport.
                    for (const auto& symbol : symbols) {
                        const auto key = canonical(symbol.get_identifier());

                        // Record the run when the binding carries this signal
                        // over bits it names. A property that only reads bits
                        // inside such a run can be rewritten exactly; one that
                        // reads outside it cannot, and saying which is the
                        // difference between carrying a property and guessing.
                        const auto member = std::lower_bound(members.begin(), members.end(),
                            symbol.get_identifier(), [](const auto& item, const irep_idt& name) {
                                return item.first < name;
                            });
                        const Slice* carries = member != members.end() && member->first == symbol.get_identifier()
                            ? &member->second : nullptr;

                        if (carries != nullptr) {
                            inverse.slices[key].push_back(*carries);
                            if (binding.isOutput) {
                                inverse.driven.insert(
                                    binding.port.get_identifier());
                                inverse.drivenKeys.insert(key);
                            }
                        }

                        else {
                            // The binding was refused: record WHY, so a refusal
                            // can name the side-condition instead of the bucket,
                            // and WHERE -- which member of the concatenation it
                            // stopped at.
                            auto& shapes = inverse.partialShapes[key];
                            if (!refusalReady) {
                                const auto* badCast = unsupportedLayoutCast(layoutExpression(binding));
                                refusal = badCast ? std::vector<ReasonCode>{ReasonCode::WidthType}
                                                  : whySliceRefused(binding);
                                witness = badCast ? "nontransparent-cast " + shown(*badCast)
                                                  : blockingMember(binding);
                                refusalReady = true;
                            }
                            for (auto reason : refusal)
                                if (std::find(shapes.begin(), shapes.end(),
                                              reason) == shapes.end())
                                    shapes.push_back(reason);

                            if (!witness.empty()) inverse.partialWitness.emplace(key, witness);
                        }

                        inverse.partial.emplace(key, BindingKind::Partial);
                    }
                    break;
                }

                case BindingKind::Computed: {
                    // An input port bound to the complement of one signal: the
                    // signal equals the complement of the port. The frontend
                    // inlines `assign v = ~s' into the binding as often as it
                    // keeps the wire.
                    const auto& rhs = binding.parentExpr;
                    if (!binding.isOutput &&
                        (rhs.id() == ID_bitnot || rhs.id() == ID_not) &&
                        to_unary_expr(rhs).op().id() == ID_symbol &&
                        to_unary_expr(rhs).op().type() == binding.port.type())
                        inverse.complemented.emplace(
                            canonical(to_symbol_expr(to_unary_expr(rhs).op())
                                          .get_identifier()),
                            binding.port);
                    for (const auto& name : computed)
                        inverse.computed.emplace(canonical(name), portName);
                    break;
                }

                case BindingKind::Constant:
                    // Carries no parent signal, so nothing to invert -- but note
                    // which signals were bound this way, so the refusal can say
                    // so instead of reporting the instance as unrelated.
                    for (const auto& symbol : symbols)
                        inverse.constants.insert(
                            canonical(symbol.get_identifier()));
                    break;
            }
        }
    }

    for (auto& inverse : indexes) {
        // A signal that is bound whole somewhere is transportable, even if it
        // also feeds a computed port elsewhere. Prefer the usable binding.
        for (const auto& entry : inverse.whole) {
            inverse.partial.erase(entry.first);
            inverse.computed.erase(entry.first);
            inverse.constants.erase(entry.first);
        }

        for (const auto& entry : inverse.whole) inverse.sequentialKeys.push_back(entry.first);
        for (const auto& entry : inverse.slices) inverse.sequentialKeys.push_back(entry.first);
        for (const auto& entry : inverse.computed) inverse.sequentialKeys.push_back(entry.first);
        std::sort(inverse.sequentialKeys.begin(), inverse.sequentialKeys.end());
        inverse.sequentialKeys.erase(
            std::unique(inverse.sequentialKeys.begin(), inverse.sequentialKeys.end()),
            inverse.sequentialKeys.end());
    }

    return indexes;
}

}  // namespace

std::vector<std::string> PropertyLanding::unreachable() const {
    std::vector<std::string> result;
    for (const auto& fate : fates)
        if (!fate.reached) result.push_back(fate.parentSignal);
    return result;
}

std::vector<std::string> PropertyLanding::arrived() const {
    std::vector<std::string> result;
    for (const auto& fate : fates)
        if (fate.reached) result.push_back(fate.parentSignal);
    return result;
}

std::vector<PropertyLanding::Dropped> PropertyLanding::dropped() const {
    std::vector<Dropped> result;
    for (const auto& fate : fates) {
        if (fate.reached) continue;
        Dropped entry;
        entry.signal = fate.parentSignal;
        const auto codes = fate.reasons();
        // No code at all is itself the answer, and it has a name: sigma has
        // no entry for this observation anywhere in this instance. Reporting
        // it as an empty reason would make the vocabulary partial exactly
        // where the largest bucket is.
        entry.reason = codes.empty() ? kNoPortBinding
                                     : toString(primaryReason(codes));
        for (auto candidate : PRECEDENCE)
            for (auto code : codes)
                if (code == candidate) entry.reasons.push_back(toString(code));
        result.push_back(std::move(entry));
    }
    return result;
}

std::string stopReason(const PropertyLanding& transport) {
    bool anyDropped = false;
    std::vector<ReasonCode> codes;
    for (const auto& fate : transport.fates) {
        if (fate.reached) continue;
        anyDropped = true;
        for (auto code : fate.reasons()) codes.push_back(code);
    }
    if (!anyDropped) return {};
    // No code on ANY dropped signal is itself the answer: sigma has no entry
    // for them anywhere in this instance. `primaryReason' would answer
    // COMPUTED-COMB for an empty list, which is a claim about the parent's
    // logic that nothing here measured.
    if (codes.empty()) return kNoPortBinding;
    return toString(primaryReason(codes));
}

std::vector<std::string> stopReasons(const PropertyLanding& transport) {
    // CODES ONLY. `no-port-binding' is the name for the ABSENCE of a code
    // and is not one, so it appears in `reason' -- where something has to be
    // said -- and never in this list. Same rule at the signal level
    // (PropertyLanding::dropped), so the two forms of the same fact agree.
    std::set<ReasonCode> codes;
    for (const auto& fate : transport.fates) {
        if (fate.reached) continue;
        for (auto code : fate.reasons()) codes.insert(code);
    }
    std::vector<std::string> named;
    for (auto candidate : PRECEDENCE)
        if (codes.count(candidate)) named.push_back(toString(candidate));
    return named;
}

// Clock/reset-only arrival is not contextual input. This is a naming convention.
bool globalClockOrReset(const std::string& shown) {
    auto last = shown.rfind('.');
    auto base = last == std::string::npos ? shown : shown.substr(last + 1);
    return base == "clk" || base == "clock" || base == "clk_i" ||
           base == "rst" || base == "reset" || base == "rst_n" ||
           base == "rst_ni" || base == "resetn" || base == "rstn";
}

// Lower one edge in stages: index bindings, carry observations, explain refusals,
// round-trip the rewritten monitor, then record the mapping. Only attribution
// reads sequential dependencies; carry() has no access to them.

namespace {

// ---- 1. the index --------------------------------------------------------

struct PreparedEdge {
    Inverse inverse;
    Inverse direct;
    bool blackbox;
};

PreparedEdge prepareEdge(const InstanceEdge& edge,
                         const std::map<irep_idt, irep_idt>& aliases,
                         const BlackboxSet& blackboxes,
                         const Complements& complements = Complements{}) {
    profile::Measure preparation("transport.edge-index-build");
    auto indexes = invert(edge, aliases, complements);
    return {std::move(indexes[0]), std::move(indexes[1]),
            looksBlackboxed(edge, blackboxes)};
}

struct EdgeIndex {
    const Inverse& inverse;
    const Inverse& direct;
    // The guard's signals, so a refusal can tell an observation that did not
    // carry apart from an abort condition that did not carry.
    std::set<irep_idt> abortAtoms;
    bool blackbox = false;
};

EdgeIndex indexEdge(const sva::Monitor& monitor,
                    const PreparedEdge& prepared) {
    EdgeIndex index{prepared.inverse, prepared.direct, {}, prepared.blackbox};
    for (const auto& atom : monitor.abortAtoms)
        index.abortAtoms.insert(atom.get_identifier());
    return index;
}

// ---- 2. the judgment -----------------------------------------------------

// Keep both the alias key (for bindings/reach) and original identifier
// (for abort-atom membership). They can differ after a scope rename.
struct Unexplained {
    std::size_t fate;
    irep_idt identifier;
    irep_idt key;
};

// Everything the substitution side produced, and everything it is allowed to
// know. There is no delayed-edge information in this structure and there is
// no path from it to any.
struct Carried {
    // The monitor with its bit selects rebased. Not yet substituted.
    sva::Monitor rewritten;
    std::map<irep_idt, exprt> substitution;
    std::vector<PropertyLanding::ClaimedRebase> rebased;
    std::vector<SignalFate> fates;
    std::vector<Unexplained> unexplained;

    bool mentionsDriven = false;
    bool mentionsArriving = false;
    bool rebasePrintable = true;
    // Some signal was carried as the complement of a port, so the rewritten
    // monitor holds `~port' and needs its complements simplified.
    bool complemented = false;
};

// Rewrite only from same-cycle bindings and property syntax. Do not pass
// Attribution here: sequential dependencies cannot justify substitutions.
// Leave unmatched observations in unexplained for attributeAll().
Carried carry(const sva::Monitor& monitor,
              const std::vector<symbol_exprt>& observations,
              const InstanceEdge& edge,
              const std::map<irep_idt, irep_idt>& aliases,
              const EdgeIndex& index) {
    Carried carried;
    carried.rewritten = monitor;
    carried.fates.reserve(observations.size());

    auto canonical = [&aliases](const irep_idt& name) {
        auto it = aliases.find(name);
        return it == aliases.end() ? name : it->second;
    };

    for (const auto& symbol : observations) {
        const auto& identifier = symbol.get_identifier();
        const auto aliasKey = canonical(identifier);
        // Preserve the actual binding when it exists. Collapsing two parent
        // aliases onto one child input loses their equality after cutting
        // the child out of the parent. Alias lookup remains the fallback for
        // source names that do not occur on this boundary.
        const bool directlyBound = index.direct.whole.count(identifier) ||
                                   index.direct.slices.count(identifier);
        const auto& inverse = directlyBound ? index.direct : index.inverse;
        const auto key = directlyBound ? identifier : aliasKey;

        SignalFate fate;
        fate.parentSignal = display(identifier);
        // AXIS 3, from the property's syntax, and AXIS 1's target flag: both
        // are known here whatever happens next, so they are recorded here
        // rather than only where they happen to be consulted.
        fate.role = index.abortAtoms.count(identifier) != 0
                        ? RoleFact::AbortAtom
                        : RoleFact::Observation;
        fate.edge.blackboxTarget = index.blackbox;

        // A signal already inside this instance needs no inverse binding.
        const auto inside = edge.instance + '.';
        if (fate.parentSignal.compare(0, inside.size(), inside) == 0) {
            // already-local: neither a refusal nor a mapping -- the third
            // destination

            carried.mentionsDriven = true;
            fate.driven = true;
            fate.edge.kind = EdgeFact::Kind::AlreadyLocal;
            fate.childPort = fate.parentSignal;
            fate.through = BindingKind::Whole;
            fate.reached = true;
            carried.fates.push_back(std::move(fate));
            continue;
        }

        auto whole = inverse.whole.find(key);
        if (whole != inverse.whole.end()) {
            fate.edge.kind = EdgeFact::Kind::Whole;
            fate.childPort = display(to_symbol_expr(whole->second)
                                         .get_identifier());
            fate.through = BindingKind::Whole;
            fate.reached = true;

            // The port may be the same net under a different SHAPE -- a
            // packed struct at the boundary and the vector it is laid out as
            // inside. Same bits, so the substitution is sound, but the
            // expression around it was typed against the original and a bare
            // swap would leave it ill-typed.
            exprt replacement = whole->second;
            if (replacement.type() != symbol.type())
                fate.edge.retyped = true,
                replacement = typecast_exprt{replacement, symbol.type()};

            if (inverse.driven.count(
                    to_symbol_expr(whole->second).get_identifier()) ||
                index.inverse.drivenKeys.count(aliasKey)) {
                carried.mentionsDriven = true;
                fate.driven = true;
            } else if (!globalClockOrReset(fate.childPort))
                carried.mentionsArriving = true;

            carried.substitution.emplace(identifier, replacement);
            carried.fates.push_back(std::move(fate));
            continue;
        }

        // Use complement only when neither a whole binding nor slice runs exist.
        // The complement map already enforces equal types.
        auto complement = inverse.complemented.find(key);
        if (complement != inverse.complemented.end() &&
            complement->second.type() == symbol.type() &&
            inverse.partial.find(key) == inverse.partial.end() &&
            inverse.slices.find(key) == inverse.slices.end()) {
            const auto& port = to_symbol_expr(complement->second);
            fate.edge.kind = EdgeFact::Kind::Whole;
            fate.edge.complemented = true;
            fate.childPort = display(port.get_identifier());
            fate.through = BindingKind::Whole;
            fate.reached = true;

            if (inverse.driven.count(port.get_identifier()) ||
                index.inverse.drivenKeys.count(aliasKey)) {
                carried.mentionsDriven = true;
                fate.driven = true;
            } else if (!globalClockOrReset(fate.childPort))
                carried.mentionsArriving = true;

            carried.substitution.emplace(identifier, complementOf(port));
            carried.complemented = true;
            carried.fates.push_back(std::move(fate));
            continue;
        }

        auto partial = inverse.partial.find(key);
        if (partial != inverse.partial.end()) {
            // Rebase only reads covered by a carried run; a whole-signal or out-of-range
            // read cannot be replaced by the child's partial view.
            auto runs = inverse.slices.find(key);
            if (runs != inverse.slices.end()) {
                for (const auto& run : runs->second) {
                    auto attempt = carried.rewritten;
                    // Whether a read of the whole signal was rewritten, over
                    // the AUTOMATON only: the round trip checks the automaton,
                    // and the printed form is rebased separately below.
                    bool wholeRead = false;
                    if (rebase(attempt.init, identifier, run.port,
                               run.segments, run.wholeSignal, &wholeRead) &&
                        rebase(attempt.trans, identifier, run.port,
                               run.segments, run.wholeSignal, &wholeRead) &&
                        rebase(attempt.errorSignal, identifier, run.port,
                               run.segments, run.wholeSignal, &wholeRead) &&
                        rebase(attempt.livenessSignal, identifier, run.port,
                               run.segments, run.wholeSignal, &wholeRead)) {
                        // Rebase printable SVA independently, on a copy: failure must not corrupt
                        // the monitor or leave a half-rewritten source expression.
                        bool printed = false;
                        if (attempt.sourceProperty.is_not_nil()) {
                            exprt candidate = attempt.sourceProperty;
                            if (rebase(candidate, identifier, run.port,
                                       run.segments, run.wholeSignal, nullptr, true)) {
                                attempt.sourceProperty = std::move(candidate);
                                printed = true;
                            }
                        }
                        if (!printed) {
                            carried.rebasePrintable = false;
                            // An unrebased source tree must not reappear as a
                            // printable property after a later whole edge.
                            attempt.sourceProperty.make_nil();
                        }

                        carried.rewritten = std::move(attempt);
                        carried.rebased.push_back({symbol, run.port,
                                                   run.segments,
                                                   run.wholeSignal,
                                                   wholeRead});
                        if (inverse.driven.count(run.port.get_identifier()) ||
                            index.inverse.drivenKeys.count(aliasKey)) {
                            carried.mentionsDriven = true;
                            fate.driven = true;
                        } else if (!globalClockOrReset(
                                     display(run.port.get_identifier())))
                            carried.mentionsArriving = true;
                        fate.childPort =
                            display(run.port.get_identifier());
                        fate.edge.kind = EdgeFact::Kind::SliceCarried;
                        fate.through = BindingKind::Partial;
                        fate.reached = true;
                        break;
                    }
                }
            }

            if (fate.reached) {
                carried.fates.push_back(std::move(fate));
                continue;
            }

            // Existing runs with out-of-range reads differ from a binding with no
            // admitted runs. Report SliceOutside and SliceRefused separately.
            fate.through = BindingKind::Partial;
            const bool anyRunsRecorded =
                runs != inverse.slices.end() && !runs->second.empty();

            const bool signingUnsupported = anyRunsRecorded &&
                (signedWholeRead(carried.rewritten.init, identifier) ||
                 signedWholeRead(carried.rewritten.trans, identifier) ||
                 signedWholeRead(carried.rewritten.errorSignal, identifier));
            if (signingUnsupported) {
                fate.edge.kind = EdgeFact::Kind::SliceRefused;
                fate.edge.shapeCodes = {ReasonCode::WidthType};
                fate.blockedBy = "signed whole value requires a preserved signing cast; "
                                 "the current expression printer cannot provide it";
                fate.witnessKind = "unsupported-signing";
                fate.witness = display(identifier);
            } else if (anyRunsRecorded) {
                fate.edge.kind = EdgeFact::Kind::SliceOutside;
                fate.blockedBy =
                    "reaches the child only through a slice, and the property "
                    "reads outside it";
                fate.witnessKind = "partial-bits";
                std::ostringstream carriedBits;
                for (const auto& run : runs->second) {
                    if (!carriedBits.str().empty()) carriedBits << ", ";
                    carriedBits << display(run.port.get_identifier());
                    mp_integer occupied = 0;
                    for (const auto& segment : run.segments) occupied += segment.width;
                    if (occupied != bitWidth(run.port.type()))
                        for (const auto& segment : run.segments)
                            carriedBits << " [" << (segment.sigLo + segment.width - 1)
                                        << ":" << segment.sigLo << "] -> ["
                                        << (segment.portLo + segment.width - 1)
                                        << ":" << segment.portLo << "]";
                }
                fate.witness = "carried by " + carriedBits.str();
            } else {
                fate.edge.kind = EdgeFact::Kind::SliceRefused;
                fate.blockedBy =
                    "the binding is a view the admitted fragment does not "
                    "certify";
                attributeSlice(fate, key, inverse);
            }
            carried.fates.push_back(std::move(fate));
            continue;
        }

        // Sigma has no edge at all for this observation. Whether that is
        // "nothing to do with this instance" or "it arrives a cycle later"
        // is a question about trans(), which this function cannot see and
        // must not answer.
        carried.unexplained.push_back({carried.fates.size(), identifier, key});
        carried.fates.push_back(std::move(fate));
    }

    return carried;
}

// ---- 3. attribution ------------------------------------------------------

// Explain unmatched observations without changing substitutions or coverage.
void attributeAll(Carried& carried, const InstanceEdge& edge,
                  const std::map<irep_idt, irep_idt>& aliases,
                  const EdgeIndex& index, const Attribution& attribution,
                  const AliasClasses& classes, ReachCache& cache) {
    for (const auto& pending : carried.unexplained) {
        // The walk belongs to the SIGNAL, not to this pair -- see
        // SequentialReach. Done once per signal for the whole run when a
        // caller supplied the cache.
        auto cached = cache.find(pending.key);
        if (cached == cache.end())
            cached = cache.emplace(pending.key,
                                   sequentialReach(pending.key, aliases,
                                                   classes, attribution))
                         .first;

        attribute(carried.fates[pending.fate], pending.key, index.inverse,
                  cached->second, index.blackbox,
                  index.abortAtoms.count(pending.identifier) != 0,
                  attribution.clocks, edge.instance);
    }
}

// ---- 5. the export -------------------------------------------------------

// Retain the original incoming monitor, before rebasing, alongside the mapping.
void exportClaims(PropertyLanding& transport, const sva::Monitor& source,
                  const std::map<irep_idt, exprt>& substitution,
                  const std::vector<PropertyLanding::ClaimedRebase>& rebased) {
    transport.source = source;
    transport.claimedSubstitution = substitution;
    transport.claimedRebases = rebased;
}

}  // namespace

bool roundTripsToTop(const sva::Monitor& original,
                     const std::vector<const PropertyLanding*>& hops) {
    std::optional<sva::Monitor> back = hops.front()->monitor;
    bool complements = false;
    for (const auto* hop : hops) {
        back = undoHop(*back, hop->claimedSubstitution, hop->claimedRebases);
        if (!back) return false;
        complements = complements || usesComplement(hop->claimedSubstitution);
        if (hop->expandedDefinitions) {
            auto source = hop->source;
            if (back->abortDropped && !source.abortDropped) {
                const auto body = sva::withoutAbort(source);
                if (!body) return false;
                source = *body;
            }
            const auto expanded = symbolic::substituteAtoms(source, *hop->expandedDefinitions);
            if (!sameMonitor(expanded, *back, complements)) return false;
            // Definitions came from unconditional same-cycle RTL equalities.
            // Restore the source only after checking the complete expression,
            // rather than guessing an inverse of an arithmetic operation.
            back = std::move(source);
        }
    }
    if (back->abortDropped && !original.abortDropped) {
        const auto body = sva::withoutAbort(original);
        return body && sameMonitor(*body, *back, complements);
    }
    return sameMonitor(original, *back, complements);
}

// A frontier monitor is immutable while it is tried at multiple boundaries.
// Read its expression DAG and strip its abort guard at most once. Keep this
// cache with that exact monitor, not with a property name: a later frontier
// has already been renamed into another scope and needs a fresh read set.
struct MonitorReadCache {
    const sva::Monitor& monitor;
    std::vector<symbol_exprt> observations;
    bool bodyComputed = false;
    std::optional<sva::Monitor> body;
    std::vector<symbol_exprt> bodyObservations;

    explicit MonitorReadCache(const sva::Monitor& source)
        : monitor(source), observations(source.atomSymbols()) {}

    const sva::Monitor* withoutAbort() {
        if (!bodyComputed) {
            body = sva::withoutAbort(monitor);
            if (body) bodyObservations = body->atomSymbols();
            bodyComputed = true;
        }
        return body ? &*body : nullptr;
    }
};

// Reporting coverage and monitor eligibility are distinct: a partial arrival
// with an entirely unbound signal cannot produce a monitor.
static void classifyCoverage(PropertyLanding& landing) {
    std::size_t arrived = 0;
    bool unbound = false, substantiveArrival = false;
    for (const auto& fate : landing.fates) {
        if (!fate.reached) {
            unbound |= fate.edge.kind == EdgeFact::Kind::None;
            continue;
        }
        ++arrived;
        const auto& name = fate.childPort.empty() ? fate.parentSignal : fate.childPort;
        substantiveArrival |= !globalClockOrReset(name);
    }

    landing.certified = arrived != 0 && !unbound;
    if (arrived != 0 && arrived == landing.fates.size())
        landing.result = LoweringCoverage::Full;
    else if (landing.certified || substantiveArrival)
        landing.result = LoweringCoverage::Partial;
    else
        landing.result = LoweringCoverage::Blocked;
}

static PropertyLanding transportDownPrepared(MonitorReadCache& readCache,
                        const std::shared_ptr<const InstanceEdge>& ownedEdge,
                        const std::map<irep_idt, irep_idt>& aliases,
                        const Attribution& attribution,
                        const AliasClasses& classes, ReachCache* cache,
                        const PreparedEdge& prepared) {
    const auto& monitor = readCache.monitor;
    const auto& edge = *ownedEdge;
    PropertyLanding transport;
    transport.property = monitor.property;
    transport.isAssumption = monitor.isAssumption;
    transport.isResetContract = monitor.isResetContract;
    transport.abortDropped = monitor.abortDropped;
    transport.instance = edge.instance;
    transport.module = edge.module;
    transport.edge = ownedEdge;

    // Standalone calls may omit the shared alias index and reach cache.
    AliasClasses local;
    if (classes.empty() && !aliases.empty()) local = aliasClasses(aliases);
    const AliasClasses& classIndex = classes.empty() ? local : classes;

    ReachCache privateCache;
    ReachCache& reachCache = cache ? *cache : privateCache;

    const auto index = indexEdge(monitor, prepared);

    auto carried = carry(monitor, readCache.observations, edge, aliases, index);
    attributeAll(carried, edge, aliases, index, attribution, classIndex,
                 reachCache);

    const bool missingAbort = std::any_of(carried.fates.begin(), carried.fates.end(),
        [](const SignalFate& fate) { return !fate.reached && fate.role == RoleFact::AbortAtom; });
    if (missingAbort) {
        if (const auto* body = readCache.withoutAbort()) {
            const auto bodyIndex = indexEdge(*body, prepared);
            auto bodyCarried = carry(*body, readCache.bodyObservations,
                                     edge, aliases, bodyIndex);
            attributeAll(bodyCarried, edge, aliases, bodyIndex, attribution, classIndex, reachCache);
            // Recheck every body observation, including a reset signal also
            // used by the body. Dropping the guard does not excuse any other
            // missing observation or partial binding.
            if (!bodyCarried.fates.empty() &&
                std::all_of(bodyCarried.fates.begin(), bodyCarried.fates.end(),
                    [](const SignalFate& fate) { return fate.reached; })) {
                carried = std::move(bodyCarried);
                transport.abortDropped = true;
            }
        }
    }

    transport.fates = std::move(carried.fates);

    classifyCoverage(transport);
    transport.isAssumptionHere = transport.certified && !carried.mentionsDriven &&
        !transport.abortDropped && !monitor.hasLocalVariables;
    transport.mentionsInput = carried.mentionsArriving;
    transport.rebasePrintable = carried.rebasePrintable;

    if (transport.certified) {
        transport.monitor =
            symbolic::substituteAtoms(carried.rewritten, carried.substitution);
        if (carried.complemented) simplifyComplements(transport.monitor);
        exportClaims(transport, monitor, carried.substitution, carried.rebased);
        transport.roundTrips = roundTripsBack(monitor, transport.monitor,
                                              carried.substitution,
                                              carried.rebased);
        transport.roundTripsOneHop = transport.roundTrips;
    }

    return transport;
}

PropertyLanding transportDown(const sva::Monitor& monitor,
                        const InstanceEdge& edge,
                        const std::map<irep_idt, irep_idt>& aliases,
                        const Attribution& attribution,
                        const AliasClasses& classes, ReachCache* cache,
                        const BlackboxSet& blackboxes) {
    // Standalone callers always prepare the exact current arguments. Nothing
    // is cached by instance name or shared across independent calls/designs.
    const auto prepared = prepareEdge(edge, aliases, blackboxes);
    MonitorReadCache readCache(monitor);
    return transportDownPrepared(readCache, std::make_shared<const InstanceEdge>(edge), aliases, attribution,
                                 classes, cache, prepared);
}


std::vector<AuthoredAssumeOverDriven> authoredAssumesOverDriven(
    const std::vector<PropertyLanding>& transports) {
    std::vector<AuthoredAssumeOverDriven> found;
    for (const auto& transport : transports) {
        // The author's keyword is the whole reason this landing is an
        // assumption. A reset contract is excluded because it is not an
        // always-assumption at all -- it is an initial-instant premise, and
        // it is STATED at a boundary about signals the design drives as a
        // matter of course.
        if (!transport.isAssumption || transport.isResetContract) continue;
        // The same gate usableAsAssumption applies: a landing this tool
        // would not grant is not a landing worth warning about, because
        // nothing stands on it.
        if (assumeGrade(transport) == AssumeGrade::Refused) continue;
        // ... and this is the direction fact, unchanged: every signal
        // arriving through an input means nothing here drives it.
        if (transport.isAssumptionHere) continue;

        AuthoredAssumeOverDriven pair;
        pair.property = transport.property;
        pair.instance = transport.instance;
        pair.module = transport.module;
        for (const auto& fate : transport.fates)
            if (fate.driven) {
                pair.signal = fate.childPort.empty() ? fate.parentSignal
                                                     : fate.childPort;
                break;
            }
        found.push_back(std::move(pair));
    }
    std::sort(found.begin(), found.end(),
              [](const AuthoredAssumeOverDriven& a,
                 const AuthoredAssumeOverDriven& b) {
                  return a.property == b.property ? a.instance < b.instance
                                                  : a.property < b.property;
              });
    return found;
}

void warnAuthoredAssumesOverDriven(
    const std::vector<AuthoredAssumeOverDriven>& driven, std::ostream& out,
    bool recorded) {
    for (const auto& pair : driven) {
        // One fact per line, and the two long names on lines of their own: a
        // chip's property and instance paths are most of a terminal width by
        // themselves, and a warning nobody can read is a warning nobody acts
        // on.
        out << "warning: AUTHORED ASSUME OVER A NET THIS DESIGN DRIVES\n"
            << "warning:   property: " << pair.property << "\n"
            << "warning:   driver:   " << pair.instance << " : "
            << pair.module << "\n"
            << "warning:   drives:   "
            << (pair.signal.empty() ? "a signal this property names"
                                    : pair.signal)
            << "\n"
            << "warning:   An `assert' of the same sentence would have been "
               "an obligation\n"
            << "warning:   there and an engine would have been asked. "
               "Written `assume' it\n"
            << "warning:   is granted instead -- at that driver, and "
               "everywhere else it\n"
            << "warning:   lands.\n"
            // Authored assumptions are trusted; this warning does not change their role.
            << "warning:   NOTHING CHANGED, HERE OR ANYWHERE. The tool takes "
               "the boundary\n"
            << "warning:   specification as written; this is information "
               "about what is\n"
            << "warning:   trusted, not a defect and not a refusal.\n";
        if (recorded)
            out << "warning:   The pair is in the manifest under "
                   "`authored_assume_over_driven'.\n";
    }
    if (!driven.empty())
        out << "warning: " << driven.size() << " authored assumption landing"
            << (driven.size() == 1 ? "" : "s")
            << " over nets the design drives.\n";
}

sva::Monitor rerootToChild(const PropertyLanding& transport,
                                 const std::string& childTop) {
    // `top.u_c1.' -> `<childTop>.'
    const auto instancePrefix = transport.instance + '.';

    std::map<irep_idt, exprt> substitution;

    for (const auto& symbol : transport.monitor.atomSymbols()) {
        auto shown = display(symbol.get_identifier());
        if (shown.compare(0, instancePrefix.size(), instancePrefix) != 0)
            continue;

        auto member = shown.substr(instancePrefix.size());

        // childTop is already a full identifier -- ElaboratedDesign::top()
        // gives the frontend's own namespacing -- so append the member to it
        // directly. Re-adding the language prefix would produce
        // `Verilog::$root.Verilog::child.v_o' and refer to nothing.
        auto renamed = childTop + '.' + member;

        substitution.emplace(symbol.get_identifier(),
                             symbol_exprt{renamed, symbol.type()});
    }

    return symbolic::substituteAtoms(transport.monitor, substitution);
}

SequentialReach sequentialReach(const irep_idt& key,
                                const std::map<irep_idt, irep_idt>& aliases,
                                const AliasClasses& classes,
                                const Attribution& attribution) {
    SequentialReach reach;
    if (attribution.empty()) return reach;

    // A node is (name, has a clock edge been crossed yet). The flag is part
    // of the identity, not a property of the name: a wire can be reachable
    // same-cycle AND, by another route, a cycle later, and collapsing the two
    // loses whichever arrives second. Only the flagged half is an answer --
    // a same-cycle route is not a refusal reason, it is a landing.
    struct Node {
        irep_idt name;
        bool afterClock;
        std::size_t stages;
        std::vector<irep_idt> route;
        irep_idt lastRegister;
    };

    // Prune only when an earlier arrival used no more hops and no more stages.
    // Keep pre-clock and post-clock states distinct. This table is lookup-only;
    // witness order comes from the frontier and sorted edges.
    std::unordered_map<irep_idt, std::size_t, irep_id_hash> fewest[2];
    std::vector<Node> frontier;

    // Start from every alias: drive maps use elaborated names, which may differ
    // from the representative chosen by union-find.
    auto seed = [&](const irep_idt& name) {
        if (fewest[0].try_emplace(name, 0).second)
            frontier.push_back({name, false, 0, {name}, irep_idt{}});
    };
    auto members = classes.find(key);
    if (members != classes.end())
        for (const auto& member : members->second) seed(member);
    seed(key);

    for (std::size_t hop = 1; hop <= sequentialHops && !frontier.empty();
         hop++) {
        std::vector<Node> next;
        auto step = [&](const Node& from, const irep_idt& to, bool delayed) {
            const bool afterClock = from.afterClock || delayed;
            const std::size_t stages = from.stages + (delayed ? 1 : 0);
            auto best = fewest[afterClock ? 1 : 0].try_emplace(to, stages);
            if (!best.second) {
                if (stages >= best.first->second) return;
                best.first->second = stages;
            }
            auto route = from.route;
            route.push_back(to);
            const irep_idt lastRegister = delayed ? to : from.lastRegister;
            next.push_back({to, afterClock, stages, route, lastRegister});
            if (!afterClock) return;
            auto seen = reach.stages.find(to);
            if (seen == reach.stages.end() || stages < seen->second) {
                reach.stages[to] = stages;
                reach.route[to] = std::move(route);
                reach.lastRegister[to] = lastRegister;
            }
        };

        for (const auto& from : frontier) {
            auto comb = attribution.combinational.find(from.name);
            if (comb != attribution.combinational.end())
                for (const auto& to : comb->second) step(from, to, false);
            auto delayed = attribution.delayed.find(from.name);
            if (delayed != attribution.delayed.end())
                for (const auto& to : delayed->second) step(from, to, true);
        }
        frontier = std::move(next);
    }

    // Keyed by alias class, holding whichever name got there in the fewest
    // stages, because that is the form every per-instance lookup wants and
    // doing it here is what makes the lookup cheap enough to run per pair.
    for (const auto& arrival : reach.stages) {
        auto alias = aliases.find(arrival.first);
        auto klass = alias == aliases.end() ? arrival.first : alias->second;
        auto held = reach.byClass.find(klass);
        if (held == reach.byClass.end() ||
            arrival.second < reach.stages.at(held->second))
            reach.byClass[klass] = arrival.first;
    }

    return reach;
}

Attribution attributionInputs(const transition_systemt& system,
                              const std::vector<InstanceEdge>& edges,
                              const std::map<irep_idt, irep_idt>& aliases,
                              std::size_t workers) {
    Attribution attribution;
    attribution.delayed = forwardDrives(system.trans_expr.trans(), true, workers);
    attribution.combinational = forwardDrives(system.trans_expr.invar(), false, workers);
    attribution.clocks = clockNets(edges, aliases);
    return attribution;
}

// Classify module bodies without entering child implementations during lowering.
BlackboxSet blackboxInstances(const transition_systemt& system,
                              const std::vector<InstanceEdge>& edges,
                              std::size_t threads) {
    std::set<std::string, std::less<>> instances;
    for (const auto& edge : edges) instances.insert(edge.instance);
    std::set<std::string> hasBody;
    const auto owner = [&](std::string_view name, bool includeSelf) -> const std::string* {
        for (;;) {
            if (includeSelf) {
                const auto found = instances.find(name);
                if (found != instances.end()) return &*found;
            }
            const auto dot = name.rfind('.');
            if (dot == std::string_view::npos) return nullptr;
            name = name.substr(0, dot);
            includeSelf = true;
        }
    };

    // Every ancestor of an instance has a body, even without internal wires.
    for (const auto& inner : instances) {
        const auto* scope = owner(inner, false);
        while (scope) {
            hasBody.insert(*scope);
            scope = owner(*scope, false);
        }
    }
    for (const auto& [id, symbol] : system.symbol_table.symbols) {
        if (hasBody.size() == instances.size()) break;
        if (symbol.is_input || symbol.is_output) continue;
        if (const auto* scope = owner(display(id), false)) hasBody.insert(*scope);
    }
    if (hasBody.size() == instances.size()) return {};

    // Constraints in port-only modules can establish a body too. Separate
    // conjuncts retain independent name sets; a parent binding is not a body.
    std::vector<const exprt*> roots;
    std::vector<const exprt*> pending{&system.trans_expr.trans(), &system.trans_expr.invar()};
    while (!pending.empty()) {
        const auto* expr = pending.back();
        pending.pop_back();
        if (expr->id() == ID_and) {
            for (auto it = expr->operands().rbegin(); it != expr->operands().rend(); ++it)
                pending.push_back(&*it);
        } else roots.push_back(expr);
    }
    const auto workers = util::workerCount(roots.size(), threads);
    std::vector<std::set<const std::string*>> discovered(workers);
    profile::count("blackbox.constraints", roots.size());
    profile::count("blackbox.workers", workers);
    util::parallelFor(roots.size(), threads, [&](std::size_t index, std::size_t worker) {
        auto& known = discovered[worker];
        const std::string* enclosing = nullptr;
        std::string firstName;
        bool multipleNames = false, settled = false;
        visitOperandDagUntil(*roots[index], [&](const exprt& e) {
            if (e.id() != ID_symbol && e.id() != ID_next_symbol) return false;
            const auto name = display(e.get(ID_identifier));
            const auto* scope = owner(name, true);
            // Different owners meet at an ancestor already counted above.
            // More names cannot move that common scope down into a new body.
            if (!scope || hasBody.count(*scope) || known.count(scope) ||
                (enclosing && scope != enclosing)) {
                settled = true;
                return true;
            }
            if (!enclosing) {
                enclosing = scope;
                firstName = name;
            } else multipleNames |= name != firstName;
            return false;
        });
        if (!settled && multipleNames) known.insert(enclosing);
    });
    for (const auto& found : discovered)
        for (const auto* instance : found) hasBody.insert(*instance);

    BlackboxSet blackboxes;
    for (const auto& instance : instances)
        if (!hasBody.count(instance)) blackboxes.insert(instance);
    return blackboxes;
}

AliasClasses aliasClasses(const std::map<irep_idt, irep_idt>& aliases) {
    AliasClasses classes;
    for (const auto& alias : aliases) classes[alias.second].push_back(alias.first);
    return classes;
}

bool singlePathPlacement = true;

// Follow one full, non-reader child per level. Keep off-path reach records,
// but mark them unplaced and report where the chain stopped.
void placeSinglePath(std::vector<PropertyLanding>& rows, std::vector<StoppedAbove>* stopped) {
    std::map<std::string, std::vector<std::size_t>> byProperty;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.isAssumption || row.isResetContract) continue;
        if (row.result != LoweringCoverage::Full &&
            !(row.result == LoweringCoverage::Partial && row.isAssumptionHere)) continue;
        byProperty[row.property].push_back(i);
    }
    std::size_t unplaced = 0;
    auto parentOf = [](const PropertyLanding& row) {
        return row.edge->parentInstance;
    };
    for (const auto& [property, indices] : byProperty) {
        std::map<std::string, std::vector<std::size_t>> children;
        for (auto i : indices)
            if (rows[i].result == LoweringCoverage::Full)
                children[parentOf(rows[i])].push_back(i);
        std::set<std::string> chain;
        std::string here, reason = "deepest";
        for (;;) {
            const auto next = children.find(here);
            if (next == children.end() || next->second.empty()) break;
            if (next->second.size() > 1) { reason = "shared-by-siblings"; break; }
            const auto& child = rows[next->second.front()];
            if (child.isAssumptionHere) { reason = "not-established-here"; break; }
            chain.insert(child.instance);
            here = child.instance;
        }
        if (reason == "deepest" && indices.size() == chain.size()) continue;
        for (auto i : indices) {
            auto& row = rows[i];
            if (chain.count(row.instance)) continue;
            const bool below = here.empty() || row.instance.rfind(here + ".", 0) == 0;
            row.placed = false;
            row.stoppedAt = here;
            row.stopReason = below ? reason : "off-path";
            if (below && row.result == LoweringCoverage::Partial && reason == "deepest")
                row.stopReason = "not-established-here";
            unplaced++;
            if (stopped) stopped->push_back({row.property, row.instance, here, row.stopReason});
        }
    }
    if (unplaced)
        std::cerr << "placement: " << unplaced << " landing(s) not placed by the single-path rule "
                  << "(the property stays at the parent; see stopped_above in transport.json)\n";
}

std::vector<PropertyLanding> lowerProperties(
    const sva::MonitorSet& set,
    const std::vector<InstanceEdge>& edges,
    const std::map<irep_idt, irep_idt>& aliases,
    const Attribution& attribution, const BlackboxSet& blackboxes,
    std::vector<StoppedAbove>* stopped, std::size_t threads,
    const symbolic::CombinationalDefinitions* definitions) {
    profile::Phase callPhase("transport.call");
    profile::count("transport.monitors", set.monitors.size());
    profile::count("transport.edges", edges.size());
    // Inverted once for the whole run, not once per edge: the search walks
    // forward and the map is keyed the other way.
    const auto classes = aliasClasses(aliases);
    // Complement definitions across the whole design, read once.
    const auto complements = collectComplements(edges);

    const auto workers = util::workerCount(set.monitors.size(), threads);
    profile::count("transport.workers", workers);
    const bool measuring = profile::enabled();
    std::vector<profile::Metrics> workerMetrics(workers);
    std::vector<std::optional<PreparedEdge>> prepared(edges.size());
    std::vector<std::shared_ptr<const InstanceEdge>> sharedEdges(edges.size());
    if (!set.monitors.empty()) {
        profile::Phase indexing("transport.edge-index");
        std::vector<profile::Metrics> metrics(util::workerCount(edges.size(), threads));
        util::parallelFor(edges.size(), threads, [&](std::size_t i, std::size_t worker) {
            profile::CollectWorkerMetrics capture(measuring ? &metrics[worker] : nullptr);
            sharedEdges[i] = std::make_shared<const InstanceEdge>(edges[i]);
            prepared[i].emplace(prepareEdge(edges[i], aliases, blackboxes, complements));
        });
        profile::merge(metrics);
    }

    struct LoweredSource {
        std::vector<PropertyLanding> rows;
        std::size_t before = 0, collapsed = 0, disagreed = 0;
        profile::Clock::duration dedupElapsed{};
        std::map<std::pair<std::string, std::string>, std::vector<std::string>> disagreements;
    };
    // Each worker keeps its own reach cache across properties. The design,
    // edge indexes and source monitors are immutable throughout the pool.
    std::vector<ReachCache> caches(workers);
    std::vector<LoweredSource> sources(set.monitors.size());
    auto lowerOne = [&](std::size_t source, std::size_t worker) {
        profile::CollectWorkerMetrics capture(measuring ? &workerMetrics[worker] : nullptr);
        profile::Measure sourceWork("transport.source-work");
        auto& reachCache = caches[worker];
        std::vector<PropertyLanding> result;
        // Every source is tried at every edge on its first frontier. Reserve
        // those retained rows once, instead of copying the accumulated records
        // during geometric growth. Extra temporary routes can still grow it.
        result.reserve(edges.size());
        std::size_t rowsBeforeDedup = 0, collapsed = 0, disagreed = 0;
        profile::Clock::duration dedupElapsed{};
        std::map<std::pair<std::string, std::string>, std::vector<std::string>> disagreements;

        auto deduplicate = [&](std::size_t first) {
            const auto began = profile::Clock::now();
            rowsBeforeDedup += result.size() - first;
            // Only the completed property's suffix is touched. Earlier rows are
            // still the final records of their properties, not temporary routes.
            std::set<std::string> arrived;
            for (auto it = result.begin() + first; it != result.end(); ++it)
                if (it->result != LoweringCoverage::Blocked)
                    arrived.insert(it->property + '\0' + it->instance);
            result.erase(
                std::remove_if(result.begin() + first, result.end(),
                               [&](const PropertyLanding& transport) {
                                   return transport.result == LoweringCoverage::Blocked &&
                                          arrived.count(transport.property + '\0' + transport.instance) != 0;
                               }),
                result.end());

            // Preserve the old stable order: the shallowest route wins, and
            // equal-depth ties retain their traversal order.
            stableSortTransports(result, first,
                             [](const PropertyLanding& a, const PropertyLanding& b) {
                                 if (a.property != b.property) return a.property < b.property;
                                 if (a.instance != b.instance) return a.instance < b.instance;
                                 return a.routeDepth < b.routeDepth;
                             });
            std::map<std::string, LoweringCoverage> verdict;
            result.erase(
                std::remove_if(result.begin() + first, result.end(),
                               [&](const PropertyLanding& transport) {
                                   auto key = transport.property + '\0' + transport.instance;
                                   auto known = verdict.find(key);
                                   if (known == verdict.end()) {
                                       verdict.emplace(key, transport.result);
                                       return false;
                                   }
                                   collapsed++;
                                   if (known->second != transport.result) {
                                       disagreed++;
                                       std::ostringstream message;
                                       message << "two routes disagree: " << transport.property
                                               << " at " << transport.instance << " is "
                                               << toString(known->second) << " and "
                                               << toString(transport.result) << "\n";
                                       disagreements[{transport.property, transport.instance}].push_back(message.str());
                                   }
                                   return true;
                               }),
                result.end());
            dedupElapsed += profile::Clock::now() - began;
        };

        {
            const auto& monitor = set.monitors[source];
            profile::Phase sourcePhase("transport.source", monitor.property);
            const auto first = result.size();
            // The first frontier visits every edge. A refused row never enters
            // the frontier, so its slot can be replaced if a later route lands.
            // Keep that one slot instead of retaining every refused attempt until
            // deduplication. The shallow first refusal is the original tie winner.
            std::vector<std::size_t> rowAt(edges.size(), result.max_size());
            std::vector<std::size_t> refused(edges.size(), 0);
            std::size_t attempts = 0;
            // What has arrived somewhere and can be pushed further. Each entry is
            // a monitor named in some instance's scope, plus that instance.
            struct Arrival {
                sva::Monitor monitor;
                std::string instance;  // empty at the top
                std::size_t depth = 0;
                // The rows this arrival came through, top first, as indices into
                // `result': the round trip of anything pushed from here undoes
                // every one of them.
                std::vector<std::size_t> route;
            };

            std::vector<Arrival> frontier{{monitor, std::string{}, 0, {}}};
            std::set<std::string> visited;

            while (!frontier.empty()) {
                profile::count("transport.frontier-pops");
                auto here = std::move(frontier.back());
                frontier.pop_back();
                MonitorReadCache readCache(here.monitor);

                for (const auto& edge : edges) {
                    // Only edges below where we are. At the top that is all of
                    // them; deeper, an edge elsewhere in the hierarchy cannot be
                    // reached from here and trying it would report a blocked
                    // transport that was never on the table.
                    if (!here.instance.empty()) {
                        const auto prefix = here.instance + '.';
                        if (edge.instance.compare(0, prefix.size(), prefix) != 0)
                            continue;
                    }

                    if (visited.count(edge.instance)) continue;

                    profile::count("transport.down-calls");
                    const auto position = static_cast<std::size_t>(&edge - edges.data());
                    auto transport =
                        transportDownPrepared(readCache, sharedEdges[position], aliases, attribution,
                                              classes, &reachCache, *prepared[position]);
                    if (definitions && transport.result != LoweringCoverage::Full) {
                        const auto& inverse = prepared[position]->inverse;
                        const auto& direct = prepared[position]->direct;
                        auto available = [&](const symbol_exprt& symbol) {
                            const auto id = symbol.get_identifier();
                            const auto alias = aliases.find(id);
                            const auto key = alias == aliases.end() ? id : alias->second;
                            const auto shown = display(id), prefix = edge.instance + '.';
                            return shown.compare(0, prefix.size(), prefix) == 0 ||
                                direct.whole.count(id) || direct.slices.count(id) ||
                                inverse.whole.count(key) || inverse.slices.count(key);
                        };
                        auto expanded = definitions->expand(readCache.observations, edge.instance, available);
                        if (!expanded.empty()) {
                            auto rewritten = symbolic::substituteAtoms(here.monitor, expanded);
                            MonitorReadCache expandedReads(rewritten);
                            auto candidate = transportDownPrepared(expandedReads, sharedEdges[position], aliases,
                                attribution, classes, &reachCache, *prepared[position]);
                            if (candidate.result == LoweringCoverage::Full && candidate.roundTrips) {
                                candidate.source = here.monitor;
                                candidate.expandedDefinitions =
                                    std::make_shared<const std::map<irep_idt, exprt>>(std::move(expanded));
                                for (const auto& [id, value] : *candidate.expandedDefinitions) {
                                    std::ostringstream equation;
                                    equation << display(id) << " = " << format(value);
                                    candidate.flowEdges.push_back({"combinational-definition", equation.str(), edge.parentInstance});
                                }
                                candidate.evidenceOrigin = monitor.property;
                                candidate.roundTrips = roundTripsToTop(here.monitor, {&candidate});
                                candidate.roundTripsOneHop = candidate.roundTrips;
                                if (candidate.roundTrips) transport = std::move(candidate);
                            }
                        }
                    }
                    ++attempts;

                    // How deep in the HIERARCHY, not how many search steps: with
                    // aliases followed, a property about a chip output can reach
                    // a grandchild in one step, and "one step" is not the number
                    // a reader wants.
                    transport.depth = edge.depth;
                    transport.routeDepth = here.depth;

                    // Below the first edge the round trip is checked against the
                    // original, through every edge of the route, not against the
                    // parent's version alone.
                    if (transport.certified && !here.route.empty()) {
                        std::vector<const PropertyLanding*> hops{&transport};
                        for (auto it = here.route.rbegin(); it != here.route.rend(); ++it)
                            hops.push_back(&result[*it]);
                        transport.roundTrips = roundTripsToTop(monitor, hops);
                        const auto& previous = result[here.route.back()];
                        if (!previous.flowEdges.empty()) {
                            transport.flowEdges.insert(transport.flowEdges.begin(),
                                previous.flowEdges.begin(), previous.flowEdges.end());
                            transport.evidenceOrigin = monitor.property;
                        }
                    }

                    if (transport.result != LoweringCoverage::Blocked) {
                        visited.insert(edge.instance);
                        // Partial transports are not pushed further: the monitor
                        // is already about only some of the bits, and taking a
                        // slice of a slice compounds an approximation this code
                        // does not track.
                        if (transport.result == LoweringCoverage::Full) {
                            auto route = here.route;
                            route.push_back(rowAt[position] == result.max_size()
                                                ? result.size() : rowAt[position]);
                            frontier.push_back(
                                {transport.monitor, edge.instance, transport.depth, std::move(route)});
                        }
                    } else {
                        ++refused[position];
                    }

                    if (rowAt[position] == result.max_size()) {
                        rowAt[position] = result.size();
                        result.push_back(std::move(transport));
                    } else if (transport.result != LoweringCoverage::Blocked) {
                        result[rowAt[position]] = std::move(transport);
                    }
                    sourcePhase.progress();
                }
            }
            // Every route index belongs to this frontier. None is live now.
            // Preserve the old raw attempt and duplicate diagnostics. Refusals
            // removed in favor of an arrival were not counted as duplicates.
            rowsBeforeDedup += attempts - (result.size() - first);
            for (std::size_t i = 0; i < rowAt.size(); ++i)
                if (refused[i] > 1 &&
                    result[rowAt[i]].result == LoweringCoverage::Blocked)
                    collapsed += refused[i] - 1;
            deduplicate(first);
        }

        sources[source] = {std::move(result), rowsBeforeDedup, collapsed, disagreed,
                           dedupElapsed, std::move(disagreements)};
    };
    util::parallelFor(set.monitors.size(), threads, lowerOne);
    profile::merge(workerMetrics);

    std::vector<PropertyLanding> result;
    std::size_t count = 0, rowsBeforeDedup = 0, collapsed = 0, disagreed = 0;
    profile::Clock::duration dedupElapsed{};
    for (const auto& source : sources) count += source.rows.size();
    result.reserve(count);
    for (auto& source : sources) {
        rowsBeforeDedup += source.before;
        collapsed += source.collapsed;
        disagreed += source.disagreed;
        dedupElapsed += source.dedupElapsed;
        for (const auto& [key, messages] : source.disagreements)
            for (const auto& message : messages) std::cerr << message;
        std::move(source.rows.begin(), source.rows.end(), std::back_inserter(result));
        std::vector<PropertyLanding>().swap(source.rows);
    }
    if (profile::enabled()) {
        try { callPhase.measured["transport.dedup"] += profile::seconds(dedupElapsed); }
        catch (...) { callPhase.sink->fail("diagnostic measurement allocation failed"); }
    }
    profile::count("transport.rows-before-dedup", rowsBeforeDedup);
    if (collapsed)
        std::cerr << collapsed << " duplicate (property, instance) row(s) "
                  << "collapsed, " << disagreed << " with disagreeing verdicts\n";

    stableSortTransports(result, 0,
                     [](const PropertyLanding& a, const PropertyLanding& b) {
                         if (a.property != b.property) return a.property < b.property;
                         if (a.depth != b.depth) return a.depth < b.depth;
                         return a.instance < b.instance;
                     });

    profile::count("transport.rows-after-dedup", result.size());
    if (singlePathPlacement) placeSinglePath(result, stopped);
    return result;
}

void writeText(const std::vector<PropertyLanding>& transports, std::ostream& out) {
    std::size_t full = 0, partial = 0, blocked = 0, abortDropped = 0;
    std::size_t roundTripped = 0, constraints = 0, resetContracts = 0;

    for (const auto& transport : transports) {
        out << transport.property << "  ->  " << transport.instance << " : "
            << transport.module << "   [" << toString(transport.result);
        if (transport.result != LoweringCoverage::Blocked)
            out << (transport.isAssumptionHere ? ", assume" : ", assert")
                << (transport.mentionsInput ? ", contextual" : ", self");
        if (transport.abortDropped) {
            out << ", abort-dropped";
            ++abortDropped;
        }
        out << "]\n";

        for (const auto& fate : transport.fates) {
            const auto codes = fate.reasons();
            out << "    " << fate.parentSignal;
            if (fate.reached)
                out << "  ->  " << fate.childPort;
            else
                out << "  --  ";
            if (!codes.empty())
                out << "[" << toString(primaryReason(codes)) << "] ";
            out << fate.blockedBy;
            if (!fate.witness.empty()) out << " -- " << fate.witness;
            out << '\n';
        }

        if (transport.result != LoweringCoverage::Blocked && !transport.roundTrips)
            out << "    ROUND TRIP FAILED -- substituting sigma back does not "
                   "recover the property\n";

        if (transport.result != LoweringCoverage::Blocked) {
            std::ostringstream signal;
            signal << format(transport.monitor.errorSignal);
            out << "    error  " << frontend::displayName(signal.str()) << '\n';
        }

        out << '\n';

        switch (transport.result) {
            case LoweringCoverage::Full:
                full++;
                break;
            case LoweringCoverage::Partial:
                partial++;
                break;
            case LoweringCoverage::Blocked:
                blocked++;
                break;
        }
        if (transport.result != LoweringCoverage::Blocked && transport.roundTrips)
            ++roundTripped;
        if (transport.isAssumptionHere) {
            ++constraints;
            if (transport.isResetContract) ++resetContracts;
        }
    }

    out << full << " full, " << partial << " partial, " << blocked
        << " blocked; " << roundTripped << " of " << (full + partial)
        << " round-trip\n";
    if (abortDropped)
        out << abortDropped << " abort-dropped assertion landing(s): stronger body; "
               "target implies source; counterexamples require replay\n";
    out << (full + partial - constraints) << " assertion(s) and "
        << constraints << " assumption(s)";
    if (resetContracts != 0)
        out << " (" << resetContracts
            << " of them reset-contract landing(s), which are environment "
               "facts and not intent)";
    out << "\n";
}

void writeJson(const std::vector<PropertyLanding>& transports, std::ostream& out,
               const std::vector<sva::UnsupportedProperty>& refused,
               const std::vector<AuthoredAssumeOverDriven>& driven,
               const std::vector<StoppedAbove>& stopped) {
    out << "{\n \"excluded\": [";
    for (std::size_t i = 0; i < refused.size(); i++)
        out << (i ? ",\n  " : "\n  ")
            << "{\"property\": \"" << escape(refused[i].property)
            << "\", \"reason\": \"" << escape(refused[i].reason) << "\"}";
    out << (refused.empty() ? "" : "\n ") << "],\n";
    // THE GUARD'S LEDGER, and only when it has something in it. A run that
    // found none writes the manifest it always wrote, which is what keeps
    // every recorded baseline in test/tools/ a statement about transport
    // rather than about this key.
    if (!driven.empty()) {
        out << " \"authored_assume_over_driven\": [\n";
        for (std::size_t i = 0; i < driven.size(); i++)
            out << "  {\"property\": \"" << escape(driven[i].property)
                << "\", \"instance\": \"" << escape(driven[i].instance)
                << "\", \"module\": \"" << escape(driven[i].module)
                << "\", \"signal\": \"" << escape(driven[i].signal) << "\"}"
                << (i + 1 == driven.size() ? "\n" : ",\n");
        out << " ],\n";
    }
    // The placement rule's ledger, only when it removed something, for the
    // same reason as the guard's ledger above.
    if (!stopped.empty()) {
        out << " \"stopped_above\": [\n";
        for (std::size_t i = 0; i < stopped.size(); i++)
            out << "  {\"property\": \"" << escape(stopped[i].property)
                << "\", \"instance\": \"" << escape(stopped[i].instance)
                << "\", \"stopped_at\": \"" << escape(stopped[i].stoppedAt)
                << "\", \"reason\": \"" << escape(stopped[i].reason) << "\"}"
                << (i + 1 == stopped.size() ? "\n" : ",\n");
        out << " ],\n";
    }
    out << " \"transports\": [\n";
    for (std::size_t i = 0; i < transports.size(); i++) {
        const auto& transport = transports[i];
        out << "  {\"property\": \"" << escape(transport.property)
            << "\", \"instance\": \"" << escape(transport.instance)
            << "\", \"module\": \"" << escape(transport.module)
            << "\", \"result\": \"" << toString(transport.result)
            << "\", \"round_trip\": " << (transport.roundTrips ? "true" : "false")
            << (transport.roundTripsOneHop != transport.roundTrips
                    ? std::string(", \"round_trip_one_hop\": ") + (transport.roundTripsOneHop ? "true" : "false")
                    : std::string())
            << ", \"depth\": " << transport.depth
            << ", \"route_depth\": " << transport.routeDepth
            << ", \"assume_here\": "
            << (transport.isAssumptionHere ? "true" : "false")
            << ", \"certified\": "
            << (transport.certified ? "true" : "false");
        if (transport.abortDropped)
            out << ", \"abort_dropped\": true, \"correspondence_direction\": \"target-implies-source\"";
        // Only on the rows the placement rule left unplaced; an absent key
        // means placed, which keeps every earlier manifest readable as is.
        if (!transport.placed)
            out << ", \"placed\": false, \"stopped_at\": \"" << escape(transport.stoppedAt)
                << "\", \"stop_reason\": \"" << escape(transport.stopReason) << "\"";
        // Always emit both observation lists, including empty ones.
        const auto arrived = transport.arrived();
        out << ", \"arrived\": [";
        for (std::size_t a = 0; a < arrived.size(); a++)
            out << (a ? ", " : "") << "\"" << escape(arrived[a]) << "\"";
        out << "], \"dropped\": [";
        const auto dropped = transport.dropped();
        for (std::size_t d = 0; d < dropped.size(); d++) {
            if (d) out << ", ";
            out << "{\"signal\": \"" << escape(dropped[d].signal)
                << "\", \"reason\": \"" << escape(dropped[d].reason)
                << "\", \"reasons\": [";
            for (std::size_t r = 0; r < dropped[d].reasons.size(); r++)
                out << (r ? ", " : "") << "\"" << escape(dropped[d].reasons[r])
                    << "\"";
            out << "]}";
        }
        out << "]";
        // The premise grade, on the records where the author's keyword makes
        // it a premise question. An assertion has no grade -- it is Tier-1 or
        // it is a note -- and printing one there would invite a reader to
        // treat `sound-as-constraint' as a claim about the assertion.
        if (transport.isAssumption)
            out << ", \"assume_grade\": \""
                << toString(assumeGrade(transport)) << "\"";
        const auto copies = copyPortAssumptions(transport);
        if (!copies.empty()) {
            out << ", \"assumption_copies\": [";
            for (std::size_t c = 0; c < copies.size(); ++c)
                out << (c ? ", " : "") << "{\"from\": \""
                    << escape(display(copies[c].from.get_identifier()))
                    << "\", \"to\": \"" << escape(display(copies[c].to.get_identifier()))
                    << "\", \"round_trip\": true}";
            out << "]";
        }
        out << ", \"signals\": [";
        for (std::size_t f = 0; f < transport.fates.size(); f++) {
            const auto& fate = transport.fates[f];
            const auto codes = fate.reasons();
            const auto route = fate.path();
            if (f != 0) out << ", ";
            out << "{\"parent\": \"" << escape(fate.parentSignal)
                << "\", \"reached\": " << (fate.reached ? "true" : "false")
                << ", \"child_port\": \"" << escape(fate.childPort)
                << "\", \"note\": \"" << escape(fate.blockedBy) << "\"";
            if (fate.edge.kind == EdgeFact::Kind::AlreadyLocal)
                out << ", \"edge\": \"already-local\"";
            if (!codes.empty()) {
                out << ", \"primary_reason_code\": \""
                    << toString(primaryReason(codes)) << "\"";
                out << ", \"all_reason_codes\": [";
                // In precedence order, not discovery order, so two runs
                // agree byte for byte whatever order the premises were
                // written in.
                bool firstCode = true;
                for (auto candidate : PRECEDENCE)
                    for (auto reason : codes)
                        if (reason == candidate) {
                            if (!firstCode) out << ", ";
                            firstCode = false;
                            out << "\"" << toString(candidate) << "\"";
                        }
                out << "]";
            }
            if (!route.empty()) {
                out << ", \"path\": [";
                for (std::size_t k = 0; k < route.size(); k++)
                    out << (k ? ", " : "") << "\"" << route[k] << "\"";
                out << "]";
            }
            if (!fate.witnessKind.empty())
                out << ", \"witness_kind\": \"" << escape(fate.witnessKind)
                    << "\", \"witness\": \"" << escape(fate.witness) << "\"";
            out << "}";
        }
        out << "]";
        if (!transport.flowEdges.empty()) {
            out << ", \"edges\": [{\"kind\": \"binding\"}";
            for (const auto& flow : transport.flowEdges) {
                out << ", {\"kind\": \"" << escape(flow.kind) << "\"";
                if (!flow.record.empty())
                    out << ", \"record\": \"" << escape(flow.record) << "\"";
                if (!flow.detail.empty())
                    out << ", \"instance\": \"" << escape(flow.detail) << "\"";
                out << "}";
            }
            out << ", {\"kind\": \"binding\"}]";
            out << ", \"evidence_origin\": \""
                << escape(transport.evidenceOrigin) << "\"";
        }
        out << "}" << (i + 1 == transports.size() ? "\n" : ",\n");
    }
    out << " ]\n}\n";
}

}  // namespace lowering
}  // namespace veval
