#include "VerdictReport.h"
#include "util/files/AtomicOutput.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace veval {
namespace environment {

namespace {

const std::vector<std::string> kVerdicts = {
    "proved", "refuted", "bounded-clean", "unknown", "assume-only",
    "unsupported"};

struct ProductBlock {
    std::filesystem::path path;
    std::string text;
};

struct ProductIndex {
    std::map<std::string, std::pair<std::filesystem::path, std::string>>
        instances;
    std::map<std::pair<std::string, std::string>, ProductBlock> blocks;
};

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string trim(const std::string& text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string trimTrailingNewlines(std::string text) {
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

std::string sanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char ch : name)
        out += (std::isalnum(ch) || ch == '_' || ch == '.' || ch == '+' ||
                ch == '-')
                   ? static_cast<char>(ch)
                   : '_';
    return out;
}

std::string headerValue(const std::string& text, const std::string& name) {
    const std::regex pattern("^//\\s*" + name + ":\\s*(\\S+)",
                             std::regex_constants::multiline);
    std::smatch found;
    const auto header = text.substr(0, 4000);
    if (std::regex_search(header, found, pattern))
        return found[1].str();
    return {};
}

ProductIndex indexProducts(const std::filesystem::path& products) {
    ProductIndex index;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(products))
        if (entry.is_regular_file() &&
            entry.path().filename().string().size() >= 8 &&
            entry.path().filename().string().rfind("_spec.sv") ==
                entry.path().filename().string().size() - 8)
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    const std::regex from("^\\s*//\\s*from:\\s*(\\S+)",
                          std::regex_constants::multiline);
    const std::regex label(
        "^(\\s*)([A-Za-z_]\\w*)\\s*:\\s*(assert|assume)\\s+property\\s*\\(",
        std::regex_constants::multiline);
    for (const auto& path : files) {
        const auto text = read(path);
        const auto module = headerValue(text, "module");
        const auto instance = headerValue(text, "instance");
        if (!instance.empty()) index.instances[instance] = {path, module};

        std::size_t begin = 0;
        while (begin <= text.size()) {
            const auto end = text.find("\n\n", begin);
            const auto paragraph = text.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            std::smatch source, property;
            if (std::regex_search(paragraph, source, from) &&
                std::regex_search(paragraph, property, label)) {
                const auto block = trimTrailingNewlines(
                    paragraph.substr(static_cast<std::size_t>(property.position(0))));
                index.blocks[{instance, source[1].str()}] = {path, block};
            }
            if (end == std::string::npos) break;
            begin = end + 2;
        }
    }
    return index;
}

std::string premiseCounts(const std::string& detail) {
    const std::regex pattern(
        "assuming ([0-9]+) environment constraint\\(s\\)(?:\\s+and\\s+([0-9]+) derived premise\\(s\\))?");
    std::smatch found;
    if (!std::regex_search(detail, found, pattern))
        return "not reported in this landing's detail line";
    return found[1].str() + " environment constraint(s), " +
           (found[2].matched ? found[2].str() : "0") +
           " derived premise(s)";
}

std::pair<std::string, std::string> splitUnknownCause(const std::string& detail) {
    std::vector<std::string> clauses;
    std::istringstream stream(detail);
    std::string clause;
    while (std::getline(stream, clause, ';')) clauses.push_back(trim(clause));
    std::size_t index = 0;
    static const std::regex attemptedFraming(
        "^whole-design instance,.*so it was attempted$");
    if (!clauses.empty() &&
        std::regex_match(clauses[0], attemptedFraming))
        index = 1;
    const auto inProcess = index < clauses.size()
                               ? clauses[index]
                               : (clauses.empty() ? std::string() : clauses[0]);

    const auto refused = detail.find("sby escalation REFUSED:");
    if (refused != std::string::npos) {
        auto reason = detail.substr(refused + std::string("sby escalation REFUSED:").size());
        const auto record = reason.find("; record:");
        if (record != std::string::npos) reason.resize(record);
        else {
            const auto bareRecord = reason.find("record:");
            if (bareRecord != std::string::npos) reason.resize(bareRecord);
        }
        return {inProcess, "refused: " + trim(reason)};
    }
    const auto escalated = detail.find("sby escalation");
    if (escalated != std::string::npos) {
        auto outcome = detail.substr(escalated);
        const auto record = outcome.find("; record:");
        if (record != std::string::npos) outcome.resize(record);
        else {
            const auto bareRecord = outcome.find("record:");
            if (bareRecord != std::string::npos) outcome.resize(bareRecord);
        }
        return {inProcess, trim(outcome)};
    }
    return {inProcess, "(no sby escalation reported in this landing's detail line)"};
}

std::string relativeTo(const std::filesystem::path& path,
                       const std::filesystem::path& root) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return error ? path.string() : relative.string();
}

std::string entryText(const std::string& property, const std::string& instance,
                      const lowering::ProofLine& verdict,
                      const std::string& module,
                      const std::filesystem::path* productPath,
                      const ProductBlock* block,
                      const ResultWriterOptions& options) {
    std::vector<std::string> lines;
    // Frozen legacy views retain their original header for byte comparison.
    lines.push_back(options.legacyLayout
        ? "// by-verdict view entry -- generated by scripts/by_verdict_view.py"
        : "// by-verdict view entry -- generated by veval (native C++)");
    lines.push_back("// this file is a read-only rendering; edit the ledger or the spec, not this.");
    lines.push_back("//");
    lines.push_back("// property:       " + property);
    lines.push_back("// instance:       " + instance);
    lines.push_back("// module:         " +
                    (module.empty() ? "(product header carried no `module: line)" : module));
    lines.push_back("// verdict:        " + verdict.status);
    lines.push_back("// detail:         " + verdict.detail);
    lines.push_back("// premise tier:   " +
                    (verdict.premiseTier.empty()
                         ? "(none recorded for this landing -- not a decided assertion)"
                         : verdict.premiseTier));
    lines.push_back("// premise counts: " + premiseCounts(verdict.detail));
    if (!verdict.route.empty())
        lines.push_back("// route:          " + verdict.route);
    if (!verdict.evidenceOrigin.empty())
        lines.push_back("// evidence_origin: " + verdict.evidenceOrigin);
    if (!verdict.symEvalRecord.empty()) {
        lines.push_back("// symeval:        yes");
        lines.push_back("// symeval record: " + verdict.symEvalRecord);
    }
    if (!verdict.symEvalAntecedent.empty()) {
        std::istringstream antecedents(verdict.symEvalAntecedent);
        std::string antecedent;
        while (std::getline(antecedents, antecedent))
            lines.push_back("// symeval antecedent: " + antecedent);
    }

    if (verdict.status == "refuted") {
        if (verdict.trace.empty()) {
            lines.push_back("// trace:          MISSING -- verdicts.json recorded no trace path for a refuted landing");
        } else {
            const auto trace = std::filesystem::path(verdict.trace);
            const auto exists = trace.is_absolute()
                                    ? std::filesystem::exists(trace)
                                    : std::filesystem::exists(
                                          std::filesystem::path(options.repositoryRoot) / trace);
            lines.push_back("// trace:          " + verdict.trace);
            lines.push_back(exists ? "// trace check:    verified to exist on disk"
                                   : "// trace check:    WARNING -- path does not exist on disk");
        }
    }
    if (verdict.status == "unknown") {
        const auto [inProcess, escalation] = splitUnknownCause(verdict.detail);
        lines.push_back("// in-process:     " + inProcess);
        lines.push_back("// escalation:     " + escalation);
    }
    if (productPath == nullptr)
        lines.push_back("// product file:   NOT FOUND -- no per-module product carries this instance");
    else
        lines.push_back("// product file:   " +
                        relativeTo(*productPath, options.repositoryRoot));

    lines.push_back("//");
    if (block == nullptr) {
        if (productPath == nullptr)
            lines.push_back("// PROPERTY TEXT NOT LOCATED: the instance named in verdicts.json has no matching product file under this Result/<bench>Spec/ directory.");
        else
            lines.push_back("// PROPERTY TEXT NOT LOCATED: no `// from: " +
                            property + "' labelled assert/assume block was found in the product file above. Not guessing at the text -- check the product by hand.");
    } else {
        lines.push_back(block->text);
    }

    std::ostringstream out;
    for (const auto& line : lines) out << line << '\n';
    return out.str();
}

std::string indexText(const ResultWriterOptions& options,
                      const std::vector<std::string>& buckets,
                      const std::map<std::string, std::size_t>& counts,
                      const std::vector<std::tuple<std::string, std::string,
                                                   std::string, std::string,
                                                   std::string>>& rows) {
    std::ostringstream out;
    out << "# " << options.benchmark << " -- results by verdict\n\n";
    out << "Additive view over `log/" << options.benchmark
        << "/verdicts.json' and the per-module products under `Result/"
        << options.benchmark
        << (options.legacyLayout ? "Spec/*_spec.sv'." : "/products/*_spec.sv'.")
        << " Nothing here is a new decision; every row already exists in the ledger. Regenerate with `make "
        << (options.legacyLayout ? "by-verdict" : "run") << " BENCH="
        << options.benchmark << "'.\n\n";
    out << "## Counts\n\n| verdict | count |\n|---|---|\n";
    std::size_t total = 0;
    for (const auto& verdict : buckets) {
        const auto count = counts.count(verdict) ? counts.at(verdict) : 0;
        total += count;
        out << "| " << verdict << " | " << count << " |\n";
    }
    out << "| **total** | **" << total << "** |\n\n";
    out << "## Every landing\n\n";
    out << "For `refuted' rows, the last column is the trace path. For `unknown' rows, it is `in-process: <what the in-process engine did or why it was skipped> | escalation: <the terminal sby-escalation outcome>' -- see each file's own header for the two as separate lines.\n\n";
    out << "| verdict | property | instance | file | trace / unknown-cause |\n|---|---|---|---|---|\n";
    for (const auto& [verdict, property, instance, file, extra] : rows) {
        auto escapePipe = [](std::string value) {
            std::string escaped;
            for (char ch : value) {
                if (ch == '|') escaped += '\\';
                escaped += ch;
            }
            return escaped;
        };
        out << "| " << verdict << " | " << escapePipe(property) << " | "
            << escapePipe(instance) << " | " << file << " | "
            << escapePipe(extra) << " |\n";
    }
    return out.str();
}

}  // namespace

std::map<std::string, std::size_t> writeByVerdict(
    const VerdictMap& verdicts, const ResultWriterOptions& options) {
    const auto products = std::filesystem::path(options.productsDir);
    const auto root = std::filesystem::path(options.resultDir);
    const auto output = root / "by-verdict";
    const auto index = indexProducts(products);

    std::vector<std::string> buckets = kVerdicts;
    for (const auto& [key, verdict] : verdicts)
        if (verdict.status == "unattempted") {
            buckets.push_back("unattempted");
            break;
        }
    // A status outside the known words was dropped from every bucket and from
    // the total. It gets a bucket of its own, so the counts still add up.
    const std::vector<std::string> known = buckets;
    for (const auto& [key, verdict] : verdicts)
        if (std::find(known.begin(), known.end(), verdict.status) == known.end()) {
            buckets.push_back("unclassified");
            break;
        }
    std::map<std::string, std::size_t> counts;
    for (const auto& bucket : buckets) counts[bucket] = 0;

    std::error_code error;
    std::filesystem::remove_all(output, error);
    std::filesystem::create_directories(output);
    for (const auto& bucket : buckets)
        std::filesystem::create_directories(output / bucket);

    std::vector<std::tuple<std::string, std::string, std::string,
                           std::string, std::string>> rows;
    for (const auto& [key, verdict] : verdicts) {
        const auto bucket = std::find(known.begin(), known.end(), verdict.status) == known.end()
                                ? std::string("unclassified") : verdict.status;
        const auto tab = key.find('\t');
        const auto instance = key.substr(0, tab);
        const auto property = tab == std::string::npos ? std::string() : key.substr(tab + 1);
        counts[bucket]++;

        std::filesystem::path productPath;
        std::string module;
        const std::filesystem::path* product = nullptr;
        const auto instanceFound = index.instances.find(instance);
        if (instanceFound != index.instances.end()) {
            productPath = instanceFound->second.first;
            module = instanceFound->second.second;
            product = &productPath;
        }
        const auto blockFound = index.blocks.find({instance, property});
        const ProductBlock* block =
            blockFound == index.blocks.end() ? nullptr : &blockFound->second;

        const auto filename = sanitize(property) + "__" + sanitize(instance) + ".sv";
        const auto path = output / bucket / filename;
        util::writeAtomic(path.string(), [&](std::ostream& file) {
            file << entryText(property, instance, verdict, module, product, block, options);
        });

        std::string extra;
        if (bucket == "unclassified") extra = "status: " + verdict.status;
        else if (verdict.status == "refuted") extra = verdict.trace;
        else if (verdict.status == "unknown") {
            const auto [inProcess, escalation] = splitUnknownCause(verdict.detail);
            extra = "in-process: " + inProcess + " | escalation: " + escalation;
        }
        rows.emplace_back(bucket, property, instance,
                          relativeTo(path, options.repositoryRoot), extra);
    }

    util::writeAtomic((output / "INDEX.md").string(), [&](std::ostream& summary) {
        summary << indexText(options, buckets, counts, rows);
    });
    return counts;
}

}  // namespace environment
}  // namespace veval
