#include "ReadLocalSpec.h"
#include "util/ReadGeneratedSva.h"
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

namespace veval::result_validation {
static J field(const std::vector<std::string> &comments, const std::string &name) {
    const std::string prefix = "  // " + name + ":";
    for (auto &s : comments)
        if (s.rfind(prefix, 0) == 0)
            return str(trim(s.substr(prefix.size())));
    return {};
}
Product::Product(const fs::path &p, Files &files) : path(p) {
    std::string content = files.read(p);
    const auto definitions = util::generatedProperties(content);
    std::istringstream in(content);
    std::vector<std::string> lines;
    std::vector<size_t> offsets;
    size_t offset = 0;
    for (std::string line; std::getline(in, line);) {
        offsets.push_back(offset);
        offset += line.size() + 1;
        lines.push_back(line);
    }
    std::regex entry(
        R"(^  (\w+):\s+(assert|assume)\s+property\s+\(@\((posedge|negedge)\s+(\w+)\)\s*$)");
    std::regex header(R"(^// (module|instance|boundary|depth):\s+(.*)$)");
    std::regex wrapper(R"(^module\s+(\w+)\s*\()");
    std::smatch match;
    std::vector<std::string> comments;
    reset = object({{"kind", str("not-provided")},
                    {"source_entry", J()},
                    {"initial_state", str("unconstrained")},
                    {"text", J()}});
    for (size_t i = 0; i < lines.size(); ++i) {
        if (files.budget)
            files.budget->check();
        auto &line = lines[i];
        if (std::regex_match(line, match, header)) {
            auto name = match[1].str(), value = trim(match[2]);
            if (name == "boundary") {
                auto at = value.find(" (");
                put(head, name, str(at == value.npos ? value : value.substr(0, at)));
                put(head, "boundary_spec",
                    at == value.npos ? J() : str(relocatedPath(value.substr(at + 2, value.size() - at - 3))));
            } else
                put(head, name, str(value));
        }
        if (std::regex_search(line, match, wrapper))
            put(head, "wrapper", str(match[1]));
        if (line.rfind("  // premise:", 0) == 0) {
            auto value = trim(line.substr(13));
            append(derived, str(value));
            std::regex re(R"(reset asserted at the initial instant, from `(\S+)')");
            if (text(reset["kind"]) == "not-provided" && std::regex_search(value, match, re))
                reset = object({{"kind", str("reset-contract")},
                                {"source_entry", str(match[1])},
                                {"initial_state", str("reset-asserted-at-0")},
                                {"text", str(value)}});
        }
        if (trim(line).rfind("//", 0) == 0) {
            comments.push_back(line);
            continue;
        }
        if (!std::regex_match(line, match, entry)) {
            if (!trim(line).empty())
                comments.clear();
            continue;
        }
        std::string label = match[1], kind = match[2], edge = match[3], clock = match[4], body;
        const auto begin = offsets[i];
        bool ended = false;
        while (++i < lines.size()) {
            if (files.budget)
                files.budget->check();
            body += ' ' + lines[i];
            auto t = trim(lines[i]);
            if (t.size() >= 2 && t.substr(t.size() - 2) == ");") {
                ended = true;
                break;
            }
        }
        if (!ended)
            throw std::runtime_error("unterminated product property: " + label);
        propertyRanges.push_back({begin, std::min(content.size(), offsets[i] + lines[i].size() + 1),
                                  label, kind == "assert"});
        body = spaces(body);
        body = trim(body.substr(0, body.size() - 2));
        if (auto definition = definitions.find(body); definition != definitions.end()) {
            if (kind != "assert") throw std::runtime_error("local variables in assumption: " + label);
            body = util::svaSentence(definition->second.declarations,definition->second.body);
        }
        J row = object({{"label", str(label)},
                        {"source_entry", field(comments, "from")},
                        {"clock", str(clock)},
                        {"clock_edge", str(edge)}});
        if (kind == "assert") {
            if (!targets[label].is_null())
                throw std::runtime_error("duplicate product assertion: " + label);
            put(row, "sentence", str(body));
            J ps = list();
            for (auto &v : words(text(field(comments, "premises"))))
                append(ps, str(v));
            put(row, "premises", ps);
            put(targets, label, row);
        } else {
            if (!assumes[label].is_null())
                throw std::runtime_error("duplicate product assumption: " + label);
            put(row, "text", str(body));
            for (auto &name : {"route", "arrival", "guarantee"})
                put(row, name, field(comments, name));
            put(row, "discharge_status", field(comments, "proof"));
            put(row, "environment_relation", field(comments, "environment-relation"));
            auto binding = field(comments, "design-binding");
            put(row, "design_binding", binding.is_null() ? J() : parse(text(binding)));
            put(assumes, label, row);
            append(order, str(label));
        }
        comments.clear();
    }
    for (auto &v : members(targets)) {
        std::set<std::string> clocks{text(v.second["clock"])};
        for (auto &name : items(v.second["premises"]))
            if (!assumes[text(name)].is_null())
                clocks.insert(text(assumes[text(name)]["clock"]));
        put(targets.make_object()[v.first], "clock_model",
            str(clocks.size() == 1 ? "single-declared-clock" : "multiple-declared-clocks"));
    }
    if (!truth(head["instance"]) || !truth(head["module"]) || !truth(head["wrapper"]))
        throw std::runtime_error("incomplete product header: " + p.string());
}
J Product::premises(const std::string &label) const {
    J out = list();
    std::set<std::string> named;
    for (auto &v : items(targets[label]["premises"]))
        named.insert(text(v));
    for (auto &v : items(derived))
        append(out, object({{"kind", str("derived")},
                            {"label", J()},
                            {"text", v},
                            {"scope", str("derived-from-design")}}));
    for (auto &name : items(order)) {
        J row = assumes[text(name)];
        put(row, "kind", str("transported-assume"));
        put(row, "scope",
            str(named.count(text(name)) ? "named-by-target" : "compiled-with-product"));
        if (truth(row["design_binding"])) {
            put(row, "kind", str("design-binding"));
            put(row, "scope", str("derived-from-design"));
        } else if (truth(row["guarantee"])) {
            throw std::runtime_error("lifted guarantees are no longer supported; regenerate local checks");
        } else if (truth(row["environment_relation"])) {
            put(row, "kind", str("environment-relation"));
            put(row, "scope", str("compiled-with-product"));
            put(row, "evidence_ref", row["environment_relation"]);
        }
        append(out, row);
    }
    return out;
}
} // namespace veval::result_validation
