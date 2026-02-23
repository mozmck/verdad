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
                                          bool paragraphMode) {
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

    while (!mod->popError() && vk->getChapter() == currentChapter) {
        int verse = vk->getVerse();
        std::string verseText = std::string(mod->renderText().c_str());

        if (!verseText.empty()) {
            verseText = postProcessHtml(verseText);
            html << "<" << verseTag << " class=\"verse\" id=\"v" << verse << "\">";
            html << "<sup class=\"versenum\">" << verse << "</sup> ";
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
    bool paragraphMode) {

    (void)paragraphMode; // Parallel view uses table layout; mode not applicable
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

    std::ostringstream html;
    html << "<table class=\"parallel\">\n";

    // Header row
    html << "<tr>\n";
    for (const auto& modName : moduleNames) {
        html << "<th>" << modName << "</th>\n";
    }
    html << "</tr>\n";

    // Verse rows
    for (int v = 1; v <= verseCount; ++v) {
        html << "<tr>\n";
        for (const auto& modName : moduleNames) {
            sword::SWModule* mod = getModule(modName);
            html << "<td>";
            if (mod) {
                std::string ref = book + " " + std::to_string(chapter)
                                  + ":" + std::to_string(v);
                mod->setKey(ref.c_str());
                if (!mod->popError()) {
                    html << "<sup class=\"versenum\">" << v << "</sup> ";
                    html << postProcessHtml(std::string(mod->renderText().c_str()));
                }
            }
            html << "</td>\n";
        }
        html << "</tr>\n";
    }

    html << "</table>\n";
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

    // Try various Strong's lexicons
    std::vector<std::string> lexicons;

    // Determine if Hebrew or Greek
    if (!strongsNumber.empty()) {
        char prefix = static_cast<char>(
            std::toupper(static_cast<unsigned char>(strongsNumber[0])));
        if (prefix == 'H') {
            lexicons = {"StrongsHebrew", "StrongsRealHebrew", "TWOT"};
        } else if (prefix == 'G') {
            lexicons = {"StrongsGreek", "StrongsRealGreek", "Thayer"};
        }
    }

    for (const auto& lexName : lexicons) {
        sword::SWModule* lex = getModule(lexName);
        if (lex) {
            // Try with and without the H/G prefix
            lex->setKey(strongsNumber.c_str());
            if (!lex->popError()) {
                std::string text = lex->stripText();
                if (!text.empty()) return text;
            }

            // Try just the number part
            if (strongsNumber.length() > 1) {
                std::string numOnly = strongsNumber.substr(1);
                lex->setKey(numOnly.c_str());
                if (!lex->popError()) {
                    std::string text = lex->stripText();
                    if (!text.empty()) return text;
                }
            }
        }
    }

    return "";
}

std::string SwordManager::getMorphDefinition(const std::string& morphCode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mgr_) return "";

    // Try morphology lexicons
    std::vector<std::string> morphLexicons = {
        "Robinson", "Packard", "OSHM"
    };

    for (const auto& lexName : morphLexicons) {
        sword::SWModule* lex = getModule(lexName);
        if (lex) {
            lex->setKey(morphCode.c_str());
            if (!lex->popError()) {
                std::string text = lex->stripText();
                if (!text.empty()) return text;
            }
        }
    }

    return "";
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
    // Helper: replace all regex matches using a formatter function
    static const auto replaceAll = [](
        const std::string& str,
        const std::regex& re,
        std::function<std::string(const std::smatch&)> fmt) -> std::string {
        std::string out;
        std::sregex_iterator it(str.begin(), str.end(), re);
        std::sregex_iterator end;
        size_t lastEnd = 0;
        for (; it != end; ++it) {
            const auto& m = *it;
            out += str.substr(lastEnd, static_cast<size_t>(m.position()) - lastEnd);
            out += fmt(m);
            lastEnd = static_cast<size_t>(m.position()) + static_cast<size_t>(m.length());
        }
        out += str.substr(lastEnd);
        return out;
    };

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

    // 1. Transform word + SWORD Strong's/morph blocks into hoverable <span>.
    //    Matches a word followed by one or more <small><em> blocks.
    {
        std::regex wordBlockRe(
            R"(([\w'\-]+))"        // Group 1: the word (including hyphens)
            R"(((?:\s*)" + smallEmBlock + R"()+))",  // Group 2: block(s)
            std::regex::icase);

        // Regex to extract type and value from passagestudy.jsp URLs
        // type may be URL-encoded (e.g. robinson%3AV-QAL), so use [^&]+ not \w+
        static const std::regex typeValueRe(
            R"(type=([^&]+)&[^"]*value=([^"&]+))", std::regex::icase);

        result = replaceAll(result, wordBlockRe,
            [](const std::smatch& m) -> std::string {
            std::string word = m[1].str();
            std::string blocks = m[2].str();

            // Classify each <small><em> block as Strong's or morph
            std::string strongNums;
            std::string morphCode;

            // Split blocks by finding each <small>...</small> segment
            std::regex oneBlockRe(
                R"(<small>\s*<em\b([^>]*)>[^<]*(?:<a\b[^>]*>[^<]*</a>[^<]*)?</em>\s*</small>)",
                std::regex::icase);
            auto bit = std::sregex_iterator(blocks.begin(), blocks.end(),
                                            oneBlockRe);
            for (; bit != std::sregex_iterator(); ++bit) {
                std::string emAttrs = (*bit)[1].str();
                std::string blockStr = (*bit)[0].str();
                bool isStrongs = emAttrs.find("strongs") != std::string::npos;
                bool isMorph   = emAttrs.find("morph")   != std::string::npos;

                // Extract type and value from URL within this block
                std::smatch tv;
                if (std::regex_search(blockStr, tv, typeValueRe)) {
                    if (isStrongs) {
                        std::string type  = tv[1].str();
                        std::string value = tv[2].str();
                        // Map type name to H/G prefix
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

            if (strongNums.empty() && morphCode.empty())
                return word;

            std::string span = R"(<span class="w")";
            if (!strongNums.empty()) span += R"( data-strong=")" + strongNums + '"';
            if (!morphCode.empty())  span += R"( data-morph=")"  + morphCode  + '"';
            span += '>' + word + "</span>";
            return span;
        });
    }

    // 2. Strip remaining <small><em class="strongs/morph"> blocks not preceded by a word.
    {
        std::regex remainingSmallRe(smallEmBlock, std::regex::icase);
        result = std::regex_replace(result, remainingSmallRe, std::string(" "));
    }

    // 3. Handle passagestudy.jsp Strong's anchor links outside <small> blocks.
    //    Strip the number text; wrap word text with data-strong attribute.
    {
        std::regex aPassageStudyRe(
            R"re(<a\b[^>]*href="passagestudy\.jsp\?[^"]*showStrongs[^"]*type=(\w+)[^"]*value=([^"&]+)"[^>]*>([^<]*)</a>)re",
            std::regex::icase);
        result = replaceAll(result, aPassageStudyRe,
            [](const std::smatch& m) -> std::string {
            std::string type  = m[1].str();
            std::string value = m[2].str();
            std::string text  = m[3].str();
            std::string prefix;
            if (type.find("ebrew") != std::string::npos) prefix = "H";
            else if (type.find("reek") != std::string::npos) prefix = "G";
            std::string strong = prefix + value;

            // If text is all digits (a Strong's number) → suppress it
            bool allDigits = !text.empty();
            for (auto c : text)
                if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
            if (allDigits) return " ";

            return R"(<span class="w" data-strong=")" + strong + "\">" + text + "</span>";
        });
    }

    // 4. Strip passagestudy.jsp morph anchor links.
    {
        std::regex aMorphPassageRe(
            R"(<a\b[^>]*href="passagestudy\.jsp\?[^"]*showMorph[^"]*"[^>]*>[^<]*</a>)",
            std::regex::icase);
        result = std::regex_replace(result, aMorphPassageRe, std::string(" "));
    }

    // 5. Transform OSIS <w> elements to hoverable spans (some SWORD configs
    //    may preserve these).
    //    <w lemma="strong:H7225" morph="x-Robinson:Ncfsa">word</w>
    //    → <span class="w" data-strong="H7225" data-morph="Ncfsa">word</span>
    {
        std::regex wRe(R"(<w\b([^>]*)>([\s\S]*?)</w>)", std::regex::icase);
        result = replaceAll(result, wRe, [](const std::smatch& m) -> std::string {
            std::string attrs   = m[1].str();
            std::string content = m[2].str();

            // Extract all strong numbers from lemma attribute (may be space-separated)
            std::string strong;
            std::regex lemmaRe(R"re(lemma="([^"]+)")re");
            std::smatch lm;
            if (std::regex_search(attrs, lm, lemmaRe)) {
                std::string lemmaVal = lm[1].str();
                std::regex strongNumRe(R"(strong:([A-Za-z]?\d+[a-z]?))");
                auto sit = std::sregex_iterator(lemmaVal.begin(), lemmaVal.end(), strongNumRe);
                for (; sit != std::sregex_iterator(); ++sit) {
                    std::string s = (*sit)[1].str();
                    if (!s.empty()) {
                        if (std::islower(static_cast<unsigned char>(s[0])))
                            s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
                        if (!strong.empty()) strong += '|';
                        strong += s;
                    }
                }
            }

            // Extract morph code (take the part after the last ':' prefix)
            std::string morph;
            std::regex morphRe(R"re(morph="([^"]+)")re");
            std::smatch mm;
            if (std::regex_search(attrs, mm, morphRe)) {
                morph = mm[1].str();
                auto colonPos = morph.find(':');
                if (colonPos != std::string::npos &&
                    morph.substr(0, colonPos).find(' ') == std::string::npos) {
                    morph = morph.substr(colonPos + 1);
                }
            }

            if (strong.empty() && morph.empty())
                return content;

            std::string span = R"(<span class="w")";
            if (!strong.empty()) span += R"( data-strong=")" + strong + '"';
            if (!morph.empty())  span += R"( data-morph=")"  + morph  + '"';
            span += '>' + content + "</span>";
            return span;
        });
    }

    // 6. Transform anchor Strong's links (strongs: scheme).
    //    If anchor text is a Strong's number → strip it entirely.
    //    If anchor text is a word → wrap it in <span class="w" data-strong="...">.
    {
        std::regex aStrongsRe(
            R"re(<a\b[^>]*href="strongs:([A-Za-z]?\d+[a-z]?)"[^>]*>([^<]*)</a>)re",
            std::regex::icase);
        result = replaceAll(result, aStrongsRe, [](const std::smatch& m) -> std::string {
            std::string strong = m[1].str();
            std::string text   = m[2].str();

            if (!strong.empty() && std::islower(static_cast<unsigned char>(strong[0])))
                strong[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(strong[0])));

            bool textIsNumber = !text.empty();
            for (size_t i = 0; i < text.size(); ++i) {
                unsigned char c = static_cast<unsigned char>(text[i]);
                if (i == 0 && std::isalpha(c)) continue;
                if (!std::isdigit(c)) { textIsNumber = false; break; }
            }

            if (textIsNumber || text.empty())
                return " ";

            return R"(<span class="w" data-strong=")" + strong + "\">" + text + "</span>";
        });
    }

    // 7. Strip anchor morph links (morph: scheme).
    {
        std::regex aMorphRe(R"(<a\b[^>]*href="morph:[^"]*"[^>]*>[^<]*</a>)",
                            std::regex::icase);
        result = std::regex_replace(result, aMorphRe, std::string(" "));
    }

    // 8. Strip GBF-style inline Strong's markers: <H0776>, <G123>, or <0776>.
    {
        std::regex gbfStrongsRe(R"(<[HGhg]?\d{1,6}>)");
        result = std::regex_replace(result, gbfStrongsRe, std::string(" "));
    }

    // 9. Strip HTML-escaped GBF Strong's markers: &lt;H0776&gt; or &lt;0776&gt;.
    {
        std::regex escapedRe(R"(&lt;[HGhg]?\d{1,6}&gt;)");
        result = std::regex_replace(result, escapedRe, std::string(" "));
    }

    // 10. Strip GBF morph codes in parentheses, e.g. (8804) or (8804b).
    {
        std::regex gbfMorphRe(R"(\(\d{4,5}[A-Za-z]?\))");
        result = std::regex_replace(result, gbfMorphRe, std::string(" "));
    }

    // 11. Collapse multiple consecutive spaces into a single space.
    {
        std::regex multiSpaceRe(R"( {2,})");
        result = std::regex_replace(result, multiSpaceRe, std::string(" "));
    }

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
