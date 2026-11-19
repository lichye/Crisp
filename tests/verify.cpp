#include "verify.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace crisp;

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("expected repository and scratch paths");
        const fs::path root = argv[1], scratch = argv[2];
        const Widths widths{{"u_dut.a",1},{"u_dut.b",4}};
        for (const auto* expression : {"(a |=> a)", "($past(a) == a)", "(b == a)", "a || a", "missing"}) {
            try { bindPredicate(expression, widths); }
            catch (const std::invalid_argument&) { continue; }
            throw std::runtime_error(std::string("accepted unsupported expression: ") + expression);
        }
        const auto parsed = bindPredicate("((! a) |-> (a == 1'b1))", widths);
        if (parsed.formula != "(=> (not [u_dut.a]) (= [u_dut.a] true))")
            throw std::runtime_error("incorrect Boolean translation");

        Options options;
        options.top = "token_formal";
        options.design = {root / "examples/token/rtl/token.sv"};
        options.spec = root / "examples/token/spec.sv";
        const Tools tools{root / "src/components/veval/build/veval", {}};
        auto falseCandidate = [](const auto&, const auto&, const auto&, const auto&, auto&) {
            return MiningResult{"success", {"left"}};
        };
        std::ostringstream report;
        const int code = verify(options, tools, scratch, report, falseCandidate);
        const auto log = report.str();
        if (code != 3 || log.find("Lemma left: refuted") == std::string::npos ||
            log.find("Counterexample (VCD)") == std::string::npos || log.find("$enddefinitions") == std::string::npos ||
            log.find("token_formal.p_no_conflict @ token_formal.dut.state: unknown") == std::string::npos ||
            log.find("Retry with") != std::string::npos) throw std::runtime_error(log);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
