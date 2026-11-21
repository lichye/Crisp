#pragma once

#include <trans-netlist/trans_trace.h>

#include <ostream>
#include <stdexcept>

namespace crisp::check {

inline bool certified(const trans_tracet& trace) {
    return trace.loop_start.has_value() || trace.total_extension;
}

inline void write_wave(const trans_tracet& trace, messaget& message, const namespacet& ns, std::ostream& wave) {
    if (trace.states.empty()) throw std::runtime_error("EBMC returned an empty counterexample");
    if (!certified(trace)) throw std::runtime_error("EBMC returned an uncertified counterexample");
    show_trans_trace_vcd(trace, message, ns, wave);
}

} // namespace crisp::check
