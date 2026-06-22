#include "translation/WikDictManager.h"

#include "translation/MorphologyProvider.h"
#include "translation/SqliteMorphologyProvider.h"
#include "translation/TranslationNormalization.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace verdad {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxWordBytes = 256;
constexpr size_t kMaxGlossBytes = 512;
constexpr size_t kMaxAttributionBytes = 256;
constexpr size_t kMaxGlosses = 5;
constexpr size_t kMaxMorphologyAnalyses = 3;

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

std::string truncateUtf8(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;

    size_t end = maxBytes;
    while (end > 0 &&
           (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

void appendUnique(std::vector<std::string>& values, const std::string& value) {
    std::string cleaned = trimCopy(value);
    if (cleaned.empty()) return;
    if (std::find(values.begin(), values.end(), cleaned) == values.end()) {
        values.push_back(std::move(cleaned));
    }
}

std::vector<std::string> splitGlosses(const std::string& translations) {
    static const std::string delimiter = " | ";

    std::vector<std::string> glosses;
    std::set<std::string> seen;
    size_t start = 0;
    while (start <= translations.size() && glosses.size() < kMaxGlosses) {
        size_t end = translations.find(delimiter, start);
        std::string gloss = trimCopy(translations.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start));
        gloss = truncateUtf8(gloss, kMaxGlossBytes);
        if (!gloss.empty() && seen.insert(gloss).second) {
            glosses.push_back(std::move(gloss));
        }

        if (end == std::string::npos) break;
        start = end + delimiter.size();
    }
    return glosses;
}

void appendMorphologyVariants(std::vector<std::string>& variants,
                              const std::string& features) {
    static const std::string delimiter = " | ";
    size_t start = 0;
    while (start <= features.size()) {
        size_t end = features.find(delimiter, start);
        appendUnique(
            variants,
            features.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + delimiter.size();
    }
}

std::string sqliteMessage(sqlite3* db, const std::string& fallback) {
    if (!db) return fallback;
    const char* message = sqlite3_errmsg(db);
    return message && *message ? message : fallback;
}

bool tableExists(sqlite3* db, const char* tableName) {
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(statement, 1, tableName, -1, SQLITE_STATIC);
    bool exists = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return exists;
}

bool tableHasColumns(sqlite3* db,
                     const char* tableName,
                     const std::set<std::string>& required) {
    sqlite3_stmt* statement = nullptr;
    std::string sql = "PRAGMA table_info(" + std::string(tableName) + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    std::set<std::string> columns;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(statement, 1);
        if (name) columns.insert(reinterpret_cast<const char*>(name));
    }
    sqlite3_finalize(statement);

    return std::includes(columns.begin(), columns.end(),
                         required.begin(), required.end());
}

bool hasTranslationSchema(sqlite3* db) {
    static const std::set<std::string> required = {
        "written_rep", "trans_list", "max_score", "rel_importance"};
    return tableExists(db, "simple_translation") &&
           tableHasColumns(db, "simple_translation", required);
}

std::string pairKey(const std::string& sourceLanguage,
                    const std::string& targetLanguage) {
    return sourceLanguage + "-" + targetLanguage;
}

void collectTranslationCandidates(const fs::path& directory,
                                  std::vector<fs::path>& candidates,
                                  WikDictScanReport& report) {
    std::error_code ec;
    bool exists = fs::exists(directory, ec);
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Cannot access dictionary folder: " +
                                     ec.message()});
        return;
    }
    if (!exists) return;

    bool isDirectory = fs::is_directory(directory, ec);
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Cannot inspect dictionary folder: " +
                                     ec.message()});
        return;
    }
    if (!isDirectory) return;

    static const std::regex filePattern(
        R"(^([A-Za-z]{2,3})-([A-Za-z]{2,3})\.sqlite3$)");
    fs::directory_iterator iterator(directory, ec);
    fs::directory_iterator end;
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Cannot read dictionary folder: " +
                                     ec.message()});
        return;
    }

    for (; iterator != end; iterator.increment(ec)) {
        if (ec) break;
        bool isRegularFile = iterator->is_regular_file(ec);
        if (ec) {
            report.issues.push_back({iterator->path().filename().string(),
                                     "Cannot inspect file: " + ec.message()});
            ec.clear();
            continue;
        }
        if (!isRegularFile) continue;

        if (std::regex_match(iterator->path().filename().string(), filePattern)) {
            candidates.push_back(iterator->path());
        }
    }
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Dictionary folder scan stopped: " +
                                     ec.message()});
    }
}

void collectMorphologyCandidates(const fs::path& directory,
                                 std::vector<fs::path>& candidates,
                                 WikDictScanReport& report) {
    std::error_code ec;
    bool exists = fs::exists(directory, ec);
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Cannot access morphology folder: " +
                                     ec.message()});
        return;
    }
    if (!exists) return;

    bool isDirectory = fs::is_directory(directory, ec);
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Cannot inspect morphology folder: " +
                                     ec.message()});
        return;
    }
    if (!isDirectory) return;

    static const std::regex filePattern(
        R"(^([A-Za-z]{2,3})-morph-([A-Za-z0-9][A-Za-z0-9._-]*)\.sqlite3$)");
    fs::directory_iterator iterator(directory, ec);
    fs::directory_iterator end;
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Cannot read morphology folder: " +
                                     ec.message()});
        return;
    }

    for (; iterator != end; iterator.increment(ec)) {
        if (ec) break;
        bool isRegularFile = iterator->is_regular_file(ec);
        if (ec) {
            report.issues.push_back({iterator->path().filename().string(),
                                     "Cannot inspect file: " + ec.message()});
            ec.clear();
            continue;
        }
        if (!isRegularFile) continue;
        if (std::regex_match(iterator->path().filename().string(), filePattern)) {
            candidates.push_back(iterator->path());
        }
    }
    if (ec) {
        report.issues.push_back({directory.filename().string(),
                                 "Morphology folder scan stopped: " +
                                     ec.message()});
    }
}

} // namespace

struct WikDictManager::Impl {
    struct Connection {
        sqlite3* db = nullptr;
        sqlite3_stmt* lookupStatement = nullptr;

        Connection() = default;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        ~Connection() {
            if (lookupStatement) sqlite3_finalize(lookupStatement);
            if (db) sqlite3_close(db);
        }
    };

    std::map<std::string, std::unique_ptr<Connection>> connections;
    std::unordered_map<std::string, std::vector<std::string>> lookupCache;
    MorphologyService morphology;

    std::vector<std::string> queryCandidate(const std::string& pair,
                                            const std::string& spelling) {
        const std::string cacheKey = pair + "\n" + spelling;
        auto cached = lookupCache.find(cacheKey);
        if (cached != lookupCache.end()) return cached->second;

        std::vector<std::string> glosses;
        auto connectionIt = connections.find(pair);
        if (connectionIt == connections.end() ||
            !connectionIt->second ||
            !connectionIt->second->lookupStatement) {
            lookupCache.emplace(cacheKey, glosses);
            return glosses;
        }

        sqlite3_stmt* statement = connectionIt->second->lookupStatement;
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        if (sqlite3_bind_text(statement, 1, spelling.data(),
                              static_cast<int>(spelling.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            sqlite3_reset(statement);
            return glosses;
        }

        std::set<std::string> seen;
        int stepResult = SQLITE_ROW;
        while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW &&
               glosses.size() < kMaxGlosses) {
            const unsigned char* value = sqlite3_column_text(statement, 0);
            if (!value) continue;

            for (std::string gloss : splitGlosses(
                     reinterpret_cast<const char*>(value))) {
                if (seen.insert(gloss).second) {
                    glosses.push_back(std::move(gloss));
                    if (glosses.size() >= kMaxGlosses) break;
                }
            }
        }
        sqlite3_reset(statement);

        if (stepResult != SQLITE_DONE && stepResult != SQLITE_ROW) return {};
        lookupCache.emplace(cacheKey, glosses);
        return glosses;
    }

    std::vector<std::string> queryWithNormalization(
            const std::string& pair,
            const std::string& spelling) {
        std::vector<std::string> glosses = queryCandidate(pair, spelling);
        if (!glosses.empty()) return glosses;

        const std::string normalized = normalizeLookupToken(spelling);
        if (normalized.empty() || normalized == spelling) return {};
        return queryCandidate(pair, normalized);
    }
};

WikDictManager::WikDictManager()
    : impl_(std::make_unique<Impl>()) {}

WikDictManager::~WikDictManager() = default;
WikDictManager::WikDictManager(WikDictManager&&) noexcept = default;
WikDictManager& WikDictManager::operator=(WikDictManager&&) noexcept = default;

WikDictScanReport WikDictManager::scan(const std::string& directory) {
    impl_->connections.clear();
    impl_->lookupCache.clear();
    impl_->morphology.clear();

    WikDictScanReport report;
    const std::string configuredDirectory = trimCopy(directory);
    if (configuredDirectory.empty()) return report;

    std::error_code ec;
    fs::path directoryPath(configuredDirectory);
    bool exists = fs::exists(directoryPath, ec);
    if (ec) {
        report.issues.push_back({directoryPath.filename().string(),
                                 "Cannot access dictionary folder: " +
                                     ec.message()});
        return report;
    }
    if (!exists) return report;

    bool isDirectory = fs::is_directory(directoryPath, ec);
    if (ec) {
        report.issues.push_back({directoryPath.filename().string(),
                                 "Cannot inspect dictionary folder: " +
                                     ec.message()});
        return report;
    }
    if (!isDirectory) {
        report.issues.push_back({directoryPath.filename().string(),
                                 "Dictionary path is not a folder"});
        return report;
    }

    std::vector<fs::path> candidates;
    collectTranslationCandidates(directoryPath, candidates, report);
    collectTranslationCandidates(directoryPath / "wikdict", candidates, report);
    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.string() < right.string();
              });

    static const std::regex filePattern(
        R"(^([A-Za-z]{2,3})-([A-Za-z]{2,3})\.sqlite3$)");
    for (const fs::path& path : candidates) {
        std::smatch match;
        const std::string fileName = path.filename().string();
        if (!std::regex_match(fileName, match, filePattern)) continue;

        const std::string sourceLanguage = normalizeLanguageCode(match[1].str());
        const std::string targetLanguage = normalizeLanguageCode(match[2].str());
        if (targetLanguage != "en") {
            report.issues.push_back({fileName,
                                     "Unsupported target language: " +
                                         targetLanguage});
            continue;
        }

        const std::string key = pairKey(sourceLanguage, targetLanguage);
        if (impl_->connections.find(key) != impl_->connections.end()) {
            report.issues.push_back({fileName,
                                     "Duplicate normalized language pair: " + key});
            continue;
        }

        auto connection = std::make_unique<Impl::Connection>();
        int openResult = sqlite3_open_v2(path.string().c_str(),
                                         &connection->db,
                                         SQLITE_OPEN_READONLY,
                                         nullptr);
        if (openResult != SQLITE_OK) {
            report.issues.push_back({fileName,
                                     "Cannot open database: " +
                                         sqliteMessage(connection->db,
                                                       "SQLite open failed")});
            continue;
        }

        sqlite3_enable_load_extension(connection->db, 0);
        char* pragmaError = nullptr;
        if (sqlite3_exec(connection->db, "PRAGMA query_only=ON", nullptr,
                         nullptr, &pragmaError) != SQLITE_OK) {
            std::string message = pragmaError ? pragmaError : "query_only failed";
            sqlite3_free(pragmaError);
            report.issues.push_back({fileName,
                                     "Cannot enable read-only query mode: " +
                                         message});
            continue;
        }

        if (!hasTranslationSchema(connection->db)) {
            report.issues.push_back({fileName,
                                     "Unsupported simple_translation schema"});
            continue;
        }

        const char* lookupSql =
            "SELECT trans_list "
            "FROM simple_translation "
            "WHERE written_rep = ?1 "
            "ORDER BY rel_importance DESC, max_score DESC";
        if (sqlite3_prepare_v2(connection->db, lookupSql, -1,
                               &connection->lookupStatement,
                               nullptr) != SQLITE_OK) {
            report.issues.push_back({fileName,
                                     "Cannot prepare lookup: " +
                                         sqliteMessage(connection->db,
                                                       "SQLite prepare failed")});
            continue;
        }

        impl_->connections.emplace(key, std::move(connection));
        report.pairs.push_back({sourceLanguage, targetLanguage});
    }

    std::vector<fs::path> morphologyCandidates;
    collectMorphologyCandidates(directoryPath, morphologyCandidates, report);
    collectMorphologyCandidates(
        directoryPath / "morphology", morphologyCandidates, report);
    std::sort(morphologyCandidates.begin(), morphologyCandidates.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.string() < right.string();
              });

    static const std::regex morphologyPattern(
        R"(^([A-Za-z]{2,3})-morph-([A-Za-z0-9][A-Za-z0-9._-]*)\.sqlite3$)");
    std::set<std::string> loadedMorphologyPacks;
    std::set<std::string> reportedMorphologyLanguages;
    for (const fs::path& path : morphologyCandidates) {
        std::smatch match;
        const std::string fileName = path.filename().string();
        if (!std::regex_match(fileName, match, morphologyPattern)) continue;
        const std::string language = normalizeLanguageCode(match[1].str());
        const std::string providerName = match[2].str();
        const std::string packKey = language + "\n" + providerName;
        if (loadedMorphologyPacks.find(packKey) != loadedMorphologyPacks.end()) {
            report.issues.push_back({path.filename().string(),
                                     "Duplicate morphology pack for " +
                                         language + ": " + providerName});
            continue;
        }

        auto provider = std::make_unique<SqliteMorphologyProvider>(path);
        if (!provider->isOpen()) {
            report.issues.push_back({path.filename().string(),
                                     "Cannot load morphology database: " +
                                         provider->error()});
            continue;
        }
        if (!provider->supportsLanguage(language)) {
            report.issues.push_back({path.filename().string(),
                                     "Morphology database has no " +
                                         language + " forms"});
            continue;
        }

        impl_->morphology.addProvider(std::move(provider));
        loadedMorphologyPacks.insert(packKey);
        if (reportedMorphologyLanguages.insert(language).second) {
            report.morphologyLanguages.push_back(language);
        }
    }

    return report;
}

std::vector<TranslationLanguagePair> WikDictManager::availablePairs() const {
    std::vector<TranslationLanguagePair> pairs;
    pairs.reserve(impl_->connections.size());
    for (const auto& entry : impl_->connections) {
        size_t separator = entry.first.find('-');
        if (separator == std::string::npos) continue;
        pairs.push_back({entry.first.substr(0, separator),
                         entry.first.substr(separator + 1)});
    }
    return pairs;
}

bool WikDictManager::hasPair(const std::string& sourceLanguage,
                             const std::string& targetLanguage) const {
    const std::string source = normalizeLanguageCode(sourceLanguage);
    const std::string target = normalizeLanguageCode(targetLanguage);
    if (source.empty() || target.empty()) return false;
    return impl_->connections.find(pairKey(source, target)) !=
           impl_->connections.end();
}

std::optional<OfflineTranslationResult> WikDictManager::lookup(
        const std::string& sourceLanguage,
        const std::string& targetLanguage,
        const std::string& word) {
    const std::string source = normalizeLanguageCode(sourceLanguage);
    const std::string target = normalizeLanguageCode(targetLanguage);
    if (source.empty() || source == "en" || target != "en" ||
        word.empty() || word.size() > kMaxWordBytes ||
        std::any_of(word.begin(), word.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        })) {
        return std::nullopt;
    }

    const std::string key = pairKey(source, target);
    if (impl_->connections.find(key) == impl_->connections.end()) {
        return std::nullopt;
    }

    OfflineTranslationResult result;
    result.sourceWord = word;
    result.sourceLanguage = source;
    result.targetLanguage = target;
    result.glosses = impl_->queryWithNormalization(key, word);
    if (!result.glosses.empty()) {
        result.attribution = truncateUtf8(
            "Source: WikDict " + source + " -> " + target +
                " - CC BY-SA - Wiktionary via DBnary",
            kMaxAttributionBytes);
        return result;
    }

    if (!impl_->morphology.supportsLanguage(source)) return std::nullopt;

    std::map<std::string, size_t> acceptedGroups;
    for (const MorphAnalysis& analysis :
         impl_->morphology.analyze(source, word)) {
        const std::string lemmaNorm = normalizeLookupToken(analysis.lemma);
        if (lemmaNorm.empty()) continue;

        std::vector<std::string> lemmaGlosses =
            impl_->queryWithNormalization(key, analysis.lemma);
        if (lemmaGlosses.empty()) continue;

        const std::string groupKey = lemmaNorm + "\n" + analysis.pos;
        auto groupIt = acceptedGroups.find(groupKey);
        if (groupIt == acceptedGroups.end()) {
            if (result.morphologyAnalyses.size() >= kMaxMorphologyAnalyses) {
                continue;
            }
            OfflineMorphologyAnalysis grouped;
            grouped.lemma = analysis.lemma;
            grouped.partOfSpeech = analysis.pos;
            result.morphologyAnalyses.push_back(std::move(grouped));
            size_t index = result.morphologyAnalyses.size() - 1;
            groupIt = acceptedGroups.emplace(groupKey, index).first;
        }

        OfflineMorphologyAnalysis& grouped =
            result.morphologyAnalyses[groupIt->second];
        appendMorphologyVariants(
            grouped.grammaticalVariants, analysis.features);
        appendUnique(grouped.providers, analysis.provider);

        for (const auto& gloss : lemmaGlosses) {
            appendUnique(grouped.glosses, gloss);
            if (result.glosses.size() < kMaxGlosses) {
                appendUnique(result.glosses, gloss);
            }
            if (grouped.glosses.size() >= kMaxGlosses) break;
        }
        appendUnique(result.morphologyProviders, analysis.provider);
    }

    if (result.glosses.empty()) return std::nullopt;

    result.morphologyDerived = true;
    std::ostringstream attribution;
    attribution << "Source: WikDict " << source << " -> " << target
                << " - CC BY-SA - Wiktionary via DBnary";
    if (!result.morphologyProviders.empty()) {
        attribution << " + ";
        for (size_t i = 0; i < result.morphologyProviders.size(); ++i) {
            if (i) attribution << ", ";
            attribution << result.morphologyProviders[i];
        }
        attribution << " morphology";
    }
    result.attribution = truncateUtf8(attribution.str(), kMaxAttributionBytes);
    return result;
}

} // namespace verdad
