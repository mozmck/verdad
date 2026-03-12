#include "ui/SearchPanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/ModuleChoiceUtils.h"
#include "ui/VerseReferenceSort.h"
#include "ui/VerseListCopyMenu.h"
#include "search/SearchIndexer.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_draw.H>


#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace verdad {
class SearchResultBrowser : public Fl_Hold_Browser {
public:
    SearchResultBrowser(SearchPanel* owner, int X, int Y, int W, int H)
        : Fl_Hold_Browser(X, Y, W, H)
        , owner_(owner) {}

    int item_height(void* item) const override;
    int item_width(void* item) const override;
    void item_draw(void* item, int X, int Y, int W, int H) const override;

    int handle(int event) override {
        if (event == FL_PUSH) {
            int button = Fl::event_button();
            if (button == FL_RIGHT_MOUSE) {
                pressedLine_ = 0;
                if (owner_) {
                    owner_->showVerseListContextMenu(Fl::event_x(), Fl::event_y());
                }
                return 1;
            }
            if (button == FL_LEFT_MOUSE || button == FL_MIDDLE_MOUSE) {
                pressedLine_ = lineAtEvent();
            } else {
                pressedLine_ = 0;
            }
        }

        const int button = (event == FL_RELEASE) ? Fl::event_button() : 0;
        const int line = (event == FL_RELEASE) ? lineAtEvent() : 0;
        const bool isDoubleClick = (event == FL_RELEASE) ? (Fl::event_clicks() > 0) : false;
        const int handled = Fl_Hold_Browser::handle(event);

        if (event == FL_RELEASE && owner_ &&
            (button == FL_LEFT_MOUSE || button == FL_MIDDLE_MOUSE)) {
            int targetLine = line;
            if (pressedLine_ > 0 && (targetLine <= 0 || targetLine == pressedLine_)) {
                targetLine = pressedLine_;
            }
            owner_->activateResultLine(targetLine > 0 ? targetLine : value(),
                                       button, isDoubleClick);
            pressedLine_ = 0;
            return 1;
        }

        return handled;
    }

    private:
    int lineAtEvent() {
        // Fl_Browser_::find_item expects the window-relative event y-coordinate.
        void* item = find_item(Fl::event_y());
        return item ? lineno(item) : 0;
    }

    SearchPanel* owner_ = nullptr;
    int pressedLine_ = 0;
};

namespace {

constexpr double kPreviewUpdateDelaySec = 0.08;

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

std::string normalizeStrongsQuery(const std::string& query) {
    std::string q = trimCopy(query);
    if (q.empty()) return "";

    std::string lower = q;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower.rfind("strong's:", 0) == 0) {
        q = trimCopy(q.substr(9));
        lower = trimCopy(lower.substr(9));
    } else if (lower.rfind("strongs:", 0) == 0) {
        q = trimCopy(q.substr(8));
        lower = trimCopy(lower.substr(8));
    } else if (lower.rfind("lemma:", 0) == 0) {
        q = trimCopy(q.substr(6));
        lower = trimCopy(lower.substr(6));
    }

    static const std::regex kWhole(R"(^[HhGg]?\d+[A-Za-z]?$)");
    if (std::regex_match(q, kWhole)) return q;

    // Also accept Strong's searches embedded in labels like:
    // "Search Strong's: G3588".
    static const std::regex kToken(R"(([HhGg]?\d+[A-Za-z]?))");
    if (lower.find("strong") != std::string::npos ||
        lower.find("lemma") != std::string::npos) {
        std::smatch m;
        if (std::regex_search(q, m, kToken)) {
            return m[1].str();
        }
    }

    return "";
}

std::string normalizeWordToken(const std::string& raw) {
    auto isWordChar = [](unsigned char c) {
        return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
    };

    size_t start = 0;
    size_t end = raw.size();
    while (start < end) {
        unsigned char uc = static_cast<unsigned char>(raw[start]);
        if (uc >= 0x80 || isWordChar(uc)) break;
        ++start;
    }
    while (end > start) {
        unsigned char uc = static_cast<unsigned char>(raw[end - 1]);
        if (uc >= 0x80 || isWordChar(uc)) break;
        --end;
    }
    return raw.substr(start, end - start);
}

std::vector<std::string> tokenizeWords(const std::string& text) {
    std::vector<std::string> tokens;
    std::unordered_set<std::string> seen;
    std::istringstream ss(text);
    std::string raw;
    while (ss >> raw) {
        std::string token = normalizeWordToken(raw);
        if (token.empty()) continue;
        std::string lower = token;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (seen.insert(lower).second) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

std::vector<std::string> extractStrongsTokens(const std::string& text) {
    std::vector<std::string> terms;
    std::unordered_set<std::string> seen;

    auto addTerm = [&](const std::string& term) {
        std::string token = trimCopy(term);
        if (token.empty()) return;
        for (char& c : token) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        if (seen.insert(token).second) {
            terms.push_back(token);
        }
    };

    auto addNumericForms = [&](const std::string& digitsRaw,
                               const std::string& suffix) {
        std::string digits = digitsRaw;
        if (digits.empty()) return;

        addTerm(digits + suffix);

        size_t nz = digits.find_first_not_of('0');
        std::string baseDigits = (nz == std::string::npos) ? "0" : digits.substr(nz);
        addTerm(baseDigits + suffix);

        for (int width : {4, 5}) {
            if (static_cast<int>(baseDigits.size()) <= width) {
                addTerm(std::string(width - baseDigits.size(), '0') +
                        baseDigits + suffix);
            }
        }
    };

    static const std::regex kTokenRe(R"(([HhGg]?\d+[A-Za-z]?))");
    auto it = std::sregex_iterator(text.begin(), text.end(), kTokenRe);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        std::string token = trimCopy((*it)[1].str());
        if (token.empty()) continue;

        for (char& c : token) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }

        char prefix = 0;
        size_t pos = 0;
        if (std::isalpha(static_cast<unsigned char>(token[0]))) {
            prefix = token[0];
            pos = 1;
        }

        size_t digitStart = pos;
        while (pos < token.size() &&
               std::isdigit(static_cast<unsigned char>(token[pos]))) {
            ++pos;
        }
        if (digitStart == pos) {
            addTerm(token);
            continue;
        }

        std::string digits = token.substr(digitStart, pos - digitStart);
        std::string suffix = token.substr(pos);

        if (prefix == 'H' || prefix == 'G') {
            addTerm(std::string(1, prefix) + digits + suffix);
            addNumericForms(digits, suffix);
            size_t nz = digits.find_first_not_of('0');
            std::string baseDigits = (nz == std::string::npos) ? "0" : digits.substr(nz);
            for (int width : {4, 5}) {
                if (static_cast<int>(baseDigits.size()) <= width) {
                    addTerm(std::string(1, prefix) +
                            std::string(width - baseDigits.size(), '0') +
                            baseDigits + suffix);
                }
            }
        } else {
            addNumericForms(digits, suffix);
            addTerm("H" + digits + suffix);
            addTerm("G" + digits + suffix);
        }
    }

    return terms;
}

std::string collapseWhitespace(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool lastWasSpace = true;
    for (char c : in) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc)) {
            if (!lastWasSpace) {
                out.push_back(' ');
                lastWasSpace = true;
            }
        } else {
            out.push_back(c);
            lastWasSpace = false;
        }
    }

    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t start = 0;
    while (start < out.size() && out[start] == ' ') ++start;
    return (start == 0) ? out : out.substr(start);
}

void replaceAll(std::string& text,
                const std::string& from,
                const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string htmlToSnippetText(std::string html) {
    static const std::regex kTagRe(R"(<[^>]+>)");
    html = std::regex_replace(html, kTagRe, " ");
    replaceAll(html, "&nbsp;", " ");
    replaceAll(html, "&amp;", "&");
    replaceAll(html, "&quot;", "\"");
    replaceAll(html, "&lt;", "<");
    replaceAll(html, "&gt;", ">");
    replaceAll(html, "&#39;", "'");
    html = collapseWhitespace(html);
    constexpr size_t kMaxSnippetLen = 220;
    if (html.size() > kMaxSnippetLen) {
        html.resize(kMaxSnippetLen);
        html += "...";
    }
    return html;
}

std::string truncateSnippetWords(const std::string& text, size_t maxWords = 18) {
    std::string collapsed = collapseWhitespace(text);
    if (collapsed.empty()) return "";

    size_t pos = 0;
    size_t words = 0;
    size_t cut = collapsed.size();
    bool truncated = false;

    while (pos < collapsed.size()) {
        while (pos < collapsed.size() && collapsed[pos] == ' ') ++pos;
        if (pos >= collapsed.size()) break;

        size_t wordEnd = collapsed.find(' ', pos);
        ++words;
        if (words > maxWords) {
            cut = pos;
            truncated = true;
            break;
        }
        if (wordEnd == std::string::npos) {
            break;
        }
        pos = wordEnd + 1;
    }

    if (!truncated) return collapsed;

    std::string out = collapsed.substr(0, cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty()) out += " ...";
    return out;
}

constexpr int kResultRefColumnWidth = 100;
constexpr int kResultLinePadding = 4;
constexpr int kResultColumnGap = 8;

struct MarkupChunk {
    std::string text;
    bool highlight = false;
};

std::string lowerAsciiCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

bool isWordByte(unsigned char c) {
    return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
}

void appendMarkupChunk(std::vector<MarkupChunk>& chunks,
                       const std::string& text,
                       bool highlight) {
    if (text.empty()) return;
    if (!chunks.empty() && chunks.back().highlight == highlight) {
        chunks.back().text += text;
        return;
    }
    chunks.push_back({text, highlight});
}

std::vector<MarkupChunk> parseHighlightedMarkup(const std::string& markup) {
    std::vector<MarkupChunk> chunks;
    bool highlight = false;

    for (size_t pos = 0; pos < markup.size();) {
        if (markup[pos] != '<') {
            size_t nextTag = markup.find('<', pos);
            size_t end = (nextTag == std::string::npos) ? markup.size() : nextTag;
            appendMarkupChunk(chunks, markup.substr(pos, end - pos), highlight);
            pos = end;
            continue;
        }

        size_t tagEnd = markup.find('>', pos);
        if (tagEnd == std::string::npos) {
            appendMarkupChunk(chunks, markup.substr(pos), highlight);
            break;
        }

        std::string tag = markup.substr(pos, tagEnd - pos + 1);
        std::string lowerTag = lowerAsciiCopy(tag);
        if (lowerTag.rfind("<span", 0) == 0 &&
            lowerTag.find("searchhit") != std::string::npos) {
            highlight = true;
        } else if (lowerTag.rfind("</span", 0) == 0) {
            highlight = false;
        }

        pos = tagEnd + 1;
    }

    return chunks;
}

std::vector<MarkupChunk> normalizeMarkupWhitespace(
    const std::vector<MarkupChunk>& chunks) {
    std::vector<MarkupChunk> normalized;
    bool pendingSpace = false;
    bool spaceHighlight = false;
    bool hasOutput = false;

    for (const auto& chunk : chunks) {
        for (char c : chunk.text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (hasOutput) {
                    pendingSpace = true;
                    spaceHighlight = chunk.highlight;
                }
                continue;
            }

            if (pendingSpace) {
                appendMarkupChunk(normalized, " ", spaceHighlight);
                pendingSpace = false;
            }

            appendMarkupChunk(normalized, std::string(1, c), chunk.highlight);
            hasOutput = true;
        }
    }

    return normalized;
}

void drawMarkupChunks(const std::vector<MarkupChunk>& chunks,
                      int X, int Y, int W, int H,
                      Fl_Color fg,
                      Fl_Color highlightBg,
                      Fl_Color highlightFg) {
    fl_push_clip(X, Y, W, H);
    fl_color(fg);

    int cursorX = X;
    const int baseline = Y + ((H + fl_height()) / 2) - fl_descent();
    for (const auto& chunk : chunks) {
        if (chunk.text.empty()) continue;

        int chunkWidth = static_cast<int>(fl_width(chunk.text.c_str()));
        if (chunk.highlight) {
            fl_color(highlightBg);
            fl_rectf(cursorX - 1, Y + 2, chunkWidth + 2, std::max(0, H - 4));
            fl_color(highlightFg);
        } else {
            fl_color(fg);
        }

        fl_draw(chunk.text.c_str(), cursorX, baseline);
        cursorX += chunkWidth;
        if (cursorX >= X + W) break;
    }

    fl_pop_clip();
}

void markRange(std::vector<bool>& mask, size_t start, size_t end) {
    if (start >= end || start >= mask.size()) return;
    end = std::min(end, mask.size());
    for (size_t i = start; i < end; ++i) {
        mask[i] = true;
    }
}

void markLiteralMatches(std::vector<bool>& mask,
                        const std::string& text,
                        const std::string& term,
                        bool requireWordBoundaries) {
    if (text.empty() || term.empty()) return;

    std::string lowerText = lowerAsciiCopy(text);
    std::string lowerTerm = lowerAsciiCopy(term);
    size_t pos = 0;
    while ((pos = lowerText.find(lowerTerm, pos)) != std::string::npos) {
        size_t end = pos + lowerTerm.size();
        bool ok = true;
        if (requireWordBoundaries) {
            if (pos > 0 && isWordByte(static_cast<unsigned char>(text[pos - 1]))) {
                ok = false;
            }
            if (end < text.size() &&
                isWordByte(static_cast<unsigned char>(text[end]))) {
                ok = false;
            }
        }
        if (ok) {
            markRange(mask, pos, end);
            pos = end;
        } else {
            ++pos;
        }
    }
}

void markRegexMatches(std::vector<bool>& mask,
                      const std::string& text,
                      const std::regex& re) {
    auto begin = std::sregex_iterator(text.begin(), text.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        size_t pos = static_cast<size_t>((*it).position());
        size_t len = static_cast<size_t>((*it).length());
        if (len == 0) continue;
        markRange(mask, pos, pos + len);
    }
}

std::string wrapHighlightMask(const std::string& text,
                              const std::vector<bool>& mask) {
    if (text.empty() || mask.empty()) return text;

    std::string out;
    out.reserve(text.size() + 32);
    bool inHighlight = false;
    for (size_t i = 0; i < text.size(); ++i) {
        bool shouldHighlight = i < mask.size() && mask[i];
        if (shouldHighlight && !inHighlight) {
            out += "<span class=\"searchhit\">";
            inHighlight = true;
        } else if (!shouldHighlight && inHighlight) {
            out += "</span>";
            inHighlight = false;
        }
        out.push_back(text[i]);
    }
    if (inHighlight) out += "</span>";
    return out;
}

std::string highlightPlainSnippetMarkup(
    const std::string& text,
    const std::vector<std::string>& terms,
    const std::string& phrase,
    const std::regex* regexPattern,
    bool phraseMode) {

    if (text.empty()) return "";

    std::vector<bool> mask(text.size(), false);
    if (regexPattern) {
        markRegexMatches(mask, text, *regexPattern);
    } else if (phraseMode && !phrase.empty()) {
        markLiteralMatches(mask, text, phrase, false);
        if (std::find(mask.begin(), mask.end(), true) == mask.end()) {
            for (const auto& term : terms) {
                markLiteralMatches(mask, text, term, true);
            }
        }
    } else {
        for (const auto& term : terms) {
            markLiteralMatches(mask, text, term, true);
        }
    }

    return wrapHighlightMask(text, mask);
}

bool extractTagAttribute(const std::string& tag,
                         const std::string& name,
                         std::string& valueOut) {
    std::string lowerTag = lowerAsciiCopy(tag);
    std::string needle = lowerAsciiCopy(name) + "=";
    size_t pos = lowerTag.find(needle);
    if (pos == std::string::npos) return false;

    size_t valueStart = pos + needle.size();
    if (valueStart >= tag.size()) return false;

    char quote = tag[valueStart];
    if (quote == '"' || quote == '\'') {
        ++valueStart;
        size_t end = tag.find(quote, valueStart);
        if (end == std::string::npos) return false;
        valueOut = tag.substr(valueStart, end - valueStart);
        return true;
    }

    size_t end = valueStart;
    while (end < tag.size() &&
           !std::isspace(static_cast<unsigned char>(tag[end])) &&
           tag[end] != '>') {
        ++end;
    }
    valueOut = tag.substr(valueStart, end - valueStart);
    return !valueOut.empty();
}

bool classListHasToken(const std::string& classList,
                       const std::string& token) {
    std::istringstream ss(classList);
    std::string part;
    while (ss >> part) {
        if (part == token) return true;
    }
    return false;
}

std::string addClassTokenToTag(const std::string& tag,
                               const std::string& token) {
    std::string classes;
    if (extractTagAttribute(tag, "class", classes)) {
        if (classListHasToken(classes, token)) return tag;

        size_t pos = lowerAsciiCopy(tag).find("class=");
        if (pos == std::string::npos) return tag;
        size_t valueStart = pos + 6;
        if (valueStart >= tag.size()) return tag;
        char quote = tag[valueStart];
        if (quote != '"' && quote != '\'') return tag;
        ++valueStart;
        size_t valueEnd = tag.find(quote, valueStart);
        if (valueEnd == std::string::npos) return tag;

        std::string out = tag;
        out.insert(valueEnd, " " + token);
        return out;
    }

    size_t insertPos = tag.rfind('>');
    if (insertPos == std::string::npos) return tag;
    std::string out = tag;
    out.insert(insertPos, " class=\"" + token + "\"");
    return out;
}

bool strongsAttrMatches(const std::string& attr,
                        const std::vector<std::string>& queryTokens) {
    if (attr.empty() || queryTokens.empty()) return false;

    std::unordered_set<std::string> wanted;
    for (const auto& token : queryTokens) {
        wanted.insert(lowerAsciiCopy(token));
    }

    size_t start = 0;
    while (start <= attr.size()) {
        size_t end = attr.find('|', start);
        std::string token = attr.substr(start, end - start);
        token = trimCopy(token);
        if (!token.empty() && wanted.count(lowerAsciiCopy(token)) != 0) {
            return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

} // namespace

int SearchResultBrowser::item_height(void* item) const {
    return Fl_Hold_Browser::item_height(item);
}

int SearchResultBrowser::item_width(void* item) const {
    if (!owner_) return w();
    int line = lineno(item);
    if (line <= 0) return w();
    return std::max(w(), owner_->resultLineWidth(line));
}

void SearchResultBrowser::item_draw(void* item,
                                    int X, int Y, int W, int H) const {
    const bool selected = item_selected(item) != 0;
    Fl_Color bg = selected ? selection_color() : color();
    Fl_Color fg = fl_contrast(textcolor(), bg);
    Fl_Color highlightBg = fl_rgb_color(255, 255, 128);
    Fl_Color highlightFg = FL_BLACK;

    fl_color(bg);
    fl_rectf(X, Y, W, H);

    if (!owner_) return;

    int line = lineno(item);
    if (line <= 0 || line > static_cast<int>(owner_->results_.size())) return;
    if (line > static_cast<int>(owner_->resultDisplayKeys_.size())) return;

    fl_font(textfont(), textsize());

    const std::string& refLabel = owner_->resultDisplayKeys_[line - 1];
    const int refColumnWidth = owner_->resultRefColumnWidth_;
    const auto refChunks = normalizeMarkupWhitespace(
        parseHighlightedMarkup(refLabel));
    drawMarkupChunks(refChunks,
                     X + kResultLinePadding,
                     Y, refColumnWidth - kResultLinePadding, H,
                     fg, highlightBg, highlightFg);

    const std::string& snippetMarkup = owner_->results_[line - 1].text;
    if (snippetMarkup.empty()) return;

    int snippetX = X + refColumnWidth + kResultColumnGap;
    int snippetW = std::max(0, W - (snippetX - X) - kResultLinePadding);
    if (snippetW <= 0) return;

    const auto snippetChunks = normalizeMarkupWhitespace(
        parseHighlightedMarkup(snippetMarkup));
    drawMarkupChunks(snippetChunks, snippetX, Y, snippetW, H,
                     fg, highlightBg, highlightFg);
}

SearchPanel::SearchPanel(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , moduleChoice_(nullptr)
    , searchType_(nullptr)
    , resultStatus_(nullptr)
    , resultBrowser_(nullptr) {

    begin();

    int padding = 2;
    int choiceH = 25;

    int cy = Y + padding;

    // Module to search
    moduleChoice_ = new Fl_Choice(X + padding, cy, (W - 2 * padding) / 2, choiceH);
    moduleChoice_->tooltip("Module to search in");

    // Search type
    searchType_ = new Fl_Choice(X + padding + (W - 2 * padding) / 2 + 2, cy,
                                 (W - 2 * padding) / 2 - 2, choiceH);
    searchType_->add("Multi-word");
    searchType_->add("Exact phrase");
    searchType_->add("Regex");
    searchType_->value(0);
    searchType_->tooltip("Search type");

    cy += choiceH + padding;

    resultStatus_ = new Fl_Box(X + padding, cy, W - 2 * padding, 20);
    resultStatus_->box(FL_THIN_DOWN_BOX);
    resultStatus_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    cy += 20 + padding;

    // Result list (occupies full area below selectors).
    resultBrowser_ = new SearchResultBrowser(this, X + padding, cy,
                                             W - 2 * padding, H - (cy - Y) - padding);
    static int widths[] = { 100, 0 };  // widths for each column
    resultBrowser_->column_widths(widths); // assign array to widget
    resultBrowser_->has_scrollbar(Fl_Browser_::BOTH);
    // Preview updates should follow selection changes, but navigation/opening
    // is handled directly from mouse-release events in SearchResultBrowser.
    resultBrowser_->when(FL_WHEN_CHANGED);
    resultBrowser_->callback(onResultSelect, this);

    end();
    resizable(resultBrowser_);

    populateModules();

    if (app_ && app_->searchIndexer()) {
        indexingIndicatorActive_ = true;
        Fl::add_timeout(0.2, onIndexingPoll, this);
        updateIndexingIndicator();
    }
}

SearchPanel::~SearchPanel() {
    cancelPendingPreviewUpdate();
    if (indexingIndicatorActive_) {
        Fl::remove_timeout(onIndexingPoll, this);
        indexingIndicatorActive_ = false;
    }
}

void SearchPanel::search(const std::string& query,
                         const std::string& moduleOverride) {
    cancelPendingPreviewUpdate();
    pendingPreviewModule_.clear();
    pendingPreviewKey_.clear();
    lastPreviewModule_.clear();
    lastPreviewKey_.clear();
    resultDisplayKeys_.clear();
    resultLineWidths_.clear();
    resultRefColumnWidth_ = kResultRefColumnWidth;
    resetHighlightState();
    results_.clear();
    resultBrowser_->clear();
    resultBrowser_->value(0);

    std::string trimmedQuery = trimCopy(query);
    if (trimmedQuery.empty()) return;

    // Module selection precedence:
    // 1) explicit override (context menu / module-aware action)
    // 2) search panel module dropdown (manual search)
    // 3) active Bible tab module (fallback)
    std::string moduleName = trimCopy(moduleOverride);
    if (moduleName.empty()) {
        moduleName = module_choice::selectedModuleName(
            moduleChoice_, moduleChoiceModules_);
    }
    if (moduleName.empty() &&
        app_->mainWindow() && app_->mainWindow()->biblePane()) {
        moduleName = trimCopy(app_->mainWindow()->biblePane()->currentModule());
    }
    if (moduleName.empty()) {
        fl_alert("No active Bible module to search.");
        return;
    }

    module_choice::applyChoiceValue(moduleChoice_, moduleChoiceModules_,
                                    moduleChoiceLabels_, moduleName);

    // Get search type
    const bool exactPhrase = (searchType_->value() == 1);
    const bool regexSearch = (searchType_->value() == 2);
    std::string strongsQuery = normalizeStrongsQuery(trimmedQuery);
    bool isStrongs = !strongsQuery.empty();
    if (isStrongs) trimmedQuery = strongsQuery;

    if (regexSearch && !isStrongs) {
        try {
            highlightRegex_ = std::regex(trimmedQuery,
                                         std::regex::ECMAScript |
                                         std::regex::icase);
            highlightRegexValid_ = true;
        } catch (const std::regex_error&) {
            highlightRegexValid_ = false;
            fl_alert("Invalid regex pattern.");
            setResultCountLabel("(invalid regex)");
            return;
        }
    }

    if (isStrongs) {
        highlightMode_ = HighlightMode::Strongs;
        highlightStrongs_ = extractStrongsTokens(strongsQuery);
    } else if (regexSearch) {
        highlightMode_ = HighlightMode::Regex;
    } else if (exactPhrase) {
        highlightMode_ = HighlightMode::Phrase;
        highlightPhrase_ = trimmedQuery;
        highlightTerms_ = tokenizeWords(trimmedQuery);
    } else {
        highlightMode_ = HighlightMode::Terms;
        highlightTerms_ = tokenizeWords(trimmedQuery);
    }

    bool indexingPending = false;
    bool usedIndexer = false;
    bool fallbackDeferred = false;
    bool moduleIndexed = false;
    SearchIndexer* indexer = app_->searchIndexer();
    if (indexer) {
        indexer->queueModuleIndex(moduleName);
        moduleIndexed = indexer->isModuleIndexed(moduleName);
        indexingPending = !moduleIndexed;
    }

    // Prefer indexed searches when the index exists.
    if (indexer) {
        usedIndexer = true;
        if (isStrongs) {
            results_ = indexer->searchStrongs(moduleName, strongsQuery);
        } else if (regexSearch) {
            if (moduleIndexed) {
                results_ = indexer->searchRegex(moduleName, trimmedQuery, false);
            } else {
                fallbackDeferred = true;
            }
        } else {
            results_ = indexer->searchWord(moduleName, trimmedQuery, exactPhrase);
        }
    }

    bool runSwordFallback = false;
    int swordSearchType = -1;
    std::string swordQuery = isStrongs ? strongsQuery : trimmedQuery;
    if (!indexer) {
        if (isStrongs) {
            runSwordFallback = true;
        } else if (regexSearch) {
            // Avoid unstable SWORD regex search path.
            fallbackDeferred = true;
        } else {
            runSwordFallback = true;
            swordSearchType = exactPhrase ? 1 : -1;
        }
    } else if (isStrongs && results_.empty() && !moduleIndexed) {
        // Allow immediate Strong's lookups before initial module indexing finishes.
        runSwordFallback = true;
    }

    if (runSwordFallback && !swordQuery.empty()) {
        swordSearchInProgress_ = true;
        setResultCountLabel("(searching...)");
        Fl::flush();

        if (isStrongs) {
            results_ = app_->swordManager().searchStrongs(moduleName, swordQuery);
        } else {
            results_ = app_->swordManager().search(
                moduleName, swordQuery, swordSearchType, "",
                [this](float progress) {
                    int pct = static_cast<int>(
                        std::clamp(progress, 0.0f, 1.0f) * 100.0f);
                    setResultCountLabel("(searching " + std::to_string(pct) + "%)");
                    Fl::flush();
                });
        }

        swordSearchInProgress_ = false;
        updateIndexingIndicator();
    } else {
        updateIndexingIndicator();
    }

    if (!isStrongs && runSwordFallback && !results_.empty()) {
        const std::regex* regexPattern = highlightRegexValid_
                                             ? &highlightRegex_
                                             : nullptr;
        const bool phraseMode = (highlightMode_ == HighlightMode::Phrase);
        for (auto& result : results_) {
            std::string snippet = collapseWhitespace(result.text);
            result.text = highlightPlainSnippetMarkup(
                snippet, highlightTerms_, highlightPhrase_,
                regexPattern, phraseMode);
        }
    }

    verse_reference_sort::sortSearchResultsCanonical(
        app_->swordManager(), moduleName, results_);

    // Populate result browser
    for (const auto& r : results_) {
        const std::string& resultModule = r.module.empty() ? moduleName : r.module;
        std::string shortKey = app_->swordManager().getShortReference(resultModule, r.key);
        resultDisplayKeys_.push_back(shortKey.empty() ? r.key : shortKey);
        resultBrowser_->add(" ");
    }
    rebuildResultMetrics();

    std::string labelSuffix;
    if (usedIndexer && indexingPending) {
        labelSuffix += "(indexing...)";
    }
    if (fallbackDeferred) {
        if (!labelSuffix.empty()) labelSuffix += " ";
        labelSuffix += "(regex requires module index)";
    }
    setResultCountLabel(labelSuffix);

    if (indexer && indexingPending) {
        startIndexingIndicator(moduleName);
    } else {
        stopIndexingIndicator();
    }

    if (app_ && app_->mainWindow()) {
        std::string mod = trimCopy(moduleName);
        if (mod.empty()) mod = "module";
        app_->mainWindow()->showTransientStatus(
            "Search (" + mod + "): " + std::to_string(results_.size()) + " result(s)",
            2.6);
    }

    redraw();
}

void SearchPanel::showReferenceResults(const std::string& moduleName,
                                       const std::vector<std::string>& references,
                                       const std::string& statusSuffix) {
    cancelPendingPreviewUpdate();
    pendingPreviewModule_.clear();
    pendingPreviewKey_.clear();
    lastPreviewModule_.clear();
    lastPreviewKey_.clear();
    resultDisplayKeys_.clear();
    resultLineWidths_.clear();
    resultRefColumnWidth_ = kResultRefColumnWidth;
    resetHighlightState();
    results_.clear();
    resultBrowser_->clear();
    resultBrowser_->value(0);
    stopIndexingIndicator();

    std::string module = trimCopy(moduleName);
    if (module.empty() &&
        app_ && app_->mainWindow() && app_->mainWindow()->biblePane()) {
        module = trimCopy(app_->mainWindow()->biblePane()->currentModule());
    }
    if (module.empty()) {
        setResultCountLabel("(no active module)");
        return;
    }

    setSelectedModule(module);

    for (const auto& rawRef : references) {
        std::string ref = trimCopy(rawRef);
        if (ref.empty()) continue;

        SearchResult result;
        result.module = module;
        result.key = ref;
        result.text = truncateSnippetWords(
            app_->swordManager().getVersePlainText(module, ref));
        if (result.text.empty()) result.text = ref;
        results_.push_back(std::move(result));
    }

    for (const auto& r : results_) {
        std::string shortKey = app_->swordManager().getShortReference(r.module, r.key);
        resultDisplayKeys_.push_back(shortKey.empty() ? r.key : shortKey);
        resultBrowser_->add(" ");
    }
    rebuildResultMetrics();

    setResultCountLabel(statusSuffix.empty() ? "(linked references)" : statusSuffix);

    if (!results_.empty()) {
        resultBrowser_->value(1);
        schedulePreviewUpdate(results_.front());
    }

    redraw();
}

void SearchPanel::clear() {
    cancelPendingPreviewUpdate();
    pendingPreviewModule_.clear();
    pendingPreviewKey_.clear();
    lastPreviewModule_.clear();
    lastPreviewKey_.clear();
    resultDisplayKeys_.clear();
    resultLineWidths_.clear();
    resultRefColumnWidth_ = kResultRefColumnWidth;
    resetHighlightState();
    results_.clear();
    resultBrowser_->clear();
    resultBrowser_->value(0);
    stopIndexingIndicator();
    setResultCountLabel();
    updateIndexingIndicator();
}

const SearchResult* SearchPanel::selectedResult() const {
    int idx = resultBrowser_->value();
    if (idx > 0 && idx <= static_cast<int>(results_.size())) {
        return &results_[idx - 1];
    }
    return nullptr;
}

void SearchPanel::populateModules() {
    if (!moduleChoice_) return;

    // Search targets Bible modules.
    auto bibles = app_->swordManager().getBibleModules();
    module_choice::populateChoice(moduleChoice_, bibles,
                                  moduleChoiceModules_, moduleChoiceLabels_);
}

void SearchPanel::setSelectedModule(const std::string& moduleName) {
    std::string name = trimCopy(moduleName);
    if (!moduleChoice_ || name.empty()) return;
    module_choice::applyChoiceValue(moduleChoice_, moduleChoiceModules_,
                                    moduleChoiceLabels_, name);
}

void SearchPanel::resetHighlightState() {
    highlightMode_ = HighlightMode::None;
    highlightTerms_.clear();
    highlightStrongs_.clear();
    highlightPhrase_.clear();
    highlightRegexValid_ = false;
}

void SearchPanel::rebuildResultMetrics() {
    resultLineWidths_.clear();
    resultRefColumnWidth_ = kResultRefColumnWidth;
    if (!resultBrowser_) return;

    fl_font(resultBrowser_->textfont(), resultBrowser_->textsize());

    auto measureMarkupWidth = [](const std::string& markup) -> int {
        const auto chunks = normalizeMarkupWhitespace(
            parseHighlightedMarkup(markup));
        int width = 0;
        for (const auto& chunk : chunks) {
            if (chunk.text.empty()) continue;
            width += static_cast<int>(fl_width(chunk.text.c_str()));
        }
        return width;
    };

    for (const auto& refLabel : resultDisplayKeys_) {
        int width = measureMarkupWidth(refLabel) + (kResultLinePadding * 2);
        resultRefColumnWidth_ = std::max(resultRefColumnWidth_, width);
    }

    resultLineWidths_.reserve(results_.size());
    for (size_t i = 0; i < results_.size(); ++i) {
        int width = resultRefColumnWidth_ + (kResultLinePadding * 2);
        if (i < results_.size() && !results_[i].text.empty()) {
            width += kResultColumnGap + measureMarkupWidth(results_[i].text);
        }
        resultLineWidths_.push_back(width);
    }
}

int SearchPanel::resultLineWidth(int line) const {
    if (line <= 0 || line > static_cast<int>(resultLineWidths_.size())) {
        return resultBrowser_ ? resultBrowser_->w() : 0;
    }
    return resultLineWidths_[line - 1];
}

std::string SearchPanel::applyPreviewHighlights(const std::string& html) const {
    if (html.empty() || highlightMode_ == HighlightMode::None) {
        return html;
    }

    if (highlightMode_ == HighlightMode::Strongs) {
        if (highlightStrongs_.empty()) return html;

        std::string out;
        out.reserve(html.size() + 32);
        for (size_t pos = 0; pos < html.size();) {
            if (html[pos] != '<') {
                size_t nextTag = html.find('<', pos);
                size_t end = (nextTag == std::string::npos) ? html.size() : nextTag;
                out.append(html, pos, end - pos);
                pos = end;
                continue;
            }

            size_t tagEnd = html.find('>', pos);
            if (tagEnd == std::string::npos) {
                out.append(html, pos, html.size() - pos);
                break;
            }

            std::string tag = html.substr(pos, tagEnd - pos + 1);
            std::string lowerTag = lowerAsciiCopy(tag);
            if (lowerTag.rfind("<span", 0) == 0 &&
                lowerTag.find("</span") != 0) {
                std::string classes;
                std::string strongsAttr;
                if (extractTagAttribute(tag, "class", classes) &&
                    classListHasToken(classes, "w") &&
                    extractTagAttribute(tag, "data-strong", strongsAttr) &&
                    strongsAttrMatches(strongsAttr, highlightStrongs_)) {
                    out += addClassTokenToTag(tag, "searchhit");
                } else {
                    out += tag;
                }
            } else {
                out += tag;
            }

            pos = tagEnd + 1;
        }
        return out;
    }

    const std::regex* regexPattern = highlightRegexValid_ ? &highlightRegex_ : nullptr;
    if (highlightTerms_.empty() && highlightPhrase_.empty() && !regexPattern) {
        return html;
    }

    const bool phraseMode = (highlightMode_ == HighlightMode::Phrase);
    std::string out;
    out.reserve(html.size() + 48);
    for (size_t pos = 0; pos < html.size();) {
        if (html[pos] == '<') {
            size_t tagEnd = html.find('>', pos);
            if (tagEnd == std::string::npos) {
                out.append(html, pos, html.size() - pos);
                break;
            }
            out.append(html, pos, tagEnd - pos + 1);
            pos = tagEnd + 1;
            continue;
        }

        size_t nextTag = html.find('<', pos);
        size_t end = (nextTag == std::string::npos) ? html.size() : nextTag;
        out += highlightPlainSnippetMarkup(
            html.substr(pos, end - pos),
            highlightTerms_,
            highlightPhrase_,
            regexPattern,
            phraseMode);
        pos = end;
    }
    return out;
}

void SearchPanel::setResultCountLabel(const std::string& suffix) {
    if (!resultStatus_) return;
    if (!suffix.empty()) {
        statusSuffix_ = suffix;
    } else {
        statusSuffix_.clear();
    }
    std::string label = "Results: " + std::to_string(results_.size());
    if (!statusSuffix_.empty()) {
        label += " " + statusSuffix_;
    }
    if (label == currentResultLabel_) return;
    currentResultLabel_ = label;
    resultStatus_->copy_label(label.c_str());
}

void SearchPanel::startIndexingIndicator(const std::string& moduleName) {
    std::string module = trimCopy(moduleName);
    if (module.empty()) return;
    if (!app_ || !app_->searchIndexer()) return;

    indexingModule_ = module;
    updateIndexingIndicator();
}

void SearchPanel::stopIndexingIndicator() {
    indexingModule_.clear();
}

void SearchPanel::updateIndexingIndicator() {
    if (!indexingIndicatorActive_) return;
    if (swordSearchInProgress_) return;

    // Only update indexing status in the Search tab.
    if (!isSearchTabActive()) {
        return;
    }

    SearchIndexer* indexer = app_ ? app_->searchIndexer() : nullptr;
    if (!indexer) {
        setResultCountLabel();
        return;
    }

    std::string displayModule;
    int progress = -1;

    std::string activeModule;
    int activePercent = 0;
    if (indexer->activeIndexingTask(activeModule, activePercent)) {
        displayModule = module_choice::displayLabelForModuleName(
            moduleChoiceModules_, moduleChoiceLabels_, activeModule);
        progress = activePercent;
    } else if (!indexingModule_.empty()) {
        int p = indexer->moduleIndexProgress(indexingModule_);
        if (p >= 0 && p < 100) {
            displayModule = module_choice::displayLabelForModuleName(
                moduleChoiceModules_, moduleChoiceLabels_, indexingModule_);
            progress = p;
        }
    } else if (moduleChoice_ && moduleChoice_->mvalue() &&
               moduleChoice_->mvalue()->label()) {
        std::string selected = module_choice::selectedModuleName(
            moduleChoice_, moduleChoiceModules_);
        int p = indexer->moduleIndexProgress(selected);
        if (p >= 0 && p < 100) {
            displayModule = module_choice::displayLabelForModuleName(
                moduleChoiceModules_, moduleChoiceLabels_, selected);
            progress = p;
        }
    }

    if (displayModule.empty() || progress < 0 || progress >= 100) {
        setResultCountLabel();
        return;
    }

    setResultCountLabel("(indexing " + displayModule + ": " +
                        std::to_string(progress) + "%)");
}

bool SearchPanel::isSearchTabActive() const {
    if (!visible_r()) return false;
    auto* tabs = dynamic_cast<Fl_Tabs*>(parent());
    if (!tabs) return true;
    return tabs->value() == this;
}

void SearchPanel::cancelPendingPreviewUpdate() {
    if (!previewUpdateScheduled_) return;
    Fl::remove_timeout(onDeferredPreviewUpdate, this);
    previewUpdateScheduled_ = false;
}

void SearchPanel::schedulePreviewUpdate(const SearchResult& result) {
    pendingPreviewModule_ = result.module;
    pendingPreviewKey_ = result.key;
    if (previewUpdateScheduled_) {
        Fl::remove_timeout(onDeferredPreviewUpdate, this);
    }

    previewUpdateScheduled_ = true;
    // Debounce rapid keyboard navigation so the browser can repaint smoothly.
    Fl::add_timeout(kPreviewUpdateDelaySec, onDeferredPreviewUpdate, this);
}

void SearchPanel::applyPendingPreviewUpdate() {
    previewUpdateScheduled_ = false;

    if (pendingPreviewKey_.empty()) return;
    if (pendingPreviewModule_ == lastPreviewModule_ &&
        pendingPreviewKey_ == lastPreviewKey_) {
        return;
    }
    if (!app_ || !app_->mainWindow() || !app_->mainWindow()->leftPane()) return;

    std::string html = app_->swordManager().getVerseText(
        pendingPreviewModule_, pendingPreviewKey_);
    html = applyPreviewHighlights(html);
    app_->mainWindow()->leftPane()->setVersePreviewText(
        html, pendingPreviewModule_, pendingPreviewKey_);
    lastPreviewModule_ = pendingPreviewModule_;
    lastPreviewKey_ = pendingPreviewKey_;
}

void SearchPanel::activateResultLine(int line, int mouseButton, bool isDoubleClick) {
    if (!app_ || !app_->mainWindow()) return;
    if (line <= 0 || line > static_cast<int>(results_.size())) return;

    const SearchResult& result = results_[line - 1];

    if (mouseButton == FL_MIDDLE_MOUSE) {
        app_->mainWindow()->openInNewStudyTab(result.module, result.key);
        return;
    }

    if (mouseButton == FL_LEFT_MOUSE && isDoubleClick) {
        app_->mainWindow()->navigateTo(result.module, result.key);
    }
}

void SearchPanel::showVerseListContextMenu(int screenX, int screenY) {
    if (!app_ || results_.empty()) return;

    std::vector<verse_list_copy::Entry> entries;
    entries.reserve(results_.size());
    for (const auto& result : results_) {
        std::string key = trimCopy(result.key);
        if (key.empty()) continue;
        entries.push_back({result.module, key});
    }

    verse_list_copy::showVerseListCopyMenu(
        app_->swordManager(), entries, screenX, screenY);
}

void SearchPanel::onIndexingPoll(void* data) {
    auto* self = static_cast<SearchPanel*>(data);
    if (!self || !self->indexingIndicatorActive_) return;

    self->updateIndexingIndicator();
    Fl::repeat_timeout(0.2, onIndexingPoll, self);
}

void SearchPanel::onDeferredPreviewUpdate(void* data) {
    auto* self = static_cast<SearchPanel*>(data);
    if (!self) return;
    self->applyPendingPreviewUpdate();
}

void SearchPanel::onResultSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<SearchPanel*>(data);

    const SearchResult* result = self->selectedResult();
    if (!result) {
        return;
    }

    self->schedulePreviewUpdate(*result);
}

void SearchPanel::onResultDoubleClick(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<SearchPanel*>(data);

    const SearchResult* result = self->selectedResult();
    if (!result) return;

    // Navigate to the verse
    if (self->app_->mainWindow()) {
        self->app_->mainWindow()->navigateTo(result->module, result->key);
    }
}

} // namespace verdad
