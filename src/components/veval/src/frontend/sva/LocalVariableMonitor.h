#pragma once
#include "PropertyMonitor.h"

namespace veval::sva {
bool hasLocalVariables(const exprt &);
// Alpha-normalize rule-local names; design identifiers are untouched.
exprt canonicalLocalVariables(exprt, bool keepDisplayNames = false);
// Local declarations, in lexical order, followed by the original body.
std::vector<exprt> localDeclarations(const exprt &);
exprt withoutLocalDeclarations(exprt);

struct LocalEncoding {
    Monitor monitor;
    std::size_t payloadWidth = 0, choiceWidth = 0;
    exprt initialPayload, nextPayload, failure, abort;
};
// Existential violation monitor for assertions. A run selects one launch
// and one antecedent match. Consequent alternatives are never guessed.
// Unsupported shapes and local-variable assumptions throw with a reason.
LocalEncoding localViolationMonitor(const exprt &, const std::string &property);
bool localAbortWindowExact(const exprt &);
} // namespace veval::sva
