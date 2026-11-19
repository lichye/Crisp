// Copy a subtree of an elaborated Verilog netlist without parsing the chip.
#include "util/database/JsonEscape.h"
#include "util/files/AtomicOutput.h"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
namespace fs = std::filesystem;
using View = std::string_view;
using Offset = uint64_t;
constexpr size_t blockSize = 1024 * 1024;

void require(bool valid, const std::string &message) {
    if (!valid) throw std::runtime_error(message);
}
class Digest {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
public:
    Digest() { require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1, "cannot initialize SHA256"); }
    void add(View bytes) { require(EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) == 1, "SHA256 update failed"); }
    std::string finish() {
        unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned count = 0;
        require(EVP_DigestFinal_ex(context.get(), bytes, &count) == 1, "SHA256 finish failed");
        std::ostringstream out;
        for (unsigned i = 0; i < count; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
        return out.str();
    }
};
struct Stamp { Offset size; int64_t mtime; };
Stamp stamp(const fs::path &path) {
    struct stat value{};
    require(::stat(path.c_str(), &value) == 0 && S_ISREG(value.st_mode), "cannot read regular file: " + path.string());
#ifdef __APPLE__
    const auto time = value.st_mtimespec;
#else
    const auto time = value.st_mtim;
#endif
    return {static_cast<Offset>(value.st_size), int64_t(time.tv_sec) * 1000000000 + time.tv_nsec};
}

// No per-line tell(), regular expression or hash update; lines can cross blocks.
template<typename Line>
void lines(std::ifstream &input, Offset start, Offset size, Line line, Digest *digest = nullptr) {
    input.clear(); input.seekg(start);
    require(bool(input), "cannot seek source netlist");
    std::array<char, blockSize> buffer;
    std::string pending;
    Offset offset = start, begin = start;
    while (size) {
        const auto count = static_cast<size_t>(std::min<Offset>(size, buffer.size()));
        input.read(buffer.data(), count);
        require(input.gcount() == static_cast<std::streamsize>(count), "source truncated while reading");
        if (digest) digest->add({buffer.data(), count});
        size_t pos = 0;
        while (pos < count) {
            const auto end = static_cast<const char *>(std::memchr(buffer.data() + pos, '\n', count - pos));
            if (!end) { pending.append(buffer.data() + pos, count - pos); break; }
            const auto next = static_cast<size_t>(end - buffer.data() + 1);
            View text(buffer.data() + pos, next - pos);
            if (pending.empty()) line(text, begin, offset + next);
            else { pending.append(text); line(View(pending), begin, offset + next); pending.clear(); }
            pos = next; begin = offset + pos;
        }
        offset += count; size -= count;
    }
    if (!pending.empty()) line(View(pending), begin, offset);
}

View trim(View text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
View word(View &text) {
    text = trim(text);
    size_t n = 0;
    const bool escaped = !text.empty() && text.front() == '\\';
    while (n < text.size() && !std::isspace(static_cast<unsigned char>(text[n])) &&
           (escaped || (text[n] != '(' && text[n] != ';' && text[n] != '#'))) ++n;
    const auto result = text.substr(0, n);
    text.remove_prefix(n);
    return result;
}
std::string name(View token) {
    if (!token.empty() && token.front() == '\\') token.remove_prefix(1);
    return std::string(token);
}

class Comments {
    bool block = false;
    std::string cleaned;
public:
    View code(View text) {
        if (!block && text.find('/') == View::npos) return text;
        size_t pos = 0, copied = 0;
        cleaned.clear();
        while (pos < text.size()) {
            if (block) {
                const auto end = text.find("*/", pos);
                if (end == View::npos) return cleaned;
                block = false; pos = copied = end + 2;
                continue;
            }
            const auto next = text.find_first_of("/\"\\", pos);
            if (next == View::npos) break;
            pos = next;
            if (text[pos] == '\\') {
                while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            } else if (text[pos] == '"') {
                ++pos;
                while (pos < text.size() && text[pos] != '"') { if (text[pos] == '\\') ++pos; ++pos; }
                if (pos < text.size()) ++pos;
            } else if (text.substr(pos, 2) == "//") {
                cleaned.append(text.substr(copied, pos - copied)); return cleaned;
            } else if (text.substr(pos, 2) == "/*") {
                cleaned.append(text.substr(copied, pos - copied)); cleaned += ' ';
                block = true; pos += 2;
            } else ++pos;
        }
        if (copied == 0 && cleaned.empty()) return text;
        cleaned.append(text.substr(copied)); return cleaned;
    }
    bool unterminated() const { return block; }
};
struct Module {
    Offset start, end = 0, lineCount = 0;
    unsigned state = 0;
    bool finalNewline = true;
};

void extract(const fs::path &source, const std::string &top, const fs::path &output) {
    require(source != output && (!fs::exists(output) || !fs::equivalent(source, output)), "source and output must differ");
    const auto started = std::chrono::steady_clock::now();
    const auto original = stamp(source);
    std::ifstream input(source, std::ios::binary);
    require(bool(input), "cannot open source netlist");
    Digest sourceHash;
    std::unordered_map<std::string, Module> modules;
    Module *current = nullptr;
    Offset lineNumber = 0, firstLine = 0;
    Comments comments;
    lines(input, 0, original.size, [&](View raw, Offset begin, Offset end) {
        ++lineNumber;
        auto code = trim(comments.code(raw));
        if (code.empty()) return;
        if (code.front() == '`') throw std::runtime_error("extract-subtree requires a preprocessed generated netlist");
        if (code.front() != 'm' && code.front() != 'e') return;
        const auto keyword = word(code);
        if (keyword == "module") {
            require(!current, "nested module declaration");
            const auto module = name(word(code));
            require(!module.empty() && !trim(code).empty() && trim(code).front() == '(',
                    "expected an elaborated module header: " + module);
            const auto inserted = modules.emplace(module, Module{begin});
            require(inserted.second, "duplicate module " + module);
            current = &inserted.first->second; firstLine = lineNumber;
        } else if (keyword == "endmodule") {
            require(current, "endmodule outside module");
            current->end = end; current->lineCount = lineNumber - firstLine + 1;
            current->finalNewline = !raw.empty() && raw.back() == '\n'; current = nullptr;
        }
    }, &sourceHash);
    require(!current && !comments.unterminated(), "unterminated module or comment");
    require(modules.count(top), "missing top " + top);

    std::vector<std::string> order;
    std::function<void(const std::string &)> visit = [&](const std::string &module) {
        auto &info = modules.at(module);
        require(info.state != 1, "recursive module hierarchy " + module);
        if (info.state == 2) return;
        info.state = 1; order.push_back(module);
        std::vector<std::string> children;
        std::unordered_set<std::string> seen;
        Comments local;
        lines(input, info.start, info.end - info.start, [&](View raw, Offset, Offset) {
            auto code = trim(local.code(raw));
            const auto type = name(word(code));
            if (!modules.count(type)) return;
            const auto instance = word(code);
            require(!instance.empty() && instance.front() != '#' && trim(code) == "(",
                    "expected a generated instance header in " + module + ": " + std::string(raw));
            if (seen.insert(type).second) children.push_back(type);
        });
        for (const auto &child : children) visit(child);
        info.state = 2;
    };
    visit(top);
    fs::create_directories(output.parent_path());
    Digest outputHash;
    Offset moduleLines = 0, outputLines = 0;
    veval::util::writeAtomic(output.string(), [&](std::ostream &out) {
        std::array<char, blockSize> buffer;
        for (const auto &module : order) {
            const auto &info = modules.at(module);
            moduleLines += info.lineCount;
            outputLines += info.lineCount + (info.finalNewline ? 1 : 0);
            input.clear(); input.seekg(info.start);
            for (Offset remaining = info.end - info.start; remaining;) {
                const auto count = static_cast<size_t>(std::min<Offset>(remaining, buffer.size()));
                input.read(buffer.data(), count);
                require(input.gcount() == static_cast<std::streamsize>(count), "source truncated while copying");
                out.write(buffer.data(), count); outputHash.add({buffer.data(), count}); remaining -= count;
            }
            out.put('\n'); outputHash.add("\n");
        }
        const auto after = stamp(source);
        require(after.size == original.size && after.mtime == original.mtime, "source changed during extraction");
    });
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    auto quote = [](const std::string &s) { return '"' + veval::util::jsonEscape(s) + '"'; };
    std::ostringstream result;
    result << "{\"source\":" << quote(source.string()) << ",\"top\":" << quote(top)
           << ",\"output\":" << quote(output.string()) << ",\"modules\":[";
    for (size_t i = 0; i < order.size(); ++i) result << (i ? "," : "") << quote(order[i]);
    result << "],\"module_count\":" << order.size() << ",\"module_lines\":" << moduleLines
           << ",\"output_lines\":" << outputLines << ",\"sha256\":" << quote(outputHash.finish())
           << ",\"source_sha256\":" << quote(sourceHash.finish()) << ",\"source_bytes\":" << original.size
           << ",\"source_mtime_ns\":" << original.mtime << ",\"extraction_s\":" << elapsed << "}\n";
    veval::util::writeAtomic(output.string() + ".json", [&](std::ostream &out) { out << result.str(); });
    std::cout << result.str();
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: veval_extract_subtree SOURCE TOP OUTPUT");
        extract(fs::canonical(argv[1]), argv[2], fs::weakly_canonical(fs::absolute(argv[3])));
        return 0;
    } catch (const std::exception &e) { std::cerr << "veval_extract_subtree: " << e.what() << '\n'; return 1; }
}
