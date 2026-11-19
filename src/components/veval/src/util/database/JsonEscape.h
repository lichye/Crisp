#ifndef VEVAL_UTIL_JSON_ESCAPE_H
#define VEVAL_UTIL_JSON_ESCAPE_H

#include <string>
#include <string_view>

namespace veval::util {
// Return JSON string contents, without surrounding quotes. Preserve UTF-8
// bytes; callers supply valid text and retain responsibility for quoting.
inline std::string jsonEscape(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out += hex[ch >> 4];
                    out += hex[ch & 0x0f];
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}
} // namespace veval::util
#endif
