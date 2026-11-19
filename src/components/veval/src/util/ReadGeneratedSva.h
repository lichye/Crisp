#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace veval::util {
inline std::string svaSpace(const std::string &text) {
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (std::isspace(c))
            space = !out.empty();
        else {
            if (space)
                out += ' ';
            out += char(c);
            space = false;
        }
    }
    return out;
}
// Identity includes declarations (widths, signedness and initializers), not
// just a reference to a named property whose definition could change.
inline std::string svaSentence(const std::string &declarations, const std::string &body) {
    return declarations.empty() ? body
                                : "property __cf_selected; " + svaSpace(declarations) + " " +
                                      svaSpace(body) + "; endproperty";
}
struct GeneratedProperty {
    std::string declarations, body;
    std::size_t begin, end;
};
struct GeneratedFunction {
    std::string text;
    std::size_t begin, end;
};
// Canonical combinational helpers emitted by TypedPrint. Parse headers only;
// a regular expression over an FP12 function body can exhaust the regex stack.
inline std::map<std::string, GeneratedFunction> generatedFunctions(const std::string &source) {
    std::map<std::string, GeneratedFunction> out;
    static const std::regex name(R"(\b(__cf_function_[A-Za-z_0-9]+)\s*\()");
    std::size_t at = 0;
    while ((at = source.find("function automatic ", at)) != source.npos) {
        const auto headerEnd = source.find('\n', at);
        const auto header =
            source.substr(at, headerEnd == source.npos ? source.size() - at : headerEnd - at);
        std::smatch match;
        if (!std::regex_search(header, match, name)) {
            at += 19;
            continue;
        }
        const auto end = source.find("endfunction", at);
        if (end == source.npos)
            throw std::runtime_error("unterminated generated function");
        if (!out.emplace(match[1],
                         GeneratedFunction{source.substr(at, end + 11 - at), at, end + 11})
                 .second)
            throw std::runtime_error("duplicate generated function definition");
        at = end + 11;
    }
    return out;
}
inline std::string withoutGeneratedFunctions(std::string source) {
    auto definitions = generatedFunctions(source);
    std::vector<GeneratedFunction> ranges;
    for (const auto &pair : definitions)
        ranges.push_back(pair.second);
    std::sort(ranges.begin(), ranges.end(),
              [](const auto &a, const auto &b) { return a.begin > b.begin; });
    for (const auto &range : ranges)
        source.replace(range.begin, range.end - range.begin, "\n");
    return source;
}
// Reader for the canonical named definitions emitted by ModuleSpec. This is
// deliberately not a second general SystemVerilog parser: the native helper
// parses and typechecks the resulting standard SVA before compiling it.
inline std::map<std::string, GeneratedProperty> generatedProperties(const std::string &source) {
    std::map<std::string, GeneratedProperty> out;
    static const std::regex header(
        R"((?:^|\n)[ \t]*property[ \t]+(__cf_rule_[A-Za-z_0-9]+)[ \t]*;)");
    for (std::sregex_iterator it(source.begin(), source.end(), header), end; it != end; ++it) {
        auto begin = std::size_t(it->position()), at = begin + it->length();
        auto finish = source.find("endproperty", at);
        if (finish == source.npos)
            throw std::runtime_error("unterminated generated property");
        std::string declarations;
        while (at < finish) {
            auto token = source.find_first_not_of(" \r\n\t", at);
            if (token == source.npos || source.compare(token, 4, "bit ") != 0)
                break;
            auto stop = source.find(';', token);
            if (stop == source.npos || stop >= finish)
                throw std::runtime_error("bad local declaration");
            declarations += source.substr(token, stop - token + 1) + " ";
            at = stop + 1;
        }
        auto body = svaSpace(source.substr(at, finish - at));
        if (body.empty() || body.back() != ';' || declarations.empty())
            throw std::runtime_error("bad generated local property definition");
        body.pop_back();
        body = svaSpace(body);
        if (!out.emplace((*it)[1],
                         GeneratedProperty{svaSpace(declarations), body, begin, finish + 11})
                 .second)
            throw std::runtime_error("duplicate generated property definition");
    }
    return out;
}
inline std::string withoutGeneratedProperties(std::string source) {
    auto definitions = generatedProperties(source);
    std::vector<GeneratedProperty> ranges;
    for (const auto &pair : definitions)
        ranges.push_back(pair.second);
    std::sort(ranges.begin(), ranges.end(),
              [](const auto &a, const auto &b) { return a.begin > b.begin; });
    for (const auto &range : ranges)
        source.replace(range.begin, range.end - range.begin, "\n");
    return source;
}
} // namespace veval::util
