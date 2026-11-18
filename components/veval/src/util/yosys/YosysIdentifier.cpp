// Validate exact compiled identifiers before emitting Yosys command tokens.
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
int main(int argc, char **argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: consflow_yosys_identifier module NAME [WIRE ...] | wire NAME");
        const std::string mode = argv[1], name = argv[2];
        // Real Coral specialization names contain static instance indices.
        // Exclude whitespace, command separators and selection metacharacters.
        const std::regex module("[A-Za-z_][A-Za-z_0-9.$]*(\\[[0-9]+\\][A-Za-z_0-9.$]*)*");
        const std::regex wire("[A-Za-z_][A-Za-z_0-9$]*");
        if (mode == "module" && std::regex_match(name, module)) {
            // hierarchy -top and select -module perform exact IdString lookup.
            // This token must never be used as a selection pattern.
            // Validate the entire batch before emitting any token.
            for (int i = 3; i < argc; ++i)
                if (!std::regex_match(argv[i], wire))
                    throw std::runtime_error("unsupported exact Yosys wire identifier");
            std::cout << name << '\n';
            for (int i = 3; i < argc; ++i) std::cout << "w:" << argv[i] << '\n';
        } else if (mode == "wire" && argc == 3 && std::regex_match(name, wire)) {
            std::cout << "w:" << name << '\n';
        } else throw std::runtime_error("unsupported exact Yosys identifier");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
