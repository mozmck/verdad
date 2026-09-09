#include "import/ImportedModuleManager.h"
#include "search/SearchIndexer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::set<std::string> titles(const std::vector<verdad::SearchResult>& results) {
    std::set<std::string> found;
    for (const auto& result : results) found.insert(result.title);
    return found;
}

void expect(const std::vector<verdad::SearchResult>& results,
            const std::set<std::string>& wanted, const std::string& label) {
    const auto found = titles(results);
    if (found == wanted) return;
    std::string message = label + ": unexpected results:";
    for (const auto& title : found) message += " [" + title + "]";
    throw std::runtime_error(message);
}

void run(const std::filesystem::path& dir) {
    const auto source = dir / "fixture.md";
    {
        std::ofstream out(source);
        out << "# A\nrail\n# B\nrailing accusation\n# C\nrailer\n"
               "# D\nrain\n# E\nfail\n# F\nruler\n# G\nreviler\n"
               "# H\nreviled\n# I\nfrail\n# J\nfaith\n# K\nfaithful\n"
               "# L\nholy spirit\n# M\nholy ghost\n# N\nevil spirit\n"
               "# O\nfig\n# P\nolive\n# Q\nsword\n# R\narmor\n"
               "# S\narmour\n# T\ncafé\n# U\nrailers\n# V\nrailings\n";
    }
    verdad::ImportedModuleManager imports;
    require(imports.load((dir / "imports.db").string(), (dir / "imports").string()),
            "open imported module store");
    auto imported = imports.importPaths({source.string()}, {});
    require(imported.importedCount() == 1, "import Markdown fixture");
    const auto module = imported.files.front().moduleName;
    verdad::SearchIndexer indexer((dir / "search.db").string(), &imports);
    require(indexer.indexBackendAvailable(), "open FTS search backend");
    indexer.synchronizeModules(imports.modules());
    verdad::SearchIndexer::SearchRequest request;
    request.moduleName = module;

    const std::set<std::string> prefixes{"A", "B", "C", "U", "V"};
    expect(indexer.searchWordDirect(request, "rail*"), prefixes, "direct prefix");
    expect(indexer.searchWordDirect(request, "rail* accusation"), {"B"}, "direct AND");
    expect(indexer.searchWordDirect(request, "rail"), {"A"}, "direct whole word");

    indexer.queueModuleIndex(module);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!indexer.isModuleIndexed(module) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(indexer.isModuleIndexed(module), "index fixture: " + indexer.moduleIndexError(module));

    expect(indexer.searchWord(request, "rail*"), prefixes, "indexed prefix");
    expect(indexer.searchWord(request, "RAIL*,"), prefixes, "case and punctuation");
    expect(indexer.searchWord(request, "rail* accusation"), {"B"}, "indexed AND");
    expect(indexer.searchWord(request, "rail"), {"A"}, "indexed whole word");
    expect(indexer.searchWord(request, "holy spirit", true), {"L"}, "exact phrase");
    expect(indexer.searchWord(request, "*"), {}, "bare wildcard");
    expect(indexer.searchWord(request, "cafe*"), {"T"}, "accent insensitive prefix");

    expect(indexer.searchSmart(request, "rail"), {"A", "B", "C", "H", "U", "V"},
           "known rail excludes rain, fail, ruler, and frail");
    expect(indexer.searchSmart(request, "railer"), {"C", "G", "U"},
           "rare railer retains synonyms without ruler");
    expect(indexer.searchSmart(request, "rail*"), prefixes, "explicit prefix skips expansions");
    expect(indexer.searchSmart(request, "railer accusation"), {}, "no correction when AND is empty");
    expect(indexer.searchSmart(request, "holy spirit"), {"L", "M"}, "synonym phrase");
    expect(indexer.searchSmart(request, "fig"), {"O"}, "fig is not olive");
    expect(indexer.searchSmart(request, "armor"), {"R", "S"}, "armor is not sword");
    expect(indexer.searchSmart(request, "faiht"), {"J"}, "unknown transposed word");

    verdad::SearchIndexer::SmartSearchOptions exact;
    exact.spellingCorrection = false;
    exact.includeSynonyms = false;
    exact.partialWordMatching = false;
    exact.fuzzyExpansion = true;
    expect(indexer.searchSmart(request, "faiht", "en", 0, true, exact), {},
           "fuzzy must not bypass disabled spelling");
    expect(indexer.searchSmart(request, "rail*", "en", 0, true, exact), prefixes,
           "explicit prefix works without assistance");
    exact.spellingCorrection = true;
    expect(indexer.searchSmart(request, "railer", "en", 0, true, exact), {"C"},
           "spelling-only protects rare valid words");

    expect(indexer.searchRegex(request, R"(\brail\w*)"), prefixes, "regex prefix");
    expect(indexer.searchRegexDirect(request, R"(\brail\w*)"), prefixes, "direct regex prefix");
    expect(indexer.searchRegex(request, "rail*"), {"A", "B", "C", "D", "I", "U", "V"},
           "regex repetition and substring semantics");
    expect(indexer.searchRegex(request, "rail"), {"A", "B", "C", "I", "U", "V"},
           "unanchored regex includes word interiors");
    auto snippets = indexer.searchWord(request, "rail*");
    for (const auto& result : snippets) {
        require(result.text.find("searchhit") != std::string::npos,
                "prefix snippet missing highlight: " + result.title);
        if (result.title == "C") {
            require(result.text.find(">railer</span>") != std::string::npos,
                    "prefix highlights the complete word");
        }
    }
    auto smartSnippets = indexer.searchSmart(request, "railer");
    for (const auto& result : smartSnippets) {
        require(result.text.find("searchhit") != std::string::npos,
                "smart snippet missing original/synonym highlight: " + result.title);
    }
}

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("verdad-search-tests-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(dir);
        run(dir);
        std::filesystem::remove_all(dir);
        std::cout << "Search regression checks passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::filesystem::remove_all(dir);
        return 1;
    }
}
