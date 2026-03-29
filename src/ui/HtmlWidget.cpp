#include "ui/HtmlWidget.h"
#include "app/PerfTrace.h"
#include "app/VerdadApp.h"

#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_utf8.h>
#include <litehtml/master_css.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

namespace verdad {
namespace {

constexpr int kScrollbarExtent = 16;
constexpr size_t kTextWidthCacheLimit = 1024;
constexpr size_t kTextWidthCacheMaxTokenBytes = 32;

bool isTextWidthCacheable(std::string_view text) {
    if (text.empty() || text.size() > kTextWidthCacheMaxTokenBytes) return false;
    for (unsigned char c : text) {
        if (c & 0x80) return false;
    }
    return true;
}

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

Fl_Font findFontVariant(Fl_Font baseFont, int targetAttrs) {
    constexpr Fl_Font kInvalidFont = static_cast<Fl_Font>(-1);
    static std::map<std::string, Fl_Font> variantCache;

    const std::string cacheKey =
        std::to_string(static_cast<int>(baseFont)) + ":" +
        std::to_string(targetAttrs);
    auto cached = variantCache.find(cacheKey);
    if (cached != variantCache.end()) {
        return cached->second;
    }

    int baseAttrs = 0;
    const char* baseName = Fl::get_font_name(baseFont, &baseAttrs);
    if (!baseName || !baseName[0]) {
        variantCache.emplace(cacheKey, kInvalidFont);
        return kInvalidFont;
    }

    static Fl_Font cachedFontCount = 0;
    if (cachedFontCount == 0) cachedFontCount = Fl::set_fonts("-*");
    Fl_Font count = cachedFontCount;
    for (bool stripRegularSuffixes : {false, true}) {
        std::string familyKey = normalizeFontVariantFamily(baseName, stripRegularSuffixes);
        for (Fl_Font f = 0; f < count; ++f) {
            int attrs = 0;
            const char* name = Fl::get_font_name(f, &attrs);
            if (!name || !name[0] || attrs != targetAttrs) continue;
            if (normalizeFontVariantFamily(name, stripRegularSuffixes) == familyKey) {
                variantCache.emplace(cacheKey, f);
                return f;
            }
        }
    }

    variantCache.emplace(cacheKey, kInvalidFont);
    return kInvalidFont;
}

bool isBuiltinStyledFamilyBase(Fl_Font base) {
    return base == FL_HELVETICA || base == FL_COURIER || base == FL_TIMES;
}

Fl_Font styledFontVariant(Fl_Font base, int weight, bool italic) {
    const bool bold = weight >= 700;
    if (!bold && !italic) return base;

    constexpr Fl_Font kInvalidFont = static_cast<Fl_Font>(-1);
    int targetAttrs = (bold ? FL_BOLD : 0) | (italic ? FL_ITALIC : 0);

    Fl_Font combined = findFontVariant(base, targetAttrs);
    if (combined != kInvalidFont) return combined;

    if (bold) {
        Fl_Font boldVariant = findFontVariant(base, FL_BOLD);
        if (italic) {
            Fl_Font italicVariant = findFontVariant(base, FL_ITALIC);
            if (boldVariant != kInvalidFont) return boldVariant;
            if (italicVariant != kInvalidFont) return italicVariant;
        } else if (boldVariant != kInvalidFont) {
            return boldVariant;
        }
    } else if (italic) {
        Fl_Font italicVariant = findFontVariant(base, FL_ITALIC);
        if (italicVariant != kInvalidFont) return italicVariant;
    }

    if (isBuiltinStyledFamilyBase(base)) {
        if (bold && italic) return base + FL_BOLD_ITALIC;
        if (bold) return base + FL_BOLD;
        if (italic) return base + FL_ITALIC;
    }

    return base;
}

std::string fontDescriptionCacheKey(const litehtml::font_description& descr) {
    std::string key = descr.family;
    key += ":sz=" + std::to_string(descr.size);
    key += ":st=" + std::to_string(static_cast<int>(descr.style));
    key += ":w=" + std::to_string(descr.weight);
    key += ":dl=" + std::to_string(descr.decoration_line);
    return key;
}

bool containsWhitespace(const std::string& text) {
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) return true;
    }
    return false;
}

std::string stripWordEdgePunct(const std::string& raw) {
    auto isAsciiWordChar = [](unsigned char c) -> bool {
        return std::isalnum(c) || c == '\'' || c == '-';
    };

    size_t start = 0;
    size_t end = raw.size();
    while (start < end) {
        unsigned char uc = static_cast<unsigned char>(raw[start]);
        if (uc >= 0x80 || isAsciiWordChar(uc)) break;
        ++start;
    }
    while (end > start) {
        unsigned char uc = static_cast<unsigned char>(raw[end - 1]);
        if (uc >= 0x80 || isAsciiWordChar(uc)) break;
        --end;
    }
    return raw.substr(start, end - start);
}

std::vector<std::string> splitWordCandidates(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream ss(text);
    std::string part;
    while (ss >> part) {
        std::string tok = stripWordEdgePunct(part);
        if (!tok.empty()) words.push_back(tok);
    }
    return words;
}

/// Extract the single word at an approximate cursor position within a
/// multi-word text fragment.  Uses the element's rendered placement to
/// estimate which character the cursor is over, then finds word boundaries.
std::string extractWordAtCursor(const std::string& text,
                                int cursorDocX,
                                const std::shared_ptr<litehtml::element>& el) {
    std::string trimmed = trimCopy(text);
    if (trimmed.empty()) return {};

    // Single-word text: return as-is after stripping punctuation
    if (!containsWhitespace(trimmed))
        return stripWordEdgePunct(trimmed);

    // Try to use the element's rendered position to estimate char index
    litehtml::position placement = el->get_placement();
    int elWidth = placement.width;
    if (elWidth > 0 && !trimmed.empty()) {
        int relX = cursorDocX - placement.x;
        relX = std::max(0, std::min(relX, elWidth));
        // Proportional character index estimate
        size_t charIdx = static_cast<size_t>(
            static_cast<long long>(relX) *
            static_cast<long long>(trimmed.size()) / elWidth);
        if (charIdx >= trimmed.size()) charIdx = trimmed.size() - 1;

        // Expand to word boundaries around the estimated position
        auto isWC = [](unsigned char c) {
            return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
        };
        size_t start = charIdx;
        size_t end   = charIdx;
        while (start > 0 && isWC(static_cast<unsigned char>(trimmed[start - 1])))
            --start;
        while (end < trimmed.size() &&
               isWC(static_cast<unsigned char>(trimmed[end])))
            ++end;
        if (end > start)
            return trimmed.substr(start, end - start);
    }

    // Fallback: return the first word
    auto words = splitWordCandidates(trimmed);
    return words.empty() ? std::string{} : words.front();
}

bool isNumericToken(const std::string& token) {
    if (token.empty()) return false;
    bool sawDigit = false;
    for (char c : token) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isdigit(uc)) {
            sawDigit = true;
            continue;
        }
        if (uc == ',' || uc == '.' || uc == ':' || uc == ';' ||
            uc == ')' || uc == '(' || uc == '[' || uc == ']') {
            continue;
        }
        return false;
    }
    return sawDigit;
}

std::vector<std::string> removeLeadingNumericTokens(
        const std::vector<std::string>& words) {
    if (words.size() < 2) return words;

    bool hasNonNumeric = false;
    for (const auto& w : words) {
        if (!isNumericToken(w)) {
            hasNonNumeric = true;
            break;
        }
    }
    if (!hasNonNumeric) return words;

    size_t firstNonNumeric = 0;
    while (firstNonNumeric < words.size() &&
           isNumericToken(words[firstNonNumeric])) {
        ++firstNonNumeric;
    }
    if (firstNonNumeric == 0 || firstNonNumeric >= words.size()) return words;
    return std::vector<std::string>(words.begin() + firstNonNumeric, words.end());
}

bool findElementHitBoxAtPoint(
        const std::shared_ptr<litehtml::element>& node,
        int docX, int docY,
        litehtml::position& bestBox,
        double& bestArea) {
    bool found = false;
    node->run_on_renderers([&](const std::shared_ptr<litehtml::render_item>& ri) -> bool {
        if (!ri) return true;
        litehtml::position box = ri->get_placement();
        if (box.width <= 0 || box.height <= 0) return true;
        if (!box.is_point_inside(docX, docY)) return true;

        double area = static_cast<double>(box.width) * static_cast<double>(box.height);
        if (area <= 0.0) return true;
        if (!found || area < bestArea) {
            bestArea = area;
            bestBox = box;
            found = true;
        }
        return true;
    });
    return found;
}

struct HitElement {
    std::shared_ptr<litehtml::element> element;
    litehtml::position box;
    bool hasBox = false;
};

bool isInlineLikeDisplay(litehtml::style_display d) {
    return d == litehtml::display_inline ||
           d == litehtml::display_inline_text ||
           d == litehtml::display_inline_block ||
           d == litehtml::display_inline_table ||
           d == litehtml::display_inline_flex;
}

HitElement findDeepestElementAtPoint(
        const std::shared_ptr<litehtml::element>& root,
        int docX, int docY) {
    HitElement best;
    if (!root) return best;

    struct StackNode {
        std::shared_ptr<litehtml::element> element;
        int depth = 0;
    };

    double bestArea = std::numeric_limits<double>::max();
    int bestDepth = -1;

    std::vector<StackNode> stack;
    stack.push_back({root, 0});

    while (!stack.empty()) {
        StackNode cur = stack.back();
        stack.pop_back();
        if (!cur.element) continue;

        litehtml::position box;
        double area = 0.0;
        if (!findElementHitBoxAtPoint(cur.element, docX, docY, box, area)) {
            continue;
        }

        litehtml::string nodeText;
        cur.element->get_text(nodeText);
        if (!trimCopy(nodeText).empty()) {
            bool betterArea = area + 0.01 < bestArea;
            bool sameArea = std::fabs(area - bestArea) <= 0.01;
            if (betterArea || (sameArea && cur.depth > bestDepth)) {
                bestArea = area;
                bestDepth = cur.depth;
                best.element = cur.element;
                best.box = box;
                best.hasBox = true;
            }
        }

        const auto& children = cur.element->children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (*it) stack.push_back({*it, cur.depth + 1});
        }
    }

    if (!best.element) {
        best.element = root;
    }
    return best;
}

std::string chooseWordByHitX(const std::string& text,
                             const litehtml::position& placement,
                             int docX) {
    std::vector<std::string> words = splitWordCandidates(text);
    words = removeLeadingNumericTokens(words);
    if (words.empty()) return "";
    if (placement.width <= 0) return words.front();

    fl_font(FL_TIMES, 14);
    std::vector<double> wordWidths;
    wordWidths.reserve(words.size());
    double wordsTotal = 0.0;
    for (const auto& w : words) {
        double ww = std::max(1.0, static_cast<double>(fl_width(w.c_str())));
        wordWidths.push_back(ww);
        wordsTotal += ww;
    }

    const size_t gapCount = words.size() - 1;
    double spaceW = std::max(1.0, static_cast<double>(fl_width(" ")));
    double gapsTotal = static_cast<double>(gapCount) * spaceW;

    double targetWidth = std::max(1.0, static_cast<double>(placement.width));
    double extraPerGap = 0.0;
    if (gapCount > 0 && targetWidth > (wordsTotal + gapsTotal)) {
        extraPerGap = (targetWidth - (wordsTotal + gapsTotal)) /
                      static_cast<double>(gapCount);
    }

    int localX = docX - placement.x;
    if (localX < 0 || localX >= placement.width) return "";
    if (words.size() == 1) {
        double ww = std::max(1.0, static_cast<double>(fl_width(words.front().c_str())));
        if (static_cast<double>(localX) <= ww + 1.0) return words.front();
        return "";
    }

    double x = static_cast<double>(localX);
    double cursor = 0.0;
    for (size_t i = 0; i < words.size(); ++i) {
        double wordStart = cursor;
        double wordEnd = cursor + wordWidths[i];
        if (x >= wordStart && x <= wordEnd) return words[i];
        cursor = wordEnd;
        if (i + 1 < words.size()) {
            double gap = spaceW + extraPerGap;
            if (x < cursor + gap) return "";
            cursor += gap;
        }
    }

    // Trailing whitespace region.
    return "";
}

std::string normalizeHitWord(const std::string& raw,
                             const litehtml::position* placement,
                             int docX) {
    std::string text = trimCopy(raw);
    if (text.empty()) return "";

    if (!containsWhitespace(text)) {
        return stripWordEdgePunct(text);
    }

    if (placement) {
        std::string picked = chooseWordByHitX(text, *placement, docX);
        // If pointer lands in inter-word/trailing whitespace, no word is selected.
        return picked;
    }

    std::vector<std::string> words = splitWordCandidates(text);
    words = removeLeadingNumericTokens(words);
    if (!words.empty()) return words.front();
    return stripWordEdgePunct(text);
}

std::shared_ptr<litehtml::element> findElementByIdRecursive(
        const std::shared_ptr<litehtml::element>& root,
        const std::string& id) {
    if (!root || id.empty()) return nullptr;

    const char* attrId = root->get_attr("id");
    if (attrId && id == attrId) {
        return root;
    }

    for (const auto& child : root->children()) {
        auto match = findElementByIdRecursive(child, id);
        if (match) return match;
    }

    return nullptr;
}

std::string normalizeInlineStyle(const std::string& style) {
    std::string normalized;
    size_t start = 0;
    while (start <= style.size()) {
        size_t end = style.find(';', start);
        if (end == std::string::npos) end = style.size();
        std::string decl = trimCopy(style.substr(start, end - start));
        if (!decl.empty()) {
            if (!normalized.empty()) normalized.push_back(';');
            normalized += decl;
        }
        if (end == style.size()) break;
        start = end + 1;
    }
    if (!normalized.empty()) normalized.push_back(';');
    return normalized;
}

std::string addInlineStyleSnippet(const std::string& currentStyle,
                                  const std::string& styleSnippet) {
    std::string normalizedCurrent = normalizeInlineStyle(currentStyle);
    const std::string normalizedSnippet = normalizeInlineStyle(styleSnippet);
    if (normalizedSnippet.empty()) return normalizedCurrent;
    if (normalizedCurrent.find(normalizedSnippet) != std::string::npos) {
        return normalizedCurrent;
    }
    normalizedCurrent += normalizedSnippet;
    return normalizedCurrent;
}

std::string removeInlineStyleSnippet(const std::string& currentStyle,
                                     const std::string& styleSnippet) {
    std::string normalizedCurrent = normalizeInlineStyle(currentStyle);
    const std::string normalizedSnippet = normalizeInlineStyle(styleSnippet);
    if (normalizedSnippet.empty() || normalizedCurrent.empty()) {
        return normalizedCurrent;
    }

    size_t pos = normalizedCurrent.find(normalizedSnippet);
    while (pos != std::string::npos) {
        normalizedCurrent.erase(pos, normalizedSnippet.size());
        pos = normalizedCurrent.find(normalizedSnippet);
    }
    return normalizeInlineStyle(normalizedCurrent);
}

bool updateElementTreeStyleSnippetRecursive(const std::shared_ptr<litehtml::element>& root,
                                            const std::string& styleSnippet,
                                            bool enable) {
    if (!root) return false;

    bool changed = false;
    const char* current = root->get_attr("style", "");
    const std::string currentStyle = current ? current : "";
    const std::string normalizedCurrent = normalizeInlineStyle(currentStyle);
    const std::string updatedStyle = enable
        ? addInlineStyleSnippet(currentStyle, styleSnippet)
        : removeInlineStyleSnippet(currentStyle, styleSnippet);
    if (updatedStyle != normalizedCurrent) {
        root->set_attr("style", updatedStyle.c_str());
        root->refresh_styles();
        root->compute_styles();
        changed = true;
    }

    for (const auto& child : root->children()) {
        changed = updateElementTreeStyleSnippetRecursive(child, styleSnippet, enable) || changed;
    }
    return changed;
}

int parseVerseNumberValue(const char* text) {
    if (!text || !*text) return 0;

    std::string value = trimCopy(text);
    if (value.empty()) return 0;

    if (value.size() > 1 && value[0] == 'v') {
        value.erase(value.begin());
    } else if (value.rfind("verse:", 0) == 0) {
        value.erase(0, 6);
    }

    if (value.empty()) return 0;
    for (char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
    }
    return std::stoi(value);
}

int parseParallelColumnValue(const char* text) {
    if (!text || !*text) return -1;

    std::string value = trimCopy(text);
    if (value.empty()) return -1;
    for (char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return -1;
    }
    return std::stoi(value);
}

int parallelColumnForElement(const std::shared_ptr<litehtml::element>& element) {
    for (auto cur = element; cur; cur = cur->parent()) {
        int column = parseParallelColumnValue(cur->get_attr("data-parallel-col"));
        if (column >= 0) return column;
    }
    return -1;
}

} // namespace

HtmlWidget::HtmlWidget(int X, int Y, int W, int H, const char* label)
    : Fl_Widget(X, Y, W, H, label) {
    // Create vertical scrollbar
    scrollbar_ = new Fl_Scrollbar(X + W - kScrollbarExtent, Y,
                                  kScrollbarExtent, H);
    scrollbar_->type(FL_VERTICAL);
    scrollbar_->callback(scrollbarCallback, this);
    scrollbar_->hide();

    hScrollbar_ = new Fl_Scrollbar(X, Y + H - kScrollbarExtent,
                                   W, kScrollbarExtent);
    hScrollbar_->type(FL_HORIZONTAL);
    hScrollbar_->callback(hScrollbarCallback, this);
    hScrollbar_->hide();

    // Default CSS — built-in defaults + minimal overrides
    masterCSS_ = std::string(litehtml::master_css) + "\n"
        "body { font-family: serif; font-size: 14px; }\n"
        "sup { font-size: 0.7em; vertical-align: super; }\n";
}

HtmlWidget::~HtmlWidget() {
    if (linkCallbackScheduled_) {
        Fl::remove_timeout(dispatchDeferredLink, this);
        linkCallbackScheduled_ = false;
    }
    if (reflowScheduled_) {
        Fl::remove_timeout(dispatchDeferredReflow, this);
        reflowScheduled_ = false;
    }

    // Destroy the document while callback targets (fonts_, etc.) are still live.
    doc_.reset();
    fonts_.clear();

    // The scrollbar is created while a parent Fl_Group is current, so FLTK owns
    // and destroys it with the group children list.
    scrollbar_ = nullptr;
    hScrollbar_ = nullptr;
}

void HtmlWidget::clearSelection() {
    selectionAnchor_ = SelectionPoint{};
    selectionFocus_ = SelectionPoint{};
    selectionParallelColumn_ = -1;
    selecting_ = false;
    dragSelecting_ = false;
}

int HtmlWidget::viewportWidth() const {
    int sbW = (scrollbar_ && scrollbar_->visible()) ? kScrollbarExtent : 0;
    return std::max(10, w() - sbW);
}

int HtmlWidget::viewportHeight() const {
    int sbH = (allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible())
                  ? kScrollbarExtent
                  : 0;
    return std::max(10, h() - sbH);
}

void HtmlWidget::setScrollX(int x) {
    int maxScroll = std::max(0, contentWidth_ - viewportWidth());
    scrollX_ = std::clamp(x, 0, maxScroll);
    if (allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible()) {
        hScrollbar_->value(scrollX_, viewportWidth(), 0, contentWidth_);
    }
    redraw();
}

bool HtmlWidget::isParallelDocument() const {
    return isParallel_;
}

void HtmlWidget::buildParallelColumnBoundaries() const {
    parallelColumnBoundaries_.clear();
    if (!doc_ || !doc_->root_render()) return;

    // Walk the render tree shallowly to find elements with data-parallel-col.
    // The structure is: html > body > div.parallel > div.parallel-row > div.parallel-col
    // We only need the first row's columns (all rows have the same column widths).
    struct StackEntry {
        std::shared_ptr<litehtml::render_item> ri;
        int depth;
    };
    std::vector<StackEntry> stack;
    stack.push_back({doc_->root_render(), 0});

    while (!stack.empty()) {
        auto [ri, depth] = stack.back();
        stack.pop_back();
        if (!ri || !ri->src_el() || depth > 6) continue;

        int col = parseParallelColumnValue(ri->src_el()->get_attr("data-parallel-col"));
        if (col >= 0) {
            // Check if this column index is already recorded
            bool found = false;
            for (const auto& b : parallelColumnBoundaries_) {
                if (b.column == col) { found = true; break; }
            }
            if (!found) {
                litehtml::position placement = ri->get_placement();
                parallelColumnBoundaries_.push_back({
                    static_cast<int>(placement.x),
                    static_cast<int>(placement.x + placement.width),
                    col
                });
            }
            continue;  // Don't recurse into column children
        }

        for (auto& child : ri->children()) {
            stack.push_back({child, depth + 1});
        }
    }

    std::sort(parallelColumnBoundaries_.begin(), parallelColumnBoundaries_.end(),
              [](const ParallelColumnBoundary& a, const ParallelColumnBoundary& b) {
                  return a.xStart < b.xStart;
              });
}

bool HtmlWidget::selectionPointLess(const SelectionPoint& lhs,
                                    const SelectionPoint& rhs) const {
    if (lhs.fragmentIndex != rhs.fragmentIndex) {
        return lhs.fragmentIndex < rhs.fragmentIndex;
    }
    return lhs.charIndex < rhs.charIndex;
}

bool HtmlWidget::selectionPointEqual(const SelectionPoint& lhs,
                                     const SelectionPoint& rhs) const {
    return lhs.valid == rhs.valid &&
           lhs.fragmentIndex == rhs.fragmentIndex &&
           lhs.charIndex == rhs.charIndex;
}

bool HtmlWidget::hasSelection() const {
    if (!selectionAnchor_.valid || !selectionFocus_.valid) return false;
    return selectionAnchor_.fragmentIndex != selectionFocus_.fragmentIndex ||
           selectionAnchor_.charIndex != selectionFocus_.charIndex;
}

int HtmlWidget::fragmentParallelColumn(int fragmentIndex) const {
    if (fragmentIndex < 0 ||
        fragmentIndex >= static_cast<int>(textFragments_.size())) {
        return -1;
    }
    return textFragments_[fragmentIndex].parallelColumn;
}

bool HtmlWidget::fragmentMatchesSelectionColumn(int fragmentIndex) const {
    if (selectionParallelColumn_ < 0) return true;
    return fragmentParallelColumn(fragmentIndex) == selectionParallelColumn_;
}

bool HtmlWidget::isWordCharAt(int fragmentIndex, int charIndex) const {
    if (fragmentIndex < 0 ||
        fragmentIndex >= static_cast<int>(textFragments_.size())) {
        return false;
    }

    const TextFragment& fragment = textFragments_[fragmentIndex];
    int charCount = static_cast<int>(fragment.byteOffsets.size()) - 1;
    if (charIndex < 0 || charIndex >= charCount) return false;

    int byteStart = fragment.byteOffsets[charIndex];
    int byteEnd = fragment.byteOffsets[charIndex + 1];
    if (byteEnd <= byteStart ||
        byteStart < 0 ||
        byteEnd > static_cast<int>(fragment.text.size())) {
        return false;
    }

    unsigned char first = static_cast<unsigned char>(fragment.text[byteStart]);
    return first >= 0x80 ||
           std::isalnum(first) ||
           first == '\'' ||
           first == '-';
}

HtmlWidget::SelectionPoint HtmlWidget::nextSelectionPoint(
        const SelectionPoint& point) const {
    if (!point.valid) return SelectionPoint{};

    SelectionPoint next = point;
    while (next.fragmentIndex >= 0 &&
           next.fragmentIndex < static_cast<int>(textFragments_.size())) {
        if (!fragmentMatchesSelectionColumn(next.fragmentIndex)) {
            ++next.fragmentIndex;
            next.charIndex = 0;
            next.valid = next.fragmentIndex < static_cast<int>(textFragments_.size());
            continue;
        }
        const TextFragment& fragment = textFragments_[next.fragmentIndex];
        int charCount = static_cast<int>(fragment.byteOffsets.size()) - 1;
        if (next.charIndex < charCount) {
            ++next.charIndex;
            next.valid = true;
            return next;
        }

        ++next.fragmentIndex;
        next.charIndex = 0;
        next.valid = next.fragmentIndex < static_cast<int>(textFragments_.size());
    }

    return SelectionPoint{};
}

HtmlWidget::SelectionPoint HtmlWidget::previousSelectionPoint(
        const SelectionPoint& point) const {
    if (!point.valid) return SelectionPoint{};

    SelectionPoint prev = point;
    if (prev.charIndex > 0) {
        --prev.charIndex;
        prev.valid = true;
        return prev;
    }

    --prev.fragmentIndex;
    while (prev.fragmentIndex >= 0) {
        if (!fragmentMatchesSelectionColumn(prev.fragmentIndex)) {
            --prev.fragmentIndex;
            continue;
        }
        const TextFragment& fragment = textFragments_[prev.fragmentIndex];
        int charCount = static_cast<int>(fragment.byteOffsets.size()) - 1;
        prev.charIndex = std::max(0, charCount);
        prev.valid = true;
        return prev;
    }

    return SelectionPoint{};
}

bool HtmlWidget::screenPointForSelectionBoundary(const SelectionPoint& point,
                                                 bool preferPreviousChar,
                                                 int& screenX,
                                                 int& screenY) const {
    if (!point.valid ||
        point.fragmentIndex < 0 ||
        point.fragmentIndex >= static_cast<int>(textFragments_.size())) {
        return false;
    }

    const TextFragment& fragment = textFragments_[point.fragmentIndex];
    int charCount = static_cast<int>(fragment.byteOffsets.size()) - 1;
    if (charCount <= 0) return false;

    if (preferPreviousChar && point.charIndex <= 0) {
        SelectionPoint prev = previousSelectionPoint(point);
        if (prev.valid) {
            return screenPointForSelectionBoundary(prev, true, screenX, screenY);
        }
    }

    int drawChar = point.charIndex;
    if (preferPreviousChar) {
        if (drawChar > 0) {
            --drawChar;
        } else {
            drawChar = 0;
        }
    } else if (drawChar >= charCount) {
        drawChar = charCount - 1;
    }

    drawChar = std::clamp(drawChar, 0, charCount - 1);
    int startX = fragment.xOffsets[drawChar];
    int endX = fragment.xOffsets[drawChar + 1];
    screenX = static_cast<int>(fragment.pos.x) +
              startX + std::max(0, (endX - startX) / 2);
    screenY = static_cast<int>(fragment.pos.y + fragment.pos.height / 2);
    return true;
}

HtmlWidget::SelectionPoint HtmlWidget::hitTestSelectionPoint(int screenX,
                                                             int screenY,
                                                             int requiredParallelColumn) const {
    SelectionPoint best;
    if (textFragments_.empty()) return best;

    long bestScore = std::numeric_limits<long>::max();

    for (size_t i = 0; i < textFragments_.size(); ++i) {
        const TextFragment& fragment = textFragments_[i];
        if (fragment.byteOffsets.empty() || fragment.xOffsets.empty()) continue;
        if (requiredParallelColumn >= 0 &&
            fragment.parallelColumn != requiredParallelColumn) {
            continue;
        }

        int left = static_cast<int>(fragment.pos.x);
        int right = static_cast<int>(fragment.pos.x + fragment.pos.width);
        int top = static_cast<int>(fragment.pos.y);
        int bottom = static_cast<int>(fragment.pos.y + fragment.pos.height);

        int verticalDistance = 0;
        if (screenY < top) verticalDistance = top - screenY;
        else if (screenY > bottom) verticalDistance = screenY - bottom;

        int horizontalDistance = 0;
        if (screenX < left) horizontalDistance = left - screenX;
        else if (screenX > right) horizontalDistance = screenX - right;

        long score = static_cast<long>(verticalDistance) * 100000L +
                     static_cast<long>(horizontalDistance) * 100L +
                     static_cast<long>(i);
        if (score >= bestScore) continue;

        int charCount = static_cast<int>(fragment.byteOffsets.size()) - 1;
        int charIndex = 0;
        if (screenX <= left) {
            charIndex = 0;
        } else if (screenX >= right) {
            charIndex = charCount;
        } else {
            int localX = screenX - left;
            charIndex = charCount;
            for (int ch = 0; ch < charCount; ++ch) {
                int startX = fragment.xOffsets[ch];
                int endX = fragment.xOffsets[ch + 1];
                int midX = startX + ((endX - startX) / 2);
                if (localX < midX) {
                    charIndex = ch;
                    break;
                }
            }
        }

        best.fragmentIndex = static_cast<int>(i);
        best.charIndex = std::clamp(charIndex, 0, charCount);
        best.valid = true;
        bestScore = score;
    }

    return best;
}

bool HtmlWidget::hitTextFragmentAtPoint(int screenX, int screenY,
                                        int& fragmentIndex,
                                        int& charIndex) const {
    fragmentIndex = -1;
    charIndex = -1;
    if (textFragments_.empty()) return false;

    long bestArea = std::numeric_limits<long>::max();
    for (size_t i = 0; i < textFragments_.size(); ++i) {
        const TextFragment& fragment = textFragments_[i];
        if (fragment.byteOffsets.size() < 2 || fragment.xOffsets.size() < 2) {
            continue;
        }

        int left = static_cast<int>(fragment.pos.x);
        int right = static_cast<int>(fragment.pos.x + fragment.pos.width);
        int top = static_cast<int>(fragment.pos.y);
        int bottom = static_cast<int>(fragment.pos.y + fragment.pos.height);
        if (screenX < left || screenX >= right ||
            screenY < top || screenY >= bottom) {
            continue;
        }

        int localX = screenX - left;
        int chars = static_cast<int>(fragment.byteOffsets.size()) - 1;
        int hitChar = -1;
        for (int ch = 0; ch < chars; ++ch) {
            int startX = fragment.xOffsets[ch];
            int endX = fragment.xOffsets[ch + 1];
            if (localX >= startX && localX < endX) {
                hitChar = ch;
                break;
            }
        }
        if (hitChar < 0 && chars > 0 && localX == fragment.xOffsets.back()) {
            hitChar = chars - 1;
        }
        if (hitChar < 0) continue;

        long area = static_cast<long>(std::max(1, right - left)) *
                    static_cast<long>(std::max(1, bottom - top));
        if (area >= bestArea) continue;

        bestArea = area;
        fragmentIndex = static_cast<int>(i);
        charIndex = hitChar;
    }

    return fragmentIndex >= 0 && charIndex >= 0;
}

std::string HtmlWidget::fragmentWordAt(int fragmentIndex, int charIndex) const {
    if (fragmentIndex < 0 ||
        fragmentIndex >= static_cast<int>(textFragments_.size())) {
        return "";
    }

    const TextFragment& fragment = textFragments_[fragmentIndex];
    int charCount = static_cast<int>(fragment.byteOffsets.size()) - 1;
    if (charIndex < 0 || charIndex >= charCount) return "";

    if (!isWordCharAt(fragmentIndex, charIndex)) return "";

    int startChar = charIndex;
    while (startChar > 0 && isWordCharAt(fragmentIndex, startChar - 1)) {
        --startChar;
    }

    int endChar = charIndex + 1;
    while (endChar < charCount && isWordCharAt(fragmentIndex, endChar)) {
        ++endChar;
    }

    int byteStart = fragment.byteOffsets[startChar];
    int byteEnd = fragment.byteOffsets[endChar];
    if (byteEnd <= byteStart) return "";

    return stripWordEdgePunct(
        fragment.text.substr(byteStart, byteEnd - byteStart));
}

std::string HtmlWidget::wordAtScreenPoint(int screenX, int screenY) const {
    int fragmentIndex = -1;
    int charIndex = -1;
    if (!hitTextFragmentAtPoint(screenX, screenY, fragmentIndex, charIndex)) {
        return "";
    }
    return fragmentWordAt(fragmentIndex, charIndex);
}

int HtmlWidget::verseAtScreenPoint(int screenX, int screenY) const {
    if (!doc_ || !doc_->root_render()) return 0;

    int docX = screenX - x() + scrollX_;
    int docY = screenY - y() + scrollY_;
    auto el = doc_->root_render()->get_element_by_point(
        docX, docY, docX, docY,
        [](const std::shared_ptr<litehtml::render_item>&) { return true; });
    if (!el) return 0;

    HitElement hit = findDeepestElementAtPoint(el, docX, docY);
    if (hit.element) el = hit.element;

    for (auto cur = el; cur; cur = cur->parent()) {
        if (int verse = parseVerseNumberValue(cur->get_attr("id"))) {
            return verse;
        }
        if (int verse = parseVerseNumberValue(cur->get_attr("href"))) {
            return verse;
        }
    }

    return 0;
}

bool HtmlWidget::fragmentSelectionRange(int fragmentIndex,
                                        int& startChar,
                                        int& endChar) const {
    startChar = 0;
    endChar = 0;
    if (!hasSelection() ||
        fragmentIndex < 0 ||
        fragmentIndex >= static_cast<int>(textFragments_.size())) {
        return false;
    }
    if (!fragmentMatchesSelectionColumn(fragmentIndex)) return false;

    SelectionPoint start = selectionAnchor_;
    SelectionPoint end = selectionFocus_;
    if (selectionPointLess(end, start)) std::swap(start, end);

    if (fragmentIndex < start.fragmentIndex || fragmentIndex > end.fragmentIndex) {
        return false;
    }

    const TextFragment& fragment = textFragments_[fragmentIndex];
    int charCount = static_cast<int>(fragment.byteOffsets.size()) - 1;
    startChar = 0;
    endChar = charCount;
    if (fragmentIndex == start.fragmentIndex) startChar = start.charIndex;
    if (fragmentIndex == end.fragmentIndex) endChar = end.charIndex;

    startChar = std::clamp(startChar, 0, charCount);
    endChar = std::clamp(endChar, 0, charCount);
    return endChar > startChar;
}

std::string HtmlWidget::selectedText() const {
    if (!hasSelection()) return "";

    SelectionPoint start = selectionAnchor_;
    SelectionPoint end = selectionFocus_;
    if (selectionPointLess(end, start)) std::swap(start, end);

    return selectedTextBetween(start, end);
}

std::string HtmlWidget::selectedTextBetween(SelectionPoint start,
                                            SelectionPoint end) const {
    if (!start.valid || !end.valid || selectionPointEqual(start, end)) {
        return "";
    }
    if (selectionPointLess(end, start)) std::swap(start, end);

    std::string out;
    int prevFragmentIndex = -1;
    int prevEndChar = 0;

    for (int i = start.fragmentIndex;
         i <= end.fragmentIndex &&
         i < static_cast<int>(textFragments_.size());
         ++i) {
        const TextFragment& fragment = textFragments_[i];
        int charStart = 0;
        int charEnd = 0;
        if (!fragmentSelectionRange(i, charStart, charEnd)) continue;

        int byteStart = fragment.byteOffsets[charStart];
        int byteEnd = fragment.byteOffsets[charEnd];
        if (byteEnd <= byteStart) continue;

        std::string part = fragment.text.substr(byteStart, byteEnd - byteStart);
        if (part.empty()) continue;

        if (!out.empty() && prevFragmentIndex >= 0 &&
            prevFragmentIndex < static_cast<int>(textFragments_.size())) {
            const TextFragment& prev = textFragments_[prevFragmentIndex];
            int prevLineThreshold =
                std::max(static_cast<int>(prev.pos.height),
                         static_cast<int>(fragment.pos.height)) / 2;
            bool newLine =
                std::abs(static_cast<int>(fragment.pos.y - prev.pos.y)) > prevLineThreshold;
            if (newLine) {
                if (out.back() != '\n') out.push_back('\n');
            } else {
                int prevEndX = static_cast<int>(prev.pos.x) + prev.xOffsets[prevEndChar];
                int curStartX = static_cast<int>(fragment.pos.x) + fragment.xOffsets[charStart];
                if (curStartX > prevEndX + 3 &&
                    !std::isspace(static_cast<unsigned char>(out.back())) &&
                    !std::isspace(static_cast<unsigned char>(part.front()))) {
                    out.push_back(' ');
                }
            }
        }

        out += part;
        prevFragmentIndex = i;
        prevEndChar = charEnd;
    }

    return out;
}

HtmlWidget::SelectionInfo HtmlWidget::selectionInfo() const {
    SelectionInfo info;
    if (!hasSelection()) return info;

    SelectionPoint start = selectionAnchor_;
    SelectionPoint end = selectionFocus_;
    if (selectionPointLess(end, start)) std::swap(start, end);

    info.hasSelection = true;
    info.text = selectedTextBetween(start, end);

    SelectionPoint prevStart = previousSelectionPoint(start);
    info.startsInsideWord =
        start.valid &&
        prevStart.valid &&
        isWordCharAt(start.fragmentIndex, start.charIndex) &&
        isWordCharAt(prevStart.fragmentIndex, prevStart.charIndex);

    SelectionPoint prevEnd = previousSelectionPoint(end);
    info.endsInsideWord =
        end.valid &&
        prevEnd.valid &&
        isWordCharAt(prevEnd.fragmentIndex, prevEnd.charIndex) &&
        isWordCharAt(end.fragmentIndex, end.charIndex);

    SelectionPoint wordStart = start;
    if (info.startsInsideWord) {
        while (!selectionPointEqual(wordStart, end) &&
               isWordCharAt(wordStart.fragmentIndex, wordStart.charIndex)) {
            SelectionPoint next = nextSelectionPoint(wordStart);
            if (!next.valid) break;
            wordStart = next;
        }
        while (!selectionPointEqual(wordStart, end) &&
               !isWordCharAt(wordStart.fragmentIndex, wordStart.charIndex)) {
            SelectionPoint next = nextSelectionPoint(wordStart);
            if (!next.valid) break;
            wordStart = next;
        }
    }

    SelectionPoint wordEnd = end;
    if (info.endsInsideWord) {
        while (true) {
            SelectionPoint prev = previousSelectionPoint(wordEnd);
            if (!prev.valid ||
                !isWordCharAt(prev.fragmentIndex, prev.charIndex)) {
                break;
            }
            wordEnd = prev;
        }
    }

    if (!selectionPointEqual(wordStart, wordEnd) &&
        !selectionPointLess(wordEnd, wordStart)) {
        info.wholeWordText = selectedTextBetween(wordStart, wordEnd);
    }

    int hitX = 0;
    int hitY = 0;
    if (screenPointForSelectionBoundary(start, false, hitX, hitY)) {
        info.startVerse = verseAtScreenPoint(hitX, hitY);
    }
    if (screenPointForSelectionBoundary(end, true, hitX, hitY)) {
        info.endVerse = verseAtScreenPoint(hitX, hitY);
    }
    if (info.startVerse == 0) info.startVerse = info.endVerse;
    if (info.endVerse == 0) info.endVerse = info.startVerse;

    return info;
}

bool HtmlWidget::copySelectionToClipboard() {
    std::string text = selectedText();
    if (text.empty()) return false;
    Fl::copy(text.c_str(), static_cast<int>(text.size()), 0);
    Fl::copy(text.c_str(), static_cast<int>(text.size()), 1);
    return true;
}

void HtmlWidget::setHtml(const std::string& html, const std::string& baseUrl) {
    perf::ScopeTimer timer("HtmlWidget::setHtml");
    if (reflowScheduled_) {
        Fl::remove_timeout(dispatchDeferredReflow, this);
        reflowScheduled_ = false;
    }

    currentHtml_ = html;
    isParallel_ = html.find("class=\"parallel\"") != std::string::npos;
    parallelBoundariesComputed_ = false;
    parallelColumnBoundaries_.clear();
    baseUrl_ = baseUrl;
    scrollX_ = 0;
    scrollY_ = 0;
    contentWidth_ = 0;
    contentHeight_ = 0;
    lastHoverWord_.clear();
    lastHoverHref_.clear();
    lastHoverStrong_.clear();
    lastHoverMorph_.clear();
    lastHoverModule_.clear();
    lastHoverTitle_.clear();
    tooltip(nullptr);
    clearSelection();
    textFragments_.clear();
    textWidthCache_.clear();
    textWidthCacheEntries_ = 0;
    textWidthCacheHits_ = 0;
    textWidthCacheMisses_ = 0;
    textWidthCacheStores_ = 0;

    // Wrap in basic HTML if not already
    std::string fullHtml = html;
    if (html.find("<html") == std::string::npos &&
        html.find("<HTML") == std::string::npos) {
        fullHtml = "<html><body>" + html + "</body></html>";
    }

    perf::StepTimer step;
    // Create litehtml document
    doc_ = litehtml::document::createFromString(
        fullHtml, this, masterCSS_, styleOverrideCSS_);
    perf::logf("HtmlWidget::setHtml createFromString: %.3f ms", step.elapsedMs());
    step.reset();

    // Predict scrollbar visibility before the first render so that
    // viewportWidth() returns the correct width on the first pass.
    // Bible/commentary content (> 500 bytes) virtually always needs a
    // scrollbar; short content (error messages) never does.  This avoids
    // the re-render loop inside updateScrollbar that otherwise toggles
    // the scrollbar and re-renders until stable.
    if (html.size() > 500) {
        scrollbar_->show();
        scrollbar_->resize(x() + w() - kScrollbarExtent, y(),
                           kScrollbarExtent, h());
    } else {
        scrollbar_->hide();
    }

    renderDocument();
    perf::logf("HtmlWidget::setHtml renderDocument: %.3f ms", step.elapsedMs());
    step.reset();
    updateScrollbar(true);
    perf::logf("HtmlWidget::setHtml updateScrollbar: %.3f ms", step.elapsedMs());
    perf::logf("HtmlWidget::setHtml textWidthCache: hits=%zu misses=%zu stores=%zu entries=%zu",
               textWidthCacheHits_,
               textWidthCacheMisses_,
               textWidthCacheStores_,
               textWidthCacheEntries_);
    redraw();
}

void HtmlWidget::setMasterCSS(const std::string& css) {
    // Prepend litehtml's built-in user-agent styles so standard HTML elements
    // (b, i, div, p, hr, etc.) get proper defaults before our overrides.
    masterCSS_ = std::string(litehtml::master_css) + "\n" + css;
}

void HtmlWidget::setStyleOverrideCss(const std::string& css) {
    if (styleOverrideCSS_ == css) return;
    styleOverrideCSS_ = css;

    if (!currentHtml_.empty()) {
        int oldScrollX = scrollX_;
        int oldScroll = scrollY_;
        std::string html = currentHtml_;
        std::string base = baseUrl_;
        setHtml(html, base);
        setScrollX(oldScrollX);
        setScrollY(oldScroll);
    } else {
        redraw();
    }
}

void HtmlWidget::setAllowHorizontalScroll(bool allow) {
    if (allowHorizontalScroll_ == allow) return;
    allowHorizontalScroll_ = allow;
    if (!allowHorizontalScroll_) {
        scrollX_ = 0;
        if (hScrollbar_) hScrollbar_->hide();
    }

    if (doc_) {
        renderDocument();
        updateScrollbar(true);
    } else {
        redraw();
    }
}

void HtmlWidget::updateElementClassById(const std::string& removeId,
                                        const std::string& addId,
                                        const std::string& className,
                                        bool relayout) {
    if (!doc_ || className.empty()) return;

    auto root = doc_->root();
    if (!root) return;

    bool changed = false;
    if (!removeId.empty()) {
        if (auto el = findElementByIdRecursive(root, removeId)) {
            changed = el->set_class(className.c_str(), false) || changed;
        }
    }
    if (!addId.empty()) {
        if (auto el = findElementByIdRecursive(root, addId)) {
            changed = el->set_class(className.c_str(), true) || changed;
        }
    }
    if (!changed) return;

    int oldScrollX = scrollX_;
    int oldScroll = scrollY_;
    int oldContentWidth = contentWidth_;
    int oldContentHeight = contentHeight_;
    root->refresh_styles();
    root->compute_styles();
    renderDocument();
    if (!relayout) {
        redraw();
        return;
    }
    if (contentWidth_ != oldContentWidth || contentHeight_ != oldContentHeight) {
        updateScrollbar(true);
    }
    setScrollX(oldScrollX);
    setScrollY(oldScroll);
    redraw();
}

void HtmlWidget::updateElementStyleById(const std::string& removeId,
                                        const std::string& removeStyle,
                                        const std::string& addId,
                                        const std::string& addStyle,
                                        bool relayout) {
    if (!doc_) return;

    auto root = doc_->root();
    if (!root) return;

    bool changed = false;
    auto updateStyle = [&](const std::string& id, const std::string& style) {
        if (id.empty()) return;
        if (auto el = findElementByIdRecursive(root, id)) {
            const char* current = el->get_attr("style", "");
            std::string currentStyle = current ? current : "";
            if (currentStyle == style) return;
            el->set_attr("style", style.c_str());
            el->refresh_styles();
            el->compute_styles();
            changed = true;
        }
    };

    updateStyle(removeId, removeStyle);
    updateStyle(addId, addStyle);
    if (!changed) return;

    int oldScrollX = scrollX_;
    int oldScroll = scrollY_;
    int oldContentWidth = contentWidth_;
    int oldContentHeight = contentHeight_;
    if (relayout) {
        renderDocument();
        if (contentWidth_ != oldContentWidth || contentHeight_ != oldContentHeight) {
            updateScrollbar(true);
        }
        setScrollX(oldScrollX);
        setScrollY(oldScroll);
    }
    redraw();
}

void HtmlWidget::updateElementTreeStyleSnippetById(const std::string& removeId,
                                                   const std::string& addId,
                                                   const std::string& styleSnippet,
                                                   bool relayout) {
    if (!doc_ || styleSnippet.empty()) return;

    auto root = doc_->root();
    if (!root) return;

    bool changed = false;
    auto updateTree = [&](const std::string& id, bool enable) {
        if (id.empty()) return;
        if (auto el = findElementByIdRecursive(root, id)) {
            changed = updateElementTreeStyleSnippetRecursive(el, styleSnippet, enable) || changed;
        }
    };

    updateTree(removeId, false);
    updateTree(addId, true);
    if (!changed) return;

    int oldScrollX = scrollX_;
    int oldScroll = scrollY_;
    int oldContentWidth = contentWidth_;
    int oldContentHeight = contentHeight_;
    if (relayout) {
        renderDocument();
        if (contentWidth_ != oldContentWidth || contentHeight_ != oldContentHeight) {
            updateScrollbar(true);
        }
        setScrollX(oldScrollX);
        setScrollY(oldScroll);
    }
    redraw();
}

void HtmlWidget::scrollToAnchor(const std::string& anchor) {
    if (!doc_ || anchor.empty()) return;

    std::string id = anchor;
    if (!id.empty() && id.front() == '#') {
        id.erase(id.begin());
    }
    if (id.empty()) return;

    std::shared_ptr<litehtml::element> target;
    if (auto root = doc_->root()) {
        // Prefer CSS selector lookup first, then recursive fallback.
        target = root->select_one("#" + id);
        if (!target) {
            target = findElementByIdRecursive(root, id);
        }
    }
    if (!target) return;

    litehtml::position placement = target->get_placement();
    int targetY = std::max(0, static_cast<int>(placement.y) - 2);
    setScrollY(targetY);
}

void HtmlWidget::scrollToTop() {
    int oldScrollY = scrollY_;
    scrollX_ = 0;
    scrollY_ = 0;
    if (allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible()) {
        hScrollbar_->value(0);
    }
    if (scrollbar_->visible()) {
        scrollbar_->value(0);
    }
    notifyScrollChanged(oldScrollY);
    redraw();
}

void HtmlWidget::setScrollY(int y) {
    int oldScrollY = scrollY_;
    int maxScroll = std::max(0, contentHeight_ - viewportHeight());
    scrollY_ = std::clamp(y, 0, maxScroll);
    if (scrollbar_->visible()) {
        scrollbar_->value(scrollY_, viewportHeight(), 0, contentHeight_);
    }
    notifyScrollChanged(oldScrollY);
    redraw();
}

int HtmlWidget::viewportHeightPixels() const {
    return viewportHeight();
}

int HtmlWidget::elementTopById(const std::string& id) const {
    if (!doc_ || id.empty()) return -1;

    std::shared_ptr<litehtml::element> target;
    if (auto root = doc_->root()) {
        target = root->select_one("#" + id);
        if (!target) {
            target = findElementByIdRecursive(root, id);
        }
    }
    if (!target) return -1;
    return std::max(0, static_cast<int>(target->get_placement().y));
}

HtmlWidget::Snapshot HtmlWidget::captureSnapshot() const {
    Snapshot snapshot;
    snapshot.doc = std::static_pointer_cast<void>(doc_);
    snapshot.html = currentHtml_;
    snapshot.baseUrl = baseUrl_;
    snapshot.scrollX = scrollX_;
    snapshot.scrollY = scrollY_;
    snapshot.contentWidth = contentWidth_;
    snapshot.contentHeight = contentHeight_;
    snapshot.renderWidth = lastRenderWidth_;
    snapshot.hScrollbarVisible = hScrollbar_ && hScrollbar_->visible();
    snapshot.scrollbarVisible = scrollbar_ && scrollbar_->visible();
    snapshot.valid = (doc_ != nullptr || !currentHtml_.empty());
    return snapshot;
}

HtmlWidget::Snapshot HtmlWidget::takeSnapshot() {
    Snapshot snapshot;
    snapshot.doc = std::static_pointer_cast<void>(doc_);
    snapshot.html = std::move(currentHtml_);
    snapshot.baseUrl = std::move(baseUrl_);
    snapshot.scrollX = scrollX_;
    snapshot.scrollY = scrollY_;
    snapshot.contentWidth = contentWidth_;
    snapshot.contentHeight = contentHeight_;
    snapshot.renderWidth = lastRenderWidth_;
    snapshot.hScrollbarVisible = hScrollbar_ && hScrollbar_->visible();
    snapshot.scrollbarVisible = scrollbar_ && scrollbar_->visible();
    snapshot.valid = (doc_ != nullptr || !snapshot.html.empty());

    doc_.reset();
    currentHtml_.clear();
    baseUrl_.clear();
    scrollX_ = 0;
    scrollY_ = 0;
    contentWidth_ = 0;
    contentHeight_ = 0;
    lastRenderWidth_ = 0;
    lastHoverWord_.clear();
    lastHoverHref_.clear();
    lastHoverStrong_.clear();
    lastHoverMorph_.clear();
    lastHoverModule_.clear();
    lastHoverTitle_.clear();
    tooltip(nullptr);
    clearSelection();
    textFragments_.clear();
    textWidthCache_.clear();
    textWidthCacheEntries_ = 0;
    textWidthCacheHits_ = 0;
    textWidthCacheMisses_ = 0;
    textWidthCacheStores_ = 0;
    updateScrollbar();

    return snapshot;
}

void HtmlWidget::restoreSnapshot(const Snapshot& snapshot) {
    perf::ScopeTimer timer("HtmlWidget::restoreSnapshot(copy)");
    if (reflowScheduled_) {
        Fl::remove_timeout(dispatchDeferredReflow, this);
        reflowScheduled_ = false;
    }

    doc_ = std::static_pointer_cast<litehtml::document>(snapshot.doc);
    currentHtml_ = snapshot.html;
    isParallel_ = currentHtml_.find("class=\"parallel\"") != std::string::npos;
    baseUrl_ = snapshot.baseUrl;
    scrollX_ = snapshot.scrollX;
    scrollY_ = snapshot.scrollY;
    contentWidth_ = snapshot.contentWidth;
    contentHeight_ = snapshot.contentHeight;
    lastRenderWidth_ = snapshot.renderWidth;
    lastHoverWord_.clear();
    lastHoverHref_.clear();
    lastHoverStrong_.clear();
    lastHoverMorph_.clear();
    lastHoverModule_.clear();
    lastHoverTitle_.clear();
    tooltip(nullptr);
    clearSelection();
    textFragments_.clear();

    bool reusedLayout = false;
    if (doc_) {
        const int sbW = kScrollbarExtent;
        const bool snapSbVisible = snapshot.scrollbarVisible;
        const bool desiredVisible = (contentHeight_ > h());
        const int widthWithSnapSb = std::max(10, w() - (snapSbVisible ? sbW : 0));
        const int widthWithDesiredSb = std::max(10, w() - (desiredVisible ? sbW : 0));

        // Fast path: reuse cached layout when the render width still matches.
        // Accept a match against either the snapshot's scrollbar state or the
        // currently desired scrollbar state -- this covers the common case
        // where widget height changed slightly (toggling scrollbar need) but
        // widget width stayed the same.
        if (!allowHorizontalScroll_ && snapshot.renderWidth > 0 &&
            (snapshot.renderWidth == widthWithDesiredSb ||
             (snapshot.renderWidth == widthWithSnapSb &&
              desiredVisible == snapSbVisible))) {
            reusedLayout = true;
            if (desiredVisible) {
                scrollbar_->show();
                scrollbar_->resize(x() + w() - sbW, y(), sbW, viewportHeight());
                scrollbar_->linesize(20);
                scrollbar_->value(scrollY_, viewportHeight(), 0,
                                  std::max(viewportHeight(), contentHeight_));
            } else {
                scrollbar_->hide();
            }
        }
        perf::logf("HtmlWidget::restoreSnapshot(copy) fastPath=%d snapW=%d widthSnapSb=%d widthDesiredSb=%d snapSB=%d desiredSB=%d",
                   reusedLayout ? 1 : 0,
                   snapshot.renderWidth,
                   widthWithSnapSb,
                   widthWithDesiredSb,
                   snapSbVisible ? 1 : 0,
                   desiredVisible ? 1 : 0);
    }
    if (!reusedLayout) {
        if (doc_) {
            renderDocument();
        }
        updateScrollbar(true);
    }
    setScrollX(scrollX_);
    setScrollY(scrollY_);
}

void HtmlWidget::restoreSnapshot(Snapshot&& snapshot) {
    perf::ScopeTimer timer("HtmlWidget::restoreSnapshot(move)");
    if (reflowScheduled_) {
        Fl::remove_timeout(dispatchDeferredReflow, this);
        reflowScheduled_ = false;
    }

    doc_ = std::static_pointer_cast<litehtml::document>(snapshot.doc);
    currentHtml_ = std::move(snapshot.html);
    isParallel_ = currentHtml_.find("class=\"parallel\"") != std::string::npos;
    baseUrl_ = std::move(snapshot.baseUrl);
    scrollX_ = snapshot.scrollX;
    scrollY_ = snapshot.scrollY;
    contentWidth_ = snapshot.contentWidth;
    contentHeight_ = snapshot.contentHeight;
    lastRenderWidth_ = snapshot.renderWidth;
    const bool snapSbVisible = snapshot.scrollbarVisible;
    lastHoverWord_.clear();
    lastHoverHref_.clear();
    lastHoverStrong_.clear();
    lastHoverMorph_.clear();
    lastHoverModule_.clear();
    lastHoverTitle_.clear();
    clearSelection();
    textFragments_.clear();

    snapshot.doc.reset();
    snapshot.scrollX = 0;
    snapshot.scrollY = 0;
    snapshot.contentWidth = 0;
    snapshot.contentHeight = 0;
    snapshot.renderWidth = 0;
    snapshot.hScrollbarVisible = false;
    snapshot.scrollbarVisible = false;
    snapshot.valid = false;

    bool reusedLayout = false;
    if (doc_) {
        const int sbW = kScrollbarExtent;
        const bool desiredVisible = (contentHeight_ > h());
        const int widthWithSnapSb = std::max(10, w() - (snapSbVisible ? sbW : 0));
        const int widthWithDesiredSb = std::max(10, w() - (desiredVisible ? sbW : 0));

        // Fast path: reuse cached layout when the render width still matches.
        if (!allowHorizontalScroll_ && lastRenderWidth_ > 0 &&
            (lastRenderWidth_ == widthWithDesiredSb ||
             (lastRenderWidth_ == widthWithSnapSb &&
              desiredVisible == snapSbVisible))) {
            reusedLayout = true;
            if (desiredVisible) {
                scrollbar_->show();
                scrollbar_->resize(x() + w() - sbW, y(), sbW, viewportHeight());
                scrollbar_->linesize(20);
                scrollbar_->value(scrollY_, viewportHeight(), 0,
                                  std::max(viewportHeight(), contentHeight_));
            } else {
                scrollbar_->hide();
            }
        }
        perf::logf("HtmlWidget::restoreSnapshot(move) fastPath=%d snapW=%d widthSnapSb=%d widthDesiredSb=%d snapSB=%d desiredSB=%d",
                   reusedLayout ? 1 : 0,
                   lastRenderWidth_,
                   widthWithSnapSb,
                   widthWithDesiredSb,
                   snapSbVisible ? 1 : 0,
                   desiredVisible ? 1 : 0);
    }
    if (!reusedLayout) {
        if (doc_) {
            renderDocument();
        }
        updateScrollbar(true);
    }
    setScrollX(scrollX_);
    setScrollY(scrollY_);
}

void HtmlWidget::renderDocument() {
    perf::ScopeTimer timer("HtmlWidget::renderDocument");
    if (!doc_) return;

    int renderWidth = viewportWidth();
    lastRenderWidth_ = renderWidth;

    doc_->render(renderWidth);
    contentWidth_ = std::max(0, static_cast<int>(doc_->width()));
    contentHeight_ = std::max(0, static_cast<int>(doc_->height()));
}

void HtmlWidget::updateScrollbar(bool layoutFresh) {
    perf::ScopeTimer timer("HtmlWidget::updateScrollbar");
    if (!doc_) {
        scrollbar_->hide();
        if (hScrollbar_) hScrollbar_->hide();
        scrollX_ = 0;
        scrollY_ = 0;
        return;
    }

    bool needV = scrollbar_ && scrollbar_->visible();
    bool needH = allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible();
    auto applyVisibility = [&](bool vertical, bool horizontal) {
        if (vertical) scrollbar_->show();
        else scrollbar_->hide();

        if (allowHorizontalScroll_ && horizontal) hScrollbar_->show();
        else if (hScrollbar_) hScrollbar_->hide();
    };

    if (!layoutFresh) {
        applyVisibility(needV, needH);
        renderDocument();
    }

    for (int iter = 0; iter < 4; ++iter) {
        bool newNeedH = allowHorizontalScroll_ &&
                        contentWidth_ > viewportWidth();
        bool newNeedV = contentHeight_ > viewportHeight();
        if (newNeedH == needH && newNeedV == needV) {
            needH = newNeedH;
            needV = newNeedV;
            break;
        }
        needH = newNeedH;
        needV = newNeedV;
        applyVisibility(needV, needH);
        renderDocument();
    }

    applyVisibility(needV, needH);

    int viewW = viewportWidth();
    int viewH = viewportHeight();

    if (scrollbar_->visible()) {
        scrollbar_->resize(x() + w() - kScrollbarExtent, y(),
                           kScrollbarExtent, viewH);
        scrollbar_->linesize(20);
        scrollbar_->value(scrollY_, viewH, 0, std::max(viewH, contentHeight_));
    } else {
        scrollY_ = 0;
    }

    if (allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible()) {
        hScrollbar_->resize(x(), y() + h() - kScrollbarExtent,
                            viewW, kScrollbarExtent);
        hScrollbar_->linesize(20);
        hScrollbar_->value(scrollX_, viewW, 0, std::max(viewW, contentWidth_));
    } else {
        scrollX_ = 0;
    }

    scrollX_ = std::clamp(scrollX_, 0, std::max(0, contentWidth_ - viewW));
    scrollY_ = std::clamp(scrollY_, 0, std::max(0, contentHeight_ - viewH));
    if (scrollbar_->visible()) {
        scrollbar_->value(scrollY_, viewH, 0, std::max(viewH, contentHeight_));
    }
    if (allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible()) {
        hScrollbar_->value(scrollX_, viewW, 0, std::max(viewW, contentWidth_));
    }
}

void HtmlWidget::scrollbarCallback(Fl_Widget* w, void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    auto* sb = static_cast<Fl_Scrollbar*>(w);
    int oldScrollY = self->scrollY_;
    self->scrollY_ = sb->value();
    self->notifyScrollChanged(oldScrollY);
    self->redraw();
}

void HtmlWidget::hScrollbarCallback(Fl_Widget* w, void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    auto* sb = static_cast<Fl_Scrollbar*>(w);
    self->scrollX_ = sb->value();
    self->redraw();
}

void HtmlWidget::notifyScrollChanged(int oldScrollY) {
    if (oldScrollY == scrollY_) return;
    if (scrollCallback_) {
        scrollCallback_(scrollY_);
    }
}

void HtmlWidget::draw() {
    // Draw background
    fl_push_clip(x(), y(), w(), h());
    fl_color(FL_WHITE);
    fl_rectf(x(), y(), w(), h());

    if (doc_) {
        textFragments_.clear();
        parallelBoundariesComputed_ = false;
        parallelColumnBoundaries_.clear();
        // Set up clip for content area (exclude scrollbar)
        int viewW = viewportWidth();
        int viewH = viewportHeight();
        fl_push_clip(x(), y(), viewW, viewH);

        // Draw the litehtml document with scroll offset
        litehtml::position clip(x(), y(), viewW, viewH);
        doc_->draw(0, x() - scrollX_, y() - scrollY_, &clip);

        fl_pop_clip();
    } else {
        textFragments_.clear();
        // No content
        fl_color(FL_DARK3);
        fl_font(FL_HELVETICA, 12);
        fl_draw("No content", x() + 10, y() + 20);
    }

    fl_pop_clip();

    // Draw scrollbar
    if (scrollbar_->visible()) {
        scrollbar_->redraw();
    }
    if (allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible()) {
        hScrollbar_->redraw();
    }
}

int HtmlWidget::handle(int event) {
    switch (event) {
    case FL_PUSH: {
        take_focus();
        if (Fl::event_button() == FL_RIGHT_MOUSE && contextCallback_) {
            // Right-click context menu
            if (doc_) {
                int docX = Fl::event_x() - x() + scrollX_;
                int docY = Fl::event_y() - y() + scrollY_;
                litehtml::position::vector redraw;
                std::vector<litehtml::position> clientRects;
                auto el = doc_->root_render()->get_element_by_point(
                    docX, docY,
                    docX, docY,
                    [](const std::shared_ptr<litehtml::render_item>&) { return true; });

                std::string word, href, strong, morph, module, title;
                if (el) {
                    word = wordAtScreenPoint(Fl::event_x(), Fl::event_y());
                    HitElement hit = findDeepestElementAtPoint(el, docX, docY);
                    if (hit.element) el = hit.element;

                    if (word.empty()) {
                        auto fontIt = fonts_.find(el->css().get_font());
                        if (fontIt != fonts_.end()) {
                            fl_font(fontIt->second->info.flFont,
                                    fontIt->second->info.size);
                        } else {
                            fl_font(FL_TIMES, static_cast<int>(get_default_font_size()));
                        }

                        litehtml::string elText;
                        el->get_text(elText);
                        word = elText;
                        if (hit.hasBox) {
                            word = normalizeHitWord(word, &hit.box, docX);
                        } else {
                            litehtml::position hitPlacement = el->get_placement();
                            word = normalizeHitWord(word, &hitPlacement, docX);
                        }
                    }

                    // Walk up parent chain for link and Strong's/morph attributes.
                    auto cur = el;
                    for (int depth = 0; cur && depth < 8; ++depth) {
                        if (href.empty()) {
                            auto h = cur->get_attr("href");
                            if (h && *h) href = h;
                        }
                        if (strong.empty()) {
                            auto s = cur->get_attr("data-strong");
                            if (s && *s) strong = s;
                        }
                        if (morph.empty()) {
                            auto m = cur->get_attr("data-morph");
                            if (m && *m) morph = m;
                        }
                        if (module.empty()) {
                            auto dm = cur->get_attr("data-module");
                            if (dm && *dm) module = dm;
                        }
                        if (!href.empty() && !strong.empty() && !morph.empty() &&
                            !module.empty()) {
                            break;
                        }
                        cur = cur->parent();
                    }

                    // In parallel view, clicks in block whitespace can resolve to
                    // container elements. Do not fabricate a word in that case.
                    bool isParallelDoc =
                        currentHtml_.find("class=\"parallel\"") != std::string::npos;
                    if (isParallelDoc &&
                        !isInlineLikeDisplay(el->css().get_display()) &&
                        strong.empty() && morph.empty() && href.empty()) {
                        word.clear();
                    }
                }

                contextCallback_(word, href, strong, morph, module,
                                 Fl::event_x(), Fl::event_y());
            }
            return 1;
        }

        if (Fl::event_button() == FL_LEFT_MOUSE) {
            selectionAnchor_ = hitTestSelectionPoint(Fl::event_x(), Fl::event_y());
            selectionFocus_ = selectionAnchor_;
            selectionParallelColumn_ =
                (selectionAnchor_.valid && isParallelDocument())
                    ? fragmentParallelColumn(selectionAnchor_.fragmentIndex)
                    : -1;
            selecting_ = selectionAnchor_.valid;
            dragSelecting_ = false;
            selectionStartX_ = Fl::event_x();
            selectionStartY_ = Fl::event_y();
            if (!hasSelection()) redraw();
        }

        if (doc_) {
            litehtml::position::vector redraw;
            if (doc_->on_lbutton_down(Fl::event_x() - x() + scrollX_,
                                       Fl::event_y() - y() + scrollY_,
                                       Fl::event_x() - x() + scrollX_,
                                       Fl::event_y() - y() + scrollY_,
                                       redraw)) {
                this->redraw();
            }
        }
        return 1;
    }

    case FL_DRAG: {
        if (selecting_) {
            SelectionPoint point = hitTestSelectionPoint(
                Fl::event_x(), Fl::event_y(), selectionParallelColumn_);
            if (point.valid) {
                selectionFocus_ = point;
                dragSelecting_ =
                    dragSelecting_ ||
                    std::abs(Fl::event_x() - selectionStartX_) > 2 ||
                    std::abs(Fl::event_y() - selectionStartY_) > 2;
                redraw();
            }
            return 1;
        }
        return 1;
    }

    case FL_RELEASE: {
        if (Fl::event_button() == FL_LEFT_MOUSE && selecting_) {
            SelectionPoint point = hitTestSelectionPoint(
                Fl::event_x(), Fl::event_y(), selectionParallelColumn_);
            if (point.valid) {
                selectionFocus_ = point;
            }
            selecting_ = false;
            bool suppressClick = dragSelecting_ || hasSelection();
            dragSelecting_ = false;
            if (suppressClick) {
                redraw();
                return 1;
            }
        }

        if (doc_) {
            litehtml::position::vector redraw;
            if (doc_->on_lbutton_up(Fl::event_x() - x() + scrollX_,
                                     Fl::event_y() - y() + scrollY_,
                                     Fl::event_x() - x() + scrollX_,
                                     Fl::event_y() - y() + scrollY_,
                                     redraw)) {
                this->redraw();
            }
        }
        return 1;
    }

    case FL_MOVE: {
        if (doc_) {
            litehtml::position::vector redraw;
            if (doc_->on_mouse_over(Fl::event_x() - x() + scrollX_,
                                     Fl::event_y() - y() + scrollY_,
                                     Fl::event_x() - x() + scrollX_,
                                     Fl::event_y() - y() + scrollY_,
                                     redraw)) {
                this->redraw();
            }

            // Check for hover over links/words
            if (hoverCallback_) {
                auto el = doc_->root_render()->get_element_by_point(
                    Fl::event_x() - x() + scrollX_,
                    Fl::event_y() - y() + scrollY_,
                    Fl::event_x() - x() + scrollX_,
                    Fl::event_y() - y() + scrollY_,
                    [](const std::shared_ptr<litehtml::render_item>&) { return true; });

                std::string word, href, strong, morph, module, title;
                litehtml::position titlePlacement;
                bool hasTitlePlacement = false;
                if (el) {
                    word = wordAtScreenPoint(Fl::event_x(), Fl::event_y());
                    if (word.empty()) {
                        // Get text content of the element under the cursor.
                        litehtml::string elText;
                        el->get_text(elText);

                        // Fall back to the older element-based heuristic if we
                        // do not have an exact text-fragment hit.
                        bool isWordSpan = false;
                        {
                            auto probe = el;
                            for (int d = 0; probe && d < 3; ++d) {
                                auto cls = probe->get_attr("class");
                                if (cls && std::string(cls).find("w") !=
                                               std::string::npos) {
                                    isWordSpan = true;
                                    break;
                                }
                                probe = probe->parent();
                            }
                        }
                        if (isWordSpan) {
                            word = elText;
                        } else {
                            int cursorDocX = Fl::event_x() - x() + scrollX_;
                            word = extractWordAtCursor(elText, cursorDocX, el);
                        }
                    }

                    // Walk up parent chain (max 5 levels) to find data-strong,
                    // data-morph, and href attributes
                    auto cur = el;
                    for (int depth = 0; cur && depth < 5; ++depth) {
                        if (href.empty()) {
                            auto h = cur->get_attr("href");
                            if (h && *h) href = h;
                        }
                        if (strong.empty()) {
                            auto s = cur->get_attr("data-strong");
                            if (s && *s) strong = s;
                        }
                        if (morph.empty()) {
                            auto m = cur->get_attr("data-morph");
                            if (m && *m) morph = m;
                        }
                        if (module.empty()) {
                            auto dm = cur->get_attr("data-module");
                            if (dm && *dm) module = dm;
                        }
                        if (title.empty()) {
                            auto t = cur->get_attr("title");
                            if (t && *t) {
                                title = t;
                                titlePlacement = cur->get_placement();
                                hasTitlePlacement = true;
                            }
                        }
                        if (!strong.empty() && !morph.empty() &&
                            !href.empty() && !module.empty() && !title.empty())
                            break;
                        cur = cur->parent();
                    }
                }

                if (href != lastHoverHref_ || word != lastHoverWord_ ||
                    strong != lastHoverStrong_ || morph != lastHoverMorph_ ||
                    module != lastHoverModule_ || title != lastHoverTitle_) {
                    lastHoverWord_   = word;
                    lastHoverHref_   = href;
                    lastHoverStrong_ = strong;
                    lastHoverMorph_  = morph;
                    lastHoverModule_ = module;
                    lastHoverTitle_  = title;
                    if (!title.empty()) {
                        copy_tooltip(title.c_str());
                        int tooltipX = Fl::event_x() - x();
                        int tooltipY = Fl::event_y() - y();
                        int tooltipW = 1;
                        int tooltipH = 1;
                        if (hasTitlePlacement) {
                            tooltipX = std::clamp(static_cast<int>(titlePlacement.x) - scrollX_,
                                                  0,
                                                  std::max(0, w() - 1));
                            tooltipY = std::clamp(static_cast<int>(titlePlacement.y) - scrollY_,
                                                  0,
                                                  std::max(0, h() - 1));
                            tooltipW = std::max(1, static_cast<int>(titlePlacement.width));
                            tooltipH = std::max(1, static_cast<int>(titlePlacement.height));
                            tooltipW = std::max(1, std::min(tooltipW, w() - tooltipX));
                            tooltipH = std::max(1, std::min(tooltipH, h() - tooltipY));
                        }
                        Fl_Tooltip::enter_area(this,
                                               tooltipX,
                                               tooltipY,
                                               tooltipW,
                                               tooltipH,
                                               tooltip());
                    } else {
                        tooltip(nullptr);
                        Fl_Tooltip::current(nullptr);
                    }
                    hoverCallback_(word, href, strong, morph, module,
                                   Fl::event_x(), Fl::event_y());
                }
            }
        }
        return 1;
    }

    case FL_MOUSEWHEEL: {
        int dx = Fl::event_dx() * 30;
        int dy = Fl::event_dy() * 30;
        bool handled = false;

        if (allowHorizontalScroll_ && hScrollbar_ && hScrollbar_->visible()) {
            int horizontalDelta = dx;
            if (horizontalDelta == 0 && (Fl::event_state() & FL_SHIFT)) {
                horizontalDelta = dy;
            }
            if (horizontalDelta != 0) {
                setScrollX(scrollX_ + horizontalDelta);
                handled = true;
            }
        }

        if (dy != 0 && !(handled && (Fl::event_state() & FL_SHIFT))) {
            setScrollY(scrollY_ + dy);
            handled = true;
        }

        return handled ? 1 : 0;
    }

    case FL_KEYBOARD:
    case FL_SHORTCUT: {
        const bool ctrl = (Fl::event_state() & FL_CTRL) != 0;
        const int key = Fl::event_key();
        if (ctrl && (key == 'c' || key == 'C')) {
            return copySelectionToClipboard() ? 1 : 0;
        }
        if (ctrl && (key == 'a' || key == 'A') && !textFragments_.empty()) {
            selectionParallelColumn_ = -1;
            selectionAnchor_.fragmentIndex = 0;
            selectionAnchor_.charIndex = 0;
            selectionAnchor_.valid = true;
            selectionFocus_.fragmentIndex = static_cast<int>(textFragments_.size()) - 1;
            selectionFocus_.charIndex =
                static_cast<int>(textFragments_.back().byteOffsets.size()) - 1;
            selectionFocus_.valid = true;
            redraw();
            return 1;
        }
        break;
    }

    case FL_ENTER:
        return 1;

    case FL_LEAVE:
        if (hoverCallback_ && (!lastHoverWord_.empty() || !lastHoverHref_.empty() ||
                                !lastHoverStrong_.empty() || !lastHoverMorph_.empty() ||
                                !lastHoverModule_.empty() || !lastHoverTitle_.empty())) {
            lastHoverWord_.clear();
            lastHoverHref_.clear();
            lastHoverStrong_.clear();
            lastHoverMorph_.clear();
            lastHoverModule_.clear();
            lastHoverTitle_.clear();
            tooltip(nullptr);
            Fl_Tooltip::current(nullptr);
            hoverCallback_("", "", "", "", "", 0, 0);
        }
        return 1;

    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1;

    default:
        break;
    }

    return Fl_Widget::handle(event);
}

void HtmlWidget::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    clearSelection();
    textFragments_.clear();

    if (scrollbar_) {
        scrollbar_->resize(X + W - kScrollbarExtent, Y,
                           kScrollbarExtent, H);
    }
    if (hScrollbar_) {
        hScrollbar_->resize(X, Y + H - kScrollbarExtent,
                            W, kScrollbarExtent);
    }

    if (doc_) {
        // Coalesce many resize events while dragging splitters.
        if (reflowScheduled_) {
            Fl::remove_timeout(dispatchDeferredReflow, this);
        }
        Fl::add_timeout(0.04, dispatchDeferredReflow, this);
        reflowScheduled_ = true;
    }
    redraw();
}

void HtmlWidget::dispatchDeferredReflow(void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    if (!self) return;

    self->reflowScheduled_ = false;
    if (!self->doc_) return;

    self->renderDocument();
    self->updateScrollbar(true);
    self->redraw();
}

// --- litehtml::document_container implementation ---

litehtml::uint_ptr HtmlWidget::create_font(const litehtml::font_description& descr,
                                             const litehtml::document* /*doc*/,
                                             litehtml::font_metrics* fm) {
    static std::map<std::string, std::shared_ptr<const CachedFont>> fontCache;

    const std::string cacheKey = fontDescriptionCacheKey(descr);
    std::shared_ptr<const CachedFont> cachedFont;
    auto cached = fontCache.find(cacheKey);
    if (cached != fontCache.end()) {
        cachedFont = cached->second;
    } else {
        auto entry = std::make_shared<CachedFont>();
        entry->info.flFont = mapFont(descr.family.c_str(), descr.weight,
                                     descr.style == litehtml::font_style_italic);
        entry->info.size = static_cast<int>(descr.size);
        entry->info.weight = descr.weight;
        entry->info.italic = (descr.style == litehtml::font_style_italic);
        entry->info.decorationLine = descr.decoration_line;

        fl_font(entry->info.flFont, entry->info.size);
        entry->metrics.height = static_cast<litehtml::pixel_t>(fl_height());
        entry->metrics.ascent =
            static_cast<litehtml::pixel_t>(fl_height() - fl_descent());
        entry->metrics.descent = static_cast<litehtml::pixel_t>(fl_descent());
        entry->metrics.x_height = entry->metrics.ascent * 0.5f;
        entry->metrics.draw_spaces = (descr.decoration_line != 0);

        cachedFont = entry;
        fontCache.emplace(cacheKey, cachedFont);
    }

    litehtml::uint_ptr id = nextFontId_++;
    fonts_[id] = cachedFont;
    if (fm) {
        *fm = cachedFont->metrics;
    }
    return id;
}

void HtmlWidget::delete_font(litehtml::uint_ptr hFont) {
    fonts_.erase(hFont);
}

litehtml::pixel_t HtmlWidget::text_width(const char* text, litehtml::uint_ptr hFont) {
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return 0;

    const char* safeText = text ? text : "";
    const auto& cachedFont = it->second;
    std::string_view token(safeText);
    const bool cacheable = isTextWidthCacheable(token);

    if (cacheable) {
        auto fontCacheIt = textWidthCache_.find(cachedFont.get());
        if (fontCacheIt != textWidthCache_.end()) {
            auto valueIt = fontCacheIt->second.find(token);
            if (valueIt != fontCacheIt->second.end()) {
                ++textWidthCacheHits_;
                return valueIt->second;
            }
        }
        ++textWidthCacheMisses_;
    }

    fl_font(cachedFont->info.flFont, cachedFont->info.size);
    litehtml::pixel_t width = static_cast<litehtml::pixel_t>(fl_width(safeText));

    if (cacheable) {
        if (textWidthCacheEntries_ >= kTextWidthCacheLimit) {
            textWidthCache_.clear();
            textWidthCacheEntries_ = 0;
        }

        auto& fontCache = textWidthCache_[cachedFont.get()];
        auto [valueIt, inserted] = fontCache.emplace(token, width);
        if (inserted) {
            ++textWidthCacheEntries_;
            ++textWidthCacheStores_;
        } else {
            valueIt->second = width;
        }
    }

    return width;
}

void HtmlWidget::draw_text(litehtml::uint_ptr hdc, const char* text,
                            litehtml::uint_ptr hFont, litehtml::web_color color,
                            const litehtml::position& pos) {
    (void)hdc;
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return;

    fl_font(it->second->info.flFont, it->second->info.size);

    TextFragment fragment;
    fragment.text = text ? text : "";
    fragment.pos = pos;
    fragment.byteOffsets.push_back(0);
    fragment.xOffsets.push_back(0);
    fragment.parallelColumn = -1;

    int cursorX = 0;
    for (size_t i = 0; i < fragment.text.size();) {
        int len = fl_utf8len1(static_cast<unsigned char>(fragment.text[i]));
        if (len <= 0 || i + static_cast<size_t>(len) > fragment.text.size()) len = 1;
        cursorX += static_cast<int>(fl_width(fragment.text.data() + i, len));
        i += static_cast<size_t>(len);
        fragment.byteOffsets.push_back(static_cast<int>(i));
        fragment.xOffsets.push_back(cursorX);
    }

    if (isParallelDocument() && doc_ && doc_->root_render()) {
        // Lazily compute column boundaries once per draw pass
        if (!parallelBoundariesComputed_) {
            parallelBoundariesComputed_ = true;
            buildParallelColumnBoundaries();
        }
        // Simple x-range lookup instead of per-fragment DOM traversal
        int docX = static_cast<int>(pos.x);
        for (const auto& boundary : parallelColumnBoundaries_) {
            if (docX >= boundary.xStart && docX < boundary.xEnd) {
                fragment.parallelColumn = boundary.column;
                break;
            }
        }
    }

    const int baselineY = static_cast<int>(pos.y + pos.height) - fl_descent();
    int fragmentIndex = static_cast<int>(textFragments_.size());
    textFragments_.push_back(fragment);

    auto drawRun = [&](int startChar,
                       int endChar,
                       Fl_Color fg,
                       bool selected) {
        if (startChar >= endChar) return;
        const TextFragment& current = textFragments_.back();
        int byteStart = current.byteOffsets[startChar];
        int byteEnd = current.byteOffsets[endChar];
        std::string chunk = current.text.substr(byteStart, byteEnd - byteStart);
        int drawX = static_cast<int>(pos.x) + current.xOffsets[startChar];
        int drawW = current.xOffsets[endChar] - current.xOffsets[startChar];
        if (selected) {
            fl_color(FL_SELECTION_COLOR);
            fl_rectf(drawX,
                     static_cast<int>(pos.y),
                     std::max(1, drawW),
                     static_cast<int>(pos.height));
        }
        fl_color(fg);
        fl_draw(chunk.c_str(), static_cast<int>(chunk.size()), drawX, baselineY);
    };

    int selStart = 0;
    int selEnd = 0;
    if (fragmentSelectionRange(fragmentIndex, selStart, selEnd)) {
        Fl_Color normalColor = fl_rgb_color(color.red, color.green, color.blue);
        Fl_Color selectedColor = fl_contrast(FL_FOREGROUND_COLOR, FL_SELECTION_COLOR);
        drawRun(0, selStart, normalColor, false);
        drawRun(selStart, selEnd, selectedColor, true);
        drawRun(selEnd,
                static_cast<int>(fragment.byteOffsets.size()) - 1,
                normalColor,
                false);
    } else {
        fl_color(color.red, color.green, color.blue);
        fl_draw(text, static_cast<int>(pos.x), baselineY);
    }

    // Draw decorations
    if (it->second->info.decorationLine &
        litehtml::text_decoration_line_underline) {
        int lineY = static_cast<int>(pos.y + pos.height) - fl_descent() + 2;
        fl_line(static_cast<int>(pos.x), lineY,
                static_cast<int>(pos.x + pos.width), lineY);
    }
    if (it->second->info.decorationLine &
        litehtml::text_decoration_line_line_through) {
        int lineY = static_cast<int>(pos.y + pos.height / 2);
        fl_line(static_cast<int>(pos.x), lineY,
                static_cast<int>(pos.x + pos.width), lineY);
    }
}

litehtml::pixel_t HtmlWidget::pt_to_px(float pt) const {
    // Approximate: 96 DPI standard
    return static_cast<litehtml::pixel_t>(pt * 96.0f / 72.0f);
}

litehtml::pixel_t HtmlWidget::get_default_font_size() const {
    return 14;
}

const char* HtmlWidget::get_default_font_name() const {
    return "serif";
}

void HtmlWidget::draw_list_marker(litehtml::uint_ptr hdc,
                                   const litehtml::list_marker& marker) {
    (void)hdc;
    fl_color(marker.color.red, marker.color.green, marker.color.blue);

    if (marker.marker_type == litehtml::list_style_type_disc) {
        fl_pie(static_cast<int>(marker.pos.x), static_cast<int>(marker.pos.y),
               static_cast<int>(marker.pos.width), static_cast<int>(marker.pos.height), 0, 360);
    } else if (marker.marker_type == litehtml::list_style_type_circle) {
        fl_arc(static_cast<int>(marker.pos.x), static_cast<int>(marker.pos.y),
               static_cast<int>(marker.pos.width), static_cast<int>(marker.pos.height), 0, 360);
    } else if (marker.marker_type == litehtml::list_style_type_square) {
        fl_rectf(static_cast<int>(marker.pos.x), static_cast<int>(marker.pos.y),
                 static_cast<int>(marker.pos.width), static_cast<int>(marker.pos.height));
    }
}

void HtmlWidget::load_image(const char* /*src*/, const char* /*baseurl*/,
                             bool /*redraw_on_ready*/) {
    // Image loading not implemented for Bible text
}

void HtmlWidget::get_image_size(const char* /*src*/, const char* /*baseurl*/,
                                 litehtml::size& sz) {
    sz.width = 0;
    sz.height = 0;
}

void HtmlWidget::draw_solid_fill(litehtml::uint_ptr hdc,
                                  const litehtml::background_layer& layer,
                                  const litehtml::web_color& color) {
    (void)hdc;
    if (color.alpha > 0) {
        fl_color(color.red, color.green, color.blue);
        fl_rectf(static_cast<int>(layer.clip_box.x),
                 static_cast<int>(layer.clip_box.y),
                 static_cast<int>(layer.clip_box.width),
                 static_cast<int>(layer.clip_box.height));
    }
}

void HtmlWidget::draw_image(litehtml::uint_ptr /*hdc*/,
                             const litehtml::background_layer& /*layer*/,
                             const std::string& /*url*/,
                             const std::string& /*base_url*/) {
    // Image drawing not implemented for Bible text
}

void HtmlWidget::draw_linear_gradient(litehtml::uint_ptr /*hdc*/,
                                       const litehtml::background_layer& /*layer*/,
                                       const litehtml::background_layer::linear_gradient& /*gradient*/) {
    // Gradient drawing not implemented
}

void HtmlWidget::draw_radial_gradient(litehtml::uint_ptr /*hdc*/,
                                       const litehtml::background_layer& /*layer*/,
                                       const litehtml::background_layer::radial_gradient& /*gradient*/) {
    // Gradient drawing not implemented
}

void HtmlWidget::draw_conic_gradient(litehtml::uint_ptr /*hdc*/,
                                      const litehtml::background_layer& /*layer*/,
                                      const litehtml::background_layer::conic_gradient& /*gradient*/) {
    // Gradient drawing not implemented
}

void HtmlWidget::draw_borders(litehtml::uint_ptr hdc,
                               const litehtml::borders& borders,
                               const litehtml::position& draw_pos,
                               bool /*root*/) {
    (void)hdc;

    // Draw each border side
    auto drawSide = [&](const litehtml::border& b, int x1, int y1, int x2, int y2) {
        if (b.width > 0 && b.style != litehtml::border_style_none &&
            b.style != litehtml::border_style_hidden) {
            fl_color(b.color.red, b.color.green, b.color.blue);
            fl_line_style(FL_SOLID, b.width);
            fl_line(x1, y1, x2, y2);
            fl_line_style(0);
        }
    };

    int x = static_cast<int>(draw_pos.x);
    int y = static_cast<int>(draw_pos.y);
    int w = static_cast<int>(draw_pos.width);
    int h = static_cast<int>(draw_pos.height);

    drawSide(borders.top, x, y, x + w, y);
    drawSide(borders.bottom, x, y + h, x + w, y + h);
    drawSide(borders.left, x, y, x, y + h);
    drawSide(borders.right, x + w, y, x + w, y + h);
}

void HtmlWidget::set_caption(const char* /*caption*/) {
    // Not used for our widget
}

void HtmlWidget::set_base_url(const char* base_url) {
    if (base_url) baseUrl_ = base_url;
}

void HtmlWidget::link(const std::shared_ptr<litehtml::document>& /*doc*/,
                       const litehtml::element::ptr& /*el*/) {
    // Handle <link> elements (e.g., stylesheets) - not needed for our use
}

void HtmlWidget::on_anchor_click(const char* url,
                                  const litehtml::element::ptr& /*el*/) {
    if (!url || !linkCallback_) return;

    // Defer callback execution until after litehtml finishes processing the
    // current mouse event. This avoids invalidating doc_ during on_lbutton_up.
    pendingLinkUrl_ = url;
    if (linkCallbackScheduled_) {
        Fl::remove_timeout(dispatchDeferredLink, this);
    }
    Fl::add_timeout(0.0, dispatchDeferredLink, this);
    linkCallbackScheduled_ = true;
}

void HtmlWidget::dispatchDeferredLink(void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    if (!self) return;

    self->linkCallbackScheduled_ = false;
    if (!self->linkCallback_ || self->pendingLinkUrl_.empty()) return;

    std::string url = std::move(self->pendingLinkUrl_);
    self->pendingLinkUrl_.clear();
    self->linkCallback_(url);
}

void HtmlWidget::on_mouse_event(const litehtml::element::ptr& /*el*/,
                                 litehtml::mouse_event /*event*/) {
    // Mouse event handling not needed beyond what handle() provides
}

void HtmlWidget::set_cursor(const char* cursor) {
    if (!cursor) return;
    if (strcmp(cursor, "pointer") == 0) {
        fl_cursor(FL_CURSOR_HAND);
    } else {
        fl_cursor(FL_CURSOR_DEFAULT);
    }
}

void HtmlWidget::transform_text(litehtml::string& text,
                                 litehtml::text_transform tt) {
    switch (tt) {
    case litehtml::text_transform_capitalize: {
        bool newWord = true;
        for (auto& ch : text) {
            unsigned char uc = static_cast<unsigned char>(ch);
            if (newWord && std::isalpha(uc)) {
                ch = static_cast<char>(std::toupper(uc));
                newWord = false;
            } else if (std::isspace(uc)) {
                newWord = true;
            }
        }
        break;
    }
    case litehtml::text_transform_uppercase:
        for (auto& ch : text) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        break;
    case litehtml::text_transform_lowercase:
        for (auto& ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        break;
    default:
        break;
    }
}

void HtmlWidget::import_css(litehtml::string& /*text*/,
                             const litehtml::string& /*url*/,
                             litehtml::string& /*baseurl*/) {
    // CSS import not needed for our use
}

void HtmlWidget::set_clip(const litehtml::position& pos,
                           const litehtml::border_radiuses& /*bdr_radius*/) {
    ClipRect cr{static_cast<int>(pos.x), static_cast<int>(pos.y),
                static_cast<int>(pos.width), static_cast<int>(pos.height)};
    clipStack_.push_back(cr);
    fl_push_clip(static_cast<int>(pos.x), static_cast<int>(pos.y),
                 static_cast<int>(pos.width), static_cast<int>(pos.height));
}

void HtmlWidget::del_clip() {
    if (!clipStack_.empty()) {
        clipStack_.pop_back();
        fl_pop_clip();
    }
}

void HtmlWidget::get_viewport(litehtml::position& client) const {
    client.x = x();
    client.y = y();
    client.width = viewportWidth();
    client.height = viewportHeight();
}

std::shared_ptr<litehtml::element> HtmlWidget::create_element(
    const char* /*tag_name*/,
    const litehtml::string_map& /*attributes*/,
    const std::shared_ptr<litehtml::document>& /*doc*/) {
    return nullptr; // Use default element creation
}

void HtmlWidget::get_media_features(litehtml::media_features& media) const {
    media.type = litehtml::media_type_screen;
    media.width = viewportWidth();
    media.height = viewportHeight();
    media.device_width = Fl::w();
    media.device_height = Fl::h();
    media.color = 8;
    media.monochrome = 0;
    media.color_index = 256;
    media.resolution = 96;
}

void HtmlWidget::get_language(litehtml::string& language,
                               litehtml::string& culture) const {
    language = "en";
    culture = "";
}

Fl_Font HtmlWidget::mapFont(const char* faceName, int weight, bool italic) {
    if (!faceName) return FL_HELVETICA;

    std::string face(faceName);
    Fl_Font base = FL_HELVETICA;

    // Try resolving via VerdadApp's system font lookup first.
    // The faceName may be a comma-separated CSS font-family list.
    auto* app = VerdadApp::instance();
    if (app) {
        // Try each font in the CSS font-family list
        std::istringstream ss(face);
        std::string token;
        while (std::getline(ss, token, ',')) {
            // Trim whitespace and quotes
            auto start = token.find_first_not_of(" \t\"'");
            auto end = token.find_last_not_of(" \t\"'");
            if (start == std::string::npos) continue;
            token = token.substr(start, end - start + 1);
            if (token.empty()) continue;

            Fl_Font found = app->fltkFontFromFamily(token);
            if (found != FL_HELVETICA || token == "Helvetica") {
                base = found;
                goto apply_modifiers;
            }
        }
    }

    // Fallback: map generic family names
    if (face.find("serif") != std::string::npos &&
        face.find("sans") == std::string::npos) {
        base = FL_TIMES;
    } else if (face.find("sans") != std::string::npos) {
        base = FL_HELVETICA;
    } else if (face.find("mono") != std::string::npos ||
               face.find("courier") != std::string::npos ||
               face.find("Courier") != std::string::npos) {
        base = FL_COURIER;
    }

apply_modifiers:
    return styledFontVariant(base, weight, italic);
}

} // namespace verdad
