#include "check/wave.hpp"

#include <util/symbol_table.h>

#include <sstream>
#include <string>

namespace {
void expect(const std::string& vcd, const std::string& text) {
    if (vcd.find(text) == std::string::npos)
        throw std::runtime_error("missing VCD text: " + text);
}
}

int main() {
    symbol_tablet symbols;
    namespacet ns{symbols};
    null_message_handlert messages;
    messaget message{messages};
    trans_tracet trace;
    trace.states.resize(3);
    trace.states[1].property_failed = true;

    // No assignments or symbols exist in any of the three frames.
    trace.loop_start = 0;
    std::ostringstream loop;
    crisp::check::write_wave(trace, message, ns, loop);
    expect(loop.str(), "$scope module ebmc $end");
    expect(loop.str(), "$comment witness loop 0 -> 2 (equal endpoints) $end");
    expect(loop.str(), "#0\n0!\n#1\n1!\n#2\n0!\n");

    trace.loop_start.reset();
    trace.total_extension = true;
    std::ostringstream total;
    crisp::check::write_wave(trace, message, ns, total);
    expect(total.str(), "$scope module ebmc $end");
    expect(total.str(), "$comment finite witness with proved total extension $end");
    expect(total.str(), "#0\n0!\n#1\n1!\n");
    if (total.str().find("\n#2") != std::string::npos)
        throw std::runtime_error("the finite witness extends past the bad frame");
}
