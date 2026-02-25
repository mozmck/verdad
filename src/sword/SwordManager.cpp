#include "sword/SwordManager.h"

#include <swmgr.h>
#include <swmodule.h>
#include <swkey.h>
#include <versekey.h>
#include <listkey.h>
#include <markupfiltmgr.h>
#include <swbuf.h>

#include <algorithm>
#include <regex>
#include <sstream>
#include <cstring>
#include <cctype>

namespace verdad {
namespace {

std::string trimCopy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() &&
           std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

bool decodeNextUtf8(const std::string& s, size_t& i, uint32_t& cp) {
    if (i >= s.size()) return false;
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        cp = c0;
        ++i;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        cp = (static_cast<uint32_t>(c0 & 0x1F) << 6) |
             static_cast<uint32_t>(c1 & 0x3F);
        i += 2;
        return true;
    }
    if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) |
             (static_cast<uint32_t>(c1 & 0x3F) << 6) |
             static_cast<uint32_t>(c2 & 0x3F);
        i += 3;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
        cp = (static_cast<uint32_t>(c0 & 0x07) << 18) |
             (static_cast<uint32_t>(c1 & 0x3F) << 12) |
             (static_cast<uint32_t>(c2 & 0x3F) << 6) |
             static_cast<uint32_t>(c3 & 0x3F);
        i += 4;
        return true;
    }
    // Invalid leading byte; skip it.
    cp = c0;
    ++i;
    return true;
}

bool isCjkCodepoint(uint32_t cp) {
    // CJK Unified Ideographs + Extensions + Compatibility + Japanese/Korean ranges.
    return
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0x20000 && cp <= 0x2EBEF) ||
        (cp >= 0x30000 && cp <= 0x3134F) ||
        (cp >= 0x3040 && cp <= 0x30FF) ||
        (cp >= 0x31F0 && cp <= 0x31FF) ||
        (cp >= 0xAC00 && cp <= 0xD7AF);
}

bool containsCjkText(const std::string& s) {
    size_t i = 0;
    uint32_t cp = 0;
    while (decodeNextUtf8(s, i, cp)) {
        if (isCjkCodepoint(cp)) return true;
    }
    return false;
}

std::string stripCjkText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = 0;
        if (!decodeNextUtf8(s, i, cp)) break;
        if (!isCjkCodepoint(cp)) {
            out.append(s.substr(start, i - start));
        }
    }
    return trimCopy(out);
}

std::string cleanupLexiconText(const std::string& s) {
    // Preserve full lexicon payload; only trim outer whitespace.
    return trimCopy(s);
}

std::string normalizeStrongsKey(const std::string& strongsNumber) {
    std::string key = trimCopy(strongsNumber);
    if (key.empty()) return key;

    static const std::regex keyRe(R"(([A-Za-z]?\d+[A-Za-z]?))");
    std::smatch match;
    if (std::regex_search(key, match, keyRe)) {
        key = match[1].str();
    }

    for (char& c : key) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    if (!key.empty() && std::isalpha(static_cast<unsigned char>(key[0]))) {
        // Strong's prefixes are H (Hebrew) or G (Greek). Reject unknown prefixes
        // to avoid lookups drifting to stale/incorrect lexicon keys.
        if (key[0] != 'H' && key[0] != 'G') return "";
    }
    return key;
}

std::string canonicalLexKey(const std::string& keyText) {
    std::string key = normalizeStrongsKey(keyText);
    if (key.empty()) return key;

    size_t i = 0;
    std::string prefix;
    if (std::isalpha(static_cast<unsigned char>(key[i]))) {
        prefix.push_back(key[i]);
        ++i;
    }

    size_t j = i;
    while (j < key.size() && std::isdigit(static_cast<unsigned char>(key[j]))) ++j;
    if (j == i) return key;

    std::string digits = key.substr(i, j - i);
    size_t nz = digits.find_first_not_of('0');
    digits = (nz == std::string::npos) ? "0" : digits.substr(nz);

    std::string suffix = key.substr(j);
    return prefix + digits + suffix;
}

bool keyMatchesRequest(const std::string& requestedKey, const std::string& resolvedKey) {
    std::string req = canonicalLexKey(requestedKey);
    std::string got = canonicalLexKey(resolvedKey);
    if (req.empty() || got.empty()) return false;
    if (req == got) return true;

    // Allow prefixed/unprefixed equivalent forms (e.g. H7225 vs 7225).
    if (req.size() > 1 && std::isalpha(static_cast<unsigned char>(req[0])) &&
        req.substr(1) == got) {
        return true;
    }
    if (got.size() > 1 && std::isalpha(static_cast<unsigned char>(got[0])) &&
        got.substr(1) == req) {
        return true;
    }
    return false;
}

std::string readLexiconEntry(sword::SWModule* lex, const std::string& key) {
    if (!lex || key.empty()) return "";

    lex->setKey(key.c_str());
    if (lex->popError()) return "";

    // Ensure entry text is loaded for this key before stripping markup.
    (void)lex->renderText();
    if (lex->popError()) return "";

    const char* resolvedKey = lex->getKeyText();
    if (!resolvedKey || !keyMatchesRequest(key, resolvedKey)) return "";

    return cleanupLexiconText(lex->stripText());
}

template <typename Formatter>
std::string regexReplaceAll(const std::string& str,
                            const std::regex& re,
                            Formatter&& fmt) {
    std::string out;
    out.reserve(str.size() + 16);
    std::sregex_iterator it(str.begin(), str.end(), re);
    std::sregex_iterator end;
    size_t lastEnd = 0;
    for (; it != end; ++it) {
        const auto& m = *it;
        out.append(str, lastEnd, static_cast<size_t>(m.position()) - lastEnd);
        out += fmt(m);
        lastEnd = static_cast<size_t>(m.position()) + static_cast<size_t>(m.length());
    }
    out.append(str, lastEnd, std::string::npos);
    return out;
}

bool mayContainMorphOrStrongsMarkup(const std::string& html) {
    return
        html.find("<small") != std::string::npos ||
        html.find("<SMALL") != std::string::npos ||
        html.find("passagestudy.jsp") != std::string::npos ||
        html.find("<w") != std::string::npos ||
        html.find("<W") != std::string::npos ||
        html.find("strongs:") != std::string::npos ||
        html.find("STRONGS:") != std::string::npos ||
        html.find("morph:") != std::string::npos ||
        html.find("MORPH:") != std::string::npos ||
        html.find("&lt;H") != std::string::npos ||
        html.find("&lt;G") != std::string::npos ||
        html.find("&lt;h") != std::string::npos ||
        html.find("&lt;g") != std::string::npos ||
        html.find("&lt;0") != std::string::npos ||
        html.find("&lt;1") != std::string::npos ||
        html.find("&lt;2") != std::string::npos ||
        html.find("&lt;3") != std::string::npos ||
        html.find("&lt;4") != std::string::npos ||
        html.find("&lt;5") != std::string::npos ||
        html.find("&lt;6") != std::string::npos ||
        html.find("&lt;7") != std::string::npos ||
        html.find("&lt;8") != std::string::npos ||
        html.find("&lt;9") != std::string::npos;
}

} // namespace

SwordManager::SwordManager() = default;
SwordManager::~SwordManager() = default;

bool SwordManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        // Create SWORD manager with XHTML markup filter
        mgr_ = std::make_unique<sword::SWMgr>(
            new sword::MarkupFilterMgr(sword::FMT_XHTML));

        if (!mgr_) {
            return false;
        }

        // Enable global options for Strong's numbers and morphology
        mgr_->setGlobalOption("Strong's Numbers", "On");
        mgr_->setGlobalOption("Morphological Tags", "On");
        mgr_->setGlobalOption("Lemmas", "On");
        mgr_->setGlobalOption("Headings", "On");
        mgr_->setGlobalOption("Footnotes", "On");
        mgr_->setGlobalOption("Cross-references", "On");
        mgr_->setGlobalOption("Words of Christ in Red", "On");

        return true;
    } catch (...) {
        return false;
    }
}

std::vector<ModuleInfo> SwordManager::getModules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModuleInfo> modules;

    if (!mgr_) return modules;

    for (auto it = mgr_->Modules.begin(); it != mgr_->Modules.end(); ++it) {
        modules.push_back(buildModuleInfo(it->second));
    }

    std::sort(modules.begin(), modules.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.name < b.name;
              });

    return modules;
}

std::vector<ModuleInfo> SwordManager::getModulesByType(const std::string& type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModuleInfo> modules;

    if (!mgr_) return modules;

    for (auto it = mgr_->Modules.begin(); it != mgr_->Modules.end(); ++it) {
        sword::SWModule* mod = it->second;
        if (mod->getType() == type) {
            modules.push_back(buildModuleInfo(mod));
        }
    }

    std::sort(modules.begin(), modules.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.name < b.name;
              });

    return modules;
}

std::vector<ModuleInfo> SwordManager::getBibleModules() const {
    return getModulesByType("Biblical Texts");
}

std::vector<ModuleInfo> SwordManager::getCommentaryModules() const {
    return getModulesByType("Commentaries");
}

std::vector<ModuleInfo> SwordManager::getDictionaryModules() const {
    return getModulesByType("Lexicons / Dictionaries");
}

std::string SwordManager::getVerseText(const std::string& moduleName,
                                        const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Module not found: " + moduleName + "</i></p>";

    mod->setKey(key.c_str());
    std::string text = std::string(mod->renderText().c_str());

    if (text.empty()) {
        return "<p><i>No text available for " + key + "</i></p>";
    }

    return text;
}

std::string SwordManager::getChapterText(const std::string& moduleName,
                                          const std::string& book,
                                          int chapter,
                                          bool paragraphMode,
                                          int selectedVerse) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Module not found: " + moduleName + "</i></p>";

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        // Clone a verse key
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return "<p><i>Cannot create verse key</i></p>";

    std::ostringstream html;
    html << "<div class=\"chapter\">\n";

    // Set to the beginning of the chapter
    std::string ref = book + " " + std::to_string(chapter) + ":1";
    vk->setText(ref.c_str());

    int currentChapter = vk->getChapter();

    // Choose element tag based on display mode:
    // verse-per-line (default) uses <div>, paragraph mode uses <span>
    const char* verseTag = paragraphMode ? "span" : "div";

    auto tryGetVerseHtmlCache =
        [this](const std::string& key, std::string& valueOut) -> bool {
        auto it = verseHtmlCache_.find(key);
        if (it == verseHtmlCache_.end()) return false;

        verseHtmlLru_.splice(verseHtmlLru_.begin(),
                             verseHtmlLru_,
                             it->second.lruIt);
        valueOut = it->second.value;
        return true;
    };

    auto storeVerseHtmlCache =
        [this](const std::string& key, const std::string& value) {
        auto it = verseHtmlCache_.find(key);
        if (it != verseHtmlCache_.end()) {
            it->second.value = value;
            verseHtmlLru_.splice(verseHtmlLru_.begin(),
                                 verseHtmlLru_,
                                 it->second.lruIt);
            return;
        }

        verseHtmlLru_.push_front(key);
        verseHtmlCache_.emplace(key, VerseHtmlCacheEntry{
            value, verseHtmlLru_.begin()
        });

        if (verseHtmlCache_.size() > kVerseHtmlCacheLimit) {
            const std::string& evictKey = verseHtmlLru_.back();
            verseHtmlCache_.erase(evictKey);
            verseHtmlLru_.pop_back();
        }
    };

    while (!mod->popError() && vk->getChapter() == currentChapter) {
        int verse = vk->getVerse();
        std::string verseRef = book + " " + std::to_string(chapter) +
                               ":" + std::to_string(verse);
        std::string cacheKey = moduleName + "|" + verseRef;
        std::string verseText;

        if (!tryGetVerseHtmlCache(cacheKey, verseText)) {
            verseText = std::string(mod->renderText().c_str());
            if (!verseText.empty()) {
                verseText = postProcessHtml(verseText);
                storeVerseHtmlCache(cacheKey, verseText);
            }
        }

        if (!verseText.empty()) {
            std::string verseClass = "verse";
            if (selectedVerse > 0 && verse == selectedVerse) {
                verseClass += " verse-selected";
            }
            html << "<" << verseTag << " class=\"" << verseClass
                 << "\" id=\"v" << verse << "\">";
            html << "<a class=\"versenum-link\" href=\"verse:" << verse << "\">"
                 << "<sup class=\"versenum\">" << verse << "</sup></a> ";
            html << verseText;
            html << "</" << verseTag << ">\n";
        }

        (*mod)++;  // Advance to next verse
    }

    html << "</div>\n";
    return html.str();
}

std::string SwordManager::getParallelText(
    const std::vector<std::string>& moduleNames,
    const std::string& book, int chapter,
    bool paragraphMode,
    int selectedVerse) {

    (void)paragraphMode; // Parallel view uses column layout; mode not applicable
    if (moduleNames.empty()) return "";

    std::lock_guard<std::mutex> lock(mutex_);

    // Determine verse count from first valid module
    int verseCount = 0;
    for (const auto& modName : moduleNames) {
        sword::SWModule* mod = getModule(modName);
        if (mod) {
            sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
            if (!vk) {
                sword::VerseKey tempKey;
                mod->setKey(tempKey);
                vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
            }
            if (vk) {
                std::string ref = book + " " + std::to_string(chapter) + ":1";
                vk->setText(ref.c_str());
                // Count verses by iterating
                int ch = vk->getChapter();
                while (!mod->popError() && vk->getChapter() == ch) {
                    verseCount++;
                    (*mod)++;
                }
            }
            break;
        }
    }

    if (verseCount == 0) verseCount = 31; // fallback

    // Use div-based float layout for reliable column rendering in litehtml
    int numCols = static_cast<int>(moduleNames.size());
    int colWidth = 100 / numCols;
    int lastColWidth = 100 - colWidth * (numCols - 1);

    std::ostringstream html;
    html << "<div class=\"parallel\">\n";

    // Header row
    html << "<div class=\"parallel-row\">\n";
    for (size_t i = 0; i < moduleNames.size(); ++i) {
        bool isLast = (i == moduleNames.size() - 1);
        int w = isLast ? lastColWidth : colWidth;
        const char* cellClass = isLast ? "parallel-header-last" : "parallel-header";
        html << "<div style=\"float: left; width: " << w << "%;\">"
             << "<div class=\"" << cellClass << "\">" << moduleNames[i] << "</div>"
             << "</div>\n";
    }
    html << "<div style=\"clear: both;\"></div>\n";
    html << "</div>\n";

    auto tryGetVerseHtmlCache =
        [this](const std::string& key, std::string& valueOut) -> bool {
        auto it = verseHtmlCache_.find(key);
        if (it == verseHtmlCache_.end()) return false;

        verseHtmlLru_.splice(verseHtmlLru_.begin(),
                             verseHtmlLru_,
                             it->second.lruIt);
        valueOut = it->second.value;
        return true;
    };

    auto storeVerseHtmlCache =
        [this](const std::string& key, const std::string& value) {
        auto it = verseHtmlCache_.find(key);
        if (it != verseHtmlCache_.end()) {
            it->second.value = value;
            verseHtmlLru_.splice(verseHtmlLru_.begin(),
                                 verseHtmlLru_,
                                 it->second.lruIt);
            return;
        }

        verseHtmlLru_.push_front(key);
        verseHtmlCache_.emplace(key, VerseHtmlCacheEntry{
            value, verseHtmlLru_.begin()
        });

        if (verseHtmlCache_.size() > kVerseHtmlCacheLimit) {
            const std::string& evictKey = verseHtmlLru_.back();
            verseHtmlCache_.erase(evictKey);
            verseHtmlLru_.pop_back();
        }
    };

    // Verse rows
    for (int v = 1; v <= verseCount; ++v) {
        html << "<div class=\"parallel-row\">\n";
        for (size_t i = 0; i < moduleNames.size(); ++i) {
            bool isLast = (i == moduleNames.size() - 1);
            int w = isLast ? lastColWidth : colWidth;
            const char* cellClass = isLast ? "parallel-cell-last" : "parallel-cell";
            std::string cellClasses = cellClass;
            if (selectedVerse > 0 && v == selectedVerse) {
                cellClasses += " verse-selected";
            }
            sword::SWModule* mod = getModule(moduleNames[i]);
            html << "<div style=\"float: left; width: " << w << "%;\">"
                 << "<div class=\"" << cellClasses << "\">";
            if (mod) {
                std::string ref = book + " " + std::to_string(chapter)
                                  + ":" + std::to_string(v);
                mod->setKey(ref.c_str());
                if (!mod->popError()) {
                    std::string cacheKey = moduleNames[i] + "|" + ref;
                    std::string verseText;
                    if (!tryGetVerseHtmlCache(cacheKey, verseText)) {
                        verseText = std::string(mod->renderText().c_str());
                        if (!verseText.empty()) {
                            verseText = postProcessHtml(verseText);
                            storeVerseHtmlCache(cacheKey, verseText);
                        }
                    }
                    html << "<a class=\"versenum-link\" href=\"verse:" << v << "\">"
                         << "<sup class=\"versenum\">" << v << "</sup></a> ";
                    html << verseText;
                }
            }
            html << "</div></div>\n";
        }
        html << "<div style=\"clear: both;\"></div>\n";
        html << "</div>\n";
    }

    html << "</div>\n";
    return html.str();
}

std::string SwordManager::getCommentaryText(const std::string& moduleName,
                                             const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Commentary module not found: " + moduleName + "</i></p>";

    mod->setKey(key.c_str());
    std::string text = std::string(mod->renderText().c_str());

    if (text.empty()) {
        return "<p><i>No commentary available for " + key + "</i></p>";
    }

    std::ostringstream html;
    html << "<div class=\"commentary\">\n";
    html << "<h3>" << key << "</h3>\n";
    html << text;
    html << "</div>\n";
    return html.str();
}

std::string SwordManager::getDictionaryEntry(const std::string& moduleName,
                                              const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Dictionary module not found: " + moduleName + "</i></p>";

    mod->setKey(key.c_str());
    std::string text = std::string(mod->renderText().c_str());

    if (text.empty()) {
        return "<p><i>No entry found for: " + key + "</i></p>";
    }

    std::ostringstream html;
    html << "<div class=\"dictionary\">\n";
    html << "<div class=\"entry-key\">" << key << "</div>\n";
    html << text;
    html << "</div>\n";
    return html.str();
}

std::vector<SearchResult> SwordManager::search(
    const std::string& moduleName,
    const std::string& searchText,
    int searchType,
    const std::string& scope,
    std::function<void(float)> callback) {

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SearchResult> results;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return results;

    // Set up scope key if provided
    sword::ListKey scopeKey;
    sword::SWKey* scopePtr = nullptr;
    if (!scope.empty()) {
        sword::VerseKey vk;
        scopeKey = vk.parseVerseList(scope.c_str(), "", true);
        scopePtr = &scopeKey;
    }

    // Perform search
    sword::ListKey& resultKeys = mod->search(
        searchText.c_str(), searchType, 0, scopePtr,
        nullptr,  // percent update function
        nullptr   // percent user data
    );

    // Collect results
    for (resultKeys = sword::TOP; !resultKeys.popError(); resultKeys++) {
        SearchResult result;
        result.key = resultKeys.getText();
        result.module = moduleName;

        // Get preview text
        mod->setKey(resultKeys.getText());
        result.text = mod->stripText();

        // Truncate long preview
        if (result.text.length() > 200) {
            result.text = result.text.substr(0, 200) + "...";
        }

        results.push_back(result);
    }

    return results;
}

std::vector<SearchResult> SwordManager::searchStrongs(
    const std::string& moduleName,
    const std::string& strongsNumber) {
    // Use attribute search for Strong's numbers
    // Search type -2 = entry attribute search
    std::string query = "Word//Lemma./" + strongsNumber;
    return search(moduleName, query, -2);
}

WordInfo SwordManager::getWordInfo(const std::string& moduleName,
                                    const std::string& verseKey,
                                    const std::string& word) {
    WordInfo info;
    info.word = word;

    // First phase: extract Strong's/morph codes under the lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sword::SWModule* mod = getModule(moduleName);
        if (!mod) return info;

        // Render the verse to populate entry attributes
        mod->setKey(verseKey.c_str());
        mod->renderText();

        // Search through word attributes to find matching word
        auto& attrs = mod->getEntryAttributes();
        auto wordIt = attrs.find("Word");
        if (wordIt != attrs.end()) {
            for (auto& entry : wordIt->second) {
                auto& wordAttrs = entry.second;

                auto textIt = wordAttrs.find("Text");
                if (textIt != wordAttrs.end()) {
                    std::string attrWord = textIt->second.c_str();
                    std::string wordLower = word;
                    std::string attrLower = attrWord;
                    std::transform(wordLower.begin(), wordLower.end(),
                                   wordLower.begin(), ::tolower);
                    std::transform(attrLower.begin(), attrLower.end(),
                                   attrLower.begin(), ::tolower);

                    if (wordLower == attrLower) {
                        auto lemmaIt = wordAttrs.find("Lemma");
                        if (lemmaIt != wordAttrs.end()) {
                            info.strongsNumber = lemmaIt->second.c_str();
                            auto colonPos = info.strongsNumber.find(':');
                            if (colonPos != std::string::npos) {
                                info.strongsNumber = info.strongsNumber.substr(colonPos + 1);
                            }
                        }

                        auto morphIt = wordAttrs.find("Morph");
                        if (morphIt != wordAttrs.end()) {
                            info.morphCode = morphIt->second.c_str();
                            auto colonPos = info.morphCode.find(':');
                            if (colonPos != std::string::npos) {
                                info.morphCode = info.morphCode.substr(colonPos + 1);
                            }
                        }

                        break;
                    }
                }
            }
        }
    } // lock released here

    // Second phase: look up definitions without holding the lock
    if (!info.strongsNumber.empty()) {
        info.strongsDef = getStrongsDefinition(info.strongsNumber);
    }

    if (!info.morphCode.empty()) {
        info.morphDef = getMorphDefinition(info.morphCode);
    }

    return info;
}

std::string SwordManager::getStrongsDefinition(const std::string& strongsNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mgr_) return "";

    std::string key = normalizeStrongsKey(strongsNumber);
    if (key.empty()) return "";

    char prefix = 0;
    if (!key.empty() && std::isalpha(static_cast<unsigned char>(key[0]))) {
        prefix = static_cast<char>(
            std::toupper(static_cast<unsigned char>(key[0])));
    }

    // Try various Strong's lexicons
    std::vector<std::string> lexicons;
    if (prefix == 'H') {
        lexicons = {"StrongsHebrew", "TWOT", "StrongsRealHebrew"};
    } else if (prefix == 'G') {
        lexicons = {"StrongsGreek", "Thayer", "StrongsRealGreek"};
    } else {
        // If prefix is missing, try both families.
        lexicons = {"StrongsHebrew", "TWOT", "StrongsRealHebrew",
                    "StrongsGreek", "Thayer", "StrongsRealGreek"};
    }

    std::vector<std::string> lookupKeys;
    lookupKeys.push_back(key);
    if (prefix && key.length() > 1) {
        lookupKeys.push_back(key.substr(1));
    } else if (!key.empty() &&
               std::isdigit(static_cast<unsigned char>(key[0]))) {
        lookupKeys.push_back("H" + key);
        lookupKeys.push_back("G" + key);
    }

    std::string fallbackWithoutCjk;
    for (const auto& lexName : lexicons) {
        sword::SWModule* lex = getModule(lexName);
        if (!lex) continue;
        for (const auto& lookupKey : lookupKeys) {
            std::string text = readLexiconEntry(lex, lookupKey);
            if (text.empty()) continue;
            if (!containsCjkText(text)) return text;
            if (fallbackWithoutCjk.empty()) {
                std::string stripped = stripCjkText(text);
                if (!stripped.empty()) fallbackWithoutCjk = stripped;
            }
        }
    }

    return fallbackWithoutCjk;
}

std::string SwordManager::getMorphDefinition(const std::string& morphCode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mgr_) return "";

    std::string key = trimCopy(morphCode);
    if (key.empty()) return "";

    std::string fallbackWithoutCjk;
    // Try morphology lexicons
    std::vector<std::string> morphLexicons = {
        "Robinson", "Packard", "OSHM"
    };

    for (const auto& lexName : morphLexicons) {
        sword::SWModule* lex = getModule(lexName);
        if (lex) {
            std::string text = readLexiconEntry(lex, key);
            if (text.empty()) continue;
            if (!containsCjkText(text)) return text;
            if (fallbackWithoutCjk.empty()) {
                std::string stripped = stripCjkText(text);
                if (!stripped.empty()) fallbackWithoutCjk = stripped;
            }
        }
    }

    return fallbackWithoutCjk;
}

std::vector<std::string> SwordManager::getBookNames(const std::string& moduleName) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> books;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return books;

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return books;

    for (int t = 0; t < 2; t++) {  // Old and New Testament
        vk->setTestament(t + 1);
        for (int b = 1; b <= vk->getBookMax(); b++) {
            vk->setBook(b);
            books.push_back(vk->getBookName());
        }
    }

    return books;
}

int SwordManager::getChapterCount(const std::string& moduleName,
                                   const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return 0;

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return 0;

    std::string ref = book + " 1:1";
    vk->setText(ref.c_str());

    return vk->getChapterMax();
}

int SwordManager::getVerseCount(const std::string& moduleName,
                                 const std::string& book, int chapter) {
    std::lock_guard<std::mutex> lock(mutex_);

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return 0;

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return 0;

    std::string ref = book + " " + std::to_string(chapter) + ":1";
    vk->setText(ref.c_str());

    return vk->getVerseMax();
}

std::string SwordManager::getModuleDescription(const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "";
    return mod->getDescription();
}

bool SwordManager::moduleHasStrongs(const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return false;
    auto feat = mod->getConfigEntry("Feature");
    if (feat) {
        std::string features(feat);
        return features.find("StrongsNumbers") != std::string::npos;
    }
    // Also check GlobalOptionFilter
    auto filter = mod->getConfigEntry("GlobalOptionFilter");
    if (filter) {
        std::string filters(filter);
        return filters.find("Strongs") != std::string::npos;
    }
    return false;
}

bool SwordManager::moduleHasMorph(const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return false;
    auto filter = mod->getConfigEntry("GlobalOptionFilter");
    if (filter) {
        std::string filters(filter);
        return filters.find("Morph") != std::string::npos;
    }
    return false;
}

SwordManager::VerseRef SwordManager::parseVerseRef(const std::string& ref) {
    VerseRef result;

    // Simple parser: "Book Chapter:Verse" or "Book Chapter:Verse-EndVerse"
    // Examples: "Genesis 1:1", "1 John 3:16", "Gen 1:1-5"
    size_t lastSpace = ref.rfind(' ');
    if (lastSpace == std::string::npos) {
        result.book = ref;
        return result;
    }

    std::string afterBook = ref.substr(lastSpace + 1);
    result.book = ref.substr(0, lastSpace);

    size_t colonPos = afterBook.find(':');
    if (colonPos != std::string::npos) {
        result.chapter = std::stoi(afterBook.substr(0, colonPos));
        std::string verseStr = afterBook.substr(colonPos + 1);

        size_t dashPos = verseStr.find('-');
        if (dashPos != std::string::npos) {
            result.verse = std::stoi(verseStr.substr(0, dashPos));
            result.verseEnd = std::stoi(verseStr.substr(dashPos + 1));
        } else {
            result.verse = std::stoi(verseStr);
        }
    } else {
        // Just chapter number
        try {
            result.chapter = std::stoi(afterBook);
        } catch (...) {
            // Might be part of the book name (e.g., "1 John")
            result.book = ref;
        }
    }

    return result;
}

std::map<std::string, std::map<std::string, std::string>>
SwordManager::getEntryAttributes(const std::string& moduleName) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::map<std::string, std::string>> result;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return result;

    auto& attrs = mod->getEntryAttributes();
    auto wordIt = attrs.find("Word");
    if (wordIt != attrs.end()) {
        for (auto& entry : wordIt->second) {
            std::map<std::string, std::string> wordAttrs;
            for (auto& attr : entry.second) {
                wordAttrs[attr.first.c_str()] = attr.second.c_str();
            }
            result[entry.first.c_str()] = wordAttrs;
        }
    }

    return result;
}

sword::SWModule* SwordManager::getModule(const std::string& name) const {
    if (!mgr_) return nullptr;
    auto it = mgr_->Modules.find(name.c_str());
    if (it != mgr_->Modules.end()) {
        return it->second;
    }
    return nullptr;
}

void SwordManager::configureFilters(sword::SWModule* mod) {
    // Filters are configured globally in the manager
    // This method can be used for per-module filter adjustments if needed
    (void)mod;
}

std::string SwordManager::postProcessHtml(const std::string& html) const {
    auto cacheIt = postProcessCache_.find(html);
    if (cacheIt != postProcessCache_.end()) {
        postProcessLru_.splice(postProcessLru_.begin(),
                               postProcessLru_,
                               cacheIt->second.lruIt);
        return cacheIt->second.value;
    }

    auto cacheStore = [this](const std::string& key, const std::string& value) {
        auto it = postProcessCache_.find(key);
        if (it != postProcessCache_.end()) {
            it->second.value = value;
            postProcessLru_.splice(postProcessLru_.begin(),
                                   postProcessLru_,
                                   it->second.lruIt);
            return;
        }

        postProcessLru_.push_front(key);
        postProcessCache_.emplace(key, PostProcessCacheEntry{
            value, postProcessLru_.begin()
        });

        if (postProcessCache_.size() > kPostProcessCacheLimit) {
            const std::string& evictKey = postProcessLru_.back();
            postProcessCache_.erase(evictKey);
            postProcessLru_.pop_back();
        }
    };

    if (!mayContainMorphOrStrongsMarkup(html)) {
        cacheStore(html, html);
        return html;
    }

    std::string result = html;

    // --- SWORD XHTML output handling ---
    // SWORD's XHTML filters (OSISXHTML, GBFXHTML, ThMLXHTML) output Strong's
    // numbers and morph codes as:
    //   <small><em class="strongs">&lt;<a class="strongs"
    //     href="passagestudy.jsp?action=showStrongs&type=Hebrew&value=7225"
    //     class="strongs">7225</a>&gt;</em></small>
    //   <small><em class="morph">(<a class="morph"
    //     href="passagestudy.jsp?action=showMorph&type=...&value=V-AAI-3S"
    //     class="morph">V-AAI-3S</a>)</em></small>
    //
    // These appear AFTER the word text they annotate, e.g.:
    //   created<small><em class="strongs">...</em></small>
    //          <small><em class="morph">...</em></small>

    // Helper regex pattern for one <small><em ...>...</em></small> block.
    // Content may include one <a>...</a> element surrounded by text/entities.
    static const std::string smallEmBlock =
        R"(<small>\s*<em\b[^>]*>[^<]*(?:<a\b[^>]*>[^<]*</a>[^<]*)?</em>\s*</small>)";

    static const std::regex wordBlockRe(
        std::string(R"(([\w'\-]+))") +
            std::string(R"(((?:\s*)") + smallEmBlock + R"()+))",
        std::regex::icase);
    static const std::regex typeValueRe(
        R"(type=([^&]+)&[^"]*value=([^"&]+))",
        std::regex::icase);
    static const std::regex oneBlockRe(
        R"(<small>\s*<em\b([^>]*)>[^<]*(?:<a\b[^>]*>[^<]*</a>[^<]*)?</em>\s*</small>)",
        std::regex::icase);
    static const std::regex remainingSmallRe(smallEmBlock, std::regex::icase);
    static const std::regex aPassageStudyRe(
        R"re(<a\b[^>]*href="passagestudy\.jsp\?[^"]*showStrongs[^"]*type=(\w+)[^"]*value=([^"&]+)"[^>]*>([^<]*)</a>)re",
        std::regex::icase);
    static const std::regex aMorphPassageRe(
        R"(<a\b[^>]*href="passagestudy\.jsp\?[^"]*showMorph[^"]*"[^>]*>[^<]*</a>)",
        std::regex::icase);
    static const std::regex wRe(R"(<w\b([^>]*)>([\s\S]*?)</w>)", std::regex::icase);
    static const std::regex lemmaRe(R"re(lemma="([^"]+)")re");
    static const std::regex strongNumRe(R"(strong:([A-Za-z]?\d+[a-z]?))");
    static const std::regex morphRe(R"re(morph="([^"]+)")re");
    static const std::regex aStrongsRe(
        R"re(<a\b[^>]*href="strongs:([A-Za-z]?\d+[a-z]?)"[^>]*>([^<]*)</a>)re",
        std::regex::icase);
    static const std::regex aMorphRe(
        R"(<a\b[^>]*href="morph:[^"]*"[^>]*>[^<]*</a>)",
        std::regex::icase);
    static const std::regex gbfStrongsRe(R"(<[HGhg]?\d{1,6}>)");
    static const std::regex escapedRe(R"(&lt;[HGhg]?\d{1,6}&gt;)");
    static const std::regex gbfMorphRe(R"(\(\d{4,5}[A-Za-z]?\))");
    static const std::regex multiSpaceRe(R"( {2,})");

    const bool hasSmallBlocks =
        result.find("<small") != std::string::npos ||
        result.find("<SMALL") != std::string::npos;

    if (hasSmallBlocks) {
        // 1. Transform word + SWORD Strong's/morph blocks into hoverable spans.
        result = regexReplaceAll(result, wordBlockRe, [](const std::smatch& m) -> std::string {
            std::string word = m[1].str();
            std::string blocks = m[2].str();

            std::string strongNums;
            std::string morphCode;
            auto bit = std::sregex_iterator(blocks.begin(), blocks.end(), oneBlockRe);
            for (; bit != std::sregex_iterator(); ++bit) {
                std::string emAttrs = (*bit)[1].str();
                std::string blockStr = (*bit)[0].str();
                bool isStrongs = emAttrs.find("strongs") != std::string::npos;
                bool isMorph = emAttrs.find("morph") != std::string::npos;

                std::smatch tv;
                if (std::regex_search(blockStr, tv, typeValueRe)) {
                    if (isStrongs) {
                        std::string type = tv[1].str();
                        std::string value = tv[2].str();
                        std::string prefix;
                        if (type.find("ebrew") != std::string::npos) prefix = "H";
                        else if (type.find("reek") != std::string::npos) prefix = "G";
                        if (!strongNums.empty()) strongNums += '|';
                        strongNums += prefix + value;
                    } else if (isMorph) {
                        std::string value = tv[2].str();
                        if (!morphCode.empty()) morphCode += ' ';
                        morphCode += value;
                    }
                }
            }

            if (strongNums.empty() && morphCode.empty()) return word;

            std::string span = R"(<span class="w")";
            if (!strongNums.empty()) span += R"( data-strong=")" + strongNums + '"';
            if (!morphCode.empty()) span += R"( data-morph=")" + morphCode + '"';
            span += '>' + word + "</span>";
            return span;
        });

        // 2. Strip remaining <small><em class="strongs/morph"> blocks.
        result = std::regex_replace(result, remainingSmallRe, std::string(" "));
    }

    const bool hasPassageStudy = result.find("passagestudy.jsp") != std::string::npos;
    if (hasPassageStudy) {
        // 3. Handle passagestudy.jsp Strong's anchor links outside <small> blocks.
        result = regexReplaceAll(result, aPassageStudyRe,
            [](const std::smatch& m) -> std::string {
            std::string type = m[1].str();
            std::string value = m[2].str();
            std::string text = m[3].str();
            std::string prefix;
            if (type.find("ebrew") != std::string::npos) prefix = "H";
            else if (type.find("reek") != std::string::npos) prefix = "G";
            std::string strong = prefix + value;

            bool allDigits = !text.empty();
            for (char c : text) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) return " ";

            return R"(<span class="w" data-strong=")" + strong + "\">" + text + "</span>";
        });

        // 4. Strip passagestudy.jsp morph anchor links.
        result = std::regex_replace(result, aMorphPassageRe, std::string(" "));
    }

    const bool hasWTag = result.find("<w") != std::string::npos ||
                         result.find("<W") != std::string::npos;
    if (hasWTag) {
        // 5. Transform OSIS <w> elements to hoverable spans.
        result = regexReplaceAll(result, wRe, [](const std::smatch& m) -> std::string {
            std::string attrs = m[1].str();
            std::string content = m[2].str();

            std::string strong;
            std::smatch lm;
            if (std::regex_search(attrs, lm, lemmaRe)) {
                std::string lemmaVal = lm[1].str();
                auto sit = std::sregex_iterator(lemmaVal.begin(), lemmaVal.end(), strongNumRe);
                for (; sit != std::sregex_iterator(); ++sit) {
                    std::string s = (*sit)[1].str();
                    if (!s.empty()) {
                        if (std::islower(static_cast<unsigned char>(s[0]))) {
                            s[0] = static_cast<char>(
                                std::toupper(static_cast<unsigned char>(s[0])));
                        }
                        if (!strong.empty()) strong += '|';
                        strong += s;
                    }
                }
            }

            std::string morph;
            std::smatch mm;
            if (std::regex_search(attrs, mm, morphRe)) {
                morph = mm[1].str();
                auto colonPos = morph.find(':');
                if (colonPos != std::string::npos &&
                    morph.substr(0, colonPos).find(' ') == std::string::npos) {
                    morph = morph.substr(colonPos + 1);
                }
            }

            if (strong.empty() && morph.empty()) return content;

            std::string span = R"(<span class="w")";
            if (!strong.empty()) span += R"( data-strong=")" + strong + '"';
            if (!morph.empty()) span += R"( data-morph=")" + morph + '"';
            span += '>' + content + "</span>";
            return span;
        });
    }

    // 6. Transform anchor Strong's links (strongs: scheme).
    if (result.find("strongs:") != std::string::npos ||
        result.find("STRONGS:") != std::string::npos) {
        result = regexReplaceAll(result, aStrongsRe, [](const std::smatch& m) -> std::string {
            std::string strong = m[1].str();
            std::string text = m[2].str();

            if (!strong.empty() && std::islower(static_cast<unsigned char>(strong[0]))) {
                strong[0] = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(strong[0])));
            }

            bool textIsNumber = !text.empty();
            for (size_t i = 0; i < text.size(); ++i) {
                unsigned char c = static_cast<unsigned char>(text[i]);
                if (i == 0 && std::isalpha(c)) continue;
                if (!std::isdigit(c)) {
                    textIsNumber = false;
                    break;
                }
            }
            if (textIsNumber || text.empty()) return " ";

            return R"(<span class="w" data-strong=")" + strong + "\">" + text + "</span>";
        });
    }

    // 7. Strip anchor morph links (morph: scheme).
    if (result.find("morph:") != std::string::npos ||
        result.find("MORPH:") != std::string::npos) {
        result = std::regex_replace(result, aMorphRe, std::string(" "));
    }

    // 8. Strip GBF-style inline Strong's markers: <H0776>, <G123>, or <0776>.
    const bool hasInlineMarker =
        result.find("<H") != std::string::npos ||
        result.find("<G") != std::string::npos ||
        result.find("<h") != std::string::npos ||
        result.find("<g") != std::string::npos ||
        result.find("<0") != std::string::npos ||
        result.find("<1") != std::string::npos ||
        result.find("<2") != std::string::npos ||
        result.find("<3") != std::string::npos ||
        result.find("<4") != std::string::npos ||
        result.find("<5") != std::string::npos ||
        result.find("<6") != std::string::npos ||
        result.find("<7") != std::string::npos ||
        result.find("<8") != std::string::npos ||
        result.find("<9") != std::string::npos;
    if (hasInlineMarker) {
        result = std::regex_replace(result, gbfStrongsRe, std::string(" "));
    }

    // 9. Strip escaped GBF Strong's markers.
    if (result.find("&lt;") != std::string::npos) {
        result = std::regex_replace(result, escapedRe, std::string(" "));
    }

    // 10. Strip GBF morph codes in parentheses.
    if (result.find('(') != std::string::npos) {
        result = std::regex_replace(result, gbfMorphRe, std::string(" "));
    }

    // 11. Collapse multiple consecutive spaces into a single space.
    if (result.find("  ") != std::string::npos) {
        result = std::regex_replace(result, multiSpaceRe, std::string(" "));
    }

    cacheStore(html, result);
    return result;
}

ModuleInfo SwordManager::buildModuleInfo(sword::SWModule* mod) const {
    ModuleInfo info;
    info.name = mod->getName();
    info.description = mod->getDescription();
    info.type = mod->getType();

    auto lang = mod->getLanguage();
    info.language = lang ? lang : "en";

    // Check for Strong's
    auto feat = mod->getConfigEntry("Feature");
    if (feat) {
        std::string features(feat);
        info.hasStrongs = features.find("StrongsNumbers") != std::string::npos;
    }
    auto filter = mod->getConfigEntry("GlobalOptionFilter");
    if (filter) {
        std::string filters(filter);
        if (!info.hasStrongs) {
            info.hasStrongs = filters.find("Strongs") != std::string::npos;
        }
        info.hasMorph = filters.find("Morph") != std::string::npos;
    }

    return info;
}

} // namespace verdad
