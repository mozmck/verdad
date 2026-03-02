#include "ui/SearchPanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "search/SearchIndexer.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Tabs.H>


#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <unordered_map>

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

int findChoiceIndexByLabel(Fl_Choice* choice, const std::string& label) {
    if (!choice || label.empty()) return -1;
    for (int i = 0; i < choice->size(); ++i) {
        const Fl_Menu_Item* item = choice->menu() ? &choice->menu()[i] : nullptr;
        if (item && item->label() && label == item->label()) {
            return i;
        }
    }
    return -1;
}

std::string normalizeBookKey(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
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

std::string stripSimpleHtmlTags(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool inTag = false;
    for (char c : in) {
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (!inTag) out.push_back(c);
    }
    return out;
}

bool parseReferenceKey(const std::string& key, SwordManager::VerseRef& out) {
    out = SwordManager::VerseRef{};
    try {
        out = SwordManager::parseVerseRef(key);
    } catch (...) {
        out = SwordManager::VerseRef{};
    }

    if (!out.book.empty() && out.chapter > 0 && out.verse > 0) {
        return true;
    }

    static const std::regex fallbackRe(R"(^\s*(.+?)\s+(\d+):(\d+)(?:-\d+)?\s*$)");
    std::smatch m;
    if (!std::regex_match(key, m, fallbackRe)) return false;

    out.book = trimCopy(m[1].str());
    try {
        out.chapter = std::stoi(m[2].str());
        out.verse = std::stoi(m[3].str());
    } catch (...) {
        return false;
    }
    return !out.book.empty() && out.chapter > 0 && out.verse > 0;
}

void sortSearchResultsCanonical(SwordManager& swordMgr,
                                const std::string& moduleName,
                                std::vector<SearchResult>& results) {
    if (results.size() < 2 || moduleName.empty()) return;

    std::unordered_map<std::string, int> bookOrder;
    const auto books = swordMgr.getBookNames(moduleName);
    for (size_t i = 0; i < books.size(); ++i) {
        std::string key = normalizeBookKey(books[i]);
        if (!key.empty() && bookOrder.find(key) == bookOrder.end()) {
            bookOrder.emplace(key, static_cast<int>(i));
        }
    }

    struct SortKey {
        bool parsed = false;
        int bookRank = std::numeric_limits<int>::max();
        int chapter = std::numeric_limits<int>::max();
        int verse = std::numeric_limits<int>::max();
        std::string normBook;
    };

    std::unordered_map<std::string, SortKey> cache;
    cache.reserve(results.size());

    auto getSortKey = [&](const std::string& ref) -> const SortKey& {
        auto it = cache.find(ref);
        if (it != cache.end()) return it->second;

        SortKey k;
        SwordManager::VerseRef parsed;
        if (parseReferenceKey(ref, parsed)) {
            k.parsed = true;
            k.chapter = parsed.chapter;
            k.verse = parsed.verse;
            k.normBook = normalizeBookKey(parsed.book);
            auto bi = bookOrder.find(k.normBook);
            if (bi != bookOrder.end()) {
                k.bookRank = bi->second;
            }
        }

        auto inserted = cache.emplace(ref, std::move(k));
        return inserted.first->second;
    };

    std::stable_sort(results.begin(), results.end(),
                     [&](const SearchResult& a, const SearchResult& b) {
        const SortKey& ka = getSortKey(a.key);
        const SortKey& kb = getSortKey(b.key);

        if (ka.parsed != kb.parsed) return ka.parsed > kb.parsed;
        if (!ka.parsed && !kb.parsed) return a.key < b.key;

        if (ka.bookRank != kb.bookRank) return ka.bookRank < kb.bookRank;
        if (ka.bookRank == std::numeric_limits<int>::max() &&
            ka.normBook != kb.normBook) {
            return ka.normBook < kb.normBook;
        }
        if (ka.chapter != kb.chapter) return ka.chapter < kb.chapter;
        if (ka.verse != kb.verse) return ka.verse < kb.verse;
        return a.key < b.key;
    });
}

} // namespace

SearchPanel::SearchPanel(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , moduleChoice_(nullptr)
    , searchType_(nullptr)
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

    // Result list (occupies full area below selectors).
    resultBrowser_ = new Fl_Hold_Browser(X + padding, cy,
                                         W - 2 * padding, H - (cy - Y) - padding);
    static int widths[] = { 100, 0 };  // widths for each column
    resultBrowser_->column_widths(widths); // assign array to widget
    //resultBrowser_->type(FL_HOLD_BROWSER);
    resultBrowser_->callback(onResultSelect, this);
    //resultBrowser_->when(FL_WHEN_RELEASE);

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
    if (indexingIndicatorActive_) {
        Fl::remove_timeout(onIndexingPoll, this);
        indexingIndicatorActive_ = false;
    }
}

void SearchPanel::search(const std::string& query,
                         const std::string& moduleOverride) {
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
        const Fl_Menu_Item* item = moduleChoice_->mvalue();
        if (item && item->label()) moduleName = item->label();
    }
    if (moduleName.empty() &&
        app_->mainWindow() && app_->mainWindow()->biblePane()) {
        moduleName = trimCopy(app_->mainWindow()->biblePane()->currentModule());
    }
    if (moduleName.empty()) {
        fl_alert("No active Bible module to search.");
        return;
    }

    int moduleIndex = findChoiceIndexByLabel(moduleChoice_, moduleName);
    if (moduleIndex >= 0) moduleChoice_->value(moduleIndex);

    // Get search type
    const bool exactPhrase = (searchType_->value() == 1);
    const bool regexSearch = (searchType_->value() == 2);
    std::string strongsQuery = normalizeStrongsQuery(trimmedQuery);
    bool isStrongs = !strongsQuery.empty();
    if (isStrongs) trimmedQuery = strongsQuery;

    if (regexSearch && !isStrongs) {
        try {
            std::regex test(trimmedQuery, std::regex::ECMAScript | std::regex::icase);
            (void)test;
        } catch (const std::regex_error&) {
            fl_alert("Invalid regex pattern.");
            setResultCountLabel("(invalid regex)");
            return;
        }
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

    sortSearchResultsCanonical(app_->swordManager(), moduleName, results_);

    // Populate result browser
    for (const auto& r : results_) {
        std::string snippet = collapseWhitespace(stripSimpleHtmlTags(r.text));
        if (snippet.size() > 36) {
            snippet = snippet.substr(0, 36) + "...";
        }
        const std::string& resultModule = r.module.empty() ? moduleName : r.module;
        std::string shortKey = app_->swordManager().getShortReference(resultModule, r.key);
        std::string line = shortKey.empty() ? r.key : shortKey;
        if (!snippet.empty()) {
            line += " \t " + snippet;
        }
        resultBrowser_->add(line.c_str());
    }

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

void SearchPanel::clear() {
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
    moduleChoice_->clear();

    // Search targets Bible modules.
    auto bibles = app_->swordManager().getBibleModules();
    for (const auto& mod : bibles) {
        moduleChoice_->add(mod.name.c_str());
    }

    if (moduleChoice_->size() > 0) {
        moduleChoice_->value(0);
    }
}

void SearchPanel::setSelectedModule(const std::string& moduleName) {
    std::string name = trimCopy(moduleName);
    if (!moduleChoice_ || name.empty()) return;
    int moduleIndex = findChoiceIndexByLabel(moduleChoice_, name);
    if (moduleIndex >= 0) {
        moduleChoice_->value(moduleIndex);
    }
}

void SearchPanel::setResultCountLabel(const std::string& suffix) {
    if (!resultBrowser_) return;
    if (!suffix.empty()) {
        statusSuffix_ = suffix;
    } else {
        statusSuffix_.clear();
    }
    std::string label = "Results: " + std::to_string(results_.size());
    if (!statusSuffix_.empty()) {
        label += " " + statusSuffix_;
    }
    resultBrowser_->copy_label(label.c_str());
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
        displayModule = activeModule;
        progress = activePercent;
    } else if (!indexingModule_.empty()) {
        int p = indexer->moduleIndexProgress(indexingModule_);
        if (p >= 0 && p < 100) {
            displayModule = indexingModule_;
            progress = p;
        }
    } else if (moduleChoice_ && moduleChoice_->mvalue() &&
               moduleChoice_->mvalue()->label()) {
        std::string selected = moduleChoice_->mvalue()->label();
        int p = indexer->moduleIndexProgress(selected);
        if (p >= 0 && p < 100) {
            displayModule = selected;
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

void SearchPanel::onIndexingPoll(void* data) {
    auto* self = static_cast<SearchPanel*>(data);
    if (!self || !self->indexingIndicatorActive_) return;

    self->updateIndexingIndicator();
    Fl::repeat_timeout(0.2, onIndexingPoll, self);
}

void SearchPanel::onResultSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<SearchPanel*>(data);

    const SearchResult* result = self->selectedResult();
    if (!result) {
        return;
    }

    // Update preview in left pane
    if (self->app_->mainWindow() && self->app_->mainWindow()->leftPane()) {
        std::string html = self->app_->swordManager().getVerseText(
            result->module, result->key);
        self->app_->mainWindow()->leftPane()->setPreviewText(html);
    }

    if (!self->app_->mainWindow()) return;
    int button = Fl::event_button();
    if (button == FL_MIDDLE_MOUSE) {
        self->app_->mainWindow()->openInNewStudyTab(result->module, result->key);
        return;
    }

    if (button == FL_LEFT_MOUSE && Fl::event_clicks() > 0) {
        self->app_->mainWindow()->navigateTo(result->module, result->key);
    }
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
