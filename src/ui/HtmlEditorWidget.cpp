#include "ui/HtmlEditorWidget.h"

#include "ui/UiFontUtils.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Device.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <regex>
#include <sstream>
#include <string_view>

namespace verdad {
namespace {

constexpr int kToolbarH = 30;
constexpr int kButtonW = 34;
constexpr int kWideButtonW = 42;
constexpr int kSizeChoiceW = 72;
constexpr int kButtonGap = 2;
constexpr int kTextDisplayTopMargin = 1;
constexpr int kTextDisplayBottomMargin = 1;
constexpr int kTextDisplayLeftMargin = 3;
constexpr int kTextDisplayRightMargin = 3;
constexpr unsigned char kAlignLeft = 0;
constexpr unsigned char kAlignCenter = 1;
constexpr unsigned char kDefaultSizeLevel = 3;
constexpr std::array<int, 7> kSizeOffsets = {{-6, -4, -2, 0, 2, 4, 8}};

using CharFormat = HtmlEditorWidget::CharFormat;
using Snapshot = HtmlEditorWidget::Snapshot;

enum class HtmlExportFlavor {
    Standard,
    Odt,
};

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

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

std::string normalizeFontVariantFamily(const char* name, bool stripRegularSuffixes) {
    std::string lower = toLowerAscii(name ? name : "");
    static constexpr std::string_view styleSuffixes[] = {
        " bold italic",
        " bold oblique",
        " demi bold italic",
        " demi bold oblique",
        " italic",
        " oblique",
        " bold",
        " demi bold",
    };
    static constexpr std::string_view regularSuffixes[] = {
        " regular",
        " medium",
        " book",
        " roman",
    };

    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (std::string_view suffix : styleSuffixes) {
            if (endsWith(lower, suffix)) {
                lower.erase(lower.size() - suffix.size());
                lower = trimCopy(lower);
                stripped = true;
                break;
            }
        }
        if (!stripped && stripRegularSuffixes) {
            for (std::string_view suffix : regularSuffixes) {
                if (endsWith(lower, suffix)) {
                    lower.erase(lower.size() - suffix.size());
                    lower = trimCopy(lower);
                    stripped = true;
                    break;
                }
            }
        }
    }
    return trimCopy(lower);
}

Fl_Font findFontVariant(Fl_Font baseFont, int targetAttrs, Fl_Font fallback) {
    int baseAttrs = 0;
    const char* baseName = Fl::get_font_name(baseFont, &baseAttrs);
    if (!baseName || !baseName[0]) return fallback;

    Fl_Font count = Fl::set_fonts("-*");
    for (bool stripRegularSuffixes : {false, true}) {
        std::string familyKey = normalizeFontVariantFamily(baseName, stripRegularSuffixes);
        for (Fl_Font f = 0; f < count; ++f) {
            int attrs = 0;
            const char* name = Fl::get_font_name(f, &attrs);
            if (!name || !name[0] || attrs != targetAttrs) continue;
            if (normalizeFontVariantFamily(name, stripRegularSuffixes) == familyKey) {
                return f;
            }
        }
    }
    return fallback;
}

struct ListPrefixInfo {
    bool valid = false;
    bool ordered = false;
    int indentLen = 0;
    int markerLen = 0;
    int prefixLen = 0;
    int number = 0;
    std::string indent;
};

struct HtmlListState {
    bool ordered = false;
    int nextNumber = 1;
};

enum class OrderedListStyle {
    Decimal,
    LowerAlpha,
    LowerRoman,
};

bool isIndentChar(char c) {
    return c == ' ' || c == '\t';
}

std::pair<std::string, std::vector<CharFormat>> stripDisplayPadding(
    const std::string& text,
    const std::vector<CharFormat>& formats) {
    std::string strippedText;
    std::vector<CharFormat> strippedFormats;
    strippedText.reserve(text.size());
    strippedFormats.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        CharFormat fmt = (i < formats.size()) ? formats[i] : CharFormat{};
        if (fmt.displayPad) continue;
        fmt.displayPad = false;
        strippedText.push_back(text[i]);
        strippedFormats.push_back(fmt);
    }
    return {std::move(strippedText), std::move(strippedFormats)};
}

int leadingIndentLength(std::string_view text) {
    int len = 0;
    while (len < static_cast<int>(text.size()) && isIndentChar(text[static_cast<size_t>(len)])) {
        ++len;
    }
    return len;
}

int indentColumns(std::string_view text) {
    int columns = 0;
    for (char c : text) {
        columns += (c == '\t') ? 4 : 1;
    }
    return columns;
}

bool usesTabsOnly(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c == '\t'; });
}

std::string normalizeIndentPrefix(std::string_view text, int spacesPerTab) {
    std::string normalized;
    int pendingSpaces = 0;
    const int tabSize = std::max(1, spacesPerTab);
    for (char c : text) {
        if (c == '\t') {
            pendingSpaces = 0;
            normalized.push_back('\t');
            continue;
        }
        if (c != ' ') break;
        ++pendingSpaces;
        if (pendingSpaces >= tabSize) {
            normalized.push_back('\t');
            pendingSpaces = 0;
        }
    }
    normalized.append(static_cast<size_t>(pendingSpaces), ' ');
    return normalized;
}

int listIndentLevel(std::string_view indentText, int indentWidth) {
    std::string normalized = normalizeIndentPrefix(indentText, indentWidth);
    return static_cast<int>(std::count(normalized.begin(), normalized.end(), '\t'));
}

std::string residualListIndentText(std::string_view indentText, int indentWidth) {
    std::string normalized = normalizeIndentPrefix(indentText, indentWidth);
    size_t level = static_cast<size_t>(std::count(normalized.begin(), normalized.end(), '\t'));
    if (level >= normalized.size()) return "";
    return normalized.substr(level);
}

int trailingIndentUnitLength(std::string_view indentText, int indentWidth) {
    if (indentText.empty()) return 0;
    if (indentText.back() == '\t') return 1;

    int removable = 0;
    for (int i = static_cast<int>(indentText.size()) - 1;
         i >= 0 && removable < indentWidth && indentText[static_cast<size_t>(i)] == ' ';
         --i) {
        ++removable;
    }
    return removable;
}

OrderedListStyle orderedListStyleForLevel(int level) {
    switch (level % 3) {
    case 1:
        return OrderedListStyle::LowerAlpha;
    case 2:
        return OrderedListStyle::LowerRoman;
    default:
        return OrderedListStyle::Decimal;
    }
}

std::string toLowerAlpha(int value) {
    if (value <= 0) return "a";

    std::string text;
    while (value > 0) {
        --value;
        text.insert(text.begin(), static_cast<char>('a' + (value % 26)));
        value /= 26;
    }
    return text;
}

std::string toLowerRoman(int value) {
    if (value <= 0) return "i";

    struct RomanDigit {
        int value;
        const char* text;
    };

    static constexpr RomanDigit digits[] = {
        {1000, "m"},
        {900, "cm"},
        {500, "d"},
        {400, "cd"},
        {100, "c"},
        {90, "xc"},
        {50, "l"},
        {40, "xl"},
        {10, "x"},
        {9, "ix"},
        {5, "v"},
        {4, "iv"},
        {1, "i"},
    };

    std::string text;
    for (const auto& digit : digits) {
        while (value >= digit.value) {
            text += digit.text;
            value -= digit.value;
        }
    }
    return text;
}

std::string formatOrderedListMarker(int number, int level) {
    number = std::max(1, number);
    switch (orderedListStyleForLevel(level)) {
    case OrderedListStyle::LowerAlpha:
        return toLowerAlpha(number);
    case OrderedListStyle::LowerRoman:
        return toLowerRoman(number);
    case OrderedListStyle::Decimal:
    default:
        return std::to_string(number);
    }
}

bool parseListPrefix(std::string_view line, ListPrefixInfo* info = nullptr) {
    ListPrefixInfo parsed;
    parsed.indentLen = leadingIndentLength(line);
    parsed.indent = std::string(line.substr(0, static_cast<size_t>(parsed.indentLen)));

    size_t markerPos = static_cast<size_t>(parsed.indentLen);
    if (markerPos >= line.size()) {
        if (info) *info = parsed;
        return false;
    }

    if ((line[markerPos] == '-' || line[markerPos] == '*') &&
        markerPos + 1 < line.size() && line[markerPos + 1] == ' ') {
        parsed.valid = true;
        parsed.ordered = false;
        parsed.markerLen = 2;
        parsed.prefixLen = parsed.indentLen + parsed.markerLen;
        if (info) *info = parsed;
        return true;
    }

    size_t digitPos = markerPos;
    while (digitPos < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[digitPos]))) {
        ++digitPos;
    }
    if (digitPos > markerPos &&
        digitPos + 1 < line.size() &&
        line[digitPos] == '.' &&
        line[digitPos + 1] == ' ') {
        parsed.valid = true;
        parsed.ordered = true;
        parsed.markerLen = static_cast<int>(digitPos - markerPos + 2);
        parsed.prefixLen = parsed.indentLen + parsed.markerLen;
        parsed.number = std::atoi(std::string(line.substr(markerPos, digitPos - markerPos)).c_str());
        if (info) *info = parsed;
        return true;
    }

    size_t alphaPos = markerPos;
    while (alphaPos < line.size() &&
           std::islower(static_cast<unsigned char>(line[alphaPos]))) {
        ++alphaPos;
    }
    if (parsed.indentLen > 0 &&
        alphaPos > markerPos &&
        alphaPos - markerPos <= 4 &&
        alphaPos + 1 < line.size() &&
        line[alphaPos] == '.' &&
        line[alphaPos + 1] == ' ') {
        parsed.valid = true;
        parsed.ordered = true;
        parsed.markerLen = static_cast<int>(alphaPos - markerPos + 2);
        parsed.prefixLen = parsed.indentLen + parsed.markerLen;
        parsed.number = 1;
        if (info) *info = parsed;
        return true;
    }

    if (info) *info = parsed;
    return false;
}

std::string escapeHtml(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string decodeEntities(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }

        size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos) {
            out.push_back(text[i]);
            continue;
        }

        std::string entity = text.substr(i, semi - i + 1);
        if (entity == "&amp;") out.push_back('&');
        else if (entity == "&lt;") out.push_back('<');
        else if (entity == "&gt;") out.push_back('>');
        else if (entity == "&quot;") out.push_back('"');
        else if (entity == "&apos;") out.push_back('\'');
        else if (entity == "&nbsp;") out.push_back(' ');
        else {
            out += entity;
        }
        i = semi;
    }

    return out;
}

unsigned char detectSizeLevel(const std::string& lowerTag) {
    static constexpr std::string_view key = "data-verdad-size=\"";
    size_t start = lowerTag.find(key);
    if (start != std::string::npos) {
        start += key.size();
        size_t end = lowerTag.find('"', start);
        std::string value = lowerTag.substr(start, end - start);
        if (value == "large") return static_cast<unsigned char>(kDefaultSizeLevel + 1);
        if (value == "small") return static_cast<unsigned char>(kDefaultSizeLevel - 1);
        try {
            int level = std::stoi(value);
            return static_cast<unsigned char>(
                std::clamp(level, 0, static_cast<int>(kSizeOffsets.size()) - 1));
        } catch (...) {
        }
    }

    if (lowerTag.find("font-size: 115%") != std::string::npos ||
        lowerTag.find("font-size:115%") != std::string::npos ||
        lowerTag.find("font-size: 1.15em") != std::string::npos ||
        lowerTag.find("font-size:1.15em") != std::string::npos) {
        return static_cast<unsigned char>(kDefaultSizeLevel + 1);
    }
    if (lowerTag.find("<small") != std::string::npos ||
        lowerTag.find("font-size: 85%") != std::string::npos ||
        lowerTag.find("font-size:85%") != std::string::npos) {
        return static_cast<unsigned char>(kDefaultSizeLevel - 1);
    }
    return kDefaultSizeLevel;
}

int detectListIndent(const std::string& lowerTag) {
    static constexpr std::string_view key = "data-verdad-indent=\"";
    size_t start = lowerTag.find(key);
    if (start == std::string::npos) return 0;
    start += key.size();

    size_t end = start;
    while (end < lowerTag.size() &&
           std::isdigit(static_cast<unsigned char>(lowerTag[end]))) {
        ++end;
    }
    if (end == start) return 0;

    try {
        return std::max(0, std::stoi(lowerTag.substr(start, end - start)));
    } catch (...) {
        return 0;
    }
}

int detectListIndentTabs(const std::string& lowerTag) {
    static constexpr std::string_view key = "data-verdad-indent-tabs=\"";
    size_t start = lowerTag.find(key);
    if (start == std::string::npos) return 0;
    start += key.size();

    size_t end = start;
    while (end < lowerTag.size() &&
           std::isdigit(static_cast<unsigned char>(lowerTag[end]))) {
        ++end;
    }
    if (end == start) return 0;

    try {
        return std::max(0, std::stoi(lowerTag.substr(start, end - start)));
    } catch (...) {
        return 0;
    }
}

std::string explicitIndentText(const std::string& lowerTag) {
    int indentTabs = detectListIndentTabs(lowerTag);
    if (indentTabs > 0) {
        return std::string(static_cast<size_t>(indentTabs), '\t');
    }

    int indentSpaces = detectListIndent(lowerTag);
    if (indentSpaces > 0) {
        return std::string(static_cast<size_t>(indentSpaces), ' ');
    }

    return "";
}

int detectListLevel(const std::string& lowerTag) {
    static constexpr std::string_view key = "data-verdad-list-level=\"";
    size_t start = lowerTag.find(key);
    if (start == std::string::npos) return 0;
    start += key.size();

    size_t end = start;
    while (end < lowerTag.size() &&
           std::isdigit(static_cast<unsigned char>(lowerTag[end]))) {
        ++end;
    }
    if (end == start) return 0;

    try {
        return std::max(0, std::stoi(lowerTag.substr(start, end - start)));
    } catch (...) {
        return 0;
    }
}

unsigned char detectAlignment(const std::string& lowerTag) {
    if (lowerTag.find("data-verdad-align=\"center\"") != std::string::npos ||
        lowerTag.find("text-align:center") != std::string::npos ||
        lowerTag.find("text-align: center") != std::string::npos) {
        return kAlignCenter;
    }
    return kAlignLeft;
}

int fontSizeForLevel(int baseSize, unsigned char level) {
    int index = std::clamp<int>(level, 0, static_cast<int>(kSizeOffsets.size()) - 1);
    return std::clamp(baseSize + kSizeOffsets[static_cast<size_t>(index)], 8, 48);
}

std::string inlineStyleForBlock(int indentColumns, unsigned char align) {
    std::ostringstream style;
    style << "white-space: pre-wrap;";
    if (indentColumns > 0) {
        style << " margin-left: " << (indentColumns * 0.5) << "em;";
    }
    if (align == kAlignCenter) {
        style << " text-align: center;";
    }
    return style.str();
}

std::string blockAttributeString(std::string_view indentText,
                                 int indentColumns,
                                 unsigned char align) {
    std::string attrs;
    if (!indentText.empty() && indentColumns > 0) {
        if (usesTabsOnly(indentText)) {
            attrs += " data-verdad-indent-tabs=\"" +
                     std::to_string(static_cast<int>(indentText.size())) + "\"";
        } else {
            attrs += " data-verdad-indent=\"" + std::to_string(indentColumns) + "\"";
        }
    }
    if (align == kAlignCenter) {
        attrs += " data-verdad-align=\"center\"";
    }
    attrs += " style=\"" + inlineStyleForBlock(indentColumns, align) + "\"";
    return attrs;
}

std::string tagNameFor(const std::string& rawTag) {
    size_t start = 0;
    while (start < rawTag.size() &&
           std::isspace(static_cast<unsigned char>(rawTag[start]))) {
        ++start;
    }
    if (start < rawTag.size() && rawTag[start] == '/') ++start;
    size_t end = start;
    while (end < rawTag.size() &&
           !std::isspace(static_cast<unsigned char>(rawTag[end])) &&
           rawTag[end] != '/') {
        ++end;
    }
    return toLowerAscii(rawTag.substr(start, end - start));
}

void appendFormattedText(std::string& textOut,
                         std::vector<CharFormat>& formatsOut,
                         const std::string& text,
                         const CharFormat& format) {
    textOut += text;
    formatsOut.insert(formatsOut.end(), text.size(), format);
}

void appendParagraphBreak(std::string& textOut,
                          std::vector<CharFormat>& formatsOut) {
    if (textOut.empty()) return;
    if (startsWith(textOut.substr(textOut.size() >= 2 ? textOut.size() - 2 : 0), "\n\n")) {
        return;
    }
    if (!textOut.empty() && textOut.back() == '\n') {
        appendFormattedText(textOut, formatsOut, "\n", {});
        return;
    }
    appendFormattedText(textOut, formatsOut, "\n\n", {});
}

void appendLineBreak(std::string& textOut,
                     std::vector<CharFormat>& formatsOut) {
    if (!textOut.empty() && textOut.back() == '\n') return;
    appendFormattedText(textOut, formatsOut, "\n", {});
}

void trimTrailingNewlines(std::string& textOut,
                          std::vector<CharFormat>& formatsOut) {
    while (!textOut.empty() && textOut.back() == '\n') {
        textOut.pop_back();
        if (!formatsOut.empty()) formatsOut.pop_back();
    }
}

void parseHtmlToEditorContent(const std::string& html,
                              std::string& textOut,
                              std::vector<CharFormat>& formatsOut) {
    textOut.clear();
    formatsOut.clear();

    CharFormat current;
    std::vector<unsigned char> sizeStack(1, kDefaultSizeLevel);
    std::vector<unsigned char> alignStack(1, kAlignLeft);
    int boldDepth = 0;
    int italicDepth = 0;
    std::vector<HtmlListState> listStack;

    auto refreshFormat = [&]() {
        current.bold = (boldDepth > 0);
        current.italic = (italicDepth > 0);
        current.size = sizeStack.empty() ? kDefaultSizeLevel : sizeStack.back();
        current.align = alignStack.empty() ? kAlignLeft : alignStack.back();
    };

    auto appendPrefixIfNeeded = [&](const std::string& prefix) {
        if (!textOut.empty() && textOut.back() != '\n') {
            appendLineBreak(textOut, formatsOut);
        }
        appendFormattedText(textOut, formatsOut, prefix, current);
    };

    for (size_t i = 0; i < html.size();) {
        if (html[i] != '<') {
            size_t next = html.find('<', i);
            std::string chunk = decodeEntities(html.substr(i, next - i));
            appendFormattedText(textOut, formatsOut, chunk, current);
            if (next == std::string::npos) break;
            i = next;
            continue;
        }

        size_t close = html.find('>', i + 1);
        if (close == std::string::npos) break;

        std::string rawTag = html.substr(i + 1, close - i - 1);
        std::string lowerTag = toLowerAscii(rawTag);
        bool closing = false;
        size_t first = 0;
        while (first < rawTag.size() &&
               std::isspace(static_cast<unsigned char>(rawTag[first]))) {
            ++first;
        }
        if (first < rawTag.size() && rawTag[first] == '/') closing = true;

        std::string tag = tagNameFor(rawTag);
        bool selfClosing = (!rawTag.empty() && rawTag.back() == '/') ||
                           tag == "br" || tag == "hr";

        if (!closing) {
            if (tag == "p" || tag == "div") {
                if (!textOut.empty()) appendParagraphBreak(textOut, formatsOut);
                alignStack.push_back(detectAlignment(lowerTag));
                refreshFormat();
                std::string indent = explicitIndentText(lowerTag);
                if (!indent.empty()) {
                    appendFormattedText(textOut, formatsOut, indent, current);
                }
            } else if (tag == "br") {
                appendLineBreak(textOut, formatsOut);
            } else if (tag == "hr") {
                appendParagraphBreak(textOut, formatsOut);
                appendFormattedText(textOut, formatsOut, "---", current);
                appendParagraphBreak(textOut, formatsOut);
            } else if (tag == "ul") {
                if (listStack.empty() && !textOut.empty()) {
                    appendParagraphBreak(textOut, formatsOut);
                }
                listStack.push_back({false, 1});
            } else if (tag == "ol") {
                if (listStack.empty() && !textOut.empty()) {
                    appendParagraphBreak(textOut, formatsOut);
                }
                listStack.push_back({true, 1});
            } else if (tag == "li") {
                alignStack.push_back(detectAlignment(lowerTag));
                refreshFormat();
                std::string explicitIndent = explicitIndentText(lowerTag);
                std::string indent;
                int listLevel = detectListLevel(lowerTag);
                if (listLevel > 0) {
                    indent.assign(static_cast<size_t>(listLevel), '\t');
                    indent += explicitIndent;
                } else if (!explicitIndent.empty()) {
                    indent = explicitIndent;
                } else if (!listStack.empty()) {
                    indent.assign(listStack.size(), '\t');
                }

                bool ordered = !listStack.empty() && listStack.back().ordered;
                if (ordered) {
                    int number = listStack.back().nextNumber++;
                    int listLevel = detectListLevel(lowerTag);
                    if (listLevel <= 0 && !listStack.empty()) {
                        listLevel = static_cast<int>(listStack.size());
                    }
                    appendPrefixIfNeeded(indent +
                                         formatOrderedListMarker(number, std::max(0, listLevel - 1)) +
                                         ". ");
                } else {
                    appendPrefixIfNeeded(indent + "- ");
                }
            } else if (tag == "strong" || tag == "b") {
                ++boldDepth;
                refreshFormat();
            } else if (tag == "em" || tag == "i") {
                ++italicDepth;
                refreshFormat();
            } else if (tag == "small") {
                sizeStack.push_back(static_cast<unsigned char>(kDefaultSizeLevel - 1));
                refreshFormat();
            } else if (tag == "span") {
                sizeStack.push_back(detectSizeLevel(lowerTag));
                refreshFormat();
            }
        } else {
            if (tag == "p" || tag == "div") {
                appendParagraphBreak(textOut, formatsOut);
                if (alignStack.size() > 1) alignStack.pop_back();
                refreshFormat();
            } else if (tag == "ul") {
                if (!listStack.empty()) listStack.pop_back();
                if (listStack.empty()) appendParagraphBreak(textOut, formatsOut);
            } else if (tag == "ol") {
                if (!listStack.empty()) listStack.pop_back();
                if (listStack.empty()) appendParagraphBreak(textOut, formatsOut);
            } else if (tag == "li") {
                appendLineBreak(textOut, formatsOut);
                if (alignStack.size() > 1) alignStack.pop_back();
                refreshFormat();
            } else if (tag == "strong" || tag == "b") {
                boldDepth = std::max(0, boldDepth - 1);
                refreshFormat();
            } else if (tag == "em" || tag == "i") {
                italicDepth = std::max(0, italicDepth - 1);
                refreshFormat();
            } else if (tag == "small" || tag == "span") {
                if (sizeStack.size() > 1) sizeStack.pop_back();
                refreshFormat();
            }
        }

        if (selfClosing && tag == "li") {
            appendLineBreak(textOut, formatsOut);
        }

        i = close + 1;
    }

    trimTrailingNewlines(textOut, formatsOut);
    if (formatsOut.size() > textOut.size()) {
        formatsOut.resize(textOut.size());
    }
}

std::vector<std::pair<int, int>> verseReferenceRanges(const std::string& text) {
    static const std::regex refRe(
        R"(((?:[1-3]\s+)?[A-Za-z]+(?:\s+[A-Za-z]+)*\s+\d+:\d+(?:-\d+)?))");

    std::vector<std::pair<int, int>> ranges;
    auto begin = std::sregex_iterator(text.begin(), text.end(), refRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string candidate = (*it)[1].str();
        try {
            auto ref = SwordManager::parseVerseRef(candidate);
            if (!ref.book.empty() && ref.chapter > 0 && ref.verse > 0) {
                int pos = static_cast<int>((*it).position(1));
                int len = static_cast<int>((*it).length(1));
                ranges.emplace_back(pos, pos + len);
            }
        } catch (...) {
        }
    }
    return ranges;
}

std::string verseReferenceAtPosition(const std::string& text,
                                     int pos,
                                     int* startOut = nullptr,
                                     int* endOut = nullptr) {
    if (startOut) *startOut = 0;
    if (endOut) *endOut = 0;
    if (text.empty()) return "";

    pos = std::clamp(pos, 0, static_cast<int>(text.size()));
    for (const auto& range : verseReferenceRanges(text)) {
        bool inside = (pos >= range.first && pos < range.second);
        bool onRightEdge = (pos > 0 && (pos - 1) >= range.first && (pos - 1) < range.second);
        if (!inside && !onRightEdge) continue;
        if (startOut) *startOut = range.first;
        if (endOut) *endOut = range.second;
        return text.substr(static_cast<size_t>(range.first),
                           static_cast<size_t>(range.second - range.first));
    }
    return "";
}

std::string escapeAndAutoLink(const std::string& text) {
    static const std::regex refRe(
        R"(((?:[1-3]\s+)?[A-Za-z]+(?:\s+[A-Za-z]+)*\s+\d+:\d+(?:-\d+)?))");

    std::string out;
    size_t cursor = 0;
    auto begin = std::sregex_iterator(text.begin(), text.end(), refRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        size_t pos = static_cast<size_t>((*it).position(1));
        size_t len = static_cast<size_t>((*it).length(1));
        std::string candidate = (*it)[1].str();

        bool valid = false;
        try {
            auto ref = SwordManager::parseVerseRef(candidate);
            valid = !ref.book.empty() && ref.chapter > 0 && ref.verse > 0;
        } catch (...) {
            valid = false;
        }

        if (!valid) continue;

        out += escapeHtml(text.substr(cursor, pos - cursor));
        out += "<a href=\"sword://" + escapeHtml(candidate) + "\">" +
               escapeHtml(candidate) + "</a>";
        cursor = pos + len;
    }
    out += escapeHtml(text.substr(cursor));
    return out;
}

bool isRuleLine(const std::string& line) {
    std::string trimmed = trimCopy(line);
    if (trimmed.size() < 3) return false;
    return std::all_of(trimmed.begin(), trimmed.end(),
                       [](char c) { return c == '-'; });
}

bool isBulletLine(const std::string& line,
                  int* prefixLen = nullptr,
                  int* indentColumnsOut = nullptr) {
    ListPrefixInfo info;
    if (!parseListPrefix(line, &info) || info.ordered) return false;
    if (prefixLen) *prefixLen = info.prefixLen;
    if (indentColumnsOut) *indentColumnsOut = indentColumns(info.indent);
    return true;
}

bool isOrderedLine(const std::string& line,
                   int* prefixLen = nullptr,
                   int* indentColumnsOut = nullptr,
                   int* numberOut = nullptr) {
    ListPrefixInfo info;
    if (!parseListPrefix(line, &info) || !info.ordered) return false;
    if (prefixLen) *prefixLen = info.prefixLen;
    if (indentColumnsOut) *indentColumnsOut = indentColumns(info.indent);
    if (numberOut) *numberOut = info.number;
    return true;
}

bool isListLine(const std::string& line, ListPrefixInfo* info = nullptr) {
    return parseListPrefix(line, info);
}

std::string listItemOpenTag(std::string_view indentText,
                            int indentColumns,
                            unsigned char align,
                            int listLevel,
                            HtmlExportFlavor flavor) {
    if (flavor == HtmlExportFlavor::Odt) {
        // LibreOffice imports li-level block styling as list-position changes.
        // Keep ODT list items plain and let the nested list structure set indent.
        return "<li>";
    }

    std::string attrs;
    if (listLevel > 0) {
        attrs += " data-verdad-list-level=\"" + std::to_string(listLevel) + "\"";
    }
    std::string blockAttrs = blockAttributeString(indentText, indentColumns, align);
    if (!blockAttrs.empty()) attrs += blockAttrs;
    return "<li" + attrs + ">";
}

std::string odtListItemContentOpenTag() {
    // Writer keeps list siblings aligned when each item's text lives in a
    // zero-margin block, but applying alignment here reintroduces bad indents.
    return "<div style=\"margin: 0;\">";
}

std::string orderedListOpenTag(int level) {
    switch (level % 3) {
    case 1:
        return "<ol type=\"a\" style=\"list-style-type: lower-alpha;\">";
    case 2:
        return "<ol type=\"i\" style=\"list-style-type: lower-roman;\">";
    default:
        return "<ol type=\"1\" style=\"list-style-type: decimal;\">";
    }
}

std::string orderedListPrefix(std::string_view indentText, int number, int level) {
    return std::string(indentText) + formatOrderedListMarker(number, level) + ". ";
}

std::string odtIndentTextHtml(std::string_view indentText) {
    std::string html;
    for (char c : indentText) {
        if (c == '\t') {
            html += "&#9;";
        } else if (c == ' ') {
            html += "&nbsp;";
        } else {
            switch (c) {
            case '&': html += "&amp;"; break;
            case '<': html += "&lt;"; break;
            case '>': html += "&gt;"; break;
            case '"': html += "&quot;"; break;
            default: html.push_back(c); break;
            }
        }
    }
    return html;
}

std::string odtParagraphAttributeString(unsigned char align,
                                        double marginLeftEm = 0.0,
                                        double textIndentEm = 0.0) {
    std::string attrs;
    if (align == kAlignCenter) {
        attrs += " align=\"center\"";
    }
    std::ostringstream style;
    style << "white-space: pre-wrap;";
    if (marginLeftEm > 0.0) {
        style << " margin-left: " << marginLeftEm << "em;";
    }
    if (textIndentEm != 0.0) {
        style << " text-indent: " << textIndentEm << "em;";
    }
    if (align == kAlignCenter) {
        style << " text-align: center;";
    }
    attrs += " style=\"" + style.str() + "\"";
    return attrs;
}

std::string wrapStyledText(const std::string& html,
                          const CharFormat& fmt,
                          int baseSize) {
    std::string out = html;
    if (fmt.size != kDefaultSizeLevel) {
        out = "<span data-verdad-size=\"" +
              std::to_string(static_cast<int>(fmt.size)) +
              "\" style=\"font-size: " +
              std::to_string(fontSizeForLevel(baseSize, fmt.size)) +
              "pt;\">" + out + "</span>";
    }
    if (fmt.italic) out = "<em>" + out + "</em>";
    if (fmt.bold) out = "<strong>" + out + "</strong>";
    return out;
}

CharFormat formatAt(const std::vector<CharFormat>& formats, int index) {
    if (index < 0 || index >= static_cast<int>(formats.size())) {
        return {};
    }
    return formats[static_cast<size_t>(index)];
}

std::string serializeInlineRange(const std::string& text,
                                 const std::vector<CharFormat>& formats,
                                 int start,
                                 int end,
                                 int baseSize) {
    if (start >= end || start < 0 || end > static_cast<int>(text.size())) {
        return "";
    }

    std::string html;
    int runStart = start;
    CharFormat current = formatAt(formats, runStart);

    for (int i = start; i <= end; ++i) {
        bool boundary = (i == end);
        if (!boundary && formatAt(formats, i) == current) {
            continue;
        }

        std::string chunk = text.substr(runStart, i - runStart);
        html += wrapStyledText(escapeAndAutoLink(chunk), current, baseSize);
        runStart = i;
        if (i < end) {
            current = formatAt(formats, i);
        }
    }

    return html;
}

struct BlockFormat {
    std::string indentText;
    int indentColumns = 0;
    unsigned char align = kAlignLeft;

    bool operator==(const BlockFormat& other) const {
        return indentText == other.indentText &&
               indentColumns == other.indentColumns &&
               align == other.align;
    }
};

struct EditorLine {
    int start = 0;
    int end = 0;
    std::string text;
};

BlockFormat blockFormatForLine(int start,
                               int end,
                               const std::string& lineText,
                               const std::vector<CharFormat>& formats) {
    BlockFormat block;
    int indentLen = leadingIndentLength(lineText);
    block.indentText = lineText.substr(0, static_cast<size_t>(indentLen));
    block.indentColumns = indentColumns(block.indentText);
    for (int i = start; i < end; ++i) {
        if (!std::isspace(static_cast<unsigned char>(
                lineText[static_cast<size_t>(i - start)]))) {
            block.align = formatAt(formats, i).align;
            return block;
        }
    }
    if (start < end) {
        block.align = formatAt(formats, start).align;
    }
    return block;
}

struct SerializedListLine {
    ListPrefixInfo prefix;
    int level = 0;
    std::string residualIndentText;
    int residualIndentColumns = 0;
    unsigned char align = kAlignLeft;
};

SerializedListLine buildSerializedListLine(const EditorLine& line,
                                          const std::vector<CharFormat>& formats,
                                          int indentWidth) {
    SerializedListLine serialized;
    parseListPrefix(line.text, &serialized.prefix);
    serialized.level = listIndentLevel(serialized.prefix.indent, indentWidth);
    serialized.residualIndentText = residualListIndentText(serialized.prefix.indent, indentWidth);
    serialized.residualIndentColumns = indentColumns(serialized.residualIndentText);
    serialized.align = blockFormatForLine(line.start, line.end, line.text, formats).align;
    return serialized;
}

std::string serializeListRun(const std::vector<EditorLine>& lines,
                             size_t& index,
                             const std::string& text,
                             const std::vector<CharFormat>& formats,
                             int baseSize,
                             int indentWidth,
                             HtmlExportFlavor flavor) {
    std::string html;
    std::vector<bool> listStack;
    int baseLevel = -1;
    int currentLevel = -1;
    bool currentItemOpen = false;

    auto openContainer = [&](bool ordered, int level) {
        html += ordered ? orderedListOpenTag(level) : "<ul>";
        listStack.push_back(ordered);
    };

    auto closeContainer = [&]() {
        if (listStack.empty()) return;
        html += listStack.back() ? "</ol>" : "</ul>";
        listStack.pop_back();
    };

    auto closeCurrentItem = [&]() {
        if (!currentItemOpen) return;
        html += "</li>";
        currentItemOpen = false;
    };

    size_t cursor = index;
    while (cursor < lines.size()) {
        ListPrefixInfo info;
        if (!isListLine(lines[cursor].text, &info)) break;

        SerializedListLine line = buildSerializedListLine(lines[cursor], formats, indentWidth);
        if (baseLevel < 0) {
            baseLevel = line.level;
        }

        int level = std::max(0, line.level - baseLevel);
        if (currentLevel < 0) {
            level = 0;
        } else {
            level = std::max(0, std::min(level, currentLevel + 1));
        }

        if (currentLevel < 0) {
            openContainer(line.prefix.ordered, level);
        } else if (level > currentLevel) {
            openContainer(line.prefix.ordered, level);
        } else {
            closeCurrentItem();
            while (currentLevel > level) {
                closeContainer();
                --currentLevel;
                html += "</li>";
            }
            if (listStack.empty()) {
                openContainer(line.prefix.ordered, level);
            } else if (listStack.back() != line.prefix.ordered) {
                closeContainer();
                openContainer(line.prefix.ordered, level);
            }
        }

        currentLevel = level;
        html += listItemOpenTag(line.residualIndentText,
                                line.residualIndentColumns,
                                line.align,
                                level + 1,
                                flavor);
        if (flavor == HtmlExportFlavor::Odt) {
            html += odtListItemContentOpenTag();
        }
        if (flavor == HtmlExportFlavor::Odt &&
            !line.residualIndentText.empty()) {
            html += odtIndentTextHtml(line.residualIndentText);
        }
        std::string itemHtml = serializeInlineRange(text, formats,
                                                    lines[cursor].start + line.prefix.prefixLen,
                                                    lines[cursor].end,
                                                    baseSize);
        html += itemHtml.empty() ? "&nbsp;" : itemHtml;
        if (flavor == HtmlExportFlavor::Odt) {
            html += "</div>";
        }
        currentItemOpen = true;
        ++cursor;
    }

    closeCurrentItem();
    while (currentLevel > 0) {
        closeContainer();
        --currentLevel;
        html += "</li>";
    }
    while (!listStack.empty()) {
        closeContainer();
    }

    index = cursor;
    return html;
}

std::string serializeEditorContent(const std::string& text,
                                   const std::vector<CharFormat>& formats,
                                   int baseSize,
                                   HtmlExportFlavor flavor,
                                   int indentWidth) {
    if (text.empty()) return "";

    std::vector<EditorLine> lines;
    int start = 0;
    for (int i = 0; i <= static_cast<int>(text.size()); ++i) {
        if (i == static_cast<int>(text.size()) || text[i] == '\n') {
            lines.push_back({start, i, text.substr(start, i - start)});
            start = i + 1;
        }
    }

    std::string html;
    size_t i = 0;
    while (i < lines.size()) {
        std::string trimmed = trimCopy(lines[i].text);
        if (trimmed.empty()) {
            ++i;
            continue;
        }

        if (isRuleLine(lines[i].text)) {
            html += "<hr />";
            ++i;
            continue;
        }

        if (isListLine(lines[i].text)) {
            html += serializeListRun(lines, i, text, formats, baseSize, indentWidth, flavor);
            continue;
        }

        BlockFormat block = blockFormatForLine(lines[i].start,
                                               lines[i].end,
                                               lines[i].text,
                                               formats);
        if (flavor == HtmlExportFlavor::Odt) {
            html += "<p" + odtParagraphAttributeString(block.align) + ">";
        } else {
            html += "<p" + blockAttributeString(block.indentText,
                                                block.indentColumns,
                                                block.align) + ">";
        }
        bool firstLine = true;
        while (i < lines.size()) {
            trimmed = trimCopy(lines[i].text);
            if (trimmed.empty() ||
                isRuleLine(lines[i].text) ||
                isListLine(lines[i].text)) {
                break;
            }

            if (!(block == blockFormatForLine(lines[i].start,
                                              lines[i].end,
                                              lines[i].text,
                                              formats))) {
                break;
            }

            int indentLen = leadingIndentLength(lines[i].text);

            if (!firstLine) html += "<br />";
            if (flavor == HtmlExportFlavor::Odt) {
                if (firstLine && indentLen > 0) {
                    std::string normalizedIndent = normalizeIndentPrefix(
                        std::string_view(lines[i].text.data(), static_cast<size_t>(indentLen)),
                        indentWidth);
                    html += odtIndentTextHtml(normalizedIndent);
                }
                html += serializeInlineRange(text, formats,
                                             lines[i].start + indentLen,
                                             lines[i].end,
                                             baseSize);
            } else {
                html += serializeInlineRange(text, formats,
                                             lines[i].start + indentLen,
                                             lines[i].end,
                                             baseSize);
            }
            firstLine = false;
            ++i;
        }
        html += "</p>";
    }

    return html;
}

std::string selectionText(Fl_Text_Buffer* buffer) {
    if (!buffer) return "";
    char* text = buffer->selection_text();
    if (!text) return "";
    std::string copied = text;
    std::free(text);
    return copied;
}

bool setButtonDown(Fl_Button* button, bool down) {
    if (!button) return false;
    if (down) button->value(1);
    else button->value(0);
    return down;
}

bool isUtf8ContinuationByte(unsigned char c) {
    return (c & 0xC0U) == 0x80U;
}

} // namespace

int nextUtf8Boundary(std::string_view text, int pos) {
    pos = std::clamp(pos, 0, static_cast<int>(text.size()));
    if (pos >= static_cast<int>(text.size())) return static_cast<int>(text.size());
    ++pos;
    while (pos < static_cast<int>(text.size()) &&
           isUtf8ContinuationByte(static_cast<unsigned char>(text[static_cast<size_t>(pos)]))) {
        ++pos;
    }
    return pos;
}

int prevUtf8Boundary(std::string_view text, int pos) {
    pos = std::clamp(pos, 0, static_cast<int>(text.size()));
    if (pos <= 0) return 0;
    --pos;
    while (pos > 0 &&
           isUtf8ContinuationByte(static_cast<unsigned char>(text[static_cast<size_t>(pos)]))) {
        --pos;
    }
    return pos;
}

bool isPrintableUtf8Text(const char* text, int len) {
    if (!text || len <= 0) return false;
    const char* p = text;
    const char* end = text + len;
    while (p < end) {
        int charLen = 0;
        unsigned ucs = fl_utf8decode(p, end, &charLen);
        if (charLen <= 0) break;
        if (ucs >= 32U || ucs == '\t') return true;
        p += charLen;
    }
    return false;
}

class HtmlEditorTextArea : public Fl_Group {
public:
    HtmlEditorTextArea(HtmlEditorWidget* owner, int X, int Y, int W, int H)
        : Fl_Group(X, Y, W, H)
        , owner_(owner) {
        box(FL_DOWN_FRAME);
        color(FL_BACKGROUND2_COLOR);
        begin();
        vscroll_ = new Fl_Scrollbar(0, 0, 1, 1);
        vscroll_->type(FL_VERTICAL);
        vscroll_->callback(scrollbar_cb, this);
        vscroll_->clear_visible_focus();
        vscroll_->hide();
        end();
    }

    int handle(int event) override;
    void draw() override;
    void resize(int X, int Y, int W, int H) override;

    void buffer(Fl_Text_Buffer* buffer);
    void highlight_data(Fl_Text_Buffer*, void*, int, char, void*, void*) { invalidateLayout(); }
    void wrap_mode(int, int) {}
    void textfont(Fl_Font) { invalidateLayout(); }
    void textsize(int) { invalidateLayout(); }
    void cursor_style(int) {}
    void tab_nav(int) {}

    int insert_position() const { return insertPos_; }
    void insert_position(int pos);
    void show_insert_position();
    void redisplay_range(int, int) { invalidateLayout(); }

    int textAreaWidth() const { return textAreaW_; }
    void setLineAdvance(int pixels);
    void invalidateLayout();

private:
    struct Glyph {
        int start = 0;
        int end = 0;
        int x = 0;
        int width = 0;
        int ascent = 0;
        int descent = 0;
        bool whitespace = false;
        bool link = false;
        bool rule = false;
    };

    struct VisualLine {
        int start = 0;
        int end = 0;
        int y = 0;
        int advance = 0;
        int ascent = 0;
        int descent = 0;
        int contentWidth = 0;
        int drawX = 0;
        std::vector<Glyph> glyphs;
    };

    struct CaretInfo {
        int lineIndex = 0;
        int x = 0;
        int top = 0;
        int height = 0;
    };

    struct MeasuredGlyph {
        int nextPos = 0;
        int width = 0;
        int ascent = 0;
        int descent = 0;
        bool whitespace = false;
        bool link = false;
        bool rule = false;
    };

    static void scrollbar_cb(Fl_Widget* widget, void* data);

    void ensureLayout() const;
    void rebuildLayout(int contentWidth) const;
    void drawSelection() const;
    void drawText() const;
    void drawCaret() const;

    int scrollY() const;
    void setScrollY(int top);
    void scrollBy(int delta);

    CaretInfo caretInfoForPosition(int pos) const;
    int positionForPoint(int px, int py) const;
    int positionForLineX(int lineIndex, int targetX) const;
    int lineIndexForPosition(int pos) const;
    int clampPosition(int pos) const;
    std::string currentText() const;
    MeasuredGlyph measureGlyph(std::string_view text, int pos, bool ruleLine) const;
    int lineAdvanceForHeights(int maxHeight) const;

    void moveCursor(int pos, bool extendSelection);
    void showContextMenuForPosition(int pos, int screenX, int screenY);
    void setSelection(int start, int end);
    void clearSelection();
    bool selectionRange(int& start, int& end) const;
    void selectAll();

    void insertText(const char* text, int len);
    void deleteRange(int start, int end);
    void deleteBackward();
    void deleteForward();
    bool handleKeyEvent();

    mutable bool layoutValid_ = false;
    mutable std::string cachedText_;
    mutable std::vector<VisualLine> lines_;
    mutable int contentHeight_ = 0;
    mutable int textAreaX_ = 0;
    mutable int textAreaY_ = 0;
    mutable int textAreaW_ = 0;
    mutable int textAreaH_ = 0;
    mutable int requestedLineAdvance_ = 0;
    int insertPos_ = 0;
    int anchorPos_ = 0;
    int preferredX_ = -1;
    bool dragging_ = false;
    Fl_Text_Buffer* buffer_ = nullptr;
    Fl_Scrollbar* vscroll_ = nullptr;
    HtmlEditorWidget* owner_;
};

int HtmlEditorTextArea::handle(int event) {
    if (!owner_) return Fl_Group::handle(event);

    switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
        redraw();
        owner_->syncToolbarState();
        return 1;
    case FL_PUSH: {
        if (vscroll_ && vscroll_->visible() && Fl::event_inside(vscroll_)) {
            return Fl_Group::handle(event);
        }
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
            take_focus();
            int pos = positionForPoint(Fl::event_x(), Fl::event_y());
            int selStart = 0;
            int selEnd = 0;
            bool insideSelection =
                selectionRange(selStart, selEnd) &&
                ((pos >= selStart && pos < selEnd) ||
                 (pos > 0 && (pos - 1) >= selStart && (pos - 1) < selEnd));
            if (!insideSelection) {
                moveCursor(pos, false);
            }
            preferredX_ = -1;
            showContextMenuForPosition(pos, Fl::event_x(), Fl::event_y());
            owner_->syncToolbarState();
            return 1;
        }
        if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
        take_focus();
        int pos = positionForPoint(Fl::event_x(), Fl::event_y());
        const bool shift = (Fl::event_state() & FL_SHIFT) != 0;
        int clicks = Fl::event_clicks();
        if (clicks >= 2 && buffer_) {
            int start = buffer_->line_start(pos);
            int end = buffer_->line_end(pos);
            anchorPos_ = start;
            insertPos_ = end;
            setSelection(start, end);
        } else if (clicks == 1 && buffer_) {
            int start = buffer_->word_start(pos);
            int end = buffer_->word_end(pos);
            anchorPos_ = start;
            insertPos_ = end;
            setSelection(start, end);
        } else {
            moveCursor(pos, shift);
        }
        preferredX_ = -1;
        dragging_ = true;
        owner_->syncToolbarState();
        return 1;
    }
    case FL_DRAG:
        if (!dragging_) {
            return Fl_Group::handle(event);
        }
        moveCursor(positionForPoint(Fl::event_x(), Fl::event_y()), true);
        preferredX_ = -1;
        owner_->syncToolbarState();
        return 1;
    case FL_RELEASE:
        if (!dragging_) {
            return Fl_Group::handle(event);
        }
        dragging_ = false;
        owner_->syncToolbarState();
        return 1;
    case FL_MOUSEWHEEL:
        if (vscroll_ && vscroll_->visible() && Fl::event_inside(vscroll_)) {
            return Fl_Group::handle(event);
        }
        scrollBy(Fl::event_dy() * std::max(24, owner_->textSize_));
        return 1;
    case FL_KEYBOARD:
    case FL_SHORTCUT:
        if (handleKeyEvent()) {
            owner_->syncToolbarState();
            return 1;
        }
        return Fl_Group::handle(event);
    case FL_PASTE:
        if (Fl::event_length() > 0) {
            insertText(Fl::event_text(), Fl::event_length());
            owner_->syncToolbarState();
            return 1;
        }
        return Fl_Group::handle(event);
    default:
        return Fl_Group::handle(event);
    }
}

void HtmlEditorTextArea::draw() {
    draw_box(box(), color());

    ensureLayout();

    fl_push_clip(textAreaX_, textAreaY_, textAreaW_, textAreaH_);
    fl_color(FL_BACKGROUND2_COLOR);
    fl_rectf(textAreaX_, textAreaY_, textAreaW_, textAreaH_);
    drawSelection();
    drawText();
    drawCaret();
    fl_pop_clip();

    if (vscroll_ && vscroll_->visible()) {
        draw_child(*vscroll_);
    }
}

void HtmlEditorTextArea::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);
    invalidateLayout();
}

void HtmlEditorTextArea::buffer(Fl_Text_Buffer* buffer) {
    buffer_ = buffer;
    insertPos_ = 0;
    anchorPos_ = 0;
    preferredX_ = -1;
    invalidateLayout();
}

void HtmlEditorTextArea::insert_position(int pos) {
    moveCursor(pos, false);
}

void HtmlEditorTextArea::show_insert_position() {
    ensureLayout();
    CaretInfo caret = caretInfoForPosition(insertPos_);
    int top = caret.top;
    int bottom = caret.top + caret.height;
    if (top < scrollY()) {
        setScrollY(top);
    } else if (bottom > scrollY() + textAreaH_) {
        setScrollY(bottom - textAreaH_);
    }
}

void HtmlEditorTextArea::setLineAdvance(int pixels) {
    requestedLineAdvance_ = std::max(1, pixels);
    invalidateLayout();
}

void HtmlEditorTextArea::invalidateLayout() {
    layoutValid_ = false;
    redraw();
}

void HtmlEditorTextArea::scrollbar_cb(Fl_Widget*, void* data) {
    auto* self = static_cast<HtmlEditorTextArea*>(data);
    if (!self) return;
    self->redraw();
}

int HtmlEditorTextArea::scrollY() const {
    return (vscroll_ && vscroll_->visible()) ? static_cast<int>(vscroll_->value()) : 0;
}

void HtmlEditorTextArea::setScrollY(int top) {
    ensureLayout();
    int maxTop = std::max(0, contentHeight_ - textAreaH_);
    top = std::clamp(top, 0, maxTop);
    if (vscroll_) {
        vscroll_->value(top);
    }
    redraw();
}

void HtmlEditorTextArea::scrollBy(int delta) {
    setScrollY(scrollY() + delta);
}

std::string HtmlEditorTextArea::currentText() const {
    return owner_ ? owner_->bufferText() : std::string();
}

int HtmlEditorTextArea::clampPosition(int pos) const {
    std::string text = currentText();
    pos = std::clamp(pos, 0, static_cast<int>(text.size()));
    if (pos == 0 || pos == static_cast<int>(text.size())) return pos;
    while (pos > 0 && pos < static_cast<int>(text.size()) &&
           isUtf8ContinuationByte(static_cast<unsigned char>(text[static_cast<size_t>(pos)]))) {
        --pos;
    }
    return pos;
}

bool HtmlEditorTextArea::selectionRange(int& start, int& end) const {
    start = 0;
    end = 0;
    if (!buffer_) return false;
    return buffer_->selection_position(&start, &end) != 0 && end > start;
}

void HtmlEditorTextArea::setSelection(int start, int end) {
    if (!buffer_) return;
    start = clampPosition(start);
    end = clampPosition(end);
    if (end > start) buffer_->select(start, end);
    else buffer_->unselect();
    redraw();
}

void HtmlEditorTextArea::clearSelection() {
    if (buffer_) buffer_->unselect();
    redraw();
}

void HtmlEditorTextArea::moveCursor(int pos, bool extendSelection) {
    pos = clampPosition(pos);
    insertPos_ = pos;
    if (extendSelection) {
        setSelection(std::min(anchorPos_, pos), std::max(anchorPos_, pos));
    } else {
        anchorPos_ = pos;
        clearSelection();
    }
    show_insert_position();
}

void HtmlEditorTextArea::showContextMenuForPosition(int pos, int screenX, int screenY) {
    if (!owner_) return;

    int selStart = 0;
    int selEnd = 0;
    bool hasSelection = selectionRange(selStart, selEnd);

    std::string text = currentText();
    int refStart = 0;
    int refEnd = 0;
    std::string verseRef = verseReferenceAtPosition(text, pos, &refStart, &refEnd);
    bool canInsertVerse = !verseRef.empty() && static_cast<bool>(owner_->verseTextProvider_);

    Fl_Menu_Button menu(screenX, screenY, 0, 0);
    ui_font::applyCurrentAppMenuFont(&menu);
    if (hasSelection) {
        menu.add("Copy");
        menu.add("Cut");
    }
    menu.add("Paste");
    if (canInsertVerse) {
        menu.add("Insert Verse Text Before Reference");
        menu.add("Insert Verse Text After Reference");
    }

    const Fl_Menu_Item* picked = menu.popup();
    if (!picked || !picked->label()) return;

    std::string label = picked->label();
    if (label == "Copy") {
        owner_->copy();
        return;
    }
    if (label == "Cut") {
        owner_->cut();
        return;
    }
    if (label == "Paste") {
        owner_->paste();
        return;
    }
    if (!canInsertVerse) return;

    std::string verseHtml = owner_->verseTextProvider_(verseRef);
    if (verseHtml.empty()) return;

    if (label == "Insert Verse Text Before Reference") {
        owner_->insertHtmlFragmentAt(refStart, refStart, verseHtml + "<br>");
    } else if (label == "Insert Verse Text After Reference") {
        owner_->insertHtmlFragmentAt(refEnd, refEnd, "<br>" + verseHtml);
    }
}

void HtmlEditorTextArea::selectAll() {
    if (!buffer_) return;
    anchorPos_ = 0;
    insertPos_ = buffer_->length();
    setSelection(0, buffer_->length());
    show_insert_position();
}

void HtmlEditorTextArea::insertText(const char* text, int len) {
    if (!buffer_ || !text) return;
    if (len < 0) len = static_cast<int>(std::strlen(text));
    if (len <= 0) return;

    owner_->prepareForUserEdit();
    int start = 0;
    int end = 0;
    if (selectionRange(start, end)) {
        buffer_->remove(start, end);
        insertPos_ = anchorPos_ = start;
    }
    buffer_->insert(insertPos_, text, len);
    insertPos_ += len;
    anchorPos_ = insertPos_;
    clearSelection();
    owner_->finalizeUserEditAttempt();
    show_insert_position();
}

void HtmlEditorTextArea::deleteRange(int start, int end) {
    if (!buffer_ || end <= start) return;
    owner_->prepareForUserEdit();
    buffer_->remove(start, end);
    insertPos_ = anchorPos_ = start;
    clearSelection();
    owner_->finalizeUserEditAttempt();
    show_insert_position();
}

void HtmlEditorTextArea::deleteBackward() {
    int start = 0;
    int end = 0;
    if (selectionRange(start, end)) {
        deleteRange(start, end);
        return;
    }
    std::string text = currentText();
    if (insertPos_ <= 0 || text.empty()) return;
    deleteRange(prevUtf8Boundary(text, insertPos_), insertPos_);
}

void HtmlEditorTextArea::deleteForward() {
    int start = 0;
    int end = 0;
    if (selectionRange(start, end)) {
        deleteRange(start, end);
        return;
    }
    std::string text = currentText();
    if (insertPos_ >= static_cast<int>(text.size())) return;
    deleteRange(insertPos_, nextUtf8Boundary(text, insertPos_));
}

bool HtmlEditorTextArea::handleKeyEvent() {
    if (!owner_) return false;

    const int key = Fl::event_key();
    const bool ctrl = (Fl::event_state() & FL_CTRL) != 0;
    const bool shift = (Fl::event_state() & FL_SHIFT) != 0;

    if (ctrl && (key == 'z' || key == 'Z')) {
        return shift ? owner_->redo() : owner_->undo();
    }
    if (ctrl && (key == 'y' || key == 'Y')) {
        return owner_->redo();
    }
    if (ctrl && (key == 'b' || key == 'B')) {
        owner_->toggleBold();
        return true;
    }
    if (ctrl && (key == 'i' || key == 'I')) {
        owner_->toggleItalic();
        return true;
    }
    if (ctrl && (key == 'a' || key == 'A')) {
        selectAll();
        return true;
    }
    if (ctrl && (key == 'c' || key == 'C')) {
        owner_->copy();
        return true;
    }
    if (ctrl && (key == 'x' || key == 'X')) {
        owner_->cut();
        return true;
    }
    if (ctrl && (key == 'v' || key == 'V')) {
        Fl::paste(*this, 1);
        return true;
    }
    if (!ctrl && (key == FL_Enter || key == FL_KP_Enter)) {
        owner_->prepareForUserEdit();
        bool handled = owner_->handleEnterKey();
        owner_->finalizeUserEditAttempt();
        return handled;
    }
    if (key == FL_Tab) {
        owner_->prepareForUserEdit();
        bool handled = owner_->handleTabKey(shift);
        owner_->finalizeUserEditAttempt();
        return handled;
    }
    if (key == FL_BackSpace) {
        deleteBackward();
        preferredX_ = -1;
        return true;
    }
    if (key == FL_Delete) {
        deleteForward();
        preferredX_ = -1;
        return true;
    }

    std::string text = currentText();
    switch (key) {
    case FL_Left:
        moveCursor(ctrl && buffer_ ? buffer_->word_start(prevUtf8Boundary(text, insertPos_))
                                   : prevUtf8Boundary(text, insertPos_),
                   shift);
        preferredX_ = -1;
        return true;
    case FL_Right:
        moveCursor(ctrl && buffer_ ? buffer_->word_end(nextUtf8Boundary(text, insertPos_))
                                   : nextUtf8Boundary(text, insertPos_),
                   shift);
        preferredX_ = -1;
        return true;
    case FL_Home:
        moveCursor(buffer_ ? buffer_->line_start(insertPos_) : 0, shift);
        preferredX_ = -1;
        return true;
    case FL_End:
        moveCursor(buffer_ ? buffer_->line_end(insertPos_) : static_cast<int>(text.size()), shift);
        preferredX_ = -1;
        return true;
    case FL_Up:
    case FL_Down: {
        ensureLayout();
        CaretInfo caret = caretInfoForPosition(insertPos_);
        int targetLine = caret.lineIndex + (key == FL_Up ? -1 : 1);
        if (targetLine < 0 || targetLine >= static_cast<int>(lines_.size())) return true;
        int targetX = (preferredX_ >= 0) ? preferredX_ : caret.x;
        preferredX_ = targetX;
        moveCursor(positionForLineX(targetLine, targetX), shift);
        return true;
    }
    default:
        break;
    }

    if (!ctrl && isPrintableUtf8Text(Fl::event_text(), Fl::event_length())) {
        insertText(Fl::event_text(), Fl::event_length());
        preferredX_ = -1;
        return true;
    }
    return false;
}

void HtmlEditorTextArea::ensureLayout() const {
    if (layoutValid_) return;

    auto* self = const_cast<HtmlEditorTextArea*>(this);
    int innerX = x() + Fl::box_dx(box());
    int innerY = y() + Fl::box_dy(box());
    int innerW = w() - Fl::box_dw(box());
    int innerH = h() - Fl::box_dh(box());
    int scrollW = vscroll_ ? vscroll_->w() : Fl::scrollbar_size();
    bool needScroll = false;

    for (int pass = 0; pass < 3; ++pass) {
        self->textAreaX_ = innerX + kTextDisplayLeftMargin;
        self->textAreaY_ = innerY + kTextDisplayTopMargin;
        self->textAreaW_ = std::max(0, innerW - kTextDisplayLeftMargin - kTextDisplayRightMargin -
                                         (needScroll ? scrollW : 0));
        self->textAreaH_ = std::max(0, innerH - kTextDisplayTopMargin - kTextDisplayBottomMargin);
        self->rebuildLayout(self->textAreaW_);
        bool newNeedScroll = self->contentHeight_ > self->textAreaH_;
        if (newNeedScroll == needScroll) break;
        needScroll = newNeedScroll;
    }

    if (vscroll_) {
        if (needScroll) {
            vscroll_->show();
            vscroll_->resize(innerX + innerW - scrollW, innerY, scrollW, innerH);
            int current = vscroll_->value();
            int maxTop = std::max(0, contentHeight_ - textAreaH_);
            vscroll_->value(std::clamp(current, 0, maxTop),
                            std::max(1, textAreaH_),
                            0,
                            std::max(textAreaH_, contentHeight_));
        } else {
            vscroll_->hide();
            vscroll_->value(0);
        }
    }

    self->layoutValid_ = true;
}

void HtmlEditorTextArea::rebuildLayout(int contentWidth) const {
    auto* self = const_cast<HtmlEditorTextArea*>(this);
    self->cachedText_ = currentText();
    self->lines_.clear();
    self->contentHeight_ = 0;

    if (!owner_) return;

    const std::string& text = cachedText_;
    std::vector<bool> linkFlags(text.size(), false);
    for (const auto& range : verseReferenceRanges(text)) {
        for (int i = std::max(0, range.first);
             i < range.second && i < static_cast<int>(linkFlags.size()); ++i) {
            linkFlags[static_cast<size_t>(i)] = true;
        }
    }

    fl_font(owner_->textFont_, owner_->textSize_);
    int defaultAscent = std::max(1, fl_height() - fl_descent());
    int defaultDescent = std::max(1, fl_descent());
    int defaultHeight = std::max(1, defaultAscent + defaultDescent);
    int y = 0;

    auto appendBlankLine = [&](int pos, unsigned char align) {
        VisualLine line;
        line.start = pos;
        line.end = pos;
        line.y = y;
        line.ascent = defaultAscent;
        line.descent = defaultDescent;
        line.advance = lineAdvanceForHeights(defaultHeight);
        line.contentWidth = 0;
        line.drawX = (align == kAlignCenter) ? std::max(0, contentWidth / 2) : 0;
        self->lines_.push_back(line);
        y += line.advance;
    };

    if (text.empty()) {
        appendBlankLine(0, kAlignLeft);
        self->contentHeight_ = y;
        return;
    }

    for (int logicalStart = 0; logicalStart <= static_cast<int>(text.size());) {
        int logicalEnd = logicalStart;
        while (logicalEnd < static_cast<int>(text.size()) && text[logicalEnd] != '\n') {
            ++logicalEnd;
        }

        std::string lineText = text.substr(logicalStart, logicalEnd - logicalStart);
        BlockFormat block = blockFormatForLine(logicalStart, logicalEnd, lineText, owner_->formats_);
        bool ruleLine = isRuleLine(lineText);
        bool centerBlock = block.align == kAlignCenter &&
                           !trimCopy(lineText).empty() &&
                           !ruleLine &&
                           !isBulletLine(lineText) &&
                           !isOrderedLine(lineText);

        if (logicalStart == logicalEnd) {
            appendBlankLine(logicalStart, block.align);
        } else {
            int pos = logicalStart;
            while (pos < logicalEnd) {
                VisualLine line;
                line.start = pos;
                line.y = y;
                int x = 0;
                int maxAscent = 0;
                int maxDescent = 0;
                int lastBreakIndex = -1;
                int lastBreakPos = -1;
                int lastBreakWidth = 0;

                while (pos < logicalEnd) {
                    MeasuredGlyph measured = measureGlyph(text, pos, ruleLine);
                    if (measured.nextPos <= pos) {
                        measured.nextPos = pos + 1;
                        measured.width = std::max(1, measured.width);
                    }

                    bool overflow = contentWidth > 0 && !line.glyphs.empty() &&
                                    (x + measured.width > contentWidth);
                    if (overflow) {
                        if (lastBreakPos > line.start) {
                            line.end = lastBreakPos;
                            line.glyphs.resize(static_cast<size_t>(lastBreakIndex + 1));
                            x = lastBreakWidth;
                            pos = lastBreakPos;
                            break;
                        }
                        line.end = pos;
                        break;
                    }

                    Glyph glyph;
                    glyph.start = pos;
                    glyph.end = measured.nextPos;
                    glyph.x = x;
                    glyph.width = measured.width;
                    glyph.ascent = measured.ascent;
                    glyph.descent = measured.descent;
                    glyph.whitespace = measured.whitespace;
                    glyph.link = linkFlags[static_cast<size_t>(pos)];
                    glyph.rule = measured.rule;
                    line.glyphs.push_back(glyph);

                    x += measured.width;
                    maxAscent = std::max(maxAscent, measured.ascent);
                    maxDescent = std::max(maxDescent, measured.descent);
                    if (measured.whitespace) {
                        lastBreakIndex = static_cast<int>(line.glyphs.size()) - 1;
                        lastBreakPos = measured.nextPos;
                        lastBreakWidth = x;
                    }
                    pos = measured.nextPos;
                }

                if (line.end == 0) {
                    line.end = pos;
                }
                if (line.end == line.start && line.start < logicalEnd) {
                    MeasuredGlyph measured = measureGlyph(text, line.start, ruleLine);
                    Glyph glyph;
                    glyph.start = line.start;
                    glyph.end = measured.nextPos;
                    glyph.x = 0;
                    glyph.width = measured.width;
                    glyph.ascent = measured.ascent;
                    glyph.descent = measured.descent;
                    glyph.whitespace = measured.whitespace;
                    glyph.link = linkFlags[static_cast<size_t>(line.start)];
                    glyph.rule = measured.rule;
                    line.glyphs.push_back(glyph);
                    line.end = measured.nextPos;
                    x = measured.width;
                    maxAscent = std::max(maxAscent, measured.ascent);
                    maxDescent = std::max(maxDescent, measured.descent);
                    pos = measured.nextPos;
                }

                if (maxAscent <= 0 || maxDescent <= 0) {
                    maxAscent = defaultAscent;
                    maxDescent = defaultDescent;
                }

                line.ascent = maxAscent;
                line.descent = maxDescent;
                line.advance = lineAdvanceForHeights(maxAscent + maxDescent);
                line.contentWidth = x;
                int centeredWidth = x;
                for (auto it = line.glyphs.rbegin(); it != line.glyphs.rend(); ++it) {
                    if (!it->whitespace) break;
                    centeredWidth = it->x;
                }
                if (centerBlock && centeredWidth > 0) {
                    line.drawX = std::max(0, (contentWidth - centeredWidth) / 2);
                } else {
                    line.drawX = 0;
                }
                self->lines_.push_back(line);
                y += line.advance;
            }
        }

        if (logicalEnd >= static_cast<int>(text.size())) break;
        logicalStart = logicalEnd + 1;
        if (logicalStart > static_cast<int>(text.size())) break;
        if (logicalStart == static_cast<int>(text.size()) && text.back() == '\n') {
            appendBlankLine(logicalStart, kAlignLeft);
            break;
        }
    }

    self->contentHeight_ = y;
}

void HtmlEditorTextArea::drawSelection() const {
    int selStart = 0;
    int selEnd = 0;
    if (!selectionRange(selStart, selEnd)) return;

    Fl_Color fill = (Fl::focus() == this) ? FL_SELECTION_COLOR : fl_rgb_color(170, 200, 255);
    fl_color(fill);

    int top = scrollY();
    for (const VisualLine& line : lines_) {
        int lineTop = textAreaY_ + line.y - top;
        int lineBottom = lineTop + std::max(line.advance, line.ascent + line.descent);
        if (lineBottom < textAreaY_ || lineTop > textAreaY_ + textAreaH_) continue;

        int runStartX = -1;
        int runEndX = -1;
        for (const Glyph& glyph : line.glyphs) {
            bool selected = glyph.end > selStart && glyph.start < selEnd;
            int gx = textAreaX_ + line.drawX + glyph.x;
            if (selected) {
                if (runStartX < 0) runStartX = gx;
                runEndX = gx + glyph.width;
            } else if (runStartX >= 0) {
                fl_rectf(runStartX, lineTop, std::max(1, runEndX - runStartX),
                         std::max(line.advance, line.ascent + line.descent));
                runStartX = runEndX = -1;
            }
        }
        if (runStartX >= 0) {
            fl_rectf(runStartX, lineTop, std::max(1, runEndX - runStartX),
                     std::max(line.advance, line.ascent + line.descent));
        }
    }
}

void HtmlEditorTextArea::drawText() const {
    if (!owner_) return;
    int selStart = 0;
    int selEnd = 0;
    bool hasSelection = selectionRange(selStart, selEnd);
    int top = scrollY();
    for (const VisualLine& line : lines_) {
        int lineTop = textAreaY_ + line.y - top;
        int lineBottom = lineTop + std::max(line.advance, line.ascent + line.descent);
        if (lineBottom < textAreaY_ || lineTop > textAreaY_ + textAreaH_) continue;

        int baseline = lineTop + line.ascent;
        for (const Glyph& glyph : line.glyphs) {
            if (glyph.whitespace) continue;

            bool selected = hasSelection && glyph.end > selStart && glyph.start < selEnd;
            CharFormat fmt = formatAt(owner_->formats_, glyph.start);
            fmt.displayPad = false;
            Fl_Font font = glyph.rule ? owner_->textFont_ : owner_->fontForFormat(fmt);
            int size = glyph.rule
                ? std::max(6, owner_->displayFontSizeForFormat(CharFormat{}) - 1)
                : owner_->displayFontSizeForFormat(fmt);
            fl_font(font, size);
            Fl_Color glyphColor = glyph.link ? FL_BLUE :
                                  (glyph.rule ? fl_rgb_color(120, 120, 120) : FL_FOREGROUND_COLOR);
            if (selected) glyphColor = FL_WHITE;
            fl_color(glyphColor);
            fl_draw(cachedText_.data() + glyph.start,
                    glyph.end - glyph.start,
                    textAreaX_ + line.drawX + glyph.x,
                    baseline);

            if (glyph.link) {
                int underlineY = baseline + 1;
                fl_color(selected ? FL_WHITE : FL_BLUE);
                fl_line(textAreaX_ + line.drawX + glyph.x,
                        underlineY,
                        textAreaX_ + line.drawX + glyph.x + glyph.width,
                        underlineY);
            }
        }
    }
}

void HtmlEditorTextArea::drawCaret() const {
    int selStart = 0;
    int selEnd = 0;
    if (selectionRange(selStart, selEnd)) return;
    if (Fl::focus() != this) return;

    CaretInfo caret = caretInfoForPosition(insertPos_);
    int lineTop = textAreaY_ + caret.top - scrollY();
    if (lineTop + caret.height < textAreaY_ || lineTop > textAreaY_ + textAreaH_) return;

    fl_color(FL_BLACK);
    fl_line(textAreaX_ + caret.x,
            lineTop + 1,
            textAreaX_ + caret.x,
            lineTop + std::max(4, caret.height) - 2);
}

HtmlEditorTextArea::CaretInfo HtmlEditorTextArea::caretInfoForPosition(int pos) const {
    ensureLayout();
    if (lines_.empty()) return {};

    pos = clampPosition(pos);
    int lineIndex = lineIndexForPosition(pos);
    const VisualLine& line = lines_[static_cast<size_t>(lineIndex)];

    CaretInfo caret;
    caret.lineIndex = lineIndex;
    caret.top = line.y;
    caret.height = std::max(line.advance, line.ascent + line.descent);
    caret.x = line.drawX;

    if (line.glyphs.empty() || pos <= line.start) {
        return caret;
    }

    for (const Glyph& glyph : line.glyphs) {
        if (pos <= glyph.start) {
            caret.x = line.drawX + glyph.x;
            return caret;
        }
        if (pos <= glyph.end) {
            caret.x = line.drawX + glyph.x + glyph.width;
            return caret;
        }
    }

    caret.x = line.drawX + line.contentWidth;
    return caret;
}

int HtmlEditorTextArea::positionForLineX(int lineIndex, int targetX) const {
    ensureLayout();
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines_.size())) {
        return clampPosition(0);
    }

    const VisualLine& line = lines_[static_cast<size_t>(lineIndex)];
    if (line.glyphs.empty()) return line.start;

    int localX = targetX - line.drawX;
    if (localX <= 0) return line.start;
    for (const Glyph& glyph : line.glyphs) {
        int mid = glyph.x + (glyph.width / 2);
        if (localX < mid) return glyph.start;
        if (localX < glyph.x + glyph.width) return glyph.end;
    }
    return line.end;
}

int HtmlEditorTextArea::lineIndexForPosition(int pos) const {
    ensureLayout();
    if (lines_.empty()) return 0;

    pos = clampPosition(pos);
    for (size_t i = 0; i < lines_.size(); ++i) {
        const VisualLine& line = lines_[i];
        if (pos < line.start) {
            return static_cast<int>(i == 0 ? 0 : i - 1);
        }
        if (line.start == line.end && pos == line.start) {
            return static_cast<int>(i);
        }
        if (pos >= line.start && pos < line.end) {
            return static_cast<int>(i);
        }
        if (pos == line.end) {
            if (i + 1 < lines_.size() && lines_[i + 1].start == pos &&
                line.end > line.start) {
                continue;
            }
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(lines_.size() - 1);
}

int HtmlEditorTextArea::positionForPoint(int px, int py) const {
    ensureLayout();
    if (lines_.empty()) return 0;

    int localY = py - textAreaY_ + scrollY();
    if (localY <= 0) return lines_.front().start;

    for (size_t i = 0; i < lines_.size(); ++i) {
        const VisualLine& line = lines_[i];
        int bottom = line.y + std::max(line.advance, line.ascent + line.descent);
        if (localY < bottom || i + 1 == lines_.size()) {
            return positionForLineX(static_cast<int>(i), px - textAreaX_);
        }
    }
    return lines_.back().end;
}

HtmlEditorTextArea::MeasuredGlyph HtmlEditorTextArea::measureGlyph(
    std::string_view text,
    int pos,
    bool ruleLine) const {
    MeasuredGlyph glyph;
    if (!owner_ || pos < 0 || pos >= static_cast<int>(text.size())) return glyph;

    glyph.nextPos = nextUtf8Boundary(text, pos);
    CharFormat fmt = formatAt(owner_->formats_, pos);
    fmt.displayPad = false;
    Fl_Font font = ruleLine ? owner_->textFont_ : owner_->fontForFormat(fmt);
    int size = ruleLine
        ? std::max(6, owner_->displayFontSizeForFormat(CharFormat{}) - 1)
        : owner_->displayFontSizeForFormat(fmt);
    fl_font(font, size);
    glyph.ascent = std::max(1, fl_height() - fl_descent());
    glyph.descent = std::max(1, fl_descent());
    glyph.rule = ruleLine;

    char c = text[static_cast<size_t>(pos)];
    glyph.whitespace = (c == ' ' || c == '\t');
    if (c == '\t') {
        glyph.width = std::max(1,
                               static_cast<int>(std::lround(
                                   owner_->spaceWidthForFormat(fmt) *
                                   std::max(1, owner_->indentWidth_))));
    } else {
        glyph.width = std::max(1, static_cast<int>(std::lround(
            fl_width(text.data() + pos, glyph.nextPos - pos))));
    }

    return glyph;
}

int HtmlEditorTextArea::lineAdvanceForHeights(int maxHeight) const {
    maxHeight = std::max(1, maxHeight);
    if (!owner_) return std::max(8, maxHeight);
    return std::max(8, static_cast<int>(std::lround(
        static_cast<double>(maxHeight) * (owner_->lineHeight_ / 1.2))));
}

HtmlEditorWidget::HtmlEditorWidget(int X, int Y, int W, int H, const char* label)
    : Fl_Group(X, Y, W, H, label) {
    box(FL_FLAT_BOX);

    begin();
    buildToolbar();
    editor_ = new HtmlEditorTextArea(this, X, Y + kToolbarH, W, H - kToolbarH);
    textBuffer_ = new Fl_Text_Buffer();
    textBuffer_->tab_distance(indentWidth_);
    textBuffer_->add_modify_callback(onTextModified, this);
    editor_->buffer(textBuffer_);
    editor_->textfont(textFont_);
    editor_->textsize(textSize_);
    end();

    editor_->setLineAdvance(editorLineAdvance());
    rebuildStyleBuffer();
    layoutChildren();
}

HtmlEditorWidget::~HtmlEditorWidget() {
    if (editor_) editor_->buffer(static_cast<Fl_Text_Buffer*>(nullptr));
    if (textBuffer_) textBuffer_->remove_modify_callback(onTextModified, this);
    delete textBuffer_;
    textBuffer_ = nullptr;
}

void HtmlEditorWidget::buildToolbar() {
    toolbar_ = new Fl_Group(x(), y(), w(), kToolbarH);
    toolbar_->begin();

    int bx = x() + 2;
    auto makeButton = [&](int width, const char* label) -> Fl_Button* {
        Fl_Button* button = new Fl_Button(bx, y() + 2, width, kToolbarH - 4, label);
        button->callback(onToolbarButton, this);
        bx += width + kButtonGap;
        return button;
    };

    undoButton_ = makeButton(kButtonW, "@undo");
    redoButton_ = makeButton(kButtonW, "@redo");
    boldButton_ = makeButton(kButtonW, "B");
    italicButton_ = makeButton(kButtonW, "I");
    sizeChoice_ = new Fl_Choice(bx, y() + 2, kSizeChoiceW, kToolbarH - 4);
    sizeChoice_->callback(onToolbarButton, this);
    bx += kSizeChoiceW + kButtonGap;
    centerButton_ = makeButton(kWideButtonW, "Ctr");
    unorderedListButton_ = makeButton(kWideButtonW, "\xE2\x80\xA2 \xE2\x80\xA2");
    orderedListButton_ = makeButton(kWideButtonW, "1. 2.");
    ruleButton_ = makeButton(kWideButtonW, "HR");

    if (boldButton_) boldButton_->type(FL_TOGGLE_BUTTON);
    if (italicButton_) italicButton_->type(FL_TOGGLE_BUTTON);
    if (centerButton_) centerButton_->type(FL_TOGGLE_BUTTON);
    if (undoButton_) undoButton_->tooltip("Undo");
    if (redoButton_) redoButton_->tooltip("Redo");
    if (boldButton_) boldButton_->tooltip("Toggle bold");
    if (italicButton_) italicButton_->tooltip("Toggle italic");
    if (sizeChoice_) sizeChoice_->tooltip("Text size");
    if (centerButton_) centerButton_->tooltip("Center selected paragraph lines");
    if (unorderedListButton_) unorderedListButton_->tooltip("Toggle bulleted list");
    if (orderedListButton_) orderedListButton_->tooltip("Toggle numbered list");
    if (ruleButton_) ruleButton_->tooltip("Insert horizontal rule");

    toolbar_->end();
    populateSizeChoice();
}

void HtmlEditorWidget::layoutChildren() {
    if (!toolbar_ || !editor_) return;
    toolbar_->resize(x(), y(), w(), kToolbarH);

    int bx = x() + 2;
    auto placeButton = [&](Fl_Button* button, int width) {
        if (!button) return;
        button->resize(bx, y() + 2, width, kToolbarH - 4);
        bx += width + kButtonGap;
    };

    placeButton(undoButton_, kButtonW);
    placeButton(redoButton_, kButtonW);
    placeButton(boldButton_, kButtonW);
    placeButton(italicButton_, kButtonW);
    if (sizeChoice_) {
        sizeChoice_->resize(bx, y() + 2, kSizeChoiceW, kToolbarH - 4);
        bx += kSizeChoiceW + kButtonGap;
    }
    placeButton(centerButton_, kWideButtonW);
    placeButton(unorderedListButton_, kWideButtonW);
    placeButton(orderedListButton_, kWideButtonW);
    placeButton(ruleButton_, kWideButtonW);

    editor_->resize(x(), y() + kToolbarH, w(), std::max(10, h() - kToolbarH));
}

void HtmlEditorWidget::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);
    layoutChildren();
    normalizeCenteredDisplay();
    rebuildStyleBuffer();
}

void HtmlEditorWidget::setMode(Mode mode) {
    mode_ = mode;
}

std::string HtmlEditorWidget::bufferText() const {
    if (!textBuffer_) return "";
    char* text = textBuffer_->text();
    if (!text) return "";
    std::string out = text;
    std::free(text);
    return out;
}

void HtmlEditorWidget::setBufferText(const std::string& text) {
    if (!textBuffer_) return;
    textBuffer_->text(text.c_str());
}

Fl_Font HtmlEditorWidget::fontForFormat(const CharFormat& format) const {
    if (format.bold && format.italic) return boldItalicTextFont_;
    if (format.bold) return boldTextFont_;
    if (format.italic) return italicTextFont_;
    return textFont_;
}

int HtmlEditorWidget::displayFontSizeForFormat(const CharFormat& format) const {
    return fontSizeForLevel(textSize_, format.size);
}

int HtmlEditorWidget::editorLineAdvance() const {
    int naturalHeight = fl_height(textFont_, textSize_);
    if (naturalHeight <= 0) naturalHeight = textSize_;

    // Treat 1.2 as the existing/default FLTK spacing and scale from there.
    int requestedAdvance = static_cast<int>(std::lround(
        static_cast<double>(naturalHeight) * (lineHeight_ / 1.2)));
    return std::clamp(std::max(requestedAdvance, minimumSafeLineAdvance()), 8, 96);
}

int HtmlEditorWidget::minimumSafeLineAdvance() const {
    int maxHeight = fl_height(textFont_, textSize_);
    if (maxHeight <= 0) maxHeight = textSize_;

    for (const CharFormat& fmt : formats_) {
        if (fmt.displayPad) continue;
        maxHeight = std::max(maxHeight,
                             fl_height(fontForFormat(fmt),
                                       displayFontSizeForFormat(fmt)));
    }
    return maxHeight;
}

double HtmlEditorWidget::measureStyledRangeWidth(const std::string& text,
                                                 int start,
                                                 int end) const {
    if (start >= end || start < 0 || end > static_cast<int>(text.size())) return 0.0;

    double width = 0.0;
    int runStart = start;
    CharFormat current = formatAt(formats_, runStart);
    current.displayPad = false;

    for (int i = start; i <= end; ++i) {
        bool boundary = (i == end);
        CharFormat next = current;
        if (!boundary) {
            next = formatAt(formats_, i);
            next.displayPad = false;
        }
        if (!boundary && next == current) continue;

        fl_font(fontForFormat(current), displayFontSizeForFormat(current));
        width += fl_width(text.data() + runStart, i - runStart);
        runStart = i;
        if (i < end) current = next;
    }
    return width;
}

double HtmlEditorWidget::spaceWidthForFormat(const CharFormat& format) const {
    CharFormat measureFormat = format;
    measureFormat.displayPad = false;
    fl_font(fontForFormat(measureFormat), displayFontSizeForFormat(measureFormat));
    return fl_width(" ", 1);
}

void HtmlEditorWidget::replaceAll(const std::string& text,
                                  const std::vector<CharFormat>& formats,
                                  bool markModified) {
    suppressCallbacks_ = true;
    setBufferText(text);
    suppressCallbacks_ = false;

    formats_ = formats;
    if (formats_.size() < text.size()) {
        formats_.resize(text.size());
    } else if (formats_.size() > text.size()) {
        formats_.resize(text.size());
    }

    if (editor_) {
        editor_->insert_position(static_cast<int>(text.size()));
        if (textBuffer_) textBuffer_->unselect();
    }

    normalizeCenteredDisplay();
    modified_ = markModified;
    discardPendingUserEdit();
    rebuildStyleBuffer();
    refreshToolbarState();
}

void HtmlEditorWidget::setHtml(const std::string& htmlText) {
    std::string text;
    std::vector<CharFormat> formats;
    parseHtmlToEditorContent(htmlText, text, formats);
    replaceAll(text, formats, false);
    clearHistory();
}

std::string HtmlEditorWidget::html() const {
    auto stripped = stripDisplayPadding(bufferText(), formats_);
    std::string text = std::move(stripped.first);
    std::vector<CharFormat> formats = std::move(stripped.second);
    formats.resize(text.size());
    return serializeEditorContent(text, formats, textSize_,
                                  HtmlExportFlavor::Standard, indentWidth_);
}

std::string HtmlEditorWidget::odtHtml() const {
    auto stripped = stripDisplayPadding(bufferText(), formats_);
    std::string text = std::move(stripped.first);
    std::vector<CharFormat> formats = std::move(stripped.second);
    formats.resize(text.size());
    return serializeEditorContent(text, formats, textSize_,
                                  HtmlExportFlavor::Odt, indentWidth_);
}

void HtmlEditorWidget::setIndentWidth(int width) {
    indentWidth_ = std::clamp(width, 1, 8);
    if (textBuffer_) {
        if (textBuffer_->tab_distance() != indentWidth_) {
            bool wasSuppressed = suppressCallbacks_;
            suppressCallbacks_ = true;
            textBuffer_->tab_distance(indentWidth_);
            suppressCallbacks_ = wasSuppressed;
        }
    }
    if (editor_) editor_->redraw();
}

void HtmlEditorWidget::setLineHeight(double lineHeight) {
    double clamped = std::clamp(lineHeight, 1.0, 2.0);
    if (std::fabs(lineHeight_ - clamped) < 0.001) return;

    lineHeight_ = clamped;
    if (editor_) editor_->setLineAdvance(editorLineAdvance());
    normalizeCenteredDisplay();
    rebuildStyleBuffer();
}

void HtmlEditorWidget::setTextFont(Fl_Font regularFont, Fl_Font boldFont, int size) {
    textFont_ = regularFont;
    boldTextFont_ = boldFont;
    italicTextFont_ = findFontVariant(textFont_, FL_ITALIC, textFont_);
    boldItalicTextFont_ = findFontVariant(textFont_,
                                          FL_BOLD | FL_ITALIC,
                                          boldTextFont_);
    textSize_ = std::clamp(size, 8, 36);
    if (editor_) {
        editor_->textfont(textFont_);
        editor_->textsize(textSize_);
        editor_->setLineAdvance(editorLineAdvance());
    }
    populateSizeChoice();
    normalizeCenteredDisplay();
    rebuildStyleBuffer();
}

void HtmlEditorWidget::populateSizeChoice() {
    if (!sizeChoice_) return;

    int current = sizeChoice_->value();
    sizeChoice_->clear();
    for (size_t i = 0; i < kSizeOffsets.size(); ++i) {
        sizeChoice_->add(std::to_string(fontSizeForLevel(textSize_,
                                                         static_cast<unsigned char>(i))).c_str());
    }
    if (current >= 0 && current < sizeChoice_->size()) {
        sizeChoice_->value(current);
    } else {
        sizeChoice_->value(static_cast<int>(kDefaultSizeLevel));
    }
}

void HtmlEditorWidget::clearDocument() {
    replaceAll("", {}, false);
    clearHistory();
}

void HtmlEditorWidget::setModified(bool modified) {
    bool changed = (modified_ != modified);
    modified_ = modified;
    refreshToolbarState();
    if (changed) emitChanged();
}

Snapshot HtmlEditorWidget::captureSnapshot() const {
    Snapshot snapshot;
    snapshot.text = bufferText();
    snapshot.formats = formats_;
    if (editor_) snapshot.insertPos = editor_->insert_position();
    if (textBuffer_) {
        textBuffer_->selection_position(&snapshot.selStart, &snapshot.selEnd);
    }
    snapshot.modified = modified_;
    return snapshot;
}

void HtmlEditorWidget::restoreSnapshot(const Snapshot& snapshot) {
    replaceAll(snapshot.text, snapshot.formats, snapshot.modified);
    if (editor_) {
        editor_->insert_position(std::clamp(snapshot.insertPos, 0,
                                            static_cast<int>(snapshot.text.size())));
    }
    if (textBuffer_ && snapshot.selEnd > snapshot.selStart) {
        textBuffer_->select(snapshot.selStart, snapshot.selEnd);
    }
    if (editor_) editor_->show_insert_position();
}

void HtmlEditorWidget::clearHistory() {
    undoStack_.clear();
    redoStack_.clear();
    discardPendingUserEdit();
    refreshToolbarState();
}

void HtmlEditorWidget::pushUndoSnapshot(const Snapshot& snapshot) {
    undoStack_.push_back(snapshot);
    if (undoStack_.size() > 256) {
        undoStack_.erase(undoStack_.begin());
    }
    redoStack_.clear();
    refreshToolbarState();
}

void HtmlEditorWidget::prepareForUserEdit() {
    if (suppressCallbacks_ || pendingUserEditValid_) return;
    pendingUserEdit_ = captureSnapshot();
    pendingUserEditValid_ = true;
}

void HtmlEditorWidget::discardPendingUserEdit() {
    pendingUserEditValid_ = false;
    pendingUserEdit_ = Snapshot{};
}

bool HtmlEditorWidget::insertHtmlFragmentAt(int start, int end, const std::string& html) {
    if (!textBuffer_ || !editor_ || html.empty()) return false;

    std::string fragmentText;
    std::vector<CharFormat> fragmentFormats;
    parseHtmlToEditorContent(html, fragmentText, fragmentFormats);
    if (fragmentText.empty()) return false;
    if (fragmentFormats.size() < fragmentText.size()) {
        fragmentFormats.resize(fragmentText.size());
    } else if (fragmentFormats.size() > fragmentText.size()) {
        fragmentFormats.resize(fragmentText.size());
    }

    std::string text = bufferText();
    start = std::clamp(start, 0, static_cast<int>(text.size()));
    end = std::clamp(end, start, static_cast<int>(text.size()));

    std::vector<CharFormat> formats = formats_;
    if (formats.size() < text.size()) {
        formats.resize(text.size());
    } else if (formats.size() > text.size()) {
        formats.resize(text.size());
    }

    discardPendingUserEdit();
    pushUndoSnapshot(captureSnapshot());

    text.erase(static_cast<size_t>(start), static_cast<size_t>(end - start));
    formats.erase(formats.begin() + start, formats.begin() + end);
    text.insert(static_cast<size_t>(start), fragmentText);
    formats.insert(formats.begin() + start, fragmentFormats.begin(), fragmentFormats.end());

    replaceAll(text, formats, true);
    editor_->insert_position(start + static_cast<int>(fragmentText.size()));
    if (textBuffer_) textBuffer_->unselect();
    editor_->show_insert_position();
    emitChanged();
    return true;
}

void HtmlEditorWidget::finalizeUserEditAttempt() {
    if (!pendingUserEditValid_) return;
    if (bufferText() == pendingUserEdit_.text) {
        discardPendingUserEdit();
    }
}

void HtmlEditorWidget::syncToolbarState() {
    refreshToolbarState();
}

void HtmlEditorWidget::normalizeCenteredDisplay() {
    if (!textBuffer_ || !editor_) return;

    std::string original = bufferText();
    auto stripped = stripDisplayPadding(original, formats_);
    if (stripped.first != original || stripped.second != formats_) {
        bool wasSuppressed = suppressCallbacks_;
        bool wasModified = modified_;
        int insertPos = editor_->insert_position();
        int selStart = 0;
        int selEnd = 0;
        bool hasSelection = textBuffer_->selection_position(&selStart, &selEnd) != 0;

        suppressCallbacks_ = true;
        setBufferText(stripped.first);
        suppressCallbacks_ = wasSuppressed;
        formats_ = std::move(stripped.second);
        modified_ = wasModified;

        int maxPos = textBuffer_->length();
        editor_->insert_position(std::clamp(insertPos, 0, maxPos));
        if (hasSelection && selEnd > selStart) {
            textBuffer_->select(std::clamp(selStart, 0, maxPos),
                                std::clamp(selEnd, 0, maxPos));
        } else {
            textBuffer_->unselect();
        }
    }

    editor_->invalidateLayout();
}

bool HtmlEditorWidget::selectionRange(int& start, int& end) const {
    start = 0;
    end = 0;
    if (!textBuffer_) return false;
    return textBuffer_->selection_position(&start, &end) != 0 && end > start;
}

bool HtmlEditorWidget::selectionOrWordRange(int& start, int& end) const {
    if (selectionRange(start, end)) return true;
    if (!textBuffer_ || !editor_) return false;
    start = textBuffer_->word_start(editor_->insert_position());
    end = textBuffer_->word_end(editor_->insert_position());
    return end > start;
}

void HtmlEditorWidget::applyInlineTransform(
    const std::function<void(CharFormat&)>& transform) {
    int start = 0;
    int end = 0;
    if (!selectionOrWordRange(start, end)) return;

    pushUndoSnapshot(captureSnapshot());
    if (static_cast<int>(formats_.size()) < textBuffer_->length()) {
        formats_.resize(textBuffer_->length());
    }

    for (int i = start; i < end && i < static_cast<int>(formats_.size()); ++i) {
        transform(formats_[i]);
    }

    modified_ = true;
    normalizeCenteredDisplay();
    rebuildStyleBuffer();
    emitChanged();
}

void HtmlEditorWidget::rebuildStyleBuffer() {
    if (!textBuffer_ || !editor_) return;
    std::string text = bufferText();
    if (formats_.size() < text.size()) {
        formats_.resize(text.size());
    } else if (formats_.size() > text.size()) {
        formats_.resize(text.size());
    }
    editor_->invalidateLayout();
    refreshToolbarState();
}

void HtmlEditorWidget::refreshToolbarState() {
    if (undoButton_) {
        if (undoStack_.empty()) undoButton_->deactivate();
        else undoButton_->activate();
    }
    if (redoButton_) {
        if (redoStack_.empty()) redoButton_->deactivate();
        else redoButton_->activate();
    }

    int start = 0;
    int end = 0;
    if (selectionOrWordRange(start, end) && !formats_.empty()) {
        bool allBold = true;
        bool allItalic = true;
        unsigned char level = formatAt(formats_, start).size;
        for (int i = start; i < end && i < static_cast<int>(formats_.size()); ++i) {
            allBold = allBold && formats_[i].bold;
            allItalic = allItalic && formats_[i].italic;
            if (formats_[i].size != level) {
                level = formatAt(formats_, start).size;
            }
        }
        setButtonDown(boldButton_, allBold);
        setButtonDown(italicButton_, allItalic);
        if (sizeChoice_) sizeChoice_->value(std::clamp<int>(level, 0, sizeChoice_->size() - 1));
    } else {
        setButtonDown(boldButton_, false);
        setButtonDown(italicButton_, false);
        if (sizeChoice_) sizeChoice_->value(static_cast<int>(kDefaultSizeLevel));
    }

    bool allCentered = false;
    if (textBuffer_ && editor_ && !formats_.empty()) {
        int lineStart = 0;
        int lineEnd = 0;
        if (!selectionRange(lineStart, lineEnd)) {
            lineStart = editor_->insert_position();
            lineEnd = lineStart;
        }
        lineStart = textBuffer_->line_start(lineStart);
        lineEnd = textBuffer_->line_end(lineEnd);
        allCentered = true;
        bool sawContent = false;
        std::string text = bufferText();
        for (int pos = lineStart; pos <= lineEnd && pos < static_cast<int>(text.size());) {
            int next = textBuffer_->line_end(pos);
            std::string line = text.substr(pos, next - pos);
            if (!trimCopy(line).empty()) {
                sawContent = true;
                BlockFormat block = blockFormatForLine(pos, next, line, formats_);
                allCentered = allCentered && (block.align == kAlignCenter);
            }
            if (next >= static_cast<int>(text.size())) break;
            pos = next + 1;
        }
        if (!sawContent) allCentered = false;
    }
    setButtonDown(centerButton_, allCentered);
}

void HtmlEditorWidget::emitChanged() {
    if (changeCallback_) changeCallback_();
}

bool HtmlEditorWidget::handleEnterKey() {
    if (!textBuffer_ || !editor_) return false;

    int insertPos = editor_->insert_position();
    int selStart = insertPos;
    int selEnd = insertPos;
    bool hadSelection = selectionRange(selStart, selEnd);
    if (hadSelection) {
        insertPos = selStart;
    }

    std::string original = bufferText();
    auto displayPadLenForLine = [&](int startPos, int endPos) {
        int len = 0;
        while (startPos + len < endPos &&
               original[startPos + len] == ' ' &&
               formatAt(formats_, startPos + len).displayPad) {
            ++len;
        }
        return len;
    };
    int lineStart = textBuffer_->line_start(insertPos);
    int lineEnd = textBuffer_->line_end(insertPos);
    int displayPadLen = displayPadLenForLine(lineStart, lineEnd);
    std::string line = original.substr(lineStart + displayPadLen,
                                       lineEnd - lineStart - displayPadLen);

    ListPrefixInfo listInfo;
    bool hasListPrefix = parseListPrefix(line, &listInfo);
    std::string continuation = normalizeIndentPrefix(
        std::string_view(line.data(), static_cast<size_t>(leadingIndentLength(line))),
        indentWidth_);
    if (hasListPrefix) {
        continuation = normalizeIndentPrefix(listInfo.indent, indentWidth_) +
                       (listInfo.ordered
                            ? "1. "
                            : "- ");
    }

    bool emptyListItem = hasListPrefix &&
                         trimCopy(line.substr(static_cast<size_t>(listInfo.prefixLen))).empty();
    bool atLineEnd = !hadSelection && insertPos >= lineEnd;

    if (hadSelection) {
        textBuffer_->remove(selStart, selEnd);
        insertPos = selStart;
    }

    if (emptyListItem && atLineEnd) {
        int indentTrim = trailingIndentUnitLength(
            std::string_view(line.data(), static_cast<size_t>(listInfo.indentLen)),
            indentWidth_);
        textBuffer_->remove(lineStart + displayPadLen + listInfo.indentLen - indentTrim,
                            lineStart + displayPadLen + listInfo.prefixLen);
        if (listInfo.ordered) {
            renumberOrderedListBlocks();
        }
        editor_->insert_position(lineStart + displayPadLen + listInfo.indentLen - indentTrim);
        editor_->show_insert_position();
        return true;
    }

    std::string inserted = "\n" + continuation;
    textBuffer_->insert(insertPos, inserted.c_str());
    if (listInfo.ordered) {
        renumberOrderedListBlocks();
    }
    int caretPos = insertPos + static_cast<int>(inserted.size());
    if (hasListPrefix) {
        int newLineStart = insertPos + 1;
        int newLineEnd = textBuffer_->line_end(newLineStart);
        char* lineText = textBuffer_->text_range(newLineStart, newLineEnd);
        std::string newLine = lineText ? lineText : "";
        std::free(lineText);

        ListPrefixInfo newListInfo;
        if (parseListPrefix(newLine, &newListInfo)) {
            caretPos = newLineStart + newListInfo.prefixLen;
        } else {
            caretPos = newLineStart;
        }
    }
    editor_->insert_position(caretPos);
    editor_->show_insert_position();
    return true;
}

bool HtmlEditorWidget::handleTabKey(bool outdent) {
    if (!textBuffer_ || !editor_) return false;

    int insertPos = editor_->insert_position();
    int selStart = insertPos;
    int selEnd = insertPos;
    bool hadSelection = selectionRange(selStart, selEnd);
    if (!hadSelection) {
        selStart = insertPos;
        selEnd = insertPos;
    }

    int startLine = textBuffer_->line_start(selStart);
    int endLine = textBuffer_->line_end(selEnd);
    if (hadSelection && selEnd > selStart && endLine < textBuffer_->length()) {
        endLine = textBuffer_->line_end(endLine + 1);
    }

    std::string original = bufferText();
    std::vector<std::pair<int, int>> lineBounds;
    int pos = startLine;
    while (pos <= endLine && pos <= static_cast<int>(original.size())) {
        int next = textBuffer_->line_end(pos);
        lineBounds.push_back({pos, next});
        if (next >= static_cast<int>(original.size())) break;
        pos = next + 1;
    }
    if (lineBounds.empty()) return true;

    int firstLineDelta = 0;
    int lastLineDelta = 0;
    bool firstLineProcessed = false;

    for (auto it = lineBounds.rbegin(); it != lineBounds.rend(); ++it) {
        int lineStartPos = it->first;
        int lineEndPos = it->second;
        int semanticStartPos = lineStartPos;
        while (semanticStartPos < lineEndPos &&
               original[semanticStartPos] == ' ' &&
               formatAt(formats_, semanticStartPos).displayPad) {
            ++semanticStartPos;
        }
        std::string line = original.substr(semanticStartPos, lineEndPos - semanticStartPos);

        int change = 0;
        if (outdent) {
            if (!line.empty() && line[0] == '\t') {
                textBuffer_->remove(semanticStartPos, semanticStartPos + 1);
                change = -1;
            } else {
                int removable = 0;
                while (removable < indentWidth_ &&
                       removable < static_cast<int>(line.size()) &&
                       line[static_cast<size_t>(removable)] == ' ') {
                    ++removable;
                }
                if (removable > 0) {
                    textBuffer_->remove(semanticStartPos, semanticStartPos + removable);
                    change = -removable;
                }
            }
        } else {
            textBuffer_->insert(semanticStartPos, "\t");
            change = 1;
        }

        if (!firstLineProcessed) {
            lastLineDelta = change;
            firstLineProcessed = true;
        }
        firstLineDelta = change;
    }

    if (hadSelection && lineBounds.size() > 1) {
        selStart = std::max(startLine, selStart + firstLineDelta);
        selEnd = std::max(selStart, selEnd + lastLineDelta);
        textBuffer_->select(selStart, selEnd);
        editor_->insert_position(selEnd);
    } else if (hadSelection) {
        int adjustedStart = std::max(startLine, selStart + firstLineDelta);
        int adjustedEnd = std::max(adjustedStart, selEnd + firstLineDelta);
        textBuffer_->select(adjustedStart, adjustedEnd);
        editor_->insert_position(adjustedEnd);
    } else {
        int adjustedPos = insertPos + firstLineDelta;
        adjustedPos = std::max(startLine, adjustedPos);
        editor_->insert_position(adjustedPos);
    }

    renumberOrderedListBlocks();
    editor_->show_insert_position();
    return true;
}

void HtmlEditorWidget::renumberOrderedListBlocks() {
    if (!textBuffer_) return;

    std::vector<int> nextNumbers;
    int baseLevel = -1;
    int pos = 0;
    while (pos <= textBuffer_->length()) {
        int lineEnd = textBuffer_->line_end(pos);
        char* lineText = textBuffer_->text_range(pos, lineEnd);
        std::string line = lineText ? lineText : "";
        std::free(lineText);

        ListPrefixInfo info;
        if (parseListPrefix(line, &info) && info.ordered) {
            int absoluteLevel = listIndentLevel(info.indent, indentWidth_);
            if (baseLevel < 0) {
                baseLevel = absoluteLevel;
            }
            int level = std::max(0, absoluteLevel - baseLevel);
            if (static_cast<int>(nextNumbers.size()) > level + 1) {
                nextNumbers.resize(static_cast<size_t>(level + 1));
            }
            if (static_cast<int>(nextNumbers.size()) <= level) {
                nextNumbers.resize(static_cast<size_t>(level + 1), 1);
            }

            int order = nextNumbers[static_cast<size_t>(level)]++;
            std::string desiredPrefix = orderedListPrefix(info.indent, order, level);
            std::string currentPrefix = line.substr(0, static_cast<size_t>(info.prefixLen));
            if (currentPrefix != desiredPrefix) {
                textBuffer_->replace(pos, pos + info.prefixLen, desiredPrefix.c_str());
                lineEnd = textBuffer_->line_end(pos);
            }
        } else if (parseListPrefix(line, &info)) {
            int absoluteLevel = listIndentLevel(info.indent, indentWidth_);
            if (baseLevel < 0) {
                baseLevel = absoluteLevel;
            }
            int level = std::max(0, absoluteLevel - baseLevel);
            if (static_cast<int>(nextNumbers.size()) >= level + 1) {
                nextNumbers.resize(static_cast<size_t>(level));
            }
        } else {
            nextNumbers.clear();
            baseLevel = -1;
        }

        if (lineEnd >= textBuffer_->length()) break;
        pos = lineEnd + 1;
    }
}

bool HtmlEditorWidget::undo() {
    discardPendingUserEdit();
    if (undoStack_.empty()) return false;

    Snapshot current = captureSnapshot();
    redoStack_.push_back(current);

    Snapshot previous = undoStack_.back();
    undoStack_.pop_back();
    restoreSnapshot(previous);
    modified_ = true;
    refreshToolbarState();
    emitChanged();
    return true;
}

bool HtmlEditorWidget::redo() {
    discardPendingUserEdit();
    if (redoStack_.empty()) return false;

    Snapshot current = captureSnapshot();
    undoStack_.push_back(current);

    Snapshot next = redoStack_.back();
    redoStack_.pop_back();
    restoreSnapshot(next);
    modified_ = true;
    refreshToolbarState();
    emitChanged();
    return true;
}

void HtmlEditorWidget::copy() {
    std::string text = selectionText(textBuffer_);
    if (!text.empty()) {
        Fl::copy(text.c_str(), static_cast<int>(text.size()), 0);
        Fl::copy(text.c_str(), static_cast<int>(text.size()), 1);
    }
}

void HtmlEditorWidget::cut() {
    int start = 0;
    int end = 0;
    if (!selectionRange(start, end)) return;

    copy();
    pushUndoSnapshot(captureSnapshot());
    textBuffer_->remove(start, end);
    if (editor_) editor_->insert_position(start);
    modified_ = true;
    rebuildStyleBuffer();
    emitChanged();
}

void HtmlEditorWidget::paste() {
    prepareForUserEdit();
    Fl::paste(*editor_, 1);
}

void HtmlEditorWidget::toggleBold() {
    int start = 0;
    int end = 0;
    if (!selectionOrWordRange(start, end)) return;
    bool enable = false;
    for (int i = start; i < end && i < static_cast<int>(formats_.size()); ++i) {
        if (!formats_[i].bold) {
            enable = true;
            break;
        }
    }
    applyInlineTransform([enable](CharFormat& fmt) { fmt.bold = enable; });
}

void HtmlEditorWidget::toggleItalic() {
    int start = 0;
    int end = 0;
    if (!selectionOrWordRange(start, end)) return;
    bool enable = false;
    for (int i = start; i < end && i < static_cast<int>(formats_.size()); ++i) {
        if (!formats_[i].italic) {
            enable = true;
            break;
        }
    }
    applyInlineTransform([enable](CharFormat& fmt) { fmt.italic = enable; });
}

void HtmlEditorWidget::setTextSizeLevel(unsigned char level) {
    applyInlineTransform([level](CharFormat& fmt) { fmt.size = level; });
}

void HtmlEditorWidget::increaseTextSize() {
    applyInlineTransform([](CharFormat& fmt) {
        fmt.size = static_cast<unsigned char>(
            std::min<int>(static_cast<int>(kSizeOffsets.size()) - 1, fmt.size + 1));
    });
}

void HtmlEditorWidget::decreaseTextSize() {
    applyInlineTransform([](CharFormat& fmt) {
        fmt.size = static_cast<unsigned char>(std::max<int>(0, fmt.size - 1));
    });
}

void HtmlEditorWidget::toggleCenterAlignment() {
    if (!textBuffer_ || !editor_) return;

    int selStart = editor_->insert_position();
    int selEnd = selStart;
    if (!selectionRange(selStart, selEnd)) {
        selStart = editor_->insert_position();
        selEnd = selStart;
    }

    int startLine = textBuffer_->line_start(selStart);
    int endLine = textBuffer_->line_end(selEnd);
    if (selEnd > selStart && endLine < textBuffer_->length()) {
        endLine = textBuffer_->line_end(endLine + 1);
    }

    std::string original = bufferText();
    bool allCentered = true;
    bool sawContent = false;
    for (int pos = startLine; pos <= endLine && pos <= static_cast<int>(original.size());) {
        int next = textBuffer_->line_end(pos);
        std::string line = original.substr(pos, next - pos);
        if (!trimCopy(line).empty()) {
            sawContent = true;
            BlockFormat block = blockFormatForLine(pos, next, line, formats_);
            allCentered = allCentered && (block.align == kAlignCenter);
        }
        if (next >= static_cast<int>(original.size())) break;
        pos = next + 1;
    }
    if (!sawContent) return;

    unsigned char align = allCentered ? kAlignLeft : kAlignCenter;
    pushUndoSnapshot(captureSnapshot());
    if (static_cast<int>(formats_.size()) < textBuffer_->length()) {
        formats_.resize(textBuffer_->length());
    }

    for (int pos = startLine; pos <= endLine && pos <= static_cast<int>(original.size());) {
        int next = textBuffer_->line_end(pos);
        for (int i = pos; i < next && i < static_cast<int>(formats_.size()); ++i) {
            formats_[i].align = align;
        }
        if (next >= static_cast<int>(original.size())) break;
        pos = next + 1;
    }

    modified_ = true;
    normalizeCenteredDisplay();
    rebuildStyleBuffer();
    emitChanged();
}

void HtmlEditorWidget::applyLinePrefixes(
    const std::function<std::string(const std::string&, int)>& formatter,
    bool removeMatching,
    bool removeNumbered) {
    if (!textBuffer_ || !editor_) return;

    int selStart = editor_->insert_position();
    int selEnd = selStart;
    if (!selectionRange(selStart, selEnd)) {
        selStart = editor_->insert_position();
        selEnd = selStart;
    }

    int startLine = textBuffer_->line_start(selStart);
    int endLine = textBuffer_->line_end(selEnd);
    if (selEnd > selStart && endLine < textBuffer_->length()) {
        endLine = textBuffer_->line_end(endLine + 1);
    }

    std::string original = bufferText();
    std::vector<std::pair<int, int>> lineBounds;
    int pos = startLine;
    while (pos <= endLine && pos <= static_cast<int>(original.size())) {
        int next = textBuffer_->line_end(pos);
        lineBounds.push_back({pos, next});
        if (next >= static_cast<int>(original.size())) break;
        pos = next + 1;
    }

    pushUndoSnapshot(captureSnapshot());
    int order = static_cast<int>(lineBounds.size());
    for (auto it = lineBounds.rbegin(); it != lineBounds.rend(); ++it) {
        int lineStartPos = it->first;
        int lineEndPos = it->second;
        std::string line = original.substr(lineStartPos, lineEndPos - lineStartPos);

        ListPrefixInfo prefixInfo;
        bool hasPrefix = parseListPrefix(line, &prefixInfo);
        bool bullet = hasPrefix && !prefixInfo.ordered;
        bool ordered = hasPrefix && prefixInfo.ordered;
        if (removeMatching && bullet) {
            int indentTrim = trailingIndentUnitLength(
                std::string_view(line.data(), static_cast<size_t>(prefixInfo.indentLen)),
                indentWidth_);
            textBuffer_->remove(lineStartPos + prefixInfo.indentLen - indentTrim,
                                lineStartPos + prefixInfo.prefixLen);
            continue;
        }
        if (removeNumbered && ordered) {
            int indentTrim = trailingIndentUnitLength(
                std::string_view(line.data(), static_cast<size_t>(prefixInfo.indentLen)),
                indentWidth_);
            textBuffer_->remove(lineStartPos + prefixInfo.indentLen - indentTrim,
                                lineStartPos + prefixInfo.prefixLen);
            continue;
        }

        if (bullet && !removeMatching) {
            textBuffer_->remove(lineStartPos + prefixInfo.indentLen,
                                lineStartPos + prefixInfo.prefixLen);
        } else if (ordered && !removeNumbered) {
            textBuffer_->remove(lineStartPos + prefixInfo.indentLen,
                                lineStartPos + prefixInfo.prefixLen);
        }

        std::string prefix = formatter(line, order--);
        if (!hasPrefix) {
            prefix = "\t" + prefix;
        }
        textBuffer_->insert(lineStartPos + prefixInfo.indentLen, prefix.c_str());
    }

    renumberOrderedListBlocks();
    modified_ = true;
    rebuildStyleBuffer();
    emitChanged();
}

void HtmlEditorWidget::restoreCaretAfterLinePrefixEdit(int lineStart, int contentOffset) {
    if (!textBuffer_ || !editor_) return;

    lineStart = std::clamp(lineStart, 0, textBuffer_->length());
    int lineEnd = textBuffer_->line_end(lineStart);
    char* lineText = textBuffer_->text_range(lineStart, lineEnd);
    std::string line = lineText ? lineText : "";
    std::free(lineText);

    ListPrefixInfo prefixInfo;
    bool hasPrefix = parseListPrefix(line, &prefixInfo);
    int contentStart = lineStart + (hasPrefix ? prefixInfo.prefixLen : 0);
    editor_->insert_position(std::min(lineEnd, contentStart + std::max(0, contentOffset)));
    textBuffer_->unselect();
    editor_->show_insert_position();
}

void HtmlEditorWidget::toggleUnorderedList() {
    std::string text = bufferText();
    int start = 0;
    int end = 0;
    bool hadSelection = selectionRange(start, end);
    int caretLineStart = 0;
    int caretContentOffset = 0;
    if (!hadSelection && editor_) {
        start = editor_->insert_position();
        end = start;
        caretLineStart = textBuffer_->line_start(start);
        int caretLineEnd = textBuffer_->line_end(start);
        std::string caretLine = text.substr(caretLineStart, caretLineEnd - caretLineStart);
        ListPrefixInfo prefixInfo;
        bool hasPrefix = parseListPrefix(caretLine, &prefixInfo);
        int contentStart = caretLineStart + (hasPrefix ? prefixInfo.prefixLen : 0);
        caretContentOffset = std::max(0, start - contentStart);
    }
    int lineStart = textBuffer_->line_start(start);
    int lineEnd = textBuffer_->line_end(end);
    bool allBullets = true;
    for (int pos = lineStart; pos <= lineEnd && pos < static_cast<int>(text.size());) {
        int next = textBuffer_->line_end(pos);
        std::string line = text.substr(pos, next - pos);
        if (!trimCopy(line).empty() && !isBulletLine(line)) {
            allBullets = false;
            break;
        }
        if (next >= static_cast<int>(text.size())) break;
        pos = next + 1;
    }

    applyLinePrefixes(
        [](const std::string&, int) { return std::string("- "); },
        allBullets,
        false);
    if (!hadSelection) restoreCaretAfterLinePrefixEdit(caretLineStart, caretContentOffset);
}

void HtmlEditorWidget::toggleOrderedList() {
    std::string text = bufferText();
    int start = 0;
    int end = 0;
    bool hadSelection = selectionRange(start, end);
    int caretLineStart = 0;
    int caretContentOffset = 0;
    if (!hadSelection && editor_) {
        start = editor_->insert_position();
        end = start;
        caretLineStart = textBuffer_->line_start(start);
        int caretLineEnd = textBuffer_->line_end(start);
        std::string caretLine = text.substr(caretLineStart, caretLineEnd - caretLineStart);
        ListPrefixInfo prefixInfo;
        bool hasPrefix = parseListPrefix(caretLine, &prefixInfo);
        int contentStart = caretLineStart + (hasPrefix ? prefixInfo.prefixLen : 0);
        caretContentOffset = std::max(0, start - contentStart);
    }
    int lineStart = textBuffer_->line_start(start);
    int lineEnd = textBuffer_->line_end(end);
    bool allOrdered = true;
    for (int pos = lineStart; pos <= lineEnd && pos < static_cast<int>(text.size());) {
        int next = textBuffer_->line_end(pos);
        std::string line = text.substr(pos, next - pos);
        if (!trimCopy(line).empty() && !isOrderedLine(line)) {
            allOrdered = false;
            break;
        }
        if (next >= static_cast<int>(text.size())) break;
        pos = next + 1;
    }

    applyLinePrefixes(
        [](const std::string&, int order) {
            return std::to_string(order) + ". ";
        },
        false,
        allOrdered);
    if (!hadSelection) restoreCaretAfterLinePrefixEdit(caretLineStart, caretContentOffset);
}

void HtmlEditorWidget::insertHorizontalRule() {
    if (!textBuffer_ || !editor_) return;
    pushUndoSnapshot(captureSnapshot());

    int pos = textBuffer_->line_start(editor_->insert_position());
    std::string rule = "---";
    if (pos > 0 && bufferText()[pos - 1] != '\n') {
        rule = "\n" + rule;
    }
    rule += "\n";
    textBuffer_->insert(pos, rule.c_str());
    editor_->insert_position(pos + static_cast<int>(rule.size()));
    modified_ = true;
    rebuildStyleBuffer();
    emitChanged();
}

void HtmlEditorWidget::focusEditor() {
    if (!editor_) return;
    editor_->take_focus();
    editor_->show_insert_position();
    syncToolbarState();
}

void HtmlEditorWidget::onToolbarButton(Fl_Widget* w, void* data) {
    auto* self = static_cast<HtmlEditorWidget*>(data);
    if (!self) return;

    if (w == self->undoButton_) self->undo();
    else if (w == self->redoButton_) self->redo();
    else if (w == self->boldButton_) self->toggleBold();
    else if (w == self->italicButton_) self->toggleItalic();
    else if (w == self->sizeChoice_) {
        int value = self->sizeChoice_ ? self->sizeChoice_->value() : -1;
        if (value >= 0) {
            self->setTextSizeLevel(static_cast<unsigned char>(value));
        }
    } else if (w == self->centerButton_) self->toggleCenterAlignment();
    else if (w == self->unorderedListButton_) self->toggleUnorderedList();
    else if (w == self->orderedListButton_) self->toggleOrderedList();
    else if (w == self->ruleButton_) self->insertHorizontalRule();

    self->focusEditor();
}

void HtmlEditorWidget::onTextModified(int pos,
                                      int nInserted,
                                      int nDeleted,
                                      int /*nRestyled*/,
                                      const char* /*deletedText*/,
                                      void* data) {
    auto* self = static_cast<HtmlEditorWidget*>(data);
    if (!self || self->suppressCallbacks_) return;

    if (self->formats_.size() < static_cast<size_t>(self->textBuffer_->length() + nDeleted)) {
        self->formats_.resize(self->textBuffer_->length() + nDeleted);
    }

    CharFormat inherited;
    if (pos > 0 && pos - 1 < static_cast<int>(self->formats_.size())) {
        inherited = self->formats_[pos - 1];
    } else if (pos < static_cast<int>(self->formats_.size())) {
        inherited = self->formats_[pos];
    }
    inherited.displayPad = false;

    if (nDeleted > 0 && pos < static_cast<int>(self->formats_.size())) {
        int eraseEnd = std::min<int>(pos + nDeleted, self->formats_.size());
        self->formats_.erase(self->formats_.begin() + pos,
                             self->formats_.begin() + eraseEnd);
    }
    if (nInserted > 0) {
        self->formats_.insert(self->formats_.begin() + pos,
                              nInserted,
                              inherited);
    }

    size_t targetLen = static_cast<size_t>(std::max(0, self->textBuffer_->length()));
    if (self->formats_.size() < targetLen) {
        self->formats_.resize(targetLen);
    } else if (self->formats_.size() > targetLen) {
        self->formats_.resize(targetLen);
    }

    if ((nInserted > 0 || nDeleted > 0) && self->pendingUserEditValid_) {
        self->pushUndoSnapshot(self->pendingUserEdit_);
        self->discardPendingUserEdit();
    }
    if (nInserted > 0 || nDeleted > 0) self->modified_ = true;

    self->normalizeCenteredDisplay();
    self->rebuildStyleBuffer();
    self->emitChanged();
}

} // namespace verdad
