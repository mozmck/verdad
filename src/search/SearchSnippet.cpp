#include "search/SearchSnippet.h"

#include <algorithm>
#include <cctype>

namespace verdad::search_snippet {
namespace {

struct MaskedText {
    std::string text;
    std::vector<bool> mask;
};

struct SnippetSlice {
    size_t left = 0;
    size_t right = 0;
    bool leadingEllipsis = false;
    bool trailingEllipsis = false;
};

bool isWordByte(unsigned char c) {
    return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
}

bool isSpaceByte(unsigned char c) {
    return std::isspace(c);
}

MaskedText collapseWhitespaceWithMask(std::string_view text,
                                      const std::vector<bool>& mask,
                                      size_t left,
                                      size_t right) {
    MaskedText collapsed;
    right = std::min(right, text.size());
    if (left > right) left = right;
    collapsed.text.reserve(right - left);
    collapsed.mask.reserve(right - left);

    bool lastWasSpace = true;
    for (size_t i = left; i < right; ++i) {
        unsigned char uc = static_cast<unsigned char>(text[i]);
        if (isSpaceByte(uc)) {
            if (!lastWasSpace) {
                collapsed.text.push_back(' ');
                collapsed.mask.push_back(false);
                lastWasSpace = true;
            }
            continue;
        }

        collapsed.text.push_back(text[i]);
        collapsed.mask.push_back(i < mask.size() ? mask[i] : false);
        lastWasSpace = false;
    }

    while (!collapsed.text.empty() && collapsed.text.back() == ' ') {
        collapsed.text.pop_back();
        collapsed.mask.pop_back();
    }

    return collapsed;
}

bool findFirstHitRun(std::string_view text,
                     const std::vector<bool>& mask,
                     size_t& startOut,
                     size_t& endOut) {
    const size_t limit = std::min(text.size(), mask.size());
    for (size_t i = 0; i < limit; ++i) {
        if (!mask[i]) continue;
        size_t end = i + 1;
        while (end < limit && mask[end]) {
            ++end;
        }
        startOut = i;
        endOut = end;
        return true;
    }

    return false;
}

size_t expandLeftToWordStart(std::string_view text, size_t pos) {
    while (pos > 0 && isWordByte(static_cast<unsigned char>(text[pos - 1]))) {
        --pos;
    }
    return pos;
}

size_t expandRightToWordEnd(std::string_view text, size_t pos) {
    while (pos < text.size() &&
           isWordByte(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return pos;
}

size_t moveLeftByWords(std::string_view text, size_t pos, size_t wordCount) {
    size_t left = pos;
    for (size_t words = 0; words < wordCount && left > 0; ++words) {
        while (left > 0 &&
               !isWordByte(static_cast<unsigned char>(text[left - 1]))) {
            --left;
        }
        if (left == 0) break;
        while (left > 0 &&
               isWordByte(static_cast<unsigned char>(text[left - 1]))) {
            --left;
        }
    }
    return left;
}

size_t moveRightByWords(std::string_view text, size_t pos, size_t wordCount) {
    size_t right = pos;
    for (size_t words = 0; words < wordCount && right < text.size(); ++words) {
        while (right < text.size() &&
               !isWordByte(static_cast<unsigned char>(text[right]))) {
            ++right;
        }
        if (right >= text.size()) break;
        while (right < text.size() &&
               isWordByte(static_cast<unsigned char>(text[right]))) {
            ++right;
        }
    }
    return right;
}

size_t includeTrailingPunctuation(std::string_view text, size_t pos) {
    while (pos < text.size() &&
           !isWordByte(static_cast<unsigned char>(text[pos])) &&
           !isSpaceByte(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return pos;
}

SnippetSlice sliceAroundHit(std::string_view text,
                            size_t hitStart,
                            size_t hitEnd,
                            size_t contextWords) {
    const size_t hitWordStart = expandLeftToWordStart(text, hitStart);
    const size_t hitWordEnd = expandRightToWordEnd(text, hitEnd);

    size_t left = moveLeftByWords(text, hitWordStart, contextWords);
    size_t right = moveRightByWords(text, hitWordEnd, contextWords);
    right = includeTrailingPunctuation(text, right);

    const bool leadingEllipsis = left > 0;
    const bool trailingEllipsis = right < text.size();

    while (left < hitWordStart && left < right &&
           isSpaceByte(static_cast<unsigned char>(text[left]))) {
        ++left;
    }
    while (right > hitWordEnd && right > left &&
           isSpaceByte(static_cast<unsigned char>(text[right - 1]))) {
        --right;
    }

    return {left, right, leadingEllipsis, trailingEllipsis};
}

size_t prefixEndAfterWords(std::string_view text, size_t maxWords) {
    if (maxWords == 0) return 0;

    size_t pos = 0;
    size_t words = 0;
    while (pos < text.size()) {
        while (pos < text.size() &&
               !isWordByte(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        if (pos >= text.size()) return text.size();

        while (pos < text.size() &&
               isWordByte(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        ++words;

        if (words == maxWords) {
            pos = includeTrailingPunctuation(text, pos);
            while (pos > 0 &&
                   isSpaceByte(static_cast<unsigned char>(text[pos - 1]))) {
                --pos;
            }
            return pos;
        }
    }

    return text.size();
}

std::string buildFromMask(std::string_view text,
                          const std::vector<bool>& mask,
                          bool highlighted,
                          size_t contextWords) {
    if (text.empty()) return "";

    size_t hitStart = 0;
    size_t hitEnd = 0;
    if (!findFirstHitRun(text, mask, hitStart, hitEnd)) {
        return truncateWords(text);
    }

    SnippetSlice slice = sliceAroundHit(text, hitStart, hitEnd, contextWords);
    if (slice.left >= slice.right) return truncateWords(text);

    MaskedText collapsed = collapseWhitespaceWithMask(text, mask,
                                                     slice.left, slice.right);
    if (collapsed.text.empty()) return truncateWords(text);

    std::string out;
    out.reserve(collapsed.text.size() + 48);
    if (slice.leadingEllipsis) out += "... ";

    bool inHighlight = false;
    for (size_t i = 0; i < collapsed.text.size(); ++i) {
        bool shouldHighlight = highlighted &&
            i < collapsed.mask.size() && collapsed.mask[i];
        if (shouldHighlight && !inHighlight) {
            out += "<span class=\"searchhit\">";
            inHighlight = true;
        } else if (!shouldHighlight && inHighlight) {
            out += "</span>";
            inHighlight = false;
        }
        out.push_back(collapsed.text[i]);
    }

    if (inHighlight) out += "</span>";
    if (slice.trailingEllipsis) out += " ...";
    return out;
}

} // namespace

std::string collapseWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    bool lastWasSpace = true;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (isSpaceByte(uc)) {
            if (!lastWasSpace) {
                out.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }

        out.push_back(c);
        lastWasSpace = false;
    }

    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string truncateWords(std::string_view text, size_t maxWords) {
    if (text.empty()) return "";

    size_t end = prefixEndAfterWords(text, maxWords);
    std::string out = collapseWhitespace(text.substr(0, end));
    if (out.empty()) return "";
    if (end >= text.size()) return out;

    if (!out.empty()) out += " ...";
    return out;
}

std::string buildPlainText(std::string_view text,
                           const std::vector<bool>& mask,
                           size_t contextWords) {
    return buildFromMask(text, mask, false, contextWords);
}

std::string buildHighlightedMarkup(std::string_view text,
                                   const std::vector<bool>& mask,
                                   size_t contextWords) {
    return buildFromMask(text, mask, true, contextWords);
}

} // namespace verdad::search_snippet
