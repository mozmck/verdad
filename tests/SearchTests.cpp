#include "import/ImportedModuleManager.h"
#include "search/SearchIndexer.h"
#include "search/SemanticSearch.h"
#include "search/SmartSearch.h"
#include "search/TopicSearch.h"

#include <sqlite3.h>

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

class FailingQueryEncoder final : public verdad::SemanticEncoder {
public:
    bool available() const override { return true; }
    std::string modelId() const override { return "verdad-test-encoder"; }
    std::string modelRevision() const override { return "1"; }
    int dimensions() const override { return 32; }
    bool encodeQuery(const std::string&,
                     std::vector<std::int8_t>&,
                     std::string& errorOut) override {
        errorOut = "fixture worker exited";
        return false;
    }
    bool encodePassages(const std::vector<std::string>&,
                        std::vector<std::vector<std::int8_t>>&,
                        std::string& errorOut) override {
        errorOut = "fixture worker exited";
        return false;
    }
};

void run(const std::filesystem::path& dir) {
    verdad::SemanticSearchService semantic((dir / "semantic").string());
    require(semantic.state() == verdad::SemanticPackState::NotInstalled,
            "semantic search starts as an optional, absent pack");
    semantic.setEncoder(std::make_shared<verdad::DeterministicSemanticEncoder>(32));
    semantic.setEnabled(true);
    const std::vector<verdad::SemanticPassage> semanticPassages{
        {"Genesis 1:1", "faith hope promise"},
        {"Genesis 1:2", "rain water clouds"},
        {"Genesis 1:3", "forgive enemies mercy"}
    };
    std::atomic<bool> semanticCancel{false};
    std::string semanticError;
    require(semantic.buildIndex("en", "TEST", "fixture-v1", semanticPassages,
                                {}, &semanticCancel, semanticError),
            "build deterministic semantic index: " + semanticError);
    const auto semanticHits = semantic.search("en", "TEST", "faith hope", 2);
    require(!semanticHits.empty() && semanticHits.front().reference == "Genesis 1:1",
            "exact vector scan ranks the related deterministic passage first");
    semantic.setReferenceModules({{"en", "TEST"}});
    require(!semantic.search("en", "ANOTHER", "faith hope", 2).empty(),
            "semantic hits from the selected reference Bible map by Scripture reference");
    semantic.setModuleSignatures({{"TEST", "fixture-v2"}});
    require(semantic.search("en", "ANOTHER", "faith hope", 2).empty(),
            "a changed reference-module signature invalidates only that semantic index");
    semantic.setModuleSignatures({{"TEST", "fixture-v1"}});
    require(!semantic.search("en", "ANOTHER", "faith hope", 2).empty(),
            "a matching reference-module signature accepts the generated index");
    int restartCount = 0;
    semantic.setEncoder(std::make_shared<verdad::RestartingSemanticEncoder>(
        std::make_shared<FailingQueryEncoder>(),
        [&]() {
            ++restartCount;
            return std::make_shared<verdad::DeterministicSemanticEncoder>(32);
        }));
    require(!semantic.search("en", "ANOTHER", "faith hope", 2).empty() &&
                restartCount == 1,
            "semantic worker failure is restarted once and the query is retried");
    int failedRestartCount = 0;
    auto disabledWorker = std::make_shared<verdad::RestartingSemanticEncoder>(
        std::make_shared<FailingQueryEncoder>(),
        [&]() {
            ++failedRestartCount;
            return std::make_shared<FailingQueryEncoder>();
        });
    semantic.setEncoder(disabledWorker);
    require(semantic.search("en", "ANOTHER", "faith hope", 2).empty() &&
                failedRestartCount == 1 && !disabledWorker->available(),
            "a twice-failed semantic worker is disabled without affecting lexical search");
    require(semantic.search("en", "ANOTHER", "faith hope", 2).empty() &&
                failedRestartCount == 1,
            "a disabled semantic worker is not repeatedly restarted");
    require(semantic.indexBytes() > 0 && semantic.indexBytes() < 25ULL * 1024ULL * 1024ULL,
            "semantic index stays separate and below the size ceiling");

    const auto packSource = dir / "semantic-pack-source";
    std::filesystem::create_directories(packSource / "bin");
    {
        std::ofstream worker(packSource / "bin" / "verdad-semantic-worker",
                             std::ios::binary);
        worker << "abc";
        std::ofstream manifest(packSource / "manifest.conf");
        manifest << "format_version=1\n"
                    "model_id=intfloat/multilingual-e5-small\n"
                    "model_revision=test-revision\n"
                    "dimensions=384\n"
                    "worker=bin/verdad-semantic-worker\n"
                    "expected_download_bytes=3\n"
                    "file=bin/verdad-semantic-worker|3|"
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    }
    verdad::SemanticPackManager packManager((dir / "pack-manager").string());
    std::string packError;
    require(packManager.installFromDirectory(packSource.string(), packError),
            "install verified semantic pack: " + packError);
    require(packManager.state() == verdad::SemanticPackState::Ready &&
                packManager.installedBytes() > 3,
            "verified semantic pack activates atomically");
    {
        std::ofstream worker(packSource / "bin" / "verdad-semantic-worker",
                             std::ios::binary | std::ios::trunc);
        worker << "abd";
    }
    packError.clear();
    require(!packManager.installFromDirectory(packSource.string(), packError) &&
                packManager.state() == verdad::SemanticPackState::Ready,
            "corrupt semantic pack is rejected without disturbing the active pack");
    {
        std::ofstream worker(std::filesystem::path(packManager.activePackDirectory()) /
                                 "bin" / "verdad-semantic-worker",
                             std::ios::binary | std::ios::trunc);
        worker << "abd";
    }
    packManager.refresh();
    require(packManager.state() == verdad::SemanticPackState::Error,
            "an installed pack is reverified and corruption is reported");
    require(packManager.remove(packError) &&
                packManager.state() == verdad::SemanticPackState::NotInstalled,
            "semantic pack removal preserves lexical availability");

    verdad::SemanticSearchService cancelledSemantic((dir / "cancelled-semantic").string());
    cancelledSemantic.setEncoder(
        std::make_shared<verdad::DeterministicSemanticEncoder>(32));
    semanticCancel.store(true);
    semanticError.clear();
    require(!cancelledSemantic.buildIndex("en", "TEST", "fixture-v1",
                                          semanticPassages, {}, &semanticCancel,
                                          semanticError) &&
                semanticError.find("cancelled") != std::string::npos,
            "semantic index build cancellation is reported without activation");

    verdad::SemanticSearchService resumedSemantic((dir / "resumed-semantic").string());
    resumedSemantic.setEncoder(std::make_shared<verdad::DeterministicSemanticEncoder>(32));
    const std::vector<verdad::SemanticPassage> resumePassages{
        {"Genesis 1:1", "light"},
        {"Genesis 1:2", "waters"},
        {"Exodus 1:1", "names"}
    };
    semanticError.clear();
    require(!resumedSemantic.buildIndex(
                "en", "TEST", "fixture-v1", resumePassages,
                [](size_t completed, size_t) { return completed < 2; },
                nullptr, semanticError),
            "semantic build can be cancelled at a book checkpoint");
    size_t resumedAt = 0;
    bool firstProgress = true;
    semanticError.clear();
    require(resumedSemantic.buildIndex(
                "en", "TEST", "fixture-v1", resumePassages,
                [&](size_t completed, size_t) {
                    if (firstProgress) {
                        resumedAt = completed;
                        firstProgress = false;
                    }
                    return true;
                }, nullptr, semanticError) && resumedAt == 2,
            "semantic index generation resumes from the last completed book: " +
                semanticError);

    verdad::TopicSearchProvider topics((dir / "topic_index.db").string());
    std::vector<verdad::TopicDocument> topicDocuments;
    topicDocuments.push_back(verdad::TopicSearchProvider::documentFromSwordEntry(
        "CARE", "Trust and care", R"(<ref osisRef="Phil.4.6" />)"));
    topicDocuments.push_back(verdad::TopicSearchProvider::documentFromSwordEntry(
        "ANXIETY", "See CARE", R"(<ref target="Nave:CARE">CARE</ref>)"));
    topicDocuments.push_back(verdad::TopicSearchProvider::documentFromSwordEntry(
        "FORGIVENESS", "Forgiving enemies",
        R"(<ref osisRef="Matt.5.44" /><ref osisRef="Luke.23.34" />)"));
    topicDocuments.push_back(verdad::TopicSearchProvider::documentFromSwordEntry(
        " Forgiveness ", "Duplicate normalized title",
        R"(<ref osisRef="Mark.11.25" />)"));
    std::string topicError;
    require(topics.rebuild("nave-fixture-v1", topicDocuments, topicError),
            "build topic index: " + topicError);
    require(topics.readyForSignature("nave-fixture-v1"),
            "topic index signature is recorded");
    const auto anxietyHits = topics.search("anxiety");
    require(!anxietyHits.empty() && anxietyHits.front().reference == "Philippians 4:6" &&
                !anxietyHits.front().exactTopic,
            "Nave redirect resolves to the target topic without an exact-topic boost");
    const auto forgivenessHits = topics.search("forgiveness");
    require(forgivenessHits.size() == 3 && forgivenessHits.front().exactTopic,
            "exact Nave topic merges duplicate normalized titles and references");
    require(topics.databaseBytes() > 0 && topics.databaseBytes() < 15ULL * 1024ULL * 1024ULL,
            "topic index stays separate and below the size ceiling");

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
    expect(indexer.searchSmart(request, "\"holy spirit\""), {"L"},
           "quoted phrase is not broadened by synonyms or prefixes");
    expect(indexer.searchSmart(request, "faith -faithful"), {"J"},
           "unary negation excludes matching results");
    expect(indexer.searchSmart(request, "fig"), {"O"}, "fig is not olive");
    expect(indexer.searchSmart(request, "armor"), {"R", "S"}, "armor is not sword");
    expect(indexer.searchSmart(request, "faiht"), {"J"}, "unknown transposed word");
    expect(indexer.searchSmart(request, "what does the Bible say about faith?"),
           {"J", "K"}, "natural-language wrapper terms are soft");

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

    sqlite3* fixtureDb = nullptr;
    require(sqlite3_open((dir / "search.db").string().c_str(), &fixtureDb) == SQLITE_OK &&
                fixtureDb,
            "open fixture database for Bible-only fallback setup");
    require(sqlite3_exec(fixtureDb,
                         "UPDATE library_entries SET resource_type='bible'",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "mark fixture rows as Bible candidates");
    sqlite3_close(fixtureDb);

    verdad::SemanticSearchService fallbackSemantic((dir / "fallback-semantic").string());
    fallbackSemantic.setEncoder(
        std::make_shared<verdad::DeterministicSemanticEncoder>(32));
    fallbackSemantic.setEnabled(true);
    semanticError.clear();
    require(fallbackSemantic.buildIndex(
                "en", module, "fallback-v1", {{"Genesis 1:1", "faith"}},
                {}, nullptr, semanticError),
            "prepare semantic worker-failure fallback fixture: " + semanticError);
    fallbackSemantic.setEncoder(std::make_shared<verdad::RestartingSemanticEncoder>(
        std::make_shared<FailingQueryEncoder>(),
        []() { return std::make_shared<FailingQueryEncoder>(); }));
    indexer.setSemanticSearchService(&fallbackSemantic);
    auto bibleRequest = request;
    bibleRequest.resourceTypes = {"bible"};
    expect(indexer.searchSmart(bibleRequest, "faith", "en", 0, false), {"J", "K"},
           "semantic worker failure falls back to enhanced lexical results");
    indexer.setSemanticSearchService(nullptr);

    using verdad::smart_search::RetrievalSource;
    std::vector<verdad::smart_search::WeightedRankList> rankedLists{
        {RetrievalSource::OriginalTerms, 2.5, {"exact", "shared"}},
        {RetrievalSource::AutomaticPrefix, 0.6, {"prefix", "shared"}}
    };
    const auto fused = verdad::smart_search::fuseRankedResults(rankedLists);
    require(fused.size() == 3 && fused[0] == "shared" && fused[1] == "exact" &&
                fused[2] == "prefix",
            "weighted RRF must favor corroboration and original terms");
    const std::vector<verdad::smart_search::WeightedRankList> tiedLists{
        {RetrievalSource::OriginalTerms, 1.0, {"lexical"}, 1},
        {RetrievalSource::Semantic, 1.0, {"semantic"}, 0}
    };
    const auto tied = verdad::smart_search::fuseRankedResults(tiedLists);
    require(tied.size() == 2 && tied.front() == "lexical",
            "equal RRF scores prefer original-term coverage before semantic rank");
    const std::vector<verdad::smart_search::WeightedRankList> canonicalLists{
        {RetrievalSource::NaveExpandedTopic, 1.0, {"later"}},
        {RetrievalSource::NaveExpandedTopic, 1.0, {"earlier"}}
    };
    const auto canonical = verdad::smart_search::fuseRankedResults(
        canonicalLists, 0, 60.0, {{"later", 10}, {"earlier", 2}});
    require(canonical.size() == 2 && canonical.front() == "earlier",
            "equal non-lexical RRF scores use canonical Scripture order");

    const auto queryPlan = verdad::smart_search::buildSmartQueryPlan(
        "what does the Bible say about faith?", "en");
    require(queryPlan.coreTerms.size() == 1 && queryPlan.coreTerms.front() == "faith",
            "Smart query planner must isolate the core concept");
    const auto protectedPlan = verdad::smart_search::buildSmartQueryPlan(
        "\"holy spirit\" -evil", "en");
    require(protectedPlan.quotedTerms == std::vector<std::string>{"holy spirit"} &&
                protectedPlan.excludedTerms == std::vector<std::string>{"evil"},
            "Smart query planner preserves quoted phrases and exclusions");
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
