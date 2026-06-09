#include "translation/SqliteMorphologyProvider.h"

#include "translation/TranslationNormalization.h"

#include <sqlite3.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace verdad {
namespace {

std::string sqliteMessage(sqlite3* db, const std::string& fallback) {
    if (!db) return fallback;
    const char* message = sqlite3_errmsg(db);
    return message && *message ? message : fallback;
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

std::string columnText(sqlite3_stmt* statement, int column) {
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

} // namespace

struct SqliteMorphologyProvider::Impl {
    sqlite3* db = nullptr;
    sqlite3_stmt* lookupStatement = nullptr;
    std::set<std::string> languages;
    mutable std::unordered_map<std::string, std::vector<MorphAnalysis>> cache;
    std::string error;
    int schemaVersion = 0;

    ~Impl() {
        if (lookupStatement) sqlite3_finalize(lookupStatement);
        if (db) sqlite3_close(db);
    }
};

SqliteMorphologyProvider::SqliteMorphologyProvider(
        std::filesystem::path dbPath)
    : impl_(std::make_unique<Impl>()) {
    int openResult = sqlite3_open_v2(dbPath.string().c_str(),
                                     &impl_->db,
                                     SQLITE_OPEN_READONLY,
                                     nullptr);
    if (openResult != SQLITE_OK) {
        impl_->error = sqliteMessage(impl_->db, "SQLite open failed");
        return;
    }

    sqlite3_enable_load_extension(impl_->db, 0);
    char* pragmaError = nullptr;
    if (sqlite3_exec(impl_->db, "PRAGMA query_only=ON", nullptr, nullptr,
                     &pragmaError) != SQLITE_OK) {
        impl_->error = pragmaError ? pragmaError : "query_only failed";
        sqlite3_free(pragmaError);
        return;
    }

    sqlite3_stmt* versionStatement = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "PRAGMA user_version", -1,
                           &versionStatement, nullptr) == SQLITE_OK &&
        sqlite3_step(versionStatement) == SQLITE_ROW) {
        impl_->schemaVersion = sqlite3_column_int(versionStatement, 0);
    }
    sqlite3_finalize(versionStatement);
    if (impl_->schemaVersion != 1 && impl_->schemaVersion != 2) {
        impl_->error = "Unsupported morphology schema version: " +
                       std::to_string(impl_->schemaVersion);
        return;
    }

    static const std::set<std::string> metadataColumns = {"key", "value"};
    static const std::set<std::string> versionOneColumns = {
        "source_lang", "surface", "surface_norm", "lemma", "lemma_norm",
        "pos", "features", "provider", "confidence"};
    static const std::set<std::string> versionTwoFormColumns = {
        "source_lang", "surface_norm", "lemma_id", "features_id", "confidence"};
    static const std::set<std::string> versionTwoLemmaColumns = {
        "id", "source_lang", "lemma", "lemma_norm", "pos", "provider"};
    static const std::set<std::string> versionTwoFeatureColumns = {
        "id", "features"};
    const bool validVersionOne =
        impl_->schemaVersion == 1 &&
        tableHasColumns(impl_->db, "morph_forms", versionOneColumns);
    const bool validVersionTwo =
        impl_->schemaVersion == 2 &&
        tableHasColumns(impl_->db, "morph_forms", versionTwoFormColumns) &&
        tableHasColumns(impl_->db, "morph_lemmas", versionTwoLemmaColumns) &&
        tableHasColumns(impl_->db, "morph_features", versionTwoFeatureColumns);
    if (!tableHasColumns(impl_->db, "metadata", metadataColumns) ||
        (!validVersionOne && !validVersionTwo)) {
        impl_->error = "Unsupported morphology database schema";
        return;
    }

    sqlite3_stmt* languageStatement = nullptr;
    if (sqlite3_prepare_v2(
            impl_->db,
            "SELECT DISTINCT source_lang FROM morph_forms ORDER BY source_lang",
            -1, &languageStatement, nullptr) != SQLITE_OK) {
        impl_->error = sqliteMessage(impl_->db,
                                     "Cannot inspect morphology languages");
        return;
    }
    while (sqlite3_step(languageStatement) == SQLITE_ROW) {
        std::string language = normalizeLanguageCode(
            columnText(languageStatement, 0));
        if (!language.empty()) impl_->languages.insert(std::move(language));
    }
    sqlite3_finalize(languageStatement);

    const char* lookupSql = impl_->schemaVersion == 1
        ? "SELECT surface, lemma, pos, features, provider, confidence "
          "FROM morph_forms "
          "WHERE source_lang = ?1 AND surface_norm = ?2 "
          "ORDER BY confidence DESC, "
          "CASE WHEN pos IS NULL OR pos = '' THEN 1 ELSE 0 END, lemma_norm "
          "LIMIT 12"
        : "SELECT forms.surface_norm, lemmas.lemma, lemmas.pos, "
          "features.features, lemmas.provider, forms.confidence "
          "FROM morph_forms AS forms "
          "JOIN morph_lemmas AS lemmas ON lemmas.id = forms.lemma_id "
          "JOIN morph_features AS features ON features.id = forms.features_id "
          "WHERE forms.source_lang = ?1 AND forms.surface_norm = ?2 "
          "ORDER BY forms.confidence DESC, "
          "CASE WHEN lemmas.pos = '' THEN 1 ELSE 0 END, lemmas.lemma_norm "
          "LIMIT 12";
    if (sqlite3_prepare_v2(impl_->db, lookupSql, -1,
                           &impl_->lookupStatement, nullptr) != SQLITE_OK) {
        impl_->error = sqliteMessage(impl_->db,
                                     "Cannot prepare morphology lookup");
    }
}

SqliteMorphologyProvider::~SqliteMorphologyProvider() = default;
SqliteMorphologyProvider::SqliteMorphologyProvider(
    SqliteMorphologyProvider&&) noexcept = default;
SqliteMorphologyProvider& SqliteMorphologyProvider::operator=(
    SqliteMorphologyProvider&&) noexcept = default;

bool SqliteMorphologyProvider::isOpen() const {
    return impl_ && impl_->lookupStatement && impl_->error.empty();
}

const std::string& SqliteMorphologyProvider::error() const {
    return impl_->error;
}

bool SqliteMorphologyProvider::supportsLanguage(
        const std::string& language) const {
    if (!isOpen()) return false;
    return impl_->languages.find(normalizeLanguageCode(language)) !=
           impl_->languages.end();
}

std::vector<MorphAnalysis> SqliteMorphologyProvider::analyze(
        const std::string& language,
        const std::string& token) const {
    const std::string normalizedLanguage = normalizeLanguageCode(language);
    const std::string normalizedToken = normalizeLookupToken(token);
    if (!supportsLanguage(normalizedLanguage) || normalizedToken.empty() ||
        normalizedToken.size() > 256) {
        return {};
    }

    const std::string cacheKey = normalizedLanguage + "\n" + normalizedToken;
    auto cached = impl_->cache.find(cacheKey);
    if (cached != impl_->cache.end()) return cached->second;

    std::vector<MorphAnalysis> analyses;
    sqlite3_stmt* statement = impl_->lookupStatement;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (sqlite3_bind_text(statement, 1, normalizedLanguage.data(),
                          static_cast<int>(normalizedLanguage.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 2, normalizedToken.data(),
                          static_cast<int>(normalizedToken.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_reset(statement);
        return {};
    }

    int stepResult = SQLITE_ROW;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
        MorphAnalysis analysis;
        analysis.surface = columnText(statement, 0);
        analysis.lemma = columnText(statement, 1);
        analysis.pos = columnText(statement, 2);
        analysis.features = columnText(statement, 3);
        analysis.provider = columnText(statement, 4);
        analysis.confidence = sqlite3_column_int(statement, 5);
        if (!analysis.lemma.empty()) analyses.push_back(std::move(analysis));
    }
    sqlite3_reset(statement);
    if (stepResult != SQLITE_DONE) return {};

    impl_->cache.emplace(cacheKey, analyses);
    return analyses;
}

} // namespace verdad
