#include "ui/HtmlEditorWidget.h"

#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string_view>

namespace verdad {
namespace {

constexpr int kToolbarH = 30;
constexpr int kButtonW = 42;
constexpr int kWideButtonW = 54;
constexpr int kButtonGap = 2;
constexpr int kStyleCount = 25;
constexpr int kHrStyleIndex = 24;

using CharFormat = HtmlEditorWidget::CharFormat;
using Snapshot = HtmlEditorWidget::Snapshot;

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

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
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

bool isIndentChar(char c) {
    return c == ' ' || c == '\t';
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
    if (digitPos == markerPos || digitPos + 1 >= line.size() ||
        line[digitPos] != '.' || line[digitPos + 1] != ' ') {
        if (info) *info = parsed;
        return false;
    }

    parsed.valid = true;
    parsed.ordered = true;
    parsed.markerLen = static_cast<int>(digitPos - markerPos + 2);
    parsed.prefixLen = parsed.indentLen + parsed.markerLen;
    parsed.number = std::atoi(std::string(line.substr(markerPos, digitPos - markerPos)).c_str());
    if (info) *info = parsed;
    return true;
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

unsigned char detectLargeSize(const std::string& lowerTag) {
    if (lowerTag.find("data-verdad-size=\"large\"") != std::string::npos ||
        lowerTag.find("font-size: 115%") != std::string::npos ||
        lowerTag.find("font-size:115%") != std::string::npos ||
        lowerTag.find("font-size: 1.15em") != std::string::npos ||
        lowerTag.find("font-size:1.15em") != std::string::npos) {
        return 2;
    }
    return 1;
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
    std::vector<unsigned char> sizeStack(1, 1);
    int boldDepth = 0;
    int italicDepth = 0;
    bool inUnorderedList = false;
    bool inOrderedList = false;
    int orderedIndex = 1;

    auto refreshFormat = [&]() {
        current.bold = (boldDepth > 0);
        current.italic = (italicDepth > 0);
        current.size = sizeStack.empty() ? 1 : sizeStack.back();
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
            } else if (tag == "br") {
                appendLineBreak(textOut, formatsOut);
            } else if (tag == "hr") {
                appendParagraphBreak(textOut, formatsOut);
                appendFormattedText(textOut, formatsOut, "---", current);
                appendParagraphBreak(textOut, formatsOut);
            } else if (tag == "ul") {
                appendParagraphBreak(textOut, formatsOut);
                inUnorderedList = true;
                inOrderedList = false;
                orderedIndex = 1;
            } else if (tag == "ol") {
                appendParagraphBreak(textOut, formatsOut);
                inOrderedList = true;
                inUnorderedList = false;
                orderedIndex = 1;
            } else if (tag == "li") {
                std::string indent;
                int indentTabs = detectListIndentTabs(lowerTag);
                if (indentTabs > 0) indent.assign(static_cast<size_t>(indentTabs), '\t');
                else indent.assign(static_cast<size_t>(detectListIndent(lowerTag)), ' ');
                if (inOrderedList) {
                    appendPrefixIfNeeded(indent + std::to_string(orderedIndex++) + ". ");
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
                sizeStack.push_back(0);
                refreshFormat();
            } else if (tag == "span") {
                sizeStack.push_back(detectLargeSize(lowerTag));
                refreshFormat();
            }
        } else {
            if (tag == "p" || tag == "div") {
                appendParagraphBreak(textOut, formatsOut);
            } else if (tag == "ul") {
                inUnorderedList = false;
                appendParagraphBreak(textOut, formatsOut);
            } else if (tag == "ol") {
                inOrderedList = false;
                appendParagraphBreak(textOut, formatsOut);
            } else if (tag == "li") {
                appendLineBreak(textOut, formatsOut);
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

std::string listItemOpenTag(std::string_view indentText, int indentColumns) {
    if (indentText.empty() || indentColumns <= 0) return "<li>";
    if (usesTabsOnly(indentText)) {
        return "<li data-verdad-indent-tabs=\"" +
               std::to_string(static_cast<int>(indentText.size())) + "\">";
    }
    return "<li data-verdad-indent=\"" + std::to_string(indentColumns) + "\">";
}

std::string wrapStyledText(const std::string& html, const CharFormat& fmt) {
    std::string out = html;
    if (fmt.size == 0) {
        out = "<small>" + out + "</small>";
    } else if (fmt.size >= 2) {
        out = "<span data-verdad-size=\"large\" style=\"font-size: 115%;\">" +
              out + "</span>";
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
                                 int end) {
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
        html += wrapStyledText(escapeAndAutoLink(chunk), current);
        runStart = i;
        if (i < end) {
            current = formatAt(formats, i);
        }
    }

    return html;
}

std::string serializeEditorContent(const std::string& text,
                                   const std::vector<CharFormat>& formats) {
    if (text.empty()) return "";

    struct Line {
        int start = 0;
        int end = 0;
        std::string text;
    };

    std::vector<Line> lines;
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

        int prefixLen = 0;
        int indentCols = 0;
        if (isBulletLine(lines[i].text, &prefixLen, &indentCols)) {
            html += "<ul>";
            while (i < lines.size() && isBulletLine(lines[i].text, &prefixLen, &indentCols)) {
                int indentLen = leadingIndentLength(lines[i].text);
                html += listItemOpenTag(std::string_view(lines[i].text.data(),
                                                        static_cast<size_t>(indentLen)),
                                        indentCols) +
                        serializeInlineRange(text, formats,
                                             lines[i].start + prefixLen,
                                             lines[i].end) +
                        "</li>";
                ++i;
            }
            html += "</ul>";
            continue;
        }

        int orderNumber = 0;
        if (isOrderedLine(lines[i].text, &prefixLen, &indentCols, &orderNumber)) {
            html += "<ol>";
            while (i < lines.size() &&
                   isOrderedLine(lines[i].text, &prefixLen, &indentCols, &orderNumber)) {
                int indentLen = leadingIndentLength(lines[i].text);
                html += listItemOpenTag(std::string_view(lines[i].text.data(),
                                                        static_cast<size_t>(indentLen)),
                                        indentCols) +
                        serializeInlineRange(text, formats,
                                             lines[i].start + prefixLen,
                                             lines[i].end) +
                        "</li>";
                ++i;
            }
            html += "</ol>";
            continue;
        }

        html += "<p>";
        bool firstLine = true;
        while (i < lines.size()) {
            trimmed = trimCopy(lines[i].text);
            if (trimmed.empty() ||
                isRuleLine(lines[i].text) ||
                isBulletLine(lines[i].text) ||
                isOrderedLine(lines[i].text)) {
                break;
            }

            if (!firstLine) html += "<br />";
            html += serializeInlineRange(text, formats,
                                         lines[i].start,
                                         lines[i].end);
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

class HtmlEditorTextArea : public Fl_Text_Editor {
public:
    HtmlEditorTextArea(HtmlEditorWidget* owner, int X, int Y, int W, int H)
        : Fl_Text_Editor(X, Y, W, H)
        , owner_(owner) {}

    int handle(int event) override;

private:
    HtmlEditorWidget* owner_;
};

} // namespace

int HtmlEditorTextArea::handle(int event) {
    if (!owner_) return Fl_Text_Editor::handle(event);

    if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
        const int key = Fl::event_key();
        const bool ctrl = (Fl::event_state() & FL_CTRL) != 0;
        const bool shift = (Fl::event_state() & FL_SHIFT) != 0;

        if (ctrl && (key == 'z' || key == 'Z')) {
            if (shift) owner_->redo();
            else owner_->undo();
            return 1;
        }
        if (ctrl && (key == 'y' || key == 'Y')) {
            owner_->redo();
            return 1;
        }
        if (ctrl && (key == 'b' || key == 'B')) {
            owner_->toggleBold();
            return 1;
        }
        if (ctrl && (key == 'i' || key == 'I')) {
            owner_->toggleItalic();
            return 1;
        }
        if (!ctrl && (key == FL_Enter || key == FL_KP_Enter)) {
            owner_->prepareForUserEdit();
            if (owner_->handleEnterKey()) return 1;
        }
        if (key == FL_Tab) {
            owner_->prepareForUserEdit();
            if (owner_->handleTabKey(shift)) return 1;
        }

        const bool isTyping = (Fl::event_length() > 0 && !ctrl);
        const bool isDeleting =
            key == FL_BackSpace || key == FL_Delete || key == FL_Enter ||
            key == FL_KP_Enter || key == FL_Tab;
        const bool isCutShortcut = ctrl && (key == 'x' || key == 'X');
        const bool isPasteShortcut = ctrl && (key == 'v' || key == 'V');

        if (isTyping || isDeleting || isCutShortcut || isPasteShortcut) {
            owner_->prepareForUserEdit();
        }
    } else if (event == FL_PASTE) {
        owner_->prepareForUserEdit();
    }

    int handled = Fl_Text_Editor::handle(event);

    if (event == FL_KEYBOARD || event == FL_SHORTCUT || event == FL_PASTE) {
        owner_->finalizeUserEditAttempt();
    }

    return handled;
}

HtmlEditorWidget::HtmlEditorWidget(int X, int Y, int W, int H, const char* label)
    : Fl_Group(X, Y, W, H, label) {
    box(FL_FLAT_BOX);

    begin();
    buildToolbar();
    editor_ = new HtmlEditorTextArea(this, X, Y + kToolbarH, W, H - kToolbarH);
    textBuffer_ = new Fl_Text_Buffer();
    styleBuffer_ = new Fl_Text_Buffer();
    textBuffer_->tab_distance(indentWidth_);
    textBuffer_->add_modify_callback(onTextModified, this);
    editor_->buffer(textBuffer_);
    editor_->highlight_data(styleBuffer_, nullptr, 0, 'A', nullptr, nullptr);
    editor_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    editor_->textfont(FL_HELVETICA);
    editor_->textsize(14);
    editor_->cursor_style(Fl_Text_Display::NORMAL_CURSOR);
    editor_->tab_nav(0);
    end();

    styleBuffer_->canUndo(0);
    rebuildStyleBuffer();
    layoutChildren();
}

HtmlEditorWidget::~HtmlEditorWidget() {
    if (editor_) {
        editor_->highlight_data(nullptr, nullptr, 0, 'A', nullptr, nullptr);
        editor_->buffer(static_cast<Fl_Text_Buffer*>(nullptr));
    }
    if (textBuffer_) {
        textBuffer_->remove_modify_callback(onTextModified, this);
    }
    delete styleBuffer_;
    delete textBuffer_;
    styleBuffer_ = nullptr;
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

    undoButton_ = makeButton(kWideButtonW, "Undo");
    redoButton_ = makeButton(kWideButtonW, "Redo");
    boldButton_ = makeButton(kButtonW, "B");
    italicButton_ = makeButton(kButtonW, "I");
    smallerButton_ = makeButton(kButtonW, "A-");
    largerButton_ = makeButton(kButtonW, "A+");
    unorderedListButton_ = makeButton(kWideButtonW, "UL");
    orderedListButton_ = makeButton(kWideButtonW, "OL");
    ruleButton_ = makeButton(kWideButtonW, "HR");

    if (boldButton_) boldButton_->type(FL_TOGGLE_BUTTON);
    if (italicButton_) italicButton_->type(FL_TOGGLE_BUTTON);

    toolbar_->end();
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

    placeButton(undoButton_, kWideButtonW);
    placeButton(redoButton_, kWideButtonW);
    placeButton(boldButton_, kButtonW);
    placeButton(italicButton_, kButtonW);
    placeButton(smallerButton_, kButtonW);
    placeButton(largerButton_, kButtonW);
    placeButton(unorderedListButton_, kWideButtonW);
    placeButton(orderedListButton_, kWideButtonW);
    placeButton(ruleButton_, kWideButtonW);

    editor_->resize(x(), y() + kToolbarH, w(), std::max(10, h() - kToolbarH));
}

void HtmlEditorWidget::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);
    layoutChildren();
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
    std::string text = bufferText();
    std::vector<CharFormat> formats = formats_;
    formats.resize(text.size());
    return serializeEditorContent(text, formats);
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

void HtmlEditorWidget::finalizeUserEditAttempt() {
    if (!pendingUserEditValid_) return;
    if (bufferText() == pendingUserEdit_.text) {
        discardPendingUserEdit();
    }
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
    rebuildStyleBuffer();
    emitChanged();
}

void HtmlEditorWidget::rebuildStyleBuffer() {
    if (!textBuffer_ || !styleBuffer_ || !editor_) return;

    static std::array<Fl_Text_Display::Style_Table_Entry, kStyleCount> styleTable = {{
        {FL_FOREGROUND_COLOR, FL_HELVETICA, 12, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_BOLD, 12, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_ITALIC, 12, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_BOLD_ITALIC, 12, 0, FL_WHITE},
        {FL_BLUE, FL_HELVETICA, 12, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_BOLD, 12, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_ITALIC, 12, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_BOLD_ITALIC, 12, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA, 14, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_BOLD, 14, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_ITALIC, 14, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_BOLD_ITALIC, 14, 0, FL_WHITE},
        {FL_BLUE, FL_HELVETICA, 14, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_BOLD, 14, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_ITALIC, 14, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_BOLD_ITALIC, 14, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA, 17, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_BOLD, 17, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_ITALIC, 17, 0, FL_WHITE},
        {FL_FOREGROUND_COLOR, FL_HELVETICA_BOLD_ITALIC, 17, 0, FL_WHITE},
        {FL_BLUE, FL_HELVETICA, 17, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_BOLD, 17, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_ITALIC, 17, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {FL_BLUE, FL_HELVETICA_BOLD_ITALIC, 17, Fl_Text_Display::ATTR_UNDERLINE, FL_WHITE},
        {fl_rgb_color(120, 120, 120), FL_HELVETICA, 13, 0, FL_WHITE},
    }};

    editor_->highlight_data(styleBuffer_, styleTable.data(), kStyleCount, 'A', nullptr, nullptr);

    std::string text = bufferText();
    if (formats_.size() < text.size()) {
        formats_.resize(text.size());
    } else if (formats_.size() > text.size()) {
        formats_.resize(text.size());
    }

    std::vector<bool> linkFlags(text.size(), false);
    for (const auto& range : verseReferenceRanges(text)) {
        for (int i = std::max(0, range.first);
             i < range.second && i < static_cast<int>(linkFlags.size()); ++i) {
            linkFlags[i] = true;
        }
    }

    std::vector<bool> hrFlags(text.size(), false);
    int lineStart = 0;
    for (int i = 0; i <= static_cast<int>(text.size()); ++i) {
        if (i == static_cast<int>(text.size()) || text[i] == '\n') {
            std::string line = text.substr(lineStart, i - lineStart);
            if (isRuleLine(line)) {
                for (int j = lineStart; j < i; ++j) {
                    hrFlags[j] = true;
                }
            }
            lineStart = i + 1;
        }
    }

    std::string styles(text.size(), 'A');
    for (size_t i = 0; i < text.size(); ++i) {
        if (hrFlags[i]) {
            styles[i] = static_cast<char>('A' + kHrStyleIndex);
            continue;
        }

        const CharFormat fmt = formats_[i];
        int sizeIndex = std::clamp<int>(fmt.size, 0, 2);
        int flags = (fmt.bold ? 1 : 0) |
                    (fmt.italic ? 2 : 0) |
                    (linkFlags[i] ? 4 : 0);
        int styleIndex = (sizeIndex * 8) + flags;
        styles[i] = static_cast<char>('A' + styleIndex);
    }
    styleBuffer_->text(styles.c_str());
    editor_->redisplay_range(0, textBuffer_->length());
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
        for (int i = start; i < end && i < static_cast<int>(formats_.size()); ++i) {
            allBold = allBold && formats_[i].bold;
            allItalic = allItalic && formats_[i].italic;
        }
        setButtonDown(boldButton_, allBold);
        setButtonDown(italicButton_, allItalic);
    } else {
        setButtonDown(boldButton_, false);
        setButtonDown(italicButton_, false);
    }
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
    int lineStart = textBuffer_->line_start(insertPos);
    int lineEnd = textBuffer_->line_end(insertPos);
    std::string line = original.substr(lineStart, lineEnd - lineStart);

    ListPrefixInfo listInfo;
    bool hasListPrefix = parseListPrefix(line, &listInfo);
    std::string continuation = normalizeIndentPrefix(
        std::string_view(line.data(), static_cast<size_t>(leadingIndentLength(line))),
        indentWidth_);
    if (hasListPrefix) {
        continuation = normalizeIndentPrefix(listInfo.indent, indentWidth_) +
                       (listInfo.ordered
                            ? std::to_string(std::max(1, listInfo.number + 1)) + ". "
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
        textBuffer_->remove(lineStart + listInfo.indentLen,
                            lineStart + listInfo.prefixLen);
        if (listInfo.ordered) {
            renumberOrderedListBlocks();
        }
        editor_->insert_position(lineStart + listInfo.indentLen);
        editor_->show_insert_position();
        return true;
    }

    std::string inserted = "\n" + continuation;
    textBuffer_->insert(insertPos, inserted.c_str());
    if (listInfo.ordered) {
        renumberOrderedListBlocks();
    }
    editor_->insert_position(insertPos + static_cast<int>(inserted.size()));
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
        std::string line = original.substr(lineStartPos, lineEndPos - lineStartPos);

        int change = 0;
        if (outdent) {
            if (!line.empty() && line[0] == '\t') {
                textBuffer_->remove(lineStartPos, lineStartPos + 1);
                change = -1;
            } else {
                int removable = 0;
                while (removable < indentWidth_ &&
                       removable < static_cast<int>(line.size()) &&
                       line[static_cast<size_t>(removable)] == ' ') {
                    ++removable;
                }
                if (removable > 0) {
                    textBuffer_->remove(lineStartPos, lineStartPos + removable);
                    change = -removable;
                }
            }
        } else {
            textBuffer_->insert(lineStartPos, "\t");
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

    editor_->show_insert_position();
    return true;
}

void HtmlEditorWidget::renumberOrderedListBlocks() {
    if (!textBuffer_) return;

    bool inOrderedBlock = false;
    int order = 1;
    int pos = 0;
    while (pos <= textBuffer_->length()) {
        int lineEnd = textBuffer_->line_end(pos);
        char* lineText = textBuffer_->text_range(pos, lineEnd);
        std::string line = lineText ? lineText : "";
        std::free(lineText);

        ListPrefixInfo info;
        if (parseListPrefix(line, &info) && info.ordered) {
            if (!inOrderedBlock) {
                inOrderedBlock = true;
                order = 1;
            }
            std::string desiredPrefix = info.indent + std::to_string(order++) + ". ";
            std::string currentPrefix = line.substr(0, static_cast<size_t>(info.prefixLen));
            if (currentPrefix != desiredPrefix) {
                textBuffer_->replace(pos, pos + info.prefixLen, desiredPrefix.c_str());
                lineEnd = textBuffer_->line_end(pos);
            }
        } else {
            inOrderedBlock = false;
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

void HtmlEditorWidget::increaseTextSize() {
    applyInlineTransform([](CharFormat& fmt) {
        fmt.size = static_cast<unsigned char>(std::min<int>(2, fmt.size + 1));
    });
}

void HtmlEditorWidget::decreaseTextSize() {
    applyInlineTransform([](CharFormat& fmt) {
        fmt.size = static_cast<unsigned char>(std::max<int>(0, fmt.size - 1));
    });
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
            textBuffer_->remove(lineStartPos + prefixInfo.indentLen,
                                lineStartPos + prefixInfo.prefixLen);
            continue;
        }
        if (removeNumbered && ordered) {
            textBuffer_->remove(lineStartPos + prefixInfo.indentLen,
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
        textBuffer_->insert(lineStartPos + prefixInfo.indentLen, prefix.c_str());
    }

    modified_ = true;
    rebuildStyleBuffer();
    emitChanged();
}

void HtmlEditorWidget::toggleUnorderedList() {
    std::string text = bufferText();
    int start = 0;
    int end = 0;
    if (!selectionRange(start, end) && editor_) {
        start = editor_->insert_position();
        end = start;
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
}

void HtmlEditorWidget::toggleOrderedList() {
    std::string text = bufferText();
    int start = 0;
    int end = 0;
    if (!selectionRange(start, end) && editor_) {
        start = editor_->insert_position();
        end = start;
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
    if (editor_) editor_->take_focus();
}

void HtmlEditorWidget::onToolbarButton(Fl_Widget* w, void* data) {
    auto* self = static_cast<HtmlEditorWidget*>(data);
    if (!self) return;

    if (w == self->undoButton_) self->undo();
    else if (w == self->redoButton_) self->redo();
    else if (w == self->boldButton_) self->toggleBold();
    else if (w == self->italicButton_) self->toggleItalic();
    else if (w == self->smallerButton_) self->decreaseTextSize();
    else if (w == self->largerButton_) self->increaseTextSize();
    else if (w == self->unorderedListButton_) self->toggleUnorderedList();
    else if (w == self->orderedListButton_) self->toggleOrderedList();
    else if (w == self->ruleButton_) self->insertHorizontalRule();
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

    self->rebuildStyleBuffer();
    self->emitChanged();
}

} // namespace verdad
