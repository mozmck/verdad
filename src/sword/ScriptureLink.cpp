#include "sword/ScriptureLink.h"

#include <algorithm>
#include <cctype>

namespace verdad {
namespace scripture {
namespace {

std::string trimCopy(const std::string& text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

bool equalsNoCase(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

std::string decodeBasicHtmlEntities(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i++]);
            continue;
        }

        if (i + 5 <= text.size() &&
            equalsNoCase(text[i + 1], 'a') &&
            equalsNoCase(text[i + 2], 'm') &&
            equalsNoCase(text[i + 3], 'p') &&
            text[i + 4] == ';') {
            out.push_back('&');
            i += 5;
            continue;
        }
        if (i + 4 <= text.size() &&
            equalsNoCase(text[i + 1], 'l') &&
            equalsNoCase(text[i + 2], 't') &&
            text[i + 3] == ';') {
            out.push_back('<');
            i += 4;
            continue;
        }
        if (i + 4 <= text.size() &&
            equalsNoCase(text[i + 1], 'g') &&
            equalsNoCase(text[i + 2], 't') &&
            text[i + 3] == ';') {
            out.push_back('>');
            i += 4;
            continue;
        }
        if (i + 6 <= text.size() &&
            equalsNoCase(text[i + 1], 'q') &&
            equalsNoCase(text[i + 2], 'u') &&
            equalsNoCase(text[i + 3], 'o') &&
            equalsNoCase(text[i + 4], 't') &&
            text[i + 5] == ';') {
            out.push_back('"');
            i += 6;
            continue;
        }
        if (i + 6 <= text.size() &&
            equalsNoCase(text[i + 1], 'a') &&
            equalsNoCase(text[i + 2], 'p') &&
            equalsNoCase(text[i + 3], 'o') &&
            equalsNoCase(text[i + 4], 's') &&
            text[i + 5] == ';') {
            out.push_back('\'');
            i += 6;
            continue;
        }
        if (i + 5 <= text.size() && text[i + 1] == '#' &&
            text[i + 2] == '3' && text[i + 3] == '9' && text[i + 4] == ';') {
            out.push_back('\'');
            i += 5;
            continue;
        }

        out.push_back(text[i++]);
    }

    return out;
}

bool isHexDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) ||
           (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

} // namespace

std::string urlEncode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() + 8);

    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }

    return out;
}

std::string urlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        if (text[i] == '+') {
            out.push_back(' ');
            ++i;
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size() &&
            isHexDigit(text[i + 1]) && isHexDigit(text[i + 2])) {
            int hi = hexValue(text[i + 1]);
            int lo = hexValue(text[i + 2]);
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 3;
            continue;
        }

        out.push_back(text[i++]);
    }

    return out;
}

std::string extractQueryValue(const std::string& url, const std::string& key) {
    if (key.empty()) return "";

    const std::string decoded = decodeBasicHtmlEntities(url);
    const size_t queryPos = decoded.find('?');
    const std::string query = (queryPos == std::string::npos)
        ? decoded
        : decoded.substr(queryPos + 1);
    const std::string keyLower = toLowerAscii(key);

    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string part = query.substr(
            pos, (amp == std::string::npos ? query.size() : amp) - pos);

        size_t eq = part.find('=');
        std::string name = (eq == std::string::npos) ? part : part.substr(0, eq);
        std::string value = (eq == std::string::npos) ? "" : part.substr(eq + 1);

        if (toLowerAscii(urlDecode(name)) == keyLower) {
            return urlDecode(value);
        }

        if (amp == std::string::npos) break;
        pos = amp + 1;
    }

    return "";
}

bool isReadingPlanOpenUrl(const std::string& url) {
    return toLowerAscii(url).rfind("verdad-plan://open", 0) == 0;
}

std::string readingPlanOpenUrl(const std::string& reference) {
    std::string ref = trimCopy(reference);
    if (ref.empty()) return "";
    return "verdad-plan://open?ref=" + urlEncode(ref);
}

std::string readingPlanOpenReference(const std::string& url) {
    if (!isReadingPlanOpenUrl(url)) return "";
    return extractQueryValue(url, "ref");
}

std::string firstReadingListItem(const std::string& reference) {
    std::string ref = trimCopy(reference);
    if (ref.empty()) return ref;

    size_t split = ref.find_first_of(",;");
    if (split == std::string::npos) return ref;

    std::string first = trimCopy(ref.substr(0, split));
    return first.empty() ? ref : first;
}

} // namespace scripture
} // namespace verdad
