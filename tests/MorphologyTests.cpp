#include "translation/SqliteMorphologyProvider.h"
#include "translation/TranslationNormalization.h"
#include "translation/WikDictManager.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void execute(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "SQLite execution failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void createMorphologyDatabase(const fs::path& path) {
    fs::create_directories(path.parent_path());
    sqlite3* db = nullptr;
    require(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK,
            "Cannot create morphology test database");
    try {
        execute(db, R"SQL(
            PRAGMA user_version = 2;
            CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE morph_lemmas(
                id INTEGER PRIMARY KEY,
                source_lang TEXT NOT NULL,
                lemma TEXT NOT NULL,
                lemma_norm TEXT NOT NULL,
                pos TEXT NOT NULL,
                provider TEXT NOT NULL
            );
            CREATE TABLE morph_features(
                id INTEGER PRIMARY KEY,
                features TEXT NOT NULL
            );
            CREATE TABLE morph_forms(
                source_lang TEXT NOT NULL,
                surface_norm TEXT NOT NULL,
                lemma_id INTEGER NOT NULL,
                features_id INTEGER NOT NULL,
                confidence INTEGER NOT NULL,
                PRIMARY KEY(source_lang, surface_norm, lemma_id, features_id)
            ) WITHOUT ROWID;
            INSERT INTO morph_lemmas VALUES
                (1, 'es', 'hablar', 'hablar', 'verb', 'apertium-spa'),
                (2, 'es', 'mujer', 'mujer', 'noun', 'apertium-spa');
            INSERT INTO morph_features VALUES
                (1, 'preterite, 3rd person singular'),
                (2, 'preterite, 3rd person plural'),
                (3, 'feminine, plural');
            INSERT INTO morph_forms(
                source_lang, surface_norm, lemma_id, features_id, confidence
            ) VALUES
                ('es', 'habló', 1, 1, 100),
                ('es', 'hablaron', 1, 2, 100),
                ('es', 'mujeres', 2, 3, 100);
        )SQL");
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
}

void createTranslationDatabase(const fs::path& path) {
    fs::create_directories(path.parent_path());
    sqlite3* db = nullptr;
    require(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK,
            "Cannot create translation test database");
    try {
        execute(db, R"SQL(
            CREATE TABLE simple_translation(
                written_rep TEXT,
                trans_list TEXT,
                max_score REAL,
                rel_importance REAL
            );
            INSERT INTO simple_translation VALUES
                ('hablar', 'speak | talk', 1, 1),
                ('hablaron', 'they spoke directly', 1, 1),
                ('mujer', 'woman', 1, 1),
                ('señor', 'lord', 1, 1);
        )SQL");
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
}

void testProvider(const fs::path& morphPath) {
    verdad::SqliteMorphologyProvider provider(morphPath);
    require(provider.isOpen(), "Morphology provider did not open: " +
                                   provider.error());
    require(provider.supportsLanguage("es-MX"),
            "Morphology provider did not normalize language code");

    auto hablo = provider.analyze("es", "habló");
    require(hablo.size() == 1 && hablo[0].lemma == "hablar",
            "habló did not resolve to hablar");
    require(hablo[0].pos == "verb", "habló POS was not preserved");

    auto hablaron = provider.analyze("es", "Hablaron");
    require(hablaron.size() == 1 && hablaron[0].lemma == "hablar",
            "capitalized Hablaron did not normalize");

    auto mujeres = provider.analyze("es", "¿mujeres?");
    require(mujeres.size() == 1 && mujeres[0].lemma == "mujer",
            "punctuated mujeres did not normalize");
    require(provider.analyze("es", "unknownword").empty(),
            "unknown morphology token returned an analysis");
}

void testEndToEnd(const fs::path& root) {
    verdad::WikDictManager manager;
    verdad::WikDictScanReport report = manager.scan(root.string());
    std::string issues;
    for (const auto& issue : report.issues) {
        if (!issues.empty()) issues += "; ";
        issues += issue.fileName + ": " + issue.message;
    }
    require(report.issues.empty(),
            "Test dictionaries produced scan issues: " + issues);
    require(report.pairs.size() == 1, "WikDict pair was not discovered");
    require(report.morphologyLanguages.size() == 1 &&
                report.morphologyLanguages[0] == "es",
            "Spanish morphology pack was not discovered");

    auto hablo = manager.lookup("es", "en", "habló");
    require(hablo.has_value(), "habló end-to-end lookup failed");
    require(hablo->morphologyDerived, "habló was not marked morphology-derived");
    require(!hablo->lemmas.empty() && hablo->lemmas[0] == "hablar",
            "habló result did not expose hablar");
    require(!hablo->glosses.empty() && hablo->glosses[0] == "speak",
            "habló did not use hablar's WikDict gloss");

    auto decomposed = manager.lookup("es", "en", "hablo\xcc\x81");
    require(decomposed.has_value() && decomposed->lemmas[0] == "hablar",
            "decomposed habló did not normalize to NFC");

    auto mujeres = manager.lookup("es", "en", "mujeres");
    require(mujeres.has_value() && mujeres->glosses[0] == "woman",
            "mujeres did not resolve through mujer");

    auto direct = manager.lookup("es", "en", "hablaron");
    require(direct.has_value() && !direct->morphologyDerived,
            "direct translation did not win over morphology");
    require(direct->glosses[0] == "they spoke directly",
            "direct translation returned the wrong gloss");

    auto normalizedDirect = manager.lookup("es", "en", "¿Señor?");
    require(normalizedDirect.has_value() &&
                normalizedDirect->glosses[0] == "lord",
            "normalized direct translation failed");
    require(!manager.lookup("es", "en", "unknownword").has_value(),
            "unknown word returned a translation");
}

void testMissingMorphology(const fs::path& root) {
    fs::remove(root / "morphology" / "es-morph-apertium.sqlite3");
    verdad::WikDictManager manager;
    verdad::WikDictScanReport report = manager.scan(root.string());
    require(report.morphologyLanguages.empty(),
            "missing morphology pack was reported as available");
    require(manager.lookup("es", "en", "señor").has_value(),
            "exact WikDict lookup failed without morphology");
    require(!manager.lookup("es", "en", "habló").has_value(),
            "inflected form resolved without a morphology pack");
}

} // namespace

int main() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("verdad-morphology-tests-" +
                           std::to_string(suffix));
    try {
        const fs::path morphPath =
            root / "morphology" / "es-morph-apertium.sqlite3";
        createMorphologyDatabase(morphPath);
        createTranslationDatabase(root / "wikdict" / "es-en.sqlite3");
        testProvider(morphPath);
        testEndToEnd(root);
        testMissingMorphology(root);
        require(verdad::normalizeLookupToken("¿Sen\xcc\x83or?") == "señor",
                "shared normalization did not compose decomposed ñ");
    } catch (const std::exception& error) {
        std::cerr << "Morphology test failure: " << error.what() << '\n';
        std::error_code ec;
        fs::remove_all(root, ec);
        return 1;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    std::cout << "Morphology tests passed\n";
    return 0;
}
