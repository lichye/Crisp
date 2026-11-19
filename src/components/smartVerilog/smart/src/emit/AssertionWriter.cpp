#include "AssertionWriter.h"

#include "../Version.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
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
                             const std::string& label) {
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
        if (!label.empty()) injected << label << ": ";
        injected << "assert property (" << assertion << ");\n";
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
                                  {assertions[index]}, label);
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
                        VerificationMode verification, int bound) {
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

    std::ofstream out(output);
    if (!out) throw std::runtime_error("cannot write " + output);
    out << header.str() << injectAssertions(source, top, assertions);
}

}  // namespace emit
}  // namespace smart
