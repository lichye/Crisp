// The single-file checker links the maintained EBMC engine directly. It runs
// the engine in a child so a time limit includes elaboration as well as solving.
#include <ebmc/build_transition_system.h>
#include <ebmc/ebmc_error.h>
#include <ebmc/format_hooks.h>
#include <ebmc/property_checker.h>
#include <langapi/mode.h>
#include <util/config.h>
#include <util/ui_message.h>
#include <verilog/verilog_language.h>
#include "util/process/CurrentExecutablePath.h"
#include "util/process/RunExternalCommand.h"
#include <algorithm>
#include <charconv>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;
namespace runner = veval::util::command_runner;

static const char* usage =
    "Usage: crisp DESIGN.sv [--top MODULE] [--budget SECONDS] [--out DIR | --log FILE.log]\n"
    "Check the assertions declared in the design. The default time limit is 60 seconds.\n"
    "Results and counterexamples are printed and saved under log/.\n";

static int checkDesign(const fs::path& source, const std::string& top) {
    std::vector<std::string> options{"crisp", "--systemverilog", "--bdd", "--trace",
                                      "-I", source.parent_path().string()};
    if (!top.empty()) options.insert(options.end(), {"--top", top});
    options.push_back(source.string());
    std::vector<const char*> args;
    for (const auto& option : options) args.push_back(option.c_str());
    cmdlinet cmdline;
    if (cmdline.parse(args.size(), args.data(), "I:(systemverilog)(bdd)(trace)(top):") || config.set(cmdline))
        throw std::runtime_error("could not initialize the SystemVerilog checker");
    register_language(new_verilog_language);
    format_hooks();
    ui_message_handlert messages(cmdline, "Crisp");
    messages.set_verbosity(messaget::M_RESULT);
    ebmc_languagest frontend(cmdline, messages);
    auto system = frontend.transition_system();
    if (!system) throw std::runtime_error("elaboration produced no transition system");
    auto properties = ebmc_propertiest::from_command_line(cmdline, *system, messages);
    properties.properties.remove_if([](const auto& property) { return property.is_exists_path(); });
    const auto isAssertion = [](const auto& property) { return !property.is_assumption(); };
    if (std::none_of(properties.properties.begin(), properties.properties.end(), isAssertion)) {
        std::cout << "No assertions found in " << source.filename().string() << '\n';
        return 3;
    }

    auto checked = property_checker(cmdline, *system, properties, messages);
    if (checked.status != property_checker_resultt::statust::VERIFICATION_RESULT) return 2;
    size_t count = 0, proved = 0, refuted = 0;
    bool error = false;
    for (const auto& property : checked.properties) {
        if (!isAssertion(property)) continue;
        ++count;
        if (property.is_proved()) ++proved;
        if (property.is_refuted() || property.status == ebmc_propertiest::propertyt::statust::REFUTED_WITH_BOUND)
            ++refuted;
        if (property.is_failure()) error = true;
    }
    std::cout << "Crisp: " << proved << '/' << count << " assertions proved, " << refuted << " refuted\n";
    if (error) return 2;
    if (refuted) return 1;
    return count && count == proved ? 0 : 3;
}

static fs::path newLog(fs::path directory, const fs::path& explicitFile) {
    if (!explicitFile.empty() && explicitFile.extension() != ".log")
        throw std::runtime_error("--log expects a .log file");
    if (!explicitFile.empty()) directory = explicitFile.parent_path();
    if (!directory.empty()) fs::create_directories(directory);
    auto name = explicitFile.empty() ? (directory / "check.XXXXXX.log").string() : explicitFile.string();
    const int fd = explicitFile.empty() ? ::mkstemps(name.data(), 4)
        : ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "create log " + name);
    ::close(fd);
    return name;
}

int main(int argc, char** argv) {
    try {
        if (argc == 1 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            std::cout << usage;
            return 0;
        }
        if (std::string(argv[1]) == "--licenses") {
            std::cout <<
#include "crisp_notices.inc"
                ;
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--run-checker") return checkDesign(argv[2], argv[3]);
        const auto self = veval::util::currentExecutable();
        const std::string command = argv[1];
        if (command == "verify" || command == "lower" || command == "check" || command == "mine") {
            const auto bundle = self.parent_path() / "components.tar.gz";
            const auto checkout = self.parent_path().parent_path();
            if (fs::is_regular_file(bundle)) {
                // The archive is part of the installed release. Component Python
                // modules, plugins and native helpers are unpacked only on demand.
                const char* launcher = R"(set -eu
bundle=$1
shift
work=$(mktemp -d /tmp/crisp-tools.XXXXXXXX)
trap 'rm -r -- "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
tar -xzf "$bundle" -C "$work"
"$work/src/cli.sh" "$@"
)";
                std::vector<const char*> args{"/bin/bash", "-c", launcher, "crisp", bundle.c_str()};
                for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
                args.push_back(nullptr);
                ::execv("/bin/bash", const_cast<char* const*>(args.data()));
            } else if (self.parent_path().filename() == "build" &&
                       fs::is_regular_file(checkout / "components/veval/build/crisp/crisp_driver")) {
                const auto script = checkout / "src/cli.sh";
                argv[0] = const_cast<char*>(script.c_str());
                ::execv(script.c_str(), argv);
            } else {
                throw std::runtime_error(command + " requires components.tar.gz next to the binary");
            }
            throw std::system_error(errno, std::generic_category(), "start component command");
        }
        fs::path source, directory, logFile;
        std::string top;
        int budget = 60;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (++i >= argc || !*argv[i]) throw std::runtime_error(arg + " needs a value");
                return argv[i];
            };
            if (arg == "--top") top = next();
            else if (arg == "--out") directory = next();
            else if (arg == "--log") logFile = next();
            else if (arg == "--budget") {
                auto value = next();
                const auto end = value.data() + value.size();
                const auto parsed = std::from_chars(value.data(), end, budget);
                if (parsed.ec != std::errc{} || parsed.ptr != end || budget <= 0)
                    throw std::runtime_error("--budget expects positive whole seconds");
            } else if (!arg.empty() && arg.front() != '-' && source.empty()) source = arg;
            else throw std::runtime_error("unexpected argument: " + arg);
        }
        if (!fs::is_regular_file(source)) throw std::runtime_error("supply an existing SystemVerilog file");
        if (!directory.empty() && !logFile.empty()) throw std::runtime_error("choose --out or --log");
        const auto log = newLog(directory.empty() ? fs::path("log") : directory, logFile);
        std::cerr << "Log: " << log.string() << '\n';
        runner::installCancellationHandlers();
        auto code = runner::command({self.string(), "--run-checker", fs::absolute(source).string(), top},
                                    log, budget, false, true);
        if (code == 124) {
            std::ofstream(log, std::ios::app) << "Verification timed out after " << budget << " s\n";
            code = 3;
        }
        std::ifstream contents(log);
        std::cout << contents.rdbuf();
        return code;
    } catch (const ebmc_errort& error) {
        std::cerr << "crisp: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "crisp: " << error.what() << '\n';
    } catch (const std::string& error) {
        std::cerr << "crisp: " << error << '\n';
    } catch (const char* error) {
        std::cerr << "crisp: " << error << '\n';
    } catch (...) {
        std::cerr << "crisp: SystemVerilog verification failed\n";
    }
    return 2;
}
