#include "ui/SearchPanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/ModuleChoiceUtils.h"
#include "ui/VerseReferenceSort.h"
#include "ui/VerseListCopyMenu.h"
#include "search/SearchIndexer.h"
#include "search/SmartSearch.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_draw.H>


#include <algorithm>
#include <cctype>
#include <map>
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
constexpr double kStatusPollDelaySec = 0.1;

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
    // Strip HTML tags without regex for performance
    {
        std::string stripped;
        stripped.reserve(html.size());
        bool inTag = false;
        for (char c : html) {
            if (c == '<') { inTag = true; stripped.push_back(' '); continue; }
            if (c == '>') { inTag = false; continue; }
            if (!inTag) stripped.push_back(c);
        }
        html = std::move(stripped);
    }
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
constexpr const char* kCurrentBibleModuleToken = "__current_bible__";
constexpr const char* kAllModulesToken = "__all_modules__";

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

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
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
    , targetChoice_(nullptr)
    , moduleChoice_(nullptr)
    , resourceTypeChoice_(nullptr)
    , bibleScopeChoice_(nullptr)
    , searchType_(nullptr)
    , sortChoice_(nullptr)
    , resultStatus_(nullptr)
    , searchProgress_(nullptr)
    , resultBrowser_(nullptr) {

    begin();

    int padding = 2;
    int choiceH = 25;
    int colGap = 2;
    int colW = (W - 2 * padding - colGap) / 2;

    int cy = Y + padding;

    targetChoice_ = new Fl_Choice(X + padding, cy, colW, choiceH);
    targetChoice_->add("Bible only");
    targetChoice_->add("Library only");
    targetChoice_->add("All resources");
    targetChoice_->value(0);
    targetChoice_->tooltip("Search target");
    targetChoice_->callback(onFilterChoiceChanged, this);

    moduleChoice_ = new Fl_Choice(X + padding + colW + colGap, cy, colW, choiceH);
    moduleChoice_->tooltip("Module filter");
    moduleChoice_->callback(onFilterChoiceChanged, this);

    cy += choiceH + padding;

    resourceTypeChoice_ = new Fl_Choice(X + padding, cy, colW, choiceH);
    resourceTypeChoice_->tooltip("Resource type filter");
    resourceTypeChoice_->callback(onFilterChoiceChanged, this);

    searchType_ = new Fl_Choice(X + padding + colW + colGap, cy, colW, choiceH);
    searchType_->add("Multi-word");
    searchType_->add("Exact phrase");
    searchType_->add("Regex");
    searchType_->add("Smart");
    searchType_->value(0);
    searchType_->tooltip("Search type");

    cy += choiceH + padding;

    bibleScopeChoice_ = new Fl_Choice(X + padding, cy, colW, choiceH);
    bibleScopeChoice_->tooltip("Bible scope");
    bibleScopeChoice_->callback(onFilterChoiceChanged, this);

    sortChoice_ = new Fl_Choice(X + padding + colW + colGap, cy, colW, choiceH);
    sortChoice_->add("Relevance");
    sortChoice_->add("Canonical");
    sortChoice_->add("By module");
    sortChoice_->value(1);
    sortChoice_->tooltip("Result ordering");
    sortChoice_->callback(onSortChoiceChanged, this);

    cy += choiceH + padding;

    resultStatus_ = new Fl_Box(X + padding, cy, W - 2 * padding, 20);
    resultStatus_->box(FL_THIN_DOWN_BOX);
    resultStatus_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    searchProgress_ = new Fl_Progress(X + padding, cy, W - 2 * padding, 20);
    searchProgress_->box(FL_THIN_DOWN_BOX);
    searchProgress_->color(FL_BACKGROUND2_COLOR, FL_DARK_GREEN);
    searchProgress_->minimum(0.0f);
    searchProgress_->maximum(1.0f);
    searchProgress_->value(0.0f);
    searchProgress_->hide();

    cy += 20 + padding;

    // Result list (occupies full area below selectors).
    resultBrowser_ = new SearchResultBrowser(this, X + padding, cy,
                                             W - 2 * padding, H - (cy - Y) - padding);
    static int widths[] = { 100, 0 };  // widths for each column
    resultBrowser_->column_widths(widths); // assign array to widget
    resultBrowser_->has_scrollbar(Fl_Browser_::BOTH);
    resultBrowser_->linespacing(app_ ? app_->appearanceSettings().browserLineSpacing : 0);
    // Preview updates should follow selection changes, but navigation/opening
    // is handled directly from mouse-release events in SearchResultBrowser.
    resultBrowser_->when(FL_WHEN_CHANGED);
    resultBrowser_->callback(onResultSelect, this);

    end();
    resizable(resultBrowser_);

    populateResourceTypes();
    populateBibleScopes();
    populateModules();
    updateFilterControls();

    if (app_ && app_->searchIndexer()) {
        indexingIndicatorActive_ = true;
        Fl::add_timeout(kStatusPollDelaySec, onIndexingPoll, this);
        updateIndexingIndicator();
    }
}

SearchPanel::~SearchPanel() {
    cancelActiveSearch();
    cancelPendingPreviewUpdate();
    if (indexingIndicatorActive_) {
        Fl::remove_timeout(onIndexingPoll, this);
        indexingIndicatorActive_ = false;
    }
}

void SearchPanel::refresh() {
    populateResourceTypes();
    populateBibleScopes();
    populateModules();
    updateFilterControls();
    updateIndexingIndicator();
    redraw();
}

void SearchPanel::setResultLineSpacing(int pixels) {
    if (!resultBrowser_) return;
    const int spacing = std::clamp(pixels, 0, 16);
    if (resultBrowser_->linespacing() == spacing) return;
    resultBrowser_->linespacing(spacing);
    rebuildResultBrowserItems();
}

void SearchPanel::resetResultView() {
    pendingPreviewModule_.clear();
    pendingPreviewKey_.clear();
    pendingPreviewResourceType_.clear();
    pendingPreviewTitle_.clear();
    lastPreviewModule_.clear();
    lastPreviewKey_.clear();
    resultDisplayKeys_.clear();
    resultLineWidths_.clear();
    resultRefColumnWidth_ = kResultRefColumnWidth;
    results_.clear();
    if (resultBrowser_) {
        resultBrowser_->clear();
        resultBrowser_->value(0);
    }
    statusResultCountOverride_ = -1;
    if (searchProgress_) {
        searchProgress_->value(searchProgress_->minimum());
        searchProgress_->copy_label("");
        searchProgress_->hide();
    }
    setResultCountLabel();
}

void SearchPanel::finalizeSearchResults(const std::string& moduleName,
                                        bool usedIndexer,
                                        bool indexingPending,
                                        bool fallbackDeferred) {
    statusResultCountOverride_ = -1;
    sortResultsForDisplay(moduleName);

    resultDisplayKeys_.clear();
    for (const auto& r : results_) {
        resultDisplayKeys_.push_back(resultDisplayLabel(r));
    }
    rebuildResultBrowserItems();

    std::string labelSuffix;
    if (usedIndexer && indexingPending) {
        labelSuffix += "(indexing...)";
    }
    if (fallbackDeferred) {
        if (!labelSuffix.empty()) labelSuffix += " ";
        labelSuffix += "(regex requires module index)";
    }
    std::string summary = resultSummarySuffix();
    if (!summary.empty()) {
        if (!labelSuffix.empty()) labelSuffix += " ";
        labelSuffix += summary;
    }
    setResultCountLabel(labelSuffix);

    SearchIndexer* indexer = app_ ? app_->searchIndexer() : nullptr;
    if (indexer && indexingPending) {
        startIndexingIndicator(moduleName);
    } else {
        stopIndexingIndicator();
    }

    if (app_ && app_->mainWindow()) {
        std::string mod = trimCopy(moduleName);
        if (mod.empty()) mod = "search";
        app_->mainWindow()->showTransientStatus(
            "Search (" + mod + "): " + std::to_string(results_.size()) + " result(s)",
            2.6);
    }

    redraw();
}

void SearchPanel::cancelActiveSearch() {
    cancelAsyncSearch_.store(true);
    if (searchThread_.joinable()) {
        searchThread_.join();
    }
    cancelAsyncSearch_.store(false);

    std::lock_guard<std::mutex> lock(asyncSearchMutex_);
    asyncSearchState_ = AsyncSearchState{};
}

void SearchPanel::startAsyncRegexSearch(const SearchIndexer::SearchRequest& request,
                                        const std::string& moduleName,
                                        const std::string& query,
                                        bool indexingPending,
                                        SearchIndexer* indexer) {
    if (!indexer) return;

    {
        std::lock_guard<std::mutex> lock(asyncSearchMutex_);
        asyncSearchState_ = AsyncSearchState{};
        asyncSearchState_.active = true;
        asyncSearchState_.usedIndexer = true;
        asyncSearchState_.indexingPending = indexingPending;
        asyncSearchState_.moduleName = moduleName;
    }

    statusResultCountOverride_ = 0;
    setResultCountLabel("(searching 0%)");
    if (searchProgress_) {
        searchProgress_->minimum(0.0f);
        searchProgress_->maximum(1.0f);
        searchProgress_->value(0.0f);
        searchProgress_->show();
    }

    cancelAsyncSearch_.store(false);
    searchThread_ = std::thread([this, request, moduleName, query, indexingPending, indexer]() {
        std::vector<SearchResult> results = indexer->searchRegex(
            request, query, false, 0,
            [this](const SearchIndexer::RegexSearchProgress& progress) {
                if (cancelAsyncSearch_.load()) return false;

                std::lock_guard<std::mutex> lock(asyncSearchMutex_);
                if (!asyncSearchState_.active) return false;
                asyncSearchState_.scanned = progress.scanned;
                asyncSearchState_.total = progress.total;
                asyncSearchState_.matches = progress.matches;
                return !cancelAsyncSearch_.load();
            });

        std::lock_guard<std::mutex> lock(asyncSearchMutex_);
        asyncSearchState_.active = false;
        asyncSearchState_.completed = true;
        asyncSearchState_.cancelled = cancelAsyncSearch_.load();
        asyncSearchState_.usedIndexer = true;
        asyncSearchState_.indexingPending = indexingPending;
        asyncSearchState_.fallbackDeferred = false;
        asyncSearchState_.moduleName = moduleName;
        asyncSearchState_.results = std::move(results);
        asyncSearchState_.matches = static_cast<int>(asyncSearchState_.results.size());
        if (asyncSearchState_.total < asyncSearchState_.scanned) {
            asyncSearchState_.total = asyncSearchState_.scanned;
        }
    });
}

bool SearchPanel::updateSearchProgressUi() {
    bool active = false;
    bool completed = false;
    int scanned = 0;
    int total = 0;
    int matches = 0;

    {
        std::lock_guard<std::mutex> lock(asyncSearchMutex_);
        active = asyncSearchState_.active;
        completed = asyncSearchState_.completed;
        scanned = asyncSearchState_.scanned;
        total = asyncSearchState_.total;
        matches = asyncSearchState_.matches;
    }

    if (completed) {
        applyCompletedAsyncSearch();
        return false;
    }

    if (!active) return false;

    statusResultCountOverride_ = matches;
    const int safeTotal = std::max(1, std::max(total, scanned));
    const int safeScanned = std::clamp(scanned, 0, safeTotal);
    const int percent = (safeScanned * 100) / safeTotal;
    setResultCountLabel("(searching " + std::to_string(percent) + "%)");

    if (searchProgress_) {
        searchProgress_->minimum(0.0f);
        searchProgress_->maximum(static_cast<float>(safeTotal));
        searchProgress_->value(static_cast<float>(safeScanned));
        if (!searchProgress_->visible()) searchProgress_->show();
    }

    return true;
}

void SearchPanel::applyCompletedAsyncSearch() {
    AsyncSearchState completed;
    {
        std::lock_guard<std::mutex> lock(asyncSearchMutex_);
        if (!asyncSearchState_.completed) return;
        completed = std::move(asyncSearchState_);
        asyncSearchState_ = AsyncSearchState{};
    }

    if (searchThread_.joinable()) {
        searchThread_.join();
    }
    cancelAsyncSearch_.store(false);

    statusResultCountOverride_ = -1;
    if (searchProgress_) {
        searchProgress_->value(searchProgress_->minimum());
        searchProgress_->copy_label("");
        searchProgress_->hide();
    }

    if (completed.cancelled) {
        setResultCountLabel();
        redraw();
        return;
    }

    results_ = std::move(completed.results);
    finalizeSearchResults(completed.moduleName,
                          completed.usedIndexer,
                          completed.indexingPending,
                          completed.fallbackDeferred);
}

void SearchPanel::search(const std::string& query,
                         const std::string& moduleOverride) {
    cancelActiveSearch();
    cancelPendingPreviewUpdate();
    resetHighlightState();
    resetResultView();
    populateBibleScopes();
    populateModules();
    updateFilterControls();

    std::string trimmedQuery = trimCopy(query);
    if (trimmedQuery.empty()) return;

    std::string moduleName = effectiveModuleSelection(moduleOverride);
    if (!moduleName.empty()) {
        setSelectedModule(moduleName);
    }
    std::vector<std::string> resourceTypes = effectiveResourceTypes();
    moduleName = effectiveModuleSelection(moduleOverride);

    SearchIndexer::SearchRequest request;
    request.resourceTypes = resourceTypes;
    request.moduleName = moduleName;
    if (bibleScopeActive() && bibleScopeChoice_) {
        switch (bibleScopeChoice_->value()) {
        case 1:
            request.bibleScope = SearchIndexer::SearchRequest::BibleScope::OldTestament;
            break;
        case 2:
            request.bibleScope = SearchIndexer::SearchRequest::BibleScope::NewTestament;
            break;
        case 3:
            request.bibleScope = SearchIndexer::SearchRequest::BibleScope::CurrentBook;
            request.currentBook = currentBibleScopeBook();
            break;
        case 0:
        default:
            request.bibleScope = SearchIndexer::SearchRequest::BibleScope::All;
            break;
        }
    }

    // Get search type
    const bool exactPhrase = (searchType_->value() == 1);
    const bool regexSearch = (searchType_->value() == 2);
    const bool smartSearch = (searchType_->value() == 3);
    std::string strongsQuery = normalizeStrongsQuery(trimmedQuery);
    bool isStrongs = !strongsQuery.empty() && requestCanUseStrongs(resourceTypes);
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
    } else if (smartSearch) {
        // Smart search highlights terms + their synonyms; the indexer
        // builds highlighted snippets, so we just track terms for preview.
        highlightMode_ = HighlightMode::Terms;
        highlightTerms_ = tokenizeWords(trimmedQuery);
        // Also add synonyms to the highlight terms for preview highlighting.
        std::string smartLang = "en";
        if (!moduleName.empty()) {
            for (const auto& mod : searchableModules_) {
                if (mod.name == moduleName && !mod.language.empty()) {
                    smartLang = mod.language;
                    break;
                }
            }
        }
        std::vector<std::string> expandedTerms;
        std::unordered_set<std::string> seen;
        for (const auto& term : highlightTerms_) {
            auto syns = smart_search::expandSynonyms(term, smartLang);
            for (const auto& syn : syns) {
                std::string lower = syn;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                if (seen.insert(lower).second) {
                    expandedTerms.push_back(syn);
                }
            }
        }
        highlightTerms_ = std::move(expandedTerms);
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
    SearchIndexer* indexer = app_->searchIndexer();
    if (indexer) {
        std::vector<std::string> relevantModules;
        if (!moduleName.empty()) {
            relevantModules.push_back(moduleName);
        } else {
            for (const auto& module : searchableModules_) {
                std::string resourceType =
                    searchResourceTypeTokenForModuleType(module.type);
                if (!resourceTypes.empty() &&
                    std::find(resourceTypes.begin(), resourceTypes.end(), resourceType) ==
                        resourceTypes.end()) {
                    continue;
                }
                relevantModules.push_back(module.name);
            }
        }

        for (const auto& name : relevantModules) {
            indexer->queueModuleIndex(name);
            if (!indexer->isModuleIndexed(name)) indexingPending = true;
        }
    }

    if (indexer && regexSearch && !isStrongs) {
        stopIndexingIndicator();
        startAsyncRegexSearch(request, moduleName, trimmedQuery, indexingPending, indexer);
        redraw();
        return;
    }

    // Prefer indexed searches when the index exists.
    if (indexer) {
        usedIndexer = true;
        if (isStrongs) {
            results_ = indexer->searchStrongs(request, strongsQuery);
        } else if (regexSearch) {
            fallbackDeferred = true;
        } else if (smartSearch) {
            // Determine language from the target module for synonym expansion.
            std::string smartLang = "en";
            if (!moduleName.empty()) {
                for (const auto& mod : searchableModules_) {
                    if (mod.name == moduleName && !mod.language.empty()) {
                        smartLang = mod.language;
                        break;
                    }
                }
            }
            results_ = indexer->searchSmart(request, trimmedQuery, smartLang);
        } else {
            results_ = indexer->searchWord(request, trimmedQuery, exactPhrase);
        }
    }

    bool runSwordFallback = false;
    int swordSearchType = -1;
    std::string swordQuery = isStrongs ? strongsQuery : trimmedQuery;
    const bool canRunSwordFallback = !moduleName.empty() &&
                                     (resourceTypes.size() == 1 && resourceTypes.front() == "bible");
    if (!indexer && canRunSwordFallback) {
        if (isStrongs) {
            runSwordFallback = true;
        } else if (regexSearch) {
            // Avoid unstable SWORD regex search path.
            fallbackDeferred = true;
        } else {
            runSwordFallback = true;
            swordSearchType = exactPhrase ? 1 : -1;
        }
    } else if (isStrongs && results_.empty() && indexingPending && canRunSwordFallback) {
        // Allow immediate Strong's lookups before initial module indexing finishes.
        runSwordFallback = true;
    }

    if (runSwordFallback && !swordQuery.empty()) {
        swordSearchInProgress_ = true;
        setResultCountLabel("(searching...)");
        Fl::flush();

        std::string scope;
        switch (request.bibleScope) {
        case SearchIndexer::SearchRequest::BibleScope::OldTestament:
            scope = "Genesis-Malachi";
            break;
        case SearchIndexer::SearchRequest::BibleScope::NewTestament:
            scope = "Matthew-Revelation";
            break;
        case SearchIndexer::SearchRequest::BibleScope::CurrentBook:
            scope = request.currentBook;
            break;
        case SearchIndexer::SearchRequest::BibleScope::All:
            break;
        }

        if (isStrongs) {
            results_ = app_->swordManager().searchStrongs(moduleName, swordQuery);
        } else {
            results_ = app_->swordManager().search(
                moduleName, swordQuery, swordSearchType, scope,
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
            if (result.title.empty()) result.title = result.key;
            if (result.resourceType.empty()) result.resourceType = "bible";
        }
    }
    finalizeSearchResults(moduleName, usedIndexer, indexingPending, fallbackDeferred);
}

void SearchPanel::showReferenceResults(const std::string& moduleName,
                                       const std::vector<std::string>& references,
                                       const std::string& statusSuffix) {
    cancelActiveSearch();
    cancelPendingPreviewUpdate();
    resetHighlightState();
    resetResultView();
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
        result.resourceType = "bible";
        result.module = module;
        result.key = ref;
        result.title = ref;
        result.text = truncateSnippetWords(
            app_->swordManager().getVersePlainText(module, ref));
        if (result.text.empty()) result.text = ref;
        results_.push_back(std::move(result));
    }

    for (const auto& r : results_) {
        resultDisplayKeys_.push_back(resultDisplayLabel(r));
    }
    rebuildResultBrowserItems();

    setResultCountLabel(statusSuffix.empty() ? "(linked references)" : statusSuffix);

    if (!results_.empty()) {
        resultBrowser_->value(1);
        schedulePreviewUpdate(results_.front());
    }

    redraw();
}

void SearchPanel::clear() {
    cancelActiveSearch();
    cancelPendingPreviewUpdate();
    resetHighlightState();
    resetResultView();
    stopIndexingIndicator();
    updateIndexingIndicator();
}

void SearchPanel::rebuildResultBrowserItems() {
    if (!resultBrowser_) return;

    const int selectedLine = resultBrowser_->value();
    resultBrowser_->clear();
    resultBrowser_->value(0);

    for (size_t i = 0; i < results_.size(); ++i) {
        resultBrowser_->add(" ");
    }
    rebuildResultMetrics();

    if (selectedLine > 0 &&
        selectedLine <= static_cast<int>(results_.size())) {
        resultBrowser_->value(selectedLine);
    }
    resultBrowser_->redraw();
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

    const std::string previousSelection =
        module_choice::selectedModuleName(moduleChoice_, moduleChoiceModules_);

    searchableModules_.clear();
    if (app_) {
        searchableModules_ = app_->swordManager().getModules();
        searchableModules_.erase(
            std::remove_if(searchableModules_.begin(), searchableModules_.end(),
                           [](const ModuleInfo& module) {
                               return module.name.empty() ||
                                      !isSearchableResourceTypeToken(
                                          searchResourceTypeTokenForModuleType(module.type));
                           }),
            searchableModules_.end());
    }

    SearchDomain domain = searchDomain();
    std::vector<std::string> allowedTypes = effectiveResourceTypes();

    moduleChoice_->clear();
    moduleChoiceModules_.clear();
    moduleChoiceLabels_.clear();

    if (domain == SearchDomain::BibleOnly) {
        std::string currentModule;
        if (app_ && app_->mainWindow() && app_->mainWindow()->biblePane()) {
            currentModule = trimCopy(app_->mainWindow()->biblePane()->currentModule());
        }
        std::string currentLabel = "Current Bible module";
        if (!currentModule.empty()) currentLabel += " (" + currentModule + ")";
        moduleChoice_->add(currentLabel.c_str());
        moduleChoiceModules_.push_back(kCurrentBibleModuleToken);
        moduleChoiceLabels_.push_back(std::move(currentLabel));

        moduleChoice_->add("All Bible modules");
        moduleChoiceModules_.push_back(kAllModulesToken);
        moduleChoiceLabels_.push_back("All Bible modules");
    } else if (domain == SearchDomain::LibraryOnly) {
        moduleChoice_->add("All library modules");
        moduleChoiceModules_.push_back(kAllModulesToken);
        moduleChoiceLabels_.push_back("All library modules");
    } else {
        moduleChoice_->add("All searchable modules");
        moduleChoiceModules_.push_back(kAllModulesToken);
        moduleChoiceLabels_.push_back("All searchable modules");
    }

    for (const auto& module : searchableModules_) {
        std::string resourceType = searchResourceTypeTokenForModuleType(module.type);
        if (!allowedTypes.empty() &&
            std::find(allowedTypes.begin(), allowedTypes.end(), resourceType) ==
                allowedTypes.end()) {
            continue;
        }
        std::string label = module_choice::formatLabel(module);
        moduleChoice_->add(module_choice::escapeMenuLabel(label).c_str());
        moduleChoiceModules_.push_back(module.name);
        moduleChoiceLabels_.push_back(std::move(label));
    }

    if (!previousSelection.empty() &&
        module_choice::applyChoiceValue(moduleChoice_,
                                        moduleChoiceModules_,
                                        moduleChoiceLabels_,
                                        previousSelection)) {
        return;
    }

    if (moduleChoice_->size() > 0) {
        moduleChoice_->value(0);
        module_choice::syncTooltipToSelection(moduleChoice_, moduleChoiceLabels_);
    }
}

void SearchPanel::populateResourceTypes() {
    if (!resourceTypeChoice_) return;

    const int previous = resourceTypeChoice_->value();
    resourceTypeChoice_->clear();
    resourceTypeChoiceTokens_.clear();
    resourceTypeChoiceLabels_.clear();

    SearchDomain domain = searchDomain();
    auto addTypeChoice = [&](const std::string& label, const std::string& token) {
        resourceTypeChoice_->add(label.c_str());
        resourceTypeChoiceTokens_.push_back(token);
        resourceTypeChoiceLabels_.push_back(label);
    };

    if (domain == SearchDomain::BibleOnly) {
        addTypeChoice("Bible", "bible");
    } else if (domain == SearchDomain::LibraryOnly) {
        addTypeChoice("All library types", "");
        addTypeChoice("Commentaries", "commentary");
        addTypeChoice("Dictionaries", "dictionary");
        addTypeChoice("General books", "general_book");
    } else {
        addTypeChoice("All types", "");
        addTypeChoice("Bible", "bible");
        addTypeChoice("Commentaries", "commentary");
        addTypeChoice("Dictionaries", "dictionary");
        addTypeChoice("General books", "general_book");
    }

    int index = previous;
    if (index < 0 || index >= resourceTypeChoice_->size()) index = 0;
    resourceTypeChoice_->value(index);
}

void SearchPanel::populateBibleScopes() {
    if (!bibleScopeChoice_) return;

    const int previous = bibleScopeChoice_->value();
    bibleScopeChoice_->clear();
    bibleScopeChoice_->add("All Bible");
    bibleScopeChoice_->add("Old Testament");
    bibleScopeChoice_->add("New Testament");

    std::string currentBookLabel = "Current Book";
    std::string currentBook = currentBibleScopeBook();
    if (!currentBook.empty()) currentBookLabel += " (" + currentBook + ")";
    bibleScopeChoice_->add(currentBookLabel.c_str());

    int index = previous;
    if (index < 0 || index >= bibleScopeChoice_->size()) index = 0;
    bibleScopeChoice_->value(index);
}

void SearchPanel::updateFilterControls() {
    if (!resourceTypeChoice_ || !bibleScopeChoice_) return;

    if (searchDomain() == SearchDomain::BibleOnly) {
        resourceTypeChoice_->deactivate();
    } else {
        resourceTypeChoice_->activate();
    }

    if (bibleScopeActive()) {
        bibleScopeChoice_->activate();
    } else {
        bibleScopeChoice_->deactivate();
        bibleScopeChoice_->value(0);
    }
}

SearchPanel::SearchDomain SearchPanel::searchDomain() const {
    if (!targetChoice_) return SearchDomain::BibleOnly;
    switch (targetChoice_->value()) {
    case 1:
        return SearchDomain::LibraryOnly;
    case 2:
        return SearchDomain::AllResources;
    case 0:
    default:
        return SearchDomain::BibleOnly;
    }
}

SearchPanel::ResultSort SearchPanel::resultSort() const {
    if (!sortChoice_) return ResultSort::Relevance;
    switch (sortChoice_->value()) {
    case 1:
        return ResultSort::Canonical;
    case 2:
        return ResultSort::Module;
    case 0:
    default:
        return ResultSort::Relevance;
    }
}

std::vector<std::string> SearchPanel::effectiveResourceTypes() const {
    SearchDomain domain = searchDomain();
    if (domain == SearchDomain::BibleOnly) {
        return {"bible"};
    }

    std::string selectedType;
    if (resourceTypeChoice_) {
        int index = resourceTypeChoice_->value();
        if (index >= 0 && index < static_cast<int>(resourceTypeChoiceTokens_.size())) {
            selectedType = resourceTypeChoiceTokens_[static_cast<size_t>(index)];
        }
    }

    if (!selectedType.empty()) {
        return {selectedType};
    }
    if (domain == SearchDomain::LibraryOnly) {
        return {"commentary", "dictionary", "general_book"};
    }
    return {};
}

std::string SearchPanel::effectiveModuleSelection(const std::string& moduleOverride) const {
    std::string explicitModule = trimCopy(moduleOverride);
    if (!explicitModule.empty()) return explicitModule;

    std::string selected = module_choice::selectedModuleName(moduleChoice_, moduleChoiceModules_);
    if (selected == kCurrentBibleModuleToken) {
        if (app_ && app_->mainWindow() && app_->mainWindow()->biblePane()) {
            return trimCopy(app_->mainWindow()->biblePane()->currentModule());
        }
        return "";
    }
    if (selected == kAllModulesToken) return "";
    return selected;
}

std::string SearchPanel::currentBibleScopeBook() const {
    if (!app_ || !app_->mainWindow() || !app_->mainWindow()->biblePane()) return "";
    return trimCopy(app_->mainWindow()->biblePane()->currentBook());
}

bool SearchPanel::bibleScopeActive() const {
    return searchDomain() == SearchDomain::BibleOnly;
}

bool SearchPanel::requestCanUseStrongs(const std::vector<std::string>& resourceTypes) const {
    if (resourceTypes.empty()) return true;
    return std::find(resourceTypes.begin(), resourceTypes.end(), "bible") != resourceTypes.end();
}

bool SearchPanel::resultIsBibleLike(const SearchResult& result) const {
    return result.resourceType.empty() || result.resourceType == "bible";
}

std::string SearchPanel::resultLocationLabel(const SearchResult& result) const {
    if (!result.title.empty()) return result.title;
    return result.key;
}

std::string SearchPanel::resultDisplayLabel(const SearchResult& result) const {
    std::string location = resultLocationLabel(result);
    if (resultIsBibleLike(result) && app_) {
        std::string shortKey = app_->swordManager().getShortReference(result.module, result.key);
        if (!shortKey.empty()) location = shortKey;
    }

    std::string label = result.module;
    if (!label.empty()) label += " ";
    label += "[" + searchResourceTypeLabel(
        result.resourceType.empty() ? "bible" : result.resourceType) + "]";
    if (!location.empty()) {
        label += " ";
        label += location;
    }
    return label;
}

std::string SearchPanel::resultSummarySuffix() const {
    if (results_.empty()) return "";

    std::map<std::string, int> typeCounts;
    std::map<std::string, int> moduleCounts;
    for (const auto& result : results_) {
        std::string type = result.resourceType.empty() ? "bible" : result.resourceType;
        ++typeCounts[type];
        if (!result.module.empty()) ++moduleCounts[result.module];
    }

    std::ostringstream out;
    out << "(";
    int shownModules = 0;
    for (const auto& [module, count] : moduleCounts) {
        if (shownModules > 0) out << ", ";
        out << module << " " << count;
        ++shownModules;
        if (shownModules >= 3) break;
    }
    if (static_cast<int>(moduleCounts.size()) > shownModules) {
        out << ", +" << (static_cast<int>(moduleCounts.size()) - shownModules)
            << " more";
    }

    bool firstType = true;
    for (const auto& [type, count] : typeCounts) {
        out << (firstType ? "; " : ", ")
            << searchResourceTypeLabel(type) << " " << count;
        firstType = false;
    }
    out << ")";
    return out.str();
}

void SearchPanel::sortResultsForDisplay(const std::string& canonicalModuleHint) {
    if (results_.size() < 2 || !app_) return;

    switch (resultSort()) {
    case ResultSort::Relevance:
        return;
    case ResultSort::Canonical: {
        std::vector<SearchResult> verseLike;
        std::vector<SearchResult> other;
        verseLike.reserve(results_.size());
        other.reserve(results_.size());
        for (const auto& result : results_) {
            if (resultIsBibleLike(result)) {
                verseLike.push_back(result);
            } else {
                other.push_back(result);
            }
        }

        std::string canonicalModule = canonicalModuleHint;
        if (canonicalModule.empty() &&
            app_->mainWindow() && app_->mainWindow()->biblePane()) {
            canonicalModule = trimCopy(app_->mainWindow()->biblePane()->currentModule());
        }
        if (!canonicalModule.empty()) {
            verse_reference_sort::sortSearchResultsCanonical(
                app_->swordManager(), canonicalModule, verseLike);
        }
        std::stable_sort(other.begin(), other.end(),
                         [](const SearchResult& lhs, const SearchResult& rhs) {
                             if (lhs.module != rhs.module) return lhs.module < rhs.module;
                             return lhs.title < rhs.title;
                         });
        results_.clear();
        results_.insert(results_.end(), verseLike.begin(), verseLike.end());
        results_.insert(results_.end(), other.begin(), other.end());
        return;
    }
    case ResultSort::Module:
        std::stable_sort(results_.begin(), results_.end(),
                         [this](const SearchResult& lhs, const SearchResult& rhs) {
                             if (lhs.module != rhs.module) return lhs.module < rhs.module;
                             return resultLocationLabel(lhs) < resultLocationLabel(rhs);
                         });
        return;
    }
}

void SearchPanel::setSelectedModule(const std::string& moduleName) {
    std::string name = trimCopy(moduleName);
    if (!moduleChoice_ || name.empty()) return;
    for (const auto& module : searchableModules_) {
        if (module.name != name) continue;
        if (targetChoice_) {
            targetChoice_->value(
                searchResourceTypeTokenForModuleType(module.type) == "bible" ? 0 : 1);
        }
        populateResourceTypes();
        populateBibleScopes();
        populateModules();
        updateFilterControls();
        break;
    }
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
    const int resultCount = (statusResultCountOverride_ >= 0)
                                ? statusResultCountOverride_
                                : static_cast<int>(results_.size());
    std::string label = "Results: " + std::to_string(resultCount);
    if (!statusSuffix_.empty()) {
        label += " " + statusSuffix_;
    }
    if (label == currentResultLabel_) return;
    currentResultLabel_ = label;
    resultStatus_->copy_label(label.c_str());
    if (searchProgress_) {
        searchProgress_->copy_label(label.c_str());
    }
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
        std::string selected = effectiveModuleSelection();
        std::string error = selected.empty() ? "" : indexer->moduleIndexError(selected);
        if (!error.empty()) {
            if (error.size() > 64) error = error.substr(0, 64) + "...";
            setResultCountLabel("(index error: " + error + ")");
            return;
        }
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
    pendingPreviewResourceType_ = result.resourceType;
    pendingPreviewTitle_ = result.title;
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

    std::string html;
    std::string resourceType = pendingPreviewResourceType_.empty()
                                   ? "bible"
                                   : pendingPreviewResourceType_;
    if (resourceType == "commentary") {
        html = app_->swordManager().getCommentaryText(
            pendingPreviewModule_, pendingPreviewKey_);
    } else if (resourceType == "dictionary") {
        html = app_->swordManager().getDictionaryEntry(
            pendingPreviewModule_, pendingPreviewKey_);
    } else if (resourceType == "general_book") {
        html = app_->swordManager().getGeneralBookEntry(
            pendingPreviewModule_, pendingPreviewKey_);
    } else {
        html = app_->swordManager().getVerseText(
            pendingPreviewModule_, pendingPreviewKey_);
    }
    html = applyPreviewHighlights(html);
    if (resourceType == "bible") {
        app_->mainWindow()->leftPane()->setVersePreviewText(
            html, pendingPreviewModule_, pendingPreviewKey_);
    } else {
        std::ostringstream wrapped;
        wrapped << "<div class=\"preview-verse-block\">\n";
        wrapped << "<div class=\"preview-verse-ref\">"
                << htmlEscape(pendingPreviewModule_);
        if (!pendingPreviewTitle_.empty()) {
            wrapped << " [" << htmlEscape(searchResourceTypeLabel(resourceType))
                    << "] " << htmlEscape(pendingPreviewTitle_);
        }
        wrapped << "</div>\n";
        wrapped << html << "\n</div>\n";
        app_->mainWindow()->leftPane()->setPreviewText(
            wrapped.str(), pendingPreviewModule_, pendingPreviewKey_);
    }
    lastPreviewModule_ = pendingPreviewModule_;
    lastPreviewKey_ = pendingPreviewKey_;
}

void SearchPanel::activateResultLine(int line, int mouseButton, bool isDoubleClick) {
    if (!app_ || !app_->mainWindow()) return;
    if (line <= 0 || line > static_cast<int>(results_.size())) return;

    const SearchResult& result = results_[line - 1];

    if (mouseButton == FL_MIDDLE_MOUSE) {
        if (resultIsBibleLike(result)) {
            app_->mainWindow()->openInNewStudyTab(result.module, result.key);
        }
        return;
    }

    if (mouseButton == FL_LEFT_MOUSE && isDoubleClick) {
        if (result.resourceType == "commentary") {
            app_->mainWindow()->showCommentary(result.module, result.key);
        } else if (result.resourceType == "dictionary") {
            app_->mainWindow()->showDictionaryEntry(result.module, result.key);
        } else if (result.resourceType == "general_book") {
            app_->mainWindow()->showGeneralBookEntry(result.module, result.key);
        } else {
            app_->mainWindow()->navigateTo(result.module, result.key);
        }
    }
}

void SearchPanel::showVerseListContextMenu(int screenX, int screenY) {
    if (!app_ || results_.empty()) return;

    if (std::any_of(results_.begin(), results_.end(),
                    [this](const SearchResult& result) {
                        return !resultIsBibleLike(result);
                    })) {
        return;
    }

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

    const bool searchActive = self->updateSearchProgressUi();
    if (!searchActive) {
        self->updateIndexingIndicator();
    }
    Fl::repeat_timeout(kStatusPollDelaySec, onIndexingPoll, self);
}

void SearchPanel::onDeferredPreviewUpdate(void* data) {
    auto* self = static_cast<SearchPanel*>(data);
    if (!self) return;
    self->applyPendingPreviewUpdate();
}

void SearchPanel::onFilterChoiceChanged(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<SearchPanel*>(data);
    if (!self) return;
    self->populateResourceTypes();
    self->populateBibleScopes();
    self->populateModules();
    self->updateFilterControls();
    self->updateIndexingIndicator();
    self->redraw();
}

void SearchPanel::onSortChoiceChanged(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<SearchPanel*>(data);
    if (!self || self->results_.empty()) return;
    std::string canonicalModule;
    if (self->app_ && self->app_->mainWindow() && self->app_->mainWindow()->biblePane()) {
        canonicalModule = trimCopy(self->app_->mainWindow()->biblePane()->currentModule());
    }
    self->sortResultsForDisplay(canonicalModule);
    self->resultDisplayKeys_.clear();
    for (const auto& result : self->results_) {
        self->resultDisplayKeys_.push_back(self->resultDisplayLabel(result));
    }
    self->rebuildResultBrowserItems();
    self->setResultCountLabel(self->resultSummarySuffix());
    self->redraw();
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

    if (self->app_->mainWindow()) {
        if (result->resourceType == "commentary") {
            self->app_->mainWindow()->showCommentary(result->module, result->key);
        } else if (result->resourceType == "dictionary") {
            self->app_->mainWindow()->showDictionaryEntry(result->module, result->key);
        } else if (result->resourceType == "general_book") {
            self->app_->mainWindow()->showGeneralBookEntry(result->module, result->key);
        } else {
            self->app_->mainWindow()->navigateTo(result->module, result->key);
        }
    }
}

} // namespace verdad
