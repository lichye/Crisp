#include "Json.h"
#include "util/files/AtomicOutput.h"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace veval::result_validation {
static volatile std::sig_atomic_t stopRequested = 0;
static void requestStop(int) {
    stopRequested = 1;
}
void installCancellationHandlers() {
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
}
bool cancelled() {
    return stopRequested != 0;
}
J str(const std::string &s) {
    return json_stringt(s);
}
J num(double n) {
    if (!std::isfinite(n))
        throw std::runtime_error("nonfinite JSON number");
    std::ostringstream s;
    s << std::setprecision(17) << n;
    return json_numbert(s.str());
}
J boolean(bool b) {
    return J::json_boolean(b);
}
J finiteOrNull(double n) { return std::isfinite(n) ? num(n) : J(); }
J object(std::initializer_list<std::pair<const std::string, J>> values) {
    json_objectt j;
    for (auto &v : values)
        j[v.first] = v.second;
    return j;
}
J list(std::initializer_list<J> values) {
    json_arrayt j;
    for (auto &v : values)
        j.push_back(v);
    return j;
}
const json_arrayt &items(const J &j) {
    static json_arrayt empty;
    if (j.is_null())
        return empty;
    if (!j.is_array())
        throw std::runtime_error("expected JSON array");
    return to_json_array(j);
}
const json_objectt &members(const J &j) {
    static json_objectt empty;
    if (j.is_null())
        return empty;
    if (!j.is_object())
        throw std::runtime_error("expected JSON object");
    return to_json_object(j);
}
J &put(J &j, const std::string &key, J value) {
    auto &target = j.make_object()[key];
    target = std::move(value);
    return target;
}
void append(J &j, J value) {
    j.make_array().push_back(std::move(value));
}
std::string text(const J &j, const std::string &fallback) {
    return j.is_string() || j.is_number() ? j.value : fallback;
}
static void output(const J &j, std::ostream &s) {
    if (j.is_object()) {
        s << '{';
        bool first = true;
        for (auto &v : members(j)) {
            if (!first)
                s << ',';
            first = false;
            str(v.first).output(s);
            s << ':';
            output(v.second, s);
        }
        s << '}';
    } else if (j.is_array()) {
        s << '[';
        bool first = true;
        for (auto &v : items(j)) {
            if (!first)
                s << ',';
            first = false;
            output(v, s);
        }
        s << ']';
    } else
        j.output(s);
}
std::string canonical(const J &j) {
    std::ostringstream s;
    output(j, s);
    return s.str();
}
bool equal(const J &a, const J &b) {
    return canonical(a) == canonical(b);
}
bool truth(const J &j) {
    if (j.is_null() || j.is_false())
        return false;
    if (j.is_array())
        return !items(j).empty();
    if (j.is_object())
        return members(j).begin() != members(j).end();
    if (j.is_string())
        return !j.value.empty();
    if (j.is_number())
        return std::stod(j.value) != 0;
    return true;
}
// Frontend identity records contain multi-megabyte JSON strings. A flex
// longest-token scanner repeatedly rescans such strings while growing its
// buffer. Decode them in one forward pass while retaining CBMC's JSON value
// type. This parser is only for JSON, never for RTL or property expressions.
class Parser {
    const std::string &input;
    size_t pos = 0;
    char peek() const {
        return pos < input.size() ? input[pos] : '\0';
    }
    void fail() const {
        throw std::runtime_error("invalid JSON at byte " + std::to_string(pos));
    }
    char take() {
        if (pos >= input.size())
            fail();
        return input[pos++];
    }
    void space() {
        while (peek() == ' ' || peek() == '\r' || peek() == '\n' || peek() == '\t')
            ++pos;
    }
    void expect(char c) {
        space();
        if (take() != c)
            fail();
    }
    unsigned hex4() {
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            char c = take();
            unsigned v;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else {
                fail();
                v = 0;
            }
            result = result * 16 + v;
        }
        return result;
    }
    static void utf8(std::string &s, unsigned c) {
        if (c < 0x80)
            s += char(c);
        else if (c < 0x800) {
            s += char(0xc0 | (c >> 6));
            s += char(0x80 | (c & 63));
        } else if (c < 0x10000) {
            s += char(0xe0 | (c >> 12));
            s += char(0x80 | ((c >> 6) & 63));
            s += char(0x80 | (c & 63));
        } else {
            s += char(0xf0 | (c >> 18));
            s += char(0x80 | ((c >> 12) & 63));
            s += char(0x80 | ((c >> 6) & 63));
            s += char(0x80 | (c & 63));
        }
    }
    std::string string() {
        expect('"');
        std::string s;
        while (true) {
            if ((pos & 65535) == 0 && cancelled())
                throw Expired("record reading interrupted");
            unsigned char c = take();
            if (c == '"')
                return s;
            if (c < 0x20)
                fail();
            if (c != '\\') {
                s += char(c);
                continue;
            }
            switch (take()) {
            case '"':
                s += '"';
                break;
            case '\\':
                s += '\\';
                break;
            case '/':
                s += '/';
                break;
            case 'b':
                s += '\b';
                break;
            case 'f':
                s += '\f';
                break;
            case 'n':
                s += '\n';
                break;
            case 'r':
                s += '\r';
                break;
            case 't':
                s += '\t';
                break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (take() != '\\' || take() != 'u')
                        fail();
                    unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff)
                        fail();
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff)
                    fail();
                utf8(s, code);
                break;
            }
            default:
                fail();
            }
        }
    }
    bool digit() const {
        return peek() >= '0' && peek() <= '9';
    }
    J value(size_t depth) {
        if (depth > 512)
            throw std::runtime_error("JSON nesting exceeds 512");
        if (cancelled())
            throw Expired("record reading interrupted");
        space();
        char c = peek();
        if (c == '"')
            return str(string());
        if (c == '{') {
            ++pos;
            J result = object();
            space();
            if (peek() == '}') {
                ++pos;
                return result;
            }
            do {
                auto key = string();
                expect(':');
                if (members(result).find(key) != members(result).end())
                    fail();
                put(result, key, value(depth + 1));
                space();
                c = take();
                if (c == '}')
                    return result;
                if (c != ',')
                    fail();
            } while (true);
        }
        if (c == '[') {
            ++pos;
            J result = list();
            space();
            if (peek() == ']') {
                ++pos;
                return result;
            }
            do {
                append(result, value(depth + 1));
                space();
                c = take();
                if (c == ']')
                    return result;
                if (c != ',')
                    fail();
            } while (true);
        }
        for (auto literal : {"true", "false", "null"})
            if (input.compare(pos, std::char_traits<char>::length(literal), literal) == 0) {
                pos += std::char_traits<char>::length(literal);
                return literal[0] == 'n' ? J() : boolean(literal[0] == 't');
            }
        auto begin = pos;
        if (peek() == '-')
            ++pos;
        if (peek() == '0')
            ++pos;
        else {
            if (peek() < '1' || peek() > '9')
                fail();
            while (digit())
                ++pos;
        }
        if (peek() == '.') {
            ++pos;
            if (!digit())
                fail();
            while (digit())
                ++pos;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos;
            if (peek() == '+' || peek() == '-')
                ++pos;
            if (!digit())
                fail();
            while (digit())
                ++pos;
        }
        return json_numbert(input.substr(begin, pos - begin));
    }

  public:
    explicit Parser(const std::string &s) : input(s) {}
    J document() {
        auto result = value(0);
        space();
        if (pos != input.size())
            fail();
        return result;
    }
};
J parse(const std::string &s) {
    return Parser(s).document();
}
std::string relocatedPath(const std::string &s) {
    static const auto mappings = [] {
        std::vector<std::pair<std::string, std::string>> out;
        const char *name = std::getenv("CONSFLOW_PATH_MAP");
        if (!name || !*name) return out;
        std::ifstream in(name);
        if (!in) throw std::runtime_error("cannot read experiment path map");
        for (std::string line; std::getline(in, line);) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos || line.empty() || line.front() != '/' ||
                tab + 1 >= line.size() || line[tab + 1] != '/')
                throw std::runtime_error("invalid experiment path mapping");
            out.emplace_back(line.substr(0, tab), line.substr(tab + 1));
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
            return a.first.size() > b.first.size();
        });
        return out;
    }();
    for (const auto &m : mappings)
        if (s == m.first || (s.rfind(m.first + '/', 0) == 0))
            return m.second + s.substr(m.first.size());
    return s;
}
J relocatedPaths(J value) {
    if (value.is_string()) value.value = relocatedPath(value.value);
    else if (value.is_array())
        for (auto &v : to_json_array(value)) v = relocatedPaths(std::move(v));
    else if (value.is_object())
        for (auto &v : to_json_object(value)) v.second = relocatedPaths(std::move(v.second));
    return value;
}
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \r\n\t");
    if (a == s.npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
std::vector<std::string> words(const std::string &s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}
std::string spaces(const std::string &s) {
    std::string out;
    for (auto &w : words(s)) {
        if (!out.empty())
            out += ' ';
        out += w;
    }
    return out;
}
double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void Budget::check() const {
    if (cancelled())
        throw Expired("record reading interrupted");
    if (monotonic() >= deadline)
        throw Expired("record reading deadline exhausted");
}
double Budget::left() const {
    return std::max(0.0, deadline - monotonic());
}
std::string Files::read(const fs::path &p) {
    std::ifstream in(relocatedPath(p.string()), std::ios::binary);
    if (!in)
        throw std::runtime_error("unreadable input: " + p.string());
    std::string out;
    char block[65536];
    while (in) {
        if (budget)
            budget->check();
        in.read(block, sizeof block);
        out.append(block, size_t(in.gcount()));
    }
    if (!in.eof())
        throw std::runtime_error("input read failed: " + p.string());
    return out;
}
J Files::json(const fs::path &p) {
    auto key = fs::absolute(p).lexically_normal().string();
    auto found = documents.find(key);
    if (found != documents.end())
        return found->second;
    auto value = relocatedPaths(parse(read(p)));
    documents.emplace(key, value);
    return value;
}
J Files::ref(const fs::path &p) {
    return object({{"path", str(fs::absolute(p).lexically_normal())}});
}
J Files::identities() const {
    J out = list();
    for (auto &v : documents)
        append(out, str(v.first));
    return out;
}

std::set<std::string> Files::openedInputs(const J &stream) {
    const bool inlineText = stream["expanded_text"].is_string();
    const bool reference = stream["expanded_text_ref"].is_string();
    if (inlineText == reference ||
        (inlineText && !stream["expanded_text_ref"].is_null()) ||
        (reference && !stream["expanded_text"].is_null()))
        throw std::runtime_error("expanded input needs exactly one text or file reference");
    std::string key;
    if (reference) {
        auto path = fs::path(relocatedPath(text(stream["expanded_text_ref"])));
        if (!path.is_absolute())
            throw std::runtime_error("relative expanded frontend input");
        key = path.lexically_normal().string();
        auto found = openedInputCache.find(key);
        if (found != openedInputCache.end())
            return found->second;
    }
    std::istringstream input(reference ? read(key) : text(stream["expanded_text"]));
    static const std::regex directive(R"REGEX(^`line [0-9]+ "([^"]+)" [0-9]+)REGEX");
    std::set<std::string> opened;
    std::string line;
    size_t lines = 0;
    while (std::getline(input, line)) {
        if (budget && ++lines % 4096 == 0)
            budget->check();
        if (line.compare(0, 6, "`line ") != 0)
            continue;
        std::smatch match;
        if (std::regex_search(line, match, directive))
            opened.insert(fs::weakly_canonical(relocatedPath(match[1].str())).string());
    }
    if (reference)
        openedInputCache.emplace(key, opened);
    return opened;
}

class Stream {
    std::ifstream in;
    Budget &budget;
    size_t count = 0;

  public:
    Stream(const fs::path &path, Budget &b) : in(path, std::ios::binary), budget(b) {
        if (!in)
            throw std::runtime_error("unreadable JSON: " + path.string());
    }
    int peek() {
        while (std::isspace(in.peek()))
            get();
        return in.peek();
    }
    int get() {
        if ((++count & 65535) == 0)
            budget.check();
        return in.get();
    }
    void take(char c) {
        if (peek() != c)
            throw std::runtime_error("invalid JSON delimiter");
        get();
    }
    std::string value() {
        int first = peek();
        if (first == EOF)
            throw std::runtime_error("truncated JSON");
        std::string s;
        int depth = 0;
        bool quoted = false, escape = false;
        do {
            int c = get();
            if (c == EOF)
                throw std::runtime_error("truncated JSON value");
            s += char(c);
            if (quoted) {
                if (escape)
                    escape = false;
                else if (c == '\\')
                    escape = true;
                else if (c == '"')
                    quoted = false;
            } else if (c == '"')
                quoted = true;
            else if (c == '[' || c == '{')
                ++depth;
            else if (c == ']' || c == '}')
                --depth;
            if (depth < 0)
                throw std::runtime_error("unbalanced JSON");
            if (!quoted && depth == 0) {
                int next = in.peek();
                if (first == '{' || first == '[' || first == '"' || next == EOF || next == ',' ||
                    next == '}' || next == ']' || std::isspace(next))
                    break;
            }
        } while (true);
        return s;
    }
    void rows(const std::function<void(J &&)> &row) {
        take('[');
        if (peek() != ']') {
            while (true) {
                row(parse(value()));
                if (peek() == ']')
                    break;
                take(',');
            }
        }
        take(']');
    }
    void finish() {
        if (peek() != EOF)
            throw std::runtime_error("trailing JSON input");
    }
};
void streamObject(const fs::path &path, Budget &b,
                  const std::function<void(const std::string &, J &&)> &scalar,
                  const std::function<void(const std::string &, J &&)> &row) {
    Stream in(path, b);
    in.take('{');
    std::map<std::string, bool> seen;
    if (in.peek() != '}') {
        while (true) {
            J k = parse(in.value());
            if (!k.is_string() || seen.count(k.value))
                throw std::runtime_error("invalid/duplicate root JSON key");
            seen[k.value] = true;
            in.take(':');
            if (in.peek() == '[')
                in.rows([&](J &&v) { row(k.value, std::move(v)); });
            else
                scalar(k.value, parse(in.value()));
            if (in.peek() == '}')
                break;
            in.take(',');
        }
    }
    in.take('}');
    in.finish();
}
// Preserve every root field (including empty arrays) while changing records.
// Read and publish one row at a time: transport logs can exceed memory.
void rewriteObjectRows(const fs::path &path,
                       const std::function<void(const std::string &, J &)> &row) {
    Budget budget(std::numeric_limits<double>::infinity());
    Stream in(path,budget); in.take('{');
    util::writeAtomic(path.string(),[&](std::ostream &out){
        out<<'{'; bool firstKey=true; std::set<std::string> seen;
        if(in.peek()!='}')while(true){
            auto key=parse(in.value());
            if(!key.is_string()||!seen.insert(key.value).second)throw std::runtime_error("invalid/duplicate JSON key");
            if(!firstKey)out<<','; firstKey=false; out<<canonical(key)<<':'; in.take(':');
            if(in.peek()=='['){
                out<<'[';bool first=true;
                in.rows([&](J &&v){row(key.value,v);if(!first)out<<',';first=false;out<<canonical(v);});
                out<<']';
            }else out<<in.value();
            if(in.peek()=='}')break;in.take(',');
        }
        in.take('}');in.finish();out<<"}\n";
    });
}
void streamArray(const fs::path &path, Budget &b, const std::function<void(J &&)> &row) {
    Stream in(path, b);
    if (in.peek() == '[')
        in.rows(row);
    else
        row(parse(in.value()));
    in.finish();
}
void save(const fs::path &p, const J &j) {
    if (!p.parent_path().empty())
        fs::create_directories(p.parent_path());
    util::writeAtomic(p.string(), [&](std::ostream& out) {
        output(j, out);
        out << '\n';
    });
}
} // namespace veval::result_validation
