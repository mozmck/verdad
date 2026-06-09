#include "translation/TranslationNormalization.h"

#include <FL/fl_utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

namespace verdad {
namespace {

bool isBoundarySpace(unsigned codepoint) {
    if (codepoint <= 0x7f) {
        return std::isspace(static_cast<unsigned char>(codepoint)) != 0;
    }
    return codepoint == 0x00a0 || codepoint == 0x2007 ||
           codepoint == 0x202f;
}

bool isBoundaryPunctuation(unsigned codepoint) {
    if (codepoint <= 0x7f) {
        return std::ispunct(static_cast<unsigned char>(codepoint)) != 0;
    }

    switch (codepoint) {
        case 0x00a1: // inverted exclamation mark
        case 0x00ab:
        case 0x00bb:
        case 0x00bf: // inverted question mark
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014:
        case 0x2015:
        case 0x2018:
        case 0x2019:
        case 0x201a:
        case 0x201b:
        case 0x201c:
        case 0x201d:
        case 0x201e:
        case 0x201f:
        case 0x2026:
        case 0x2039:
        case 0x203a:
            return true;
        default:
            return false;
    }
}

unsigned composeLatin(unsigned base, unsigned mark) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(base) << 32) | mark;
    switch (key) {
        case (static_cast<std::uint64_t>('A') << 32) | 0x0300: return 0x00c0;
        case (static_cast<std::uint64_t>('A') << 32) | 0x0301: return 0x00c1;
        case (static_cast<std::uint64_t>('A') << 32) | 0x0302: return 0x00c2;
        case (static_cast<std::uint64_t>('A') << 32) | 0x0303: return 0x00c3;
        case (static_cast<std::uint64_t>('A') << 32) | 0x0308: return 0x00c4;
        case (static_cast<std::uint64_t>('C') << 32) | 0x0327: return 0x00c7;
        case (static_cast<std::uint64_t>('E') << 32) | 0x0300: return 0x00c8;
        case (static_cast<std::uint64_t>('E') << 32) | 0x0301: return 0x00c9;
        case (static_cast<std::uint64_t>('E') << 32) | 0x0302: return 0x00ca;
        case (static_cast<std::uint64_t>('E') << 32) | 0x0308: return 0x00cb;
        case (static_cast<std::uint64_t>('I') << 32) | 0x0300: return 0x00cc;
        case (static_cast<std::uint64_t>('I') << 32) | 0x0301: return 0x00cd;
        case (static_cast<std::uint64_t>('I') << 32) | 0x0302: return 0x00ce;
        case (static_cast<std::uint64_t>('I') << 32) | 0x0308: return 0x00cf;
        case (static_cast<std::uint64_t>('N') << 32) | 0x0303: return 0x00d1;
        case (static_cast<std::uint64_t>('O') << 32) | 0x0300: return 0x00d2;
        case (static_cast<std::uint64_t>('O') << 32) | 0x0301: return 0x00d3;
        case (static_cast<std::uint64_t>('O') << 32) | 0x0302: return 0x00d4;
        case (static_cast<std::uint64_t>('O') << 32) | 0x0303: return 0x00d5;
        case (static_cast<std::uint64_t>('O') << 32) | 0x0308: return 0x00d6;
        case (static_cast<std::uint64_t>('U') << 32) | 0x0300: return 0x00d9;
        case (static_cast<std::uint64_t>('U') << 32) | 0x0301: return 0x00da;
        case (static_cast<std::uint64_t>('U') << 32) | 0x0302: return 0x00db;
        case (static_cast<std::uint64_t>('U') << 32) | 0x0308: return 0x00dc;
        case (static_cast<std::uint64_t>('Y') << 32) | 0x0301: return 0x00dd;
        case (static_cast<std::uint64_t>('a') << 32) | 0x0300: return 0x00e0;
        case (static_cast<std::uint64_t>('a') << 32) | 0x0301: return 0x00e1;
        case (static_cast<std::uint64_t>('a') << 32) | 0x0302: return 0x00e2;
        case (static_cast<std::uint64_t>('a') << 32) | 0x0303: return 0x00e3;
        case (static_cast<std::uint64_t>('a') << 32) | 0x0308: return 0x00e4;
        case (static_cast<std::uint64_t>('c') << 32) | 0x0327: return 0x00e7;
        case (static_cast<std::uint64_t>('e') << 32) | 0x0300: return 0x00e8;
        case (static_cast<std::uint64_t>('e') << 32) | 0x0301: return 0x00e9;
        case (static_cast<std::uint64_t>('e') << 32) | 0x0302: return 0x00ea;
        case (static_cast<std::uint64_t>('e') << 32) | 0x0308: return 0x00eb;
        case (static_cast<std::uint64_t>('i') << 32) | 0x0300: return 0x00ec;
        case (static_cast<std::uint64_t>('i') << 32) | 0x0301: return 0x00ed;
        case (static_cast<std::uint64_t>('i') << 32) | 0x0302: return 0x00ee;
        case (static_cast<std::uint64_t>('i') << 32) | 0x0308: return 0x00ef;
        case (static_cast<std::uint64_t>('n') << 32) | 0x0303: return 0x00f1;
        case (static_cast<std::uint64_t>('o') << 32) | 0x0300: return 0x00f2;
        case (static_cast<std::uint64_t>('o') << 32) | 0x0301: return 0x00f3;
        case (static_cast<std::uint64_t>('o') << 32) | 0x0302: return 0x00f4;
        case (static_cast<std::uint64_t>('o') << 32) | 0x0303: return 0x00f5;
        case (static_cast<std::uint64_t>('o') << 32) | 0x0308: return 0x00f6;
        case (static_cast<std::uint64_t>('u') << 32) | 0x0300: return 0x00f9;
        case (static_cast<std::uint64_t>('u') << 32) | 0x0301: return 0x00fa;
        case (static_cast<std::uint64_t>('u') << 32) | 0x0302: return 0x00fb;
        case (static_cast<std::uint64_t>('u') << 32) | 0x0308: return 0x00fc;
        case (static_cast<std::uint64_t>('y') << 32) | 0x0301: return 0x00fd;
        case (static_cast<std::uint64_t>('y') << 32) | 0x0308: return 0x00ff;
        default: return 0;
    }
}

std::vector<unsigned> decodeUtf8(const std::string& text) {
    std::vector<unsigned> codepoints;
    const char* current = text.data();
    const char* end = current + text.size();
    while (current < end) {
        int length = 0;
        unsigned codepoint = fl_utf8decode(current, end, &length);
        if (length <= 0) length = 1;
        codepoints.push_back(codepoint);
        current += length;
    }
    return codepoints;
}

std::string encodeUtf8(const std::vector<unsigned>& codepoints) {
    std::string result;
    result.reserve(codepoints.size() * 2);
    for (unsigned codepoint : codepoints) {
        char encoded[5] = {};
        int length = fl_utf8encode(codepoint, encoded);
        if (length > 0) result.append(encoded, static_cast<size_t>(length));
    }
    return result;
}

} // namespace

std::string normalizeLanguageCode(const std::string& language) {
    size_t start = 0;
    while (start < language.size() &&
           std::isspace(static_cast<unsigned char>(language[start]))) {
        ++start;
    }
    size_t end = language.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(language[end - 1]))) {
        --end;
    }

    std::string code = language.substr(start, end - start);
    size_t separator = code.find_first_of("-_");
    if (separator != std::string::npos) code.resize(separator);
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return code;
}

std::string lowercaseUtf8(const std::string& text) {
    if (text.empty()) return "";

    std::string lowered(text.size() * 4 + 4, '\0');
    int loweredLength = fl_utf_tolower(
        reinterpret_cast<const unsigned char*>(text.data()),
        static_cast<int>(text.size()),
        lowered.data());
    if (loweredLength <= 0) return text;

    lowered.resize(static_cast<size_t>(loweredLength));
    return lowered;
}

std::string normalizeLookupToken(const std::string& token) {
    std::vector<unsigned> codepoints = decodeUtf8(token);
    size_t start = 0;
    while (start < codepoints.size() &&
           (isBoundarySpace(codepoints[start]) ||
            isBoundaryPunctuation(codepoints[start]))) {
        ++start;
    }
    size_t end = codepoints.size();
    while (end > start &&
           (isBoundarySpace(codepoints[end - 1]) ||
            isBoundaryPunctuation(codepoints[end - 1]))) {
        --end;
    }

    std::vector<unsigned> normalized;
    normalized.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        if (!normalized.empty()) {
            unsigned composed = composeLatin(normalized.back(), codepoints[i]);
            if (composed != 0) {
                normalized.back() = composed;
                continue;
            }
        }
        normalized.push_back(codepoints[i]);
    }

    return lowercaseUtf8(encodeUtf8(normalized));
}

} // namespace verdad
