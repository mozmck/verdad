#include "ui/HtmlWidget.h"
#include "app/PerfTrace.h"
#include "app/VerdadApp.h"

#include <FL/Fl.H>
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
#include <vector>

namespace verdad {
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

} // namespace

HtmlWidget::HtmlWidget(int X, int Y, int W, int H, const char* label)
    : Fl_Widget(X, Y, W, H, label) {
    // Create vertical scrollbar
    int sbW = 16;
    scrollbar_ = new Fl_Scrollbar(X + W - sbW, Y, sbW, H);
    scrollbar_->type(FL_VERTICAL);
    scrollbar_->callback(scrollbarCallback, this);
    scrollbar_->hide();

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
}

void HtmlWidget::clearSelection() {
    selectionAnchor_ = SelectionPoint{};
    selectionFocus_ = SelectionPoint{};
    selecting_ = false;
    dragSelecting_ = false;
}

bool HtmlWidget::selectionPointLess(const SelectionPoint& lhs,
                                    const SelectionPoint& rhs) const {
    if (lhs.fragmentIndex != rhs.fragmentIndex) {
        return lhs.fragmentIndex < rhs.fragmentIndex;
    }
    return lhs.charIndex < rhs.charIndex;
}

bool HtmlWidget::hasSelection() const {
    if (!selectionAnchor_.valid || !selectionFocus_.valid) return false;
    return selectionAnchor_.fragmentIndex != selectionFocus_.fragmentIndex ||
           selectionAnchor_.charIndex != selectionFocus_.charIndex;
}

HtmlWidget::SelectionPoint HtmlWidget::hitTestSelectionPoint(int screenX,
                                                             int screenY) const {
    SelectionPoint best;
    if (textFragments_.empty()) return best;

    long bestScore = std::numeric_limits<long>::max();

    for (size_t i = 0; i < textFragments_.size(); ++i) {
        const TextFragment& fragment = textFragments_[i];
        if (fragment.byteOffsets.empty() || fragment.xOffsets.empty()) continue;

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
    baseUrl_ = baseUrl;
    scrollY_ = 0;
    lastHoverWord_.clear();
    lastHoverHref_.clear();
    lastHoverStrong_.clear();
    lastHoverMorph_.clear();
    lastHoverModule_.clear();
    lastHoverTitle_.clear();
    tooltip(nullptr);
    clearSelection();
    textFragments_.clear();

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

    renderDocument();
    perf::logf("HtmlWidget::setHtml renderDocument: %.3f ms", step.elapsedMs());
    step.reset();
    updateScrollbar();
    perf::logf("HtmlWidget::setHtml updateScrollbar: %.3f ms", step.elapsedMs());
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
        int oldScroll = scrollY_;
        std::string html = currentHtml_;
        std::string base = baseUrl_;
        setHtml(html, base);
        setScrollY(oldScroll);
    } else {
        redraw();
    }
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
    scrollY_ = 0;
    if (scrollbar_->visible()) {
        scrollbar_->value(0);
    }
    redraw();
}

void HtmlWidget::setScrollY(int y) {
    int maxScroll = std::max(0, contentHeight_ - h());
    scrollY_ = std::clamp(y, 0, maxScroll);
    if (scrollbar_->visible()) {
        scrollbar_->value(scrollY_, h(), 0, contentHeight_);
    }
    redraw();
}

HtmlWidget::Snapshot HtmlWidget::captureSnapshot() const {
    Snapshot snapshot;
    snapshot.doc = std::static_pointer_cast<void>(doc_);
    snapshot.html = currentHtml_;
    snapshot.baseUrl = baseUrl_;
    snapshot.scrollY = scrollY_;
    snapshot.contentHeight = contentHeight_;
    snapshot.renderWidth = lastRenderWidth_;
    snapshot.scrollbarVisible = scrollbar_ && scrollbar_->visible();
    snapshot.valid = (doc_ != nullptr || !currentHtml_.empty());
    return snapshot;
}

HtmlWidget::Snapshot HtmlWidget::takeSnapshot() {
    Snapshot snapshot;
    snapshot.doc = std::static_pointer_cast<void>(doc_);
    snapshot.html = std::move(currentHtml_);
    snapshot.baseUrl = std::move(baseUrl_);
    snapshot.scrollY = scrollY_;
    snapshot.contentHeight = contentHeight_;
    snapshot.renderWidth = lastRenderWidth_;
    snapshot.scrollbarVisible = scrollbar_ && scrollbar_->visible();
    snapshot.valid = (doc_ != nullptr || !snapshot.html.empty());

    doc_.reset();
    currentHtml_.clear();
    baseUrl_.clear();
    scrollY_ = 0;
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
    baseUrl_ = snapshot.baseUrl;
    scrollY_ = snapshot.scrollY;
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
        const int sbW = 16;
        const bool snapSbVisible = snapshot.scrollbarVisible;
        const int expectedRenderWidth =
            std::max(10, w() - (snapSbVisible ? sbW : 0));
        const bool desiredVisible = (contentHeight_ > h());

        // Fast path: geometry/scrollbar mode unchanged, so reuse cached layout.
        if (snapshot.renderWidth > 0 &&
            snapshot.renderWidth == expectedRenderWidth &&
            desiredVisible == snapSbVisible) {
            reusedLayout = true;
            if (snapSbVisible) {
                scrollbar_->show();
                scrollbar_->resize(x() + w() - sbW, y(), sbW, h());
                scrollbar_->linesize(20);
            } else {
                scrollbar_->hide();
            }
        }
        perf::logf("HtmlWidget::restoreSnapshot(copy) fastPath=%d snapW=%d expectedW=%d snapSB=%d desiredSB=%d",
                   reusedLayout ? 1 : 0,
                   snapshot.renderWidth,
                   expectedRenderWidth,
                   snapSbVisible ? 1 : 0,
                   desiredVisible ? 1 : 0);
    }
    if (!reusedLayout) {
        if (doc_) {
            renderDocument();
        }
        updateScrollbar();
    }
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
    baseUrl_ = std::move(snapshot.baseUrl);
    scrollY_ = snapshot.scrollY;
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
    snapshot.scrollY = 0;
    snapshot.contentHeight = 0;
    snapshot.renderWidth = 0;
    snapshot.scrollbarVisible = false;
    snapshot.valid = false;

    bool reusedLayout = false;
    if (doc_) {
        const int sbW = 16;
        const int expectedRenderWidth =
            std::max(10, w() - (snapSbVisible ? sbW : 0));
        const bool desiredVisible = (contentHeight_ > h());

        // Fast path: geometry/scrollbar mode unchanged, so reuse cached layout.
        if (lastRenderWidth_ > 0 &&
            lastRenderWidth_ == expectedRenderWidth &&
            desiredVisible == snapSbVisible) {
            reusedLayout = true;
            if (snapSbVisible) {
                scrollbar_->show();
                scrollbar_->resize(x() + w() - sbW, y(), sbW, h());
                scrollbar_->linesize(20);
            } else {
                scrollbar_->hide();
            }
        }
        perf::logf("HtmlWidget::restoreSnapshot(move) fastPath=%d snapW=%d expectedW=%d snapSB=%d desiredSB=%d",
                   reusedLayout ? 1 : 0,
                   lastRenderWidth_,
                   expectedRenderWidth,
                   snapSbVisible ? 1 : 0,
                   desiredVisible ? 1 : 0);
    }
    if (!reusedLayout) {
        if (doc_) {
            renderDocument();
        }
        updateScrollbar();
    }
    setScrollY(scrollY_);
}

void HtmlWidget::renderDocument() {
    perf::ScopeTimer timer("HtmlWidget::renderDocument");
    if (!doc_) return;

    int sbW = scrollbar_->visible() ? 16 : 0;
    int renderWidth = w() - sbW;
    if (renderWidth < 10) renderWidth = 10;
    lastRenderWidth_ = renderWidth;

    doc_->render(renderWidth);
    contentHeight_ = doc_->height();
}

void HtmlWidget::updateScrollbar() {
    perf::ScopeTimer timer("HtmlWidget::updateScrollbar");
    if (!doc_) {
        scrollbar_->hide();
        return;
    }

    const bool wasVisible = scrollbar_->visible();
    const bool needVisible = (contentHeight_ > h());

    if (needVisible) {
        scrollbar_->show();
        scrollbar_->resize(x() + w() - 16, y(), 16, h());
        scrollbar_->linesize(20);

        // Render once more only on visibility transition (hide -> show),
        // where width changed from full pane to pane-minus-scrollbar.
        if (!wasVisible) {
            renderDocument();
        }

        scrollbar_->value(scrollY_, h(), 0, contentHeight_);
    } else {
        // Render once when transitioning show -> hide, so wrapping can use
        // full width again.
        if (wasVisible) {
            scrollbar_->hide();
            renderDocument();
        } else {
            scrollbar_->hide();
        }
        scrollY_ = 0;
    }
}

void HtmlWidget::scrollbarCallback(Fl_Widget* w, void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    auto* sb = static_cast<Fl_Scrollbar*>(w);
    self->scrollY_ = sb->value();
    self->redraw();
}

void HtmlWidget::draw() {
    // Draw background
    fl_push_clip(x(), y(), w(), h());
    fl_color(FL_WHITE);
    fl_rectf(x(), y(), w(), h());

    if (doc_) {
        textFragments_.clear();
        // Set up clip for content area (exclude scrollbar)
        int sbW = scrollbar_->visible() ? 16 : 0;
        fl_push_clip(x(), y(), w() - sbW, h());

        // Draw the litehtml document with scroll offset
        litehtml::position clip(x(), y(), w() - sbW, h());
        doc_->draw(0, x(), y() - scrollY_, &clip);

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
}

int HtmlWidget::handle(int event) {
    switch (event) {
    case FL_PUSH: {
        take_focus();
        if (Fl::event_button() == FL_RIGHT_MOUSE && contextCallback_) {
            // Right-click context menu
            if (doc_) {
                int docX = Fl::event_x() - x();
                int docY = Fl::event_y() - y() + scrollY_;
                litehtml::position::vector redraw;
                std::vector<litehtml::position> clientRects;
                auto el = doc_->root_render()->get_element_by_point(
                    docX, docY,
                    docX, docY,
                    [](const std::shared_ptr<litehtml::render_item>&) { return true; });

                std::string word, href, strong, morph, module, title;
                if (el) {
                    HitElement hit = findDeepestElementAtPoint(el, docX, docY);
                    if (hit.element) el = hit.element;

                    auto fontIt = fonts_.find(el->css().get_font());
                    if (fontIt != fonts_.end()) {
                        fl_font(fontIt->second.flFont, fontIt->second.size);
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
            selecting_ = selectionAnchor_.valid;
            dragSelecting_ = false;
            selectionStartX_ = Fl::event_x();
            selectionStartY_ = Fl::event_y();
            if (!hasSelection()) redraw();
        }

        if (doc_) {
            litehtml::position::vector redraw;
            if (doc_->on_lbutton_down(Fl::event_x() - x(),
                                       Fl::event_y() - y() + scrollY_,
                                       Fl::event_x() - x(),
                                       Fl::event_y() - y() + scrollY_,
                                       redraw)) {
                this->redraw();
            }
        }
        return 1;
    }

    case FL_DRAG: {
        if (selecting_) {
            SelectionPoint point = hitTestSelectionPoint(Fl::event_x(), Fl::event_y());
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
            SelectionPoint point = hitTestSelectionPoint(Fl::event_x(), Fl::event_y());
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
            if (doc_->on_lbutton_up(Fl::event_x() - x(),
                                     Fl::event_y() - y() + scrollY_,
                                     Fl::event_x() - x(),
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
            if (doc_->on_mouse_over(Fl::event_x() - x(),
                                     Fl::event_y() - y() + scrollY_,
                                     Fl::event_x() - x(),
                                     Fl::event_y() - y() + scrollY_,
                                     redraw)) {
                this->redraw();
            }

            // Check for hover over links/words
            if (hoverCallback_) {
                auto el = doc_->root_render()->get_element_by_point(
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    [](const std::shared_ptr<litehtml::render_item>&) { return true; });

                std::string word, href, strong, morph, module, title;
                if (el) {
                    // Get text content of the element under the cursor
                    litehtml::string elText;
                    el->get_text(elText);

                    // Check if element (or a close ancestor) is a span.w
                    // word wrapper — if so, use text directly.  Otherwise
                    // extract the single word at the cursor position.
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
                        int cursorDocX = Fl::event_x() - x();
                        word = extractWordAtCursor(elText, cursorDocX, el);
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
                            if (t && *t) title = t;
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
                    } else {
                        tooltip(nullptr);
                    }
                    hoverCallback_(word, href, strong, morph, module,
                                   Fl::event_x(), Fl::event_y());
                }
            }
        }
        return 1;
    }

    case FL_MOUSEWHEEL: {
        int dy = Fl::event_dy() * 30;
        scrollY_ += dy;
        if (scrollY_ < 0) scrollY_ = 0;
        if (scrollY_ > contentHeight_ - h())
            scrollY_ = std::max(0, contentHeight_ - h());

        if (scrollbar_->visible()) {
            scrollbar_->value(scrollY_, h(), 0, contentHeight_);
        }
        this->redraw();
        return 1;
    }

    case FL_KEYBOARD:
    case FL_SHORTCUT: {
        const bool ctrl = (Fl::event_state() & FL_CTRL) != 0;
        const int key = Fl::event_key();
        if (ctrl && (key == 'c' || key == 'C')) {
            return copySelectionToClipboard() ? 1 : 0;
        }
        if (ctrl && (key == 'a' || key == 'A') && !textFragments_.empty()) {
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
        const int sbW = 16;
        scrollbar_->resize(X + W - sbW, Y, sbW, H);
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
    self->updateScrollbar();
    self->redraw();
}

// --- litehtml::document_container implementation ---

litehtml::uint_ptr HtmlWidget::create_font(const litehtml::font_description& descr,
                                             const litehtml::document* /*doc*/,
                                             litehtml::font_metrics* fm) {
    FontInfo fi;
    fi.flFont = mapFont(descr.family.c_str(), descr.weight,
                         descr.style == litehtml::font_style_italic);
    fi.size = static_cast<int>(descr.size);
    fi.weight = descr.weight;
    fi.italic = (descr.style == litehtml::font_style_italic);
    fi.decorationLine = descr.decoration_line;

    // Measure font metrics
    fl_font(fi.flFont, fi.size);
    if (fm) {
        fm->height = static_cast<litehtml::pixel_t>(fl_height());
        fm->ascent = static_cast<litehtml::pixel_t>(fl_height() - fl_descent());
        fm->descent = static_cast<litehtml::pixel_t>(fl_descent());
        fm->x_height = fm->ascent * 0.5f;
        fm->draw_spaces = (descr.decoration_line != 0);
    }

    litehtml::uint_ptr id = nextFontId_++;
    fonts_[id] = fi;
    return id;
}

void HtmlWidget::delete_font(litehtml::uint_ptr hFont) {
    fonts_.erase(hFont);
}

litehtml::pixel_t HtmlWidget::text_width(const char* text, litehtml::uint_ptr hFont) {
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return 0;

    fl_font(it->second.flFont, it->second.size);
    return static_cast<litehtml::pixel_t>(fl_width(text));
}

void HtmlWidget::draw_text(litehtml::uint_ptr hdc, const char* text,
                            litehtml::uint_ptr hFont, litehtml::web_color color,
                            const litehtml::position& pos) {
    (void)hdc;
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return;

    fl_font(it->second.flFont, it->second.size);

    TextFragment fragment;
    fragment.text = text ? text : "";
    fragment.pos = pos;
    fragment.byteOffsets.push_back(0);
    fragment.xOffsets.push_back(0);

    int cursorX = 0;
    for (size_t i = 0; i < fragment.text.size();) {
        int len = fl_utf8len1(static_cast<unsigned char>(fragment.text[i]));
        if (len <= 0 || i + static_cast<size_t>(len) > fragment.text.size()) len = 1;
        cursorX += static_cast<int>(fl_width(fragment.text.data() + i, len));
        i += static_cast<size_t>(len);
        fragment.byteOffsets.push_back(static_cast<int>(i));
        fragment.xOffsets.push_back(cursorX);
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
    if (it->second.decorationLine & litehtml::text_decoration_line_underline) {
        int lineY = static_cast<int>(pos.y + pos.height) - fl_descent() + 2;
        fl_line(static_cast<int>(pos.x), lineY,
                static_cast<int>(pos.x + pos.width), lineY);
    }
    if (it->second.decorationLine & litehtml::text_decoration_line_line_through) {
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
    int sbW = scrollbar_->visible() ? 16 : 0;
    client.x = x();
    client.y = y();
    client.width = w() - sbW;
    client.height = h();
}

std::shared_ptr<litehtml::element> HtmlWidget::create_element(
    const char* /*tag_name*/,
    const litehtml::string_map& /*attributes*/,
    const std::shared_ptr<litehtml::document>& /*doc*/) {
    return nullptr; // Use default element creation
}

void HtmlWidget::get_media_features(litehtml::media_features& media) const {
    int sbW = scrollbar_->visible() ? 16 : 0;
    media.type = litehtml::media_type_screen;
    media.width = w() - sbW;
    media.height = h();
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
    if (weight >= 700 && italic) {
        return base + FL_BOLD_ITALIC;
    } else if (weight >= 700) {
        return base + FL_BOLD;
    } else if (italic) {
        return base + FL_ITALIC;
    }
    return base;
}

} // namespace verdad
