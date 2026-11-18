#include "ReplaceDontCareConstants.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace veval {
namespace frontend {

namespace {

// A sized literal with at least one don't-care digit (x or z): `1'hx',
// `32'hxxxxxxxx', `8'h0x', `4'b1x0x', `8'sbx'. Each don't-care BIT becomes a
// free wire and each defined bit keeps its value. Yosys does write mixed
// literals -- a default branch {4'h0, 4'bxxxx} comes out as `8'h0x' -- so
// leaving them alone read their x bits as zero, the very thing this pass is
// for.
struct Literal {
    std::size_t begin = 0;      // index of the first digit of the width
    std::size_t end = 0;        // one past the last digit
    bool isSigned = false;      // the `s' of `8'sbx'
    std::string bits;           // most significant first: '0', '1' or 'x'
};

bool dontCareDigit(char c) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c == 'x' || c == 'z' || c == '?';
}

// Is the apostrophe at `tick' an UNSIZED literal holding a don't-care
// (`'x', `'bx', `'hz0')? Its width comes from the context, which a line
// rewrite cannot see, so the caller refuses it rather than leave it as 0.
bool unsizedDontCare(const std::string& line, std::size_t tick) {
    if (tick > 0) {
        char before = line[tick - 1];
        if (std::isalnum(static_cast<unsigned char>(before)) || before == '_' ||
            before == '$' || before == '\\')
            return false;
    }
    std::size_t at = tick + 1;
    if (at < line.size() && std::tolower(static_cast<unsigned char>(line[at])) == 's') at++;
    if (at < line.size() && std::strchr("bBhHdDoO", line[at]) != nullptr) at++;
    bool any = false;
    for (; at < line.size(); at++) {
        if (dontCareDigit(line[at])) any = true;
        else if (!(std::isxdigit(static_cast<unsigned char>(line[at])) || line[at] == '_')) break;
    }
    return any;
}

bool literalAt(const std::string& line, std::size_t tick, Literal& found) {
    // Walk back over the width digits.
    std::size_t start = tick;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(line[start - 1])))
        start--;
    if (start == tick) return false;   // no width: handled by the caller

    // A width must not be glued to an identifier -- `abc12'h...' is not a
    // literal, and identifiers may contain digits.
    if (start > 0) {
        char before = line[start - 1];
        if (std::isalnum(static_cast<unsigned char>(before)) || before == '_' ||
            before == '$' || before == '\\')
            return false;
    }

    std::size_t base = tick + 1;
    bool isSigned = false;
    if (base < line.size() && std::tolower(static_cast<unsigned char>(line[base])) == 's') {
        isSigned = true;
        base++;
    }
    if (base >= line.size()) return false;
    char radix = static_cast<char>(
        std::tolower(static_cast<unsigned char>(line[base])));
    std::size_t bitsPerDigit = radix == 'b' ? 1 : radix == 'o' ? 3 : radix == 'h' ? 4 : 0;
    if (bitsPerDigit == 0 && radix != 'd') return false;

    // The digits, least significant last.
    std::size_t scan = base + 1;
    std::string digits;
    while (scan < line.size() &&
           (std::isxdigit(static_cast<unsigned char>(line[scan])) ||
            dontCareDigit(line[scan]) || line[scan] == '_')) {
        if (line[scan] != '_') digits += line[scan];
        scan++;
    }
    if (digits.empty()) return false;
    if (scan < line.size() &&
        (std::isalnum(static_cast<unsigned char>(line[scan])) || line[scan] == '_'))
        return false;
    if (std::none_of(digits.begin(), digits.end(), dontCareDigit)) return false;

    const auto width = static_cast<std::size_t>(std::stoul(line.substr(start, tick - start)));
    if (width == 0) return false;

    // Bits least significant first.
    std::string low;
    if (radix == 'd') {
        // A decimal literal may hold a don't-care only as its single digit.
        if (digits.size() != 1) return false;
        low.assign(width, 'x');
    } else {
        for (auto digit = digits.rbegin(); digit != digits.rend(); ++digit) {
            if (dontCareDigit(*digit)) {
                low.append(bitsPerDigit, 'x');
                continue;
            }
            const auto value = std::stoul(std::string(1, *digit), nullptr, 16);
            if (value >> bitsPerDigit) return false;   // `2'b2' is not ours
            for (std::size_t b = 0; b < bitsPerDigit; b++)
                low += ((value >> b) & 1) ? '1' : '0';
        }
        // Extension: with an x or z leftmost digit the literal is padded
        // with x, otherwise with 0 (IEEE 1800 5.7.1); excess bits drop.
        const char fill = dontCareDigit(digits.front()) ? 'x' : '0';
        if (low.size() < width) low.append(width - low.size(), fill);
        low.resize(width);
    }

    found.begin = start;
    found.end = scan;
    found.isSigned = isSigned;
    found.bits.assign(low.rbegin(), low.rend());
    return true;
}

// Does this line close the module header -- the `);' that ends the port list?
bool closesHeader(const std::string& line) {
    for (std::size_t i = 0; i + 1 < line.size(); i++)
        if (line[i] == ')' && line[i + 1] == ';') return true;
    return false;
}

}  // namespace

FreeDontCareResult freeDontCares(std::istream& in, std::ostream& out,
                                 const std::string& prefix) {
    FreeDontCareResult result;
    std::size_t counter = 0;

    // The whole module is buffered before being written, because the
    // declarations belong after its header and the literals needing them are
    // found while reading the body.
    std::vector<std::string> body;
    std::vector<std::string> declare;
    std::size_t headerEnd = 0;      // index in `body' just past the header
    bool inModule = false, headerOpen = false;

    std::string line;
    auto flush = [&]() {
        for (std::size_t i = 0; i < body.size(); i++) {
            if (i == headerEnd && !declare.empty()) {
                out << "  // Free values standing in for synthesis "
                       "don't-cares; see FreeDontCare.h. Undriven, so free "
                       "every cycle.\n";
                for (const auto& name : declare) out << "  wire " << name << ";\n";
                result.modules++;
            }
            out << body[i] << "\n";
        }
        body.clear();
        declare.clear();
        headerEnd = 0;
    };

    while (std::getline(in, line)) {
        if (!inModule && line.compare(0, 7, "module ") == 0) {
            inModule = true;
            headerOpen = true;
            body.push_back(line);
            if (closesHeader(line)) {
                headerOpen = false;
                headerEnd = body.size();
            }
            continue;
        }

        if (!inModule) {
            out << line << "\n";
            continue;
        }

        if (headerOpen) {
            body.push_back(line);
            if (closesHeader(line)) {
                headerOpen = false;
                headerEnd = body.size();
            }
            continue;
        }

        // Body: free every don't-care bit in it.
        std::string rewritten;
        rewritten.reserve(line.size());
        std::size_t at = 0;
        while (at < line.size()) {
            auto tick = line.find('\'', at);
            Literal found;
            if (tick == std::string::npos || !literalAt(line, tick, found)) {
                if (tick != std::string::npos && unsizedDontCare(line, tick))
                    throw std::runtime_error(
                        "freex: unsized don't-care literal, width unknown: " + line);
                rewritten.append(line, at, tick == std::string::npos
                                               ? std::string::npos
                                               : tick + 1 - at);
                if (tick == std::string::npos) break;
                at = tick + 1;
                continue;
            }

            rewritten.append(line, at, found.begin - at);

            // Most significant first, as Verilog concatenation reads; a
            // defined bit keeps its value, a don't-care bit gets its own wire.
            std::vector<std::string> parts;
            for (char bit : found.bits) {
                if (bit != 'x') {
                    parts.push_back(bit == '1' ? "1'b1" : "1'b0");
                    continue;
                }
                auto name = prefix + "_" + std::to_string(counter++);
                declare.push_back(name);
                result.names.push_back(name);
                result.wires++;
                parts.push_back(name);
            }
            result.literals++;

            std::string value;
            if (parts.size() == 1) {
                value = parts.front();
            } else {
                value = "{";
                for (std::size_t i = 0; i < parts.size(); i++) {
                    if (i) value += ", ";
                    value += parts[i];
                }
                value += "}";
            }
            rewritten += found.isSigned ? "$signed(" + value + ")" : value;
            at = found.end;
        }
        body.push_back(rewritten);

        if (line.compare(0, 9, "endmodule") == 0) {
            flush();
            inModule = false;
        }
    }

    if (inModule) flush();
    return result;
}

}  // namespace frontend
}  // namespace veval
