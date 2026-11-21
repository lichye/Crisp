#include "AssertionWriter.h"

#include "../Version.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string_view>
#include <sstream>
#include <thread>


namespace smart {
namespace emit {
namespace {

namespace fs = std::filesystem;

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// An overlapped implication of two Boolean expressions can be used as an
// immediate assertion after replacing its single, outermost |-> operator.
std::string immediateExpression(const std::string& assertion) {
    const std::string_view text(assertion);
    const auto reject = [&](std::string_view reason) {
        throw std::runtime_error("cannot emit unclocked assertion '" + assertion +
                                 "': " + std::string(reason));
    };
    const auto word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    };
    const auto space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c));
    };
    static const std::set<std::string_view> temporal{
        "accept_on", "and", "always", "disable", "eventually", "first_match",
        "iff", "implies", "intersect", "nexttime", "not", "or", "reject_on",
        "s_always", "s_eventually", "s_nexttime", "s_until", "s_until_with",
        "strong", "sync_accept_on", "sync_reject_on", "throughout", "until",
        "until_with", "weak", "within", "$changed", "$fell", "$past", "$rose",
        "$sampled", "$stable", "$future_gclk", "$past_gclk", "$rose_gclk",
        "$fell_gclk", "$stable_gclk", "$changed_gclk",
    };
    struct Token {
        std::string_view spelling;
        std::size_t position, depth;
        std::size_t close = std::string_view::npos;
    };
    std::vector<Token> tokens;
    std::vector<std::size_t> opens;
    for (std::size_t at = 0; at < text.size();) {
        const auto begin = at;
        const auto c = text[at++];
        if (space(c)) continue;
        if (c == '\\') {
            while (at < text.size() && !space(text[at])) ++at;
        } else if (c == '"') {
            while (at < text.size()) {
                if (text[at++] == '"') break;
                if (text[at - 1] == '\\' && at < text.size()) ++at;
            }
        } else if (word(c)) {
            while (at < text.size() && word(text[at])) ++at;
            if (temporal.count(text.substr(begin, at - begin)))
                reject("temporal operators require a clock");
        } else if (text.substr(begin, 3) == "|->") {
            at = begin + 3;
        } else if (c == '#' || c == '@' ||
                   text.substr(begin, 3) == "|=>" || text.substr(begin, 2) == "->") {
            reject("temporal operators require a clock");
        }
        if (c == '[') {
            auto next = at;
            while (next < text.size() && space(text[next])) ++next;
            auto afterPlus = next;
            if (afterPlus < text.size() && text[afterPlus] == '+') {
                ++afterPlus;
                while (afterPlus < text.size() && space(text[afterPlus])) ++afterPlus;
            }
            if (next < text.size() &&
                (text[next] == '*' || text[next] == '=' || text.substr(next, 2) == "->" ||
                 (afterPlus != next && afterPlus < text.size() && text[afterPlus] == ']')))
                reject("sequence repetition requires a clock");
        }
        if (c == ')' || c == ']' || c == '}') {
            if (opens.empty() ||
                std::string_view("([{").find(tokens[opens.back()].spelling) !=
                std::string_view(")]}").find(c))
                reject("unbalanced expression delimiters");
            tokens[opens.back()].close = tokens.size();
            opens.pop_back();
        }
        tokens.push_back({text.substr(begin, at - begin), begin, opens.size()});
        if (c == '(' || c == '[' || c == '{') opens.push_back(tokens.size() - 1);
    }
    if (!opens.empty()) reject("unbalanced expression delimiters");

    std::size_t begin = 0, end = tokens.size();
    while (begin < end && tokens[begin].spelling == "(" && tokens[begin].close == end - 1) {
        ++begin;
        --end;
    }
    std::size_t implication = end;
    for (auto i = begin; i < end; ++i) {
        if (tokens[i].spelling != "|->") continue;
        if (i == begin || i + 1 == end || tokens[i].depth != tokens[begin].depth ||
            implication != end)
            reject("only one outermost Boolean |-> is supported without a clock");
        implication = i;
    }
    if (implication == end) return assertion;

    const auto leftAt = tokens[begin].position;
    const auto leftEnd = tokens[implication - 1].position + tokens[implication - 1].spelling.size();
    const auto rightAt = tokens[implication + 1].position;
    const auto rightEnd = tokens[end - 1].position + tokens[end - 1].spelling.size();
    return "(!(" + std::string(text.substr(leftAt, leftEnd - leftAt)) +
           ") || (" + std::string(text.substr(rightAt, rightEnd - rightAt)) + "))";
}

}  // namespace

std::string ebmcCommand(const std::string& designFile, const CheckOptions& options) {
    return helper::ebmcCommand(designFile, options);
}

std::string fileDeclaringModule(const std::vector<std::string>& files,
                                const std::string& name) {
    const std::regex declaration("\\bmodule\\s+" + name + "\\b");
    for (const auto& file : files) {
        std::error_code error;
        if (!fs::exists(file, error)) continue;
        const auto source = readFile(file);
        if (std::regex_search(source, declaration)) return file;
    }
    return {};
}

std::string injectAssertions(const std::string& source, const std::string& top,
                             const std::vector<std::string>& assertions,
                             const std::string& clock, const std::string& label) {
    if (assertions.empty()) return source;

    // Find `module <top>` and then the first `endmodule` after it. Textual on
    // purpose: the user's file is re-emitted as written, so it has to be
    // edited as written (plan WP2).
    const std::regex moduleStart("\\bmodule\\s+" + top + "\\b");
    std::smatch match;
    if (!std::regex_search(source, match, moduleStart))
        throw std::runtime_error("module '" + top + "' not found in the design");

    const auto searchFrom = static_cast<std::size_t>(match.position(0));
    const auto endmodule = source.find("endmodule", searchFrom);
    if (endmodule == std::string::npos)
        throw std::runtime_error("no endmodule for module '" + top + "'");

    std::ostringstream injected;
    injected << "\n";
    for (const auto& assertion : assertions) {
        injected << "    ";
        if (clock.empty()) injected << "always_comb ";
        if (!label.empty()) injected << label << ": ";
        if (clock.empty())
            injected << "assert (" << immediateExpression(assertion) << ");\n";
        else
            injected << "assert property (@(posedge " << clock << ") " << assertion << ");\n";
    }

    return source.substr(0, endmodule) + injected.str() + source.substr(endmodule);
}

std::vector<CheckResult> checkAssertions(const std::string& designFile,
                                         const std::vector<std::string>& assertions,
                                         const CheckOptions& options) {
    std::vector<CheckResult> results(assertions.size());
    if (assertions.empty()) return results;

    fs::create_directories(options.scratchDir);
    const auto source = readFile(designFile);
    const auto label = helper::freshPropertyLabel(source, options.extraFiles);
    const auto stem = fs::path(designFile).stem().string();
    const auto extension = fs::path(designFile).extension().string();

    std::atomic<std::size_t> next{0};
    const int workers =
        std::max(1, std::min<int>(options.jobs, static_cast<int>(assertions.size())));

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (int worker = 0; worker < workers; ++worker) {
        pool.emplace_back([&] {
            for (;;) {
                const std::size_t index = next.fetch_add(1);
                if (index >= assertions.size()) return;

                CheckResult& result = results[index];
                result.assertion = assertions[index];

                const auto start = std::chrono::steady_clock::now();
                const auto candidate = options.scratchDir + "/" + stem + "_check" +
                                       std::to_string(index) + extension;
                try {
                    std::ofstream out(candidate);
                    out.exceptions(std::ios::failbit | std::ios::badbit);
                    out << injectAssertions(source,
                                  options.injectModule.empty() ? options.topModule : options.injectModule,
                                  {assertions[index]}, options.clock, label);
                    out.close();

                    const auto checked = helper::checkProperty(candidate, options, label);
                    result.status = checked.status;
                    result.detail = checked.detail;
                } catch (const std::exception& e) {
                    result.status = CheckStatus::Error;
                    result.detail = e.what();
                }

                result.seconds = std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();

                if (result.status == CheckStatus::Verified) {
                    std::error_code error;
                    fs::remove(candidate, error);
                }
            }
        });
    }
    for (auto& thread : pool) thread.join();

    return results;
}

void writeAssertionFile(const std::string& designFile, const std::string& output,
                        const std::string& top,
                        const std::vector<std::string>& assertions,
                        const std::string& configJson,
                        VerificationMode verification, int bound,
                        const std::string& clock) {
    const auto source = readFile(designFile);

    std::ostringstream header;
    header << "// Generated by SMART " << smart::version() << ": "
           << assertions.size() << " ";
    switch (verification) {
        case VerificationMode::KInduction:
            header << "k-induction-proved invariant";
            break;
        case VerificationMode::Bounded:
            header << "bounded-verified assertion";
            break;
        case VerificationMode::Unchecked:
            header << "unverified candidate assertion";
            break;
    }
    header << (assertions.size() == 1 ? "" : "s") << " added to module " << top
           << ".\n";
    if (verification == VerificationMode::Bounded)
        header << "// Holds only through bound " << bound
               << "; this output is not an invariant.\n";
    if (verification == VerificationMode::Unchecked)
        header << "// Final formal checking was disabled; prove before use.\n";
    header << "//\n"
           << "// Settings that produced this file:\n";
    std::istringstream configLines(configJson);
    std::string line;
    while (std::getline(configLines, line)) header << "//   " << line << "\n";
    header << "\n";

    const auto withAssertions = injectAssertions(source, top, assertions, clock);
    std::ofstream out(output);
    if (!out) throw std::runtime_error("cannot write " + output);
    out << header.str() << withAssertions;
}

}  // namespace emit
}  // namespace smart
