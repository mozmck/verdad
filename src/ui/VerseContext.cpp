#include "ui/VerseContext.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/BiblePane.h"
#include "ui/LeftPane.h"
#include "ui/TagPanel.h"
#include "ui/UiFontUtils.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Button.H>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_set>
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

std::string collapseWhitespace(const std::string& text) {
    std::ostringstream out;
    bool inSpace = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            inSpace = true;
            continue;
        }
        if (inSpace && out.tellp() > 0) {
            out << ' ';
        }
        inSpace = false;
        out << static_cast<char>(c);
    }
    return trimCopy(out.str());
}

bool isLikelyMorphFragmentToken(const std::string& token) {
    return token.size() == 2 &&
           std::isdigit(static_cast<unsigned char>(token[0])) &&
           std::isalpha(static_cast<unsigned char>(token[1]));
}

bool isAsciiWordChar(unsigned char c) {
    return std::isalnum(c) || c == '\'' || c == '-';
}

std::string stripWordEdgePunct(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end) {
        unsigned char c = static_cast<unsigned char>(text[start]);
        if (c >= 0x80 || isAsciiWordChar(c)) break;
        ++start;
    }
    while (end > start) {
        unsigned char c = static_cast<unsigned char>(text[end - 1]);
        if (c >= 0x80 || isAsciiWordChar(c)) break;
        --end;
    }
    return text.substr(start, end - start);
}

bool isNumericToken(const std::string& token) {
    bool sawDigit = false;
    for (unsigned char c : token) {
        if (std::isdigit(c)) {
            sawDigit = true;
            continue;
        }
        if (c == ',' || c == '.' || c == ':' || c == ';' ||
            c == ')' || c == '(' || c == '[' || c == ']') {
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
    for (const auto& word : words) {
        if (!isNumericToken(word)) {
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

std::vector<std::string> splitWhitespaceWords(const std::string& text) {
    std::istringstream ss(text);
    std::vector<std::string> words;
    std::string word;
    while (ss >> word) {
        words.push_back(word);
    }
    return words;
}

std::string joinWords(const std::vector<std::string>& words) {
    std::ostringstream out;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) out << ' ';
        out << words[i];
    }
    return out.str();
}

std::string stripLeadingNumericText(const std::string& text) {
    return joinWords(removeLeadingNumericTokens(splitWhitespaceWords(text)));
}

std::string normalizeWordForSearch(const std::string& raw) {
    std::string text = trimCopy(raw);
    if (text.empty()) return "";

    auto trimEdges = [&](const std::string& in) -> std::string {
        size_t start = 0;
        size_t end = in.size();
        while (start < end) {
            unsigned char uc = static_cast<unsigned char>(in[start]);
            if (uc >= 0x80 || isAsciiWordChar(uc)) break;
            ++start;
        }
        while (end > start) {
            unsigned char uc = static_cast<unsigned char>(in[end - 1]);
            if (uc >= 0x80 || isAsciiWordChar(uc)) break;
            --end;
        }
        return in.substr(start, end - start);
    };

    std::istringstream ss(text);
    std::string tok;
    while (ss >> tok) {
        tok = trimEdges(tok);
        if (!tok.empty()) return tok;
    }

    return trimEdges(text);
}

std::vector<std::string> extractStrongsTokens(const std::string& text) {
    std::vector<std::string> prefixed;
    std::vector<std::string> numeric;
    std::unordered_set<std::string> seen;

    static const std::regex kToken(
        R"((?:^|[|,;\s:=?&#/])([HhGg]?\d+[A-Za-z]?)(?=$|[|,;\s:=?&#/]))");
    auto it = std::sregex_iterator(text.begin(), text.end(), kToken);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        std::string tok = trimCopy((*it)[1].str());
        if (tok.empty()) continue;
        for (char& c : tok) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        if (!tok.empty() &&
            std::isalpha(static_cast<unsigned char>(tok[0])) &&
            tok[0] != 'H' && tok[0] != 'G') {
            continue;
        }
        if (isLikelyMorphFragmentToken(tok)) continue;
        if (seen.insert(tok).second) {
            if (!tok.empty() &&
                std::isalpha(static_cast<unsigned char>(tok[0]))) {
                prefixed.push_back(tok);
            } else {
                numeric.push_back(tok);
            }
        }
    }

    if (!prefixed.empty()) return prefixed;
    return numeric;
}

std::vector<std::string> normalizedWordTokens(const std::string& text) {
    std::vector<std::string> tokens;
    for (const auto& word : splitWhitespaceWords(text)) {
        std::string stripped = stripWordEdgePunct(word);
        if (stripped.empty()) continue;
        std::transform(stripped.begin(), stripped.end(), stripped.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        tokens.push_back(std::move(stripped));
    }
    return tokens;
}

struct TokenSequenceMatch {
    bool matched = false;
    size_t start = 0;
    size_t end = 0;
};

TokenSequenceMatch findTokenSequence(const std::vector<std::string>& haystack,
                                     const std::vector<std::string>& needle) {
    if (haystack.empty() || needle.empty() || needle.size() > haystack.size()) {
        return {};
    }

    for (size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool match = true;
        for (size_t i = 0; i < needle.size(); ++i) {
            if (haystack[start + i] != needle[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            return {true, start, start + needle.size()};
        }
    }

    return {};
}

bool startsWithPilcrow(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return pos + 1 < text.size() &&
           static_cast<unsigned char>(text[pos]) == 0xC2 &&
           static_cast<unsigned char>(text[pos + 1]) == 0xB6;
}

} // namespace

VerseContext::VerseContext(VerdadApp* app)
    : app_(app) {}

VerseContext::~VerseContext() = default;

void VerseContext::show(const std::string& word, const std::string& href,
                        const std::string& strong, const std::string& morph,
                        const std::string& module,
                        const std::string& book,
                        int chapter,
                        int verse,
                        bool paragraphMode,
                        const SelectionContext& selection,
                        int screenX, int screenY) {
    currentWord_ = word;
    std::string searchWord = normalizeWordForSearch(word);
    if (!searchWord.empty()) currentWord_ = searchWord;
    currentHref_ = href;
    currentStrong_ = strong;
    currentMorph_ = morph;
    currentContextModule_ = trimCopy(module);
    if (currentContextModule_.empty() &&
        app_->mainWindow() && app_->mainWindow()->biblePane()) {
        currentContextModule_ =
            app_->mainWindow()->biblePane()->currentModule();
    }
    currentBook_ = book;
    currentChapter_ = std::max(1, chapter);
    currentVerse_ = std::max(1, verse);
    currentParagraphMode_ = paragraphMode;
    currentSelection_ = selection;
    if (currentSelection_.hasSelection) {
        if (currentSelection_.startVerse == 0) {
            currentSelection_.startVerse = currentVerse_;
        }
        if (currentSelection_.endVerse == 0) {
            currentSelection_.endVerse = currentSelection_.startVerse;
        }
        if (currentSelection_.endVerse < currentSelection_.startVerse) {
            std::swap(currentSelection_.startVerse, currentSelection_.endVerse);
        }
    }

    strongActions_.clear();
    copyActions_.clear();
    currentDictionaryLookupKey_.clear();

    const bool hasSelection =
        currentSelection_.hasSelection &&
        !trimCopy(currentSelection_.text).empty();

    std::vector<std::string> strongsNums;
    bool isParallel = false;
    if (app_->mainWindow() && app_->mainWindow()->biblePane()) {
        isParallel = app_->mainWindow()->biblePane()->isParallel();
    }
    if (!hasSelection) {
        strongsNums = extractStrongsNumbers(href, strong);
        if (strongsNums.empty() && !word.empty() && !isParallel &&
            !currentContextModule_.empty()) {
            WordInfo info = app_->swordManager().getWordInfo(
                currentContextModule_, currentVerseKey(), word);
            if (!info.strongsNumber.empty()) {
                strongsNums = extractStrongsNumbers("", info.strongsNumber);
            }
        }
        if (!currentWord_.empty()) {
            currentDictionaryLookupKey_ = currentWord_;
        }
    } else {
        currentWord_.clear();
        currentDictionaryLookupKey_.clear();
    }

    Fl_Menu_Button menu(screenX, screenY, 0, 0);
    ui_font::applyCurrentAppMenuFont(&menu);
    copyActions_.reserve(4);

    if (hasSelection) {
        const int startVerse = currentSelection_.startVerse;
        const int endVerse = currentSelection_.endVerse;
        if (startVerse != endVerse) {
            addCopyMenuItem(menu, "Copy Verses: Ref first",
                            CopyActionKind::Verses, true, startVerse, endVerse);
            addCopyMenuItem(menu, "Copy Verses: Ref last",
                            CopyActionKind::Verses, false, startVerse, endVerse);
        } else {
            const std::string fullVerse =
                singleVerseText(startVerse, false);
            const std::string selectionWords =
                trimCopy(selectionCopyText());
            const std::string comparableSelection =
                collapseWhitespace(stripLeadingNumericText(
                    currentSelection_.wholeWordText.empty()
                        ? currentSelection_.text
                        : currentSelection_.wholeWordText));
            const bool wholeVerseSelected =
                !fullVerse.empty() &&
                comparableSelection == collapseWhitespace(fullVerse);

            if (!wholeVerseSelected && !selectionWords.empty()) {
                addCopyMenuItem(menu, "Copy Selection: Ref first",
                                CopyActionKind::Selection, true,
                                startVerse, endVerse);
                addCopyMenuItem(menu, "Copy Selection: Ref last",
                                CopyActionKind::Selection, false,
                                startVerse, endVerse);
            }

            addCopyMenuItem(menu, "Copy Verse: Ref first",
                            CopyActionKind::Verse, true, startVerse, endVerse);
            addCopyMenuItem(menu, "Copy Verse: Ref last",
                            CopyActionKind::Verse, false, startVerse, endVerse);
        }
        menu.add(startVerse != endVerse ? "Tag Verses..." : "Add Tag...",
                 0, onAddTag, this);
    } else {
        addCopyMenuItem(menu, "Copy Verse: Ref first",
                        CopyActionKind::Verse, true, currentVerse_, currentVerse_);
        addCopyMenuItem(menu, "Copy Verse: Ref last",
                        CopyActionKind::Verse, false, currentVerse_, currentVerse_);

        if (!currentWord_.empty()) {
            std::string searchLabel = "Search for word: " + currentWord_;
            menu.add(searchLabel.c_str(), 0, onSearchWord, this);
            if (!currentDictionaryLookupKey_.empty()) {
                std::string dictLabel = "Look up in Dictionary: " + currentWord_;
                menu.add(dictLabel.c_str(), 0, onLookupDictionary, this);
            }
        } else if (!currentDictionaryLookupKey_.empty()) {
            std::string dictLabel =
                "Look up in Dictionary: " + currentDictionaryLookupKey_;
            menu.add(dictLabel.c_str(), 0, onLookupDictionary, this);
        }

        if (!strongsNums.empty()) {
            strongActions_.reserve(strongsNums.size() * 2);
            std::vector<std::pair<std::string, std::string>> taggedItems;
            taggedItems.reserve(strongsNums.size());
            for (const auto& strongsNum : strongsNums) {
                std::string labelSuffix = strongsNum;
                std::string lemma = app_->swordManager().getStrongsLemma(strongsNum);
                if (!lemma.empty()) {
                    labelSuffix += " (" + lemma + ")";
                }
                taggedItems.push_back({strongsNum, labelSuffix});
            }

            for (const auto& item : taggedItems) {
                strongActions_.push_back(StrongsMenuAction{
                    this, item.first, false
                });
                std::string searchLabel = "Search Strong's: " + item.second;
                menu.add(searchLabel.c_str(), 0,
                         onStrongsAction, &strongActions_.back());
            }

            for (const auto& item : taggedItems) {
                strongActions_.push_back(StrongsMenuAction{
                    this, item.first, true
                });
                std::string dictLabel =
                    "Look up Strong's in Dictionary: " + item.second;
                menu.add(dictLabel.c_str(), 0,
                         onStrongsAction, &strongActions_.back());
            }
        }

        menu.add("Add Tag...", 0, onAddTag, this);
    }

    menu.popup();
}

std::vector<std::string> VerseContext::extractStrongsNumbers(
        const std::string& href, const std::string& strong) const {
    std::vector<std::string> tokens = extractStrongsTokens(strong);
    if (!tokens.empty()) return tokens;

    std::string lowerHref = href;
    for (char& c : lowerHref) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowerHref.find("strong") == std::string::npos &&
        lowerHref.find("lemma") == std::string::npos) {
        return tokens;
    }

    return extractStrongsTokens(href);
}

std::string VerseContext::currentVerseKey() const {
    return verseKeyForNumber(currentVerse_);
}

std::string VerseContext::verseKeyForNumber(int verse) const {
    if (currentBook_.empty() || currentChapter_ <= 0 || verse <= 0) return "";
    return currentBook_ + " " + std::to_string(currentChapter_) +
           ":" + std::to_string(verse);
}

std::string VerseContext::referenceLabel(int startVerse, int endVerse) const {
    if (currentBook_.empty() || currentChapter_ <= 0) return "";
    if (startVerse <= 0) startVerse = currentVerse_;
    if (endVerse <= 0) endVerse = startVerse;
    if (endVerse < startVerse) std::swap(startVerse, endVerse);

    std::ostringstream ref;
    ref << '(' << currentBook_ << ' ' << currentChapter_ << ':'
        << startVerse;
    if (endVerse != startVerse) {
        ref << '-' << endVerse;
    }
    ref << ')';
    return ref.str();
}

std::string VerseContext::singleVerseText(int verse, bool includeVerseNumber) const {
    if (!app_ || currentContextModule_.empty()) return "";

    std::string text = trimCopy(
        app_->swordManager().getVersePlainText(
            currentContextModule_, verseKeyForNumber(verse)));
    if (text.empty()) return "";
    if (!includeVerseNumber) return text;
    return std::to_string(verse) + ' ' + text;
}

std::string VerseContext::multiVerseText(int startVerse, int endVerse) const {
    if (endVerse < startVerse) std::swap(startVerse, endVerse);

    std::string out;
    for (int verse = startVerse; verse <= endVerse; ++verse) {
        std::string line = singleVerseText(verse, true);
        if (line.empty()) continue;

        if (out.empty()) {
            out = line;
            continue;
        }

        if (!currentParagraphMode_) {
            out += '\n';
        } else if (startsWithPilcrow(line)) {
            out += "\n\n";
        } else if (!std::isspace(static_cast<unsigned char>(out.back()))) {
            out.push_back(' ');
        }

        out += line;
    }

    return out;
}

std::string VerseContext::selectionCopyText() const {
    if (!currentSelection_.hasSelection || currentSelection_.startVerse != currentSelection_.endVerse) {
        return "";
    }

    const int verse = currentSelection_.startVerse;
    std::string fullVerse = singleVerseText(verse, false);
    if (fullVerse.empty()) return "";

    std::string selected = currentSelection_.wholeWordText.empty()
        ? currentSelection_.text
        : currentSelection_.wholeWordText;
    selected = trimCopy(stripLeadingNumericText(selected));
    if (selected.empty()) return "";

    const std::string normalizedFull = collapseWhitespace(fullVerse);
    const std::string normalizedSelected = collapseWhitespace(selected);
    if (normalizedSelected.empty()) return "";
    if (normalizedSelected == normalizedFull) {
        return fullVerse;
    }

    std::vector<std::string> fullTokens = normalizedWordTokens(fullVerse);
    std::vector<std::string> selectedTokens = normalizedWordTokens(selected);
    if (selectedTokens.empty()) return "";
    TokenSequenceMatch match = findTokenSequence(fullTokens, selectedTokens);

    bool needsPrefixEllipsis = true;
    bool needsSuffixEllipsis = true;
    if (match.matched) {
        needsPrefixEllipsis = match.start > 0;
        needsSuffixEllipsis = match.end < fullTokens.size();
    }

    std::string body = selected;
    if (needsPrefixEllipsis) body = "... " + body;
    if (needsSuffixEllipsis) body += " ...";
    return body;
}

std::string VerseContext::formattedCopyText(const std::string& body,
                                            int startVerse,
                                            int endVerse,
                                            bool referenceFirst) const {
    std::string trimmedBody = trimCopy(body);
    std::string ref = referenceLabel(startVerse, endVerse);
    if (trimmedBody.empty()) return ref;
    if (ref.empty()) return trimmedBody;

    const bool multiLine = trimmedBody.find('\n') != std::string::npos;
    if (referenceFirst) {
        return multiLine ? ref + "\n" + trimmedBody : ref + " " + trimmedBody;
    }
    return multiLine ? trimmedBody + "\n" + ref : trimmedBody + " " + ref;
}

void VerseContext::copyToClipboard(const std::string& text) const {
    if (text.empty()) return;
    Fl::copy(text.c_str(), static_cast<int>(text.size()), 0);
    Fl::copy(text.c_str(), static_cast<int>(text.size()), 1);
}

void VerseContext::addCopyMenuItem(Fl_Menu_Button& menu,
                                   const char* label,
                                   CopyActionKind kind,
                                   bool referenceFirst,
                                   int startVerse,
                                   int endVerse) {
    copyActions_.push_back(CopyMenuAction{
        this, kind, referenceFirst, startVerse, endVerse
    });
    menu.add(label, 0, onCopyAction, &copyActions_.back());
}

void VerseContext::onStrongsAction(Fl_Widget* /*w*/, void* data) {
    auto* action = static_cast<StrongsMenuAction*>(data);
    if (!action || !action->owner) return;
    VerseContext* self = action->owner;

    if (!self->app_ || !self->app_->mainWindow()) return;
    if (action->dictionaryLookup) {
        self->app_->mainWindow()->showDictionary(action->strongsNumber);
    } else {
        self->app_->mainWindow()->showSearchResults(
            action->strongsNumber, self->currentContextModule_);
    }
}

void VerseContext::onAddTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<VerseContext*>(data);
    if (!self || !self->app_ || !self->app_->mainWindow() ||
        !self->app_->mainWindow()->leftPane()) {
        return;
    }

    std::string key = self->currentVerseKey();
    if (self->currentSelection_.hasSelection &&
        self->currentSelection_.startVerse > 0) {
        const int startVerse = self->currentSelection_.startVerse;
        const int endVerse = std::max(startVerse, self->currentSelection_.endVerse);
        key = self->verseKeyForNumber(startVerse);
        if (!key.empty() && endVerse != startVerse) {
            key += "-" + std::to_string(endVerse);
        }
    }
    self->app_->mainWindow()->leftPane()->tagPanel()->showAddTagDialog(key);
}

void VerseContext::onCopyAction(Fl_Widget* /*w*/, void* data) {
    auto* action = static_cast<CopyMenuAction*>(data);
    if (!action || !action->owner) return;
    VerseContext* self = action->owner;

    std::string body;
    switch (action->kind) {
    case CopyActionKind::Verse:
        body = self->singleVerseText(action->startVerse, false);
        break;
    case CopyActionKind::Verses:
        body = self->multiVerseText(action->startVerse, action->endVerse);
        break;
    case CopyActionKind::Selection:
        body = self->selectionCopyText();
        break;
    }

    self->copyToClipboard(self->formattedCopyText(
        body, action->startVerse, action->endVerse, action->referenceFirst));
}

void VerseContext::onSearchWord(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<VerseContext*>(data);
    if (!self || !self->app_ || !self->app_->mainWindow()) return;

    std::string query = trimCopy(self->currentWord_);
    if (query.empty()) return;
    self->app_->mainWindow()->showSearchResults(
        query, self->currentContextModule_);
}

void VerseContext::onLookupDictionary(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<VerseContext*>(data);
    if (!self || !self->app_ || !self->app_->mainWindow()) return;

    std::string key = trimCopy(self->currentDictionaryLookupKey_);
    if (key.empty()) return;

    self->app_->mainWindow()->showDictionary(key, self->currentContextModule_);
}

} // namespace verdad
