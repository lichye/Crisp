#pragma once

struct buechi_transt;
class message_handlert;

namespace veval::sva {
// Replace a safety NFA's existential run with the set of all reachable states.
// Keep already deterministic encodings. Must run before adding the abort
// wrapper. Liveness automata are unchanged.
void determinizeSafety(buechi_transt&, message_handlert&);
}
