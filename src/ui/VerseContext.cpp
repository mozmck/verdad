#include "ui/VerseContext.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/BiblePane.h"
#include "ui/LeftPane.h"
#include "ui/TagPanel.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/fl_ask.H>

#include <cstring>
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

std::vector<std::string> extractStrongsTokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::unordered_set<std::string> seen;

    static const std::regex kToken(R"(([HhGg]?\d+[A-Za-z]?))");
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
        if (seen.insert(tok).second) {
            tokens.push_back(tok);
        }
    }

    return tokens;
}

std::string normalizeWordForSearch(const std::string& raw) {
    std::string text = trimCopy(raw);
    if (text.empty()) return "";

    auto isAsciiWordChar = [](unsigned char c) -> bool {
        return std::isalnum(c) || c == '\'' || c == '-';
    };
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

} // namespace

VerseContext::VerseContext(VerdadApp* app)
    : app_(app) {}

VerseContext::~VerseContext() = default;

void VerseContext::show(const std::string& word, const std::string& href,
                         const std::string& strong, const std::string& morph,
                         const std::string& module,
                         const std::string& verseKey,
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
    currentVerseKey_ = verseKey;
    strongActions_.clear();
    currentDictionaryLookupKey_.clear();

    std::vector<std::string> strongsNums = extractStrongsNumbers(href, strong);
    bool isParallel = false;
    if (app_->mainWindow() && app_->mainWindow()->biblePane()) {
        isParallel = app_->mainWindow()->biblePane()->isParallel();
    }
    if (strongsNums.empty() && !word.empty() && !isParallel) {
        // Try to get Strong's from word info
        if (!currentContextModule_.empty()) {
            WordInfo info = app_->swordManager().getWordInfo(
                currentContextModule_, verseKey, word);
            if (!info.strongsNumber.empty()) {
                strongsNums = extractStrongsNumbers("", info.strongsNumber);
            }
        }
    }

    if (!currentWord_.empty()) {
        currentDictionaryLookupKey_ = currentWord_;
    }

    Fl_Menu_Button menu(screenX, screenY, 0, 0);

    // Always available options
    menu.add("Copy Verse Reference", 0, onCopyVerse, this);

    if (!currentWord_.empty()) {
        menu.add("Copy Word", 0, onCopyWord, this);
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
            menu.add(searchLabel.c_str(), 0, onStrongsAction, &strongActions_.back());
        }

        for (const auto& item : taggedItems) {
            strongActions_.push_back(StrongsMenuAction{
                this, item.first, true
            });
            std::string dictLabel = "Look up Strong's in Dictionary: " + item.second;
            menu.add(dictLabel.c_str(), 0, onStrongsAction, &strongActions_.back());
        }
    }

    // Tag options
    menu.add("Add Tag...", 0, onAddTag, this);

    menu.popup();
}

std::vector<std::string> VerseContext::extractStrongsNumbers(
        const std::string& href, const std::string& strong) const {
    std::vector<std::string> tokens = extractStrongsTokens(strong);
    if (!tokens.empty()) return tokens;

    // Extract from strong links (e.g. "strongs:H1234", "strong:G5678",
    // "passagestudy.jsp?action=showStrongs&type=Greek&value=3588").
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

    if (self->app_->mainWindow() && self->app_->mainWindow()->leftPane()) {
        self->app_->mainWindow()->leftPane()->tagPanel()->showAddTagDialog(
            self->currentVerseKey_);
    }
}

void VerseContext::onCopyVerse(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<VerseContext*>(data);

    // Copy verse reference to clipboard
    Fl::copy(self->currentVerseKey_.c_str(),
             static_cast<int>(self->currentVerseKey_.length()), 1);
}

void VerseContext::onCopyWord(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<VerseContext*>(data);

    Fl::copy(self->currentWord_.c_str(),
             static_cast<int>(self->currentWord_.length()), 1);
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
