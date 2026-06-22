#include "translation/SqliteMorphologyProvider.h"
#include "translation/TranslationNormalization.h"
#include "translation/WikDictManager.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

void createMorphologySchema(sqlite3* db) {
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
    )SQL");
}

void createSpanishMorphologyDatabase(const fs::path& path) {
    fs::create_directories(path.parent_path());
    sqlite3* db = nullptr;
    require(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK,
            "Cannot create Spanish morphology test database");
    try {
        createMorphologySchema(db);
        execute(db, R"SQL(
            INSERT INTO metadata VALUES ('provider', 'apertium-spa');
            INSERT INTO morph_lemmas VALUES
                (1, 'es', 'hablar', 'hablar', 'verb', 'apertium-spa'),
                (2, 'es', 'mujer', 'mujer', 'noun', 'apertium-spa');
            INSERT INTO morph_features VALUES
                (1, 'preterite, 3rd person singular'),
                (2, 'preterite, 3rd person plural'),
                (3, 'feminine, plural');
            INSERT INTO morph_forms VALUES
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

void createGermanMorphologyDatabase(const fs::path& path) {
    fs::create_directories(path.parent_path());
    sqlite3* db = nullptr;
    require(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK,
            "Cannot create German morphology test database");
    try {
        createMorphologySchema(db);
        execute(db, R"SQL(
            INSERT INTO metadata VALUES ('provider', 'apertium-deu');
            INSERT INTO morph_lemmas VALUES
                (1, 'de', 'Frau', 'frau', 'noun', 'apertium-deu'),
                (2, 'de', 'Haus', 'haus', 'noun', 'apertium-deu'),
                (3, 'de', 'Kind', 'kind', 'noun', 'apertium-deu'),
                (4, 'de', 'gehen', 'gehen', 'verb', 'apertium-deu'),
                (5, 'de', 'sein', 'sein', 'verb', 'apertium-deu'),
                (6, 'de', 'groß', 'gross', 'adjective', 'apertium-deu'),
                (7, 'de', 'Band', 'band', 'noun', 'apertium-deu'),
                (8, 'de', 'binden', 'binden', 'verb', 'apertium-deu'),
                (9, 'de', 'fehlen', 'fehlen', 'verb', 'apertium-deu'),
                (10, 'de', 'aufstehen', 'aufstehen', 'verb', 'apertium-deu');
            INSERT INTO morph_features VALUES
                (1, 'feminine, plural, nominative'),
                (2, 'feminine, plural, dative'),
                (3, 'neuter, plural, nominative'),
                (4, 'neuter, plural, dative'),
                (5, 'past participle'),
                (6, 'past indicative, 3rd person singular'),
                (7, 'comparative, mixed adjective declension'),
                (8, 'neuter, singular, nominative'),
                (9, 'past indicative, 1st person singular'),
                (10, 'past indicative, 3rd person plural'),
                (11, 'separable verb, past participle');
            INSERT INTO morph_forms VALUES
                ('de', 'frauen', 1, 1, 100),
                ('de', 'frauen', 1, 2, 100),
                ('de', 'häuser', 2, 3, 100),
                ('de', 'kindern', 3, 4, 100),
                ('de', 'gegangen', 4, 5, 100),
                ('de', 'war', 5, 6, 100),
                ('de', 'größeren', 6, 7, 100),
                ('de', 'band', 7, 8, 100),
                ('de', 'band', 8, 9, 95),
                ('de', 'fehlten', 9, 10, 100),
                ('de', 'aufgestanden', 10, 11, 100);
        )SQL");
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
}

void createTranslationDatabase(const fs::path& path, bool german) {
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
        )SQL");
        if (german) {
            execute(db, R"SQL(
                INSERT INTO simple_translation VALUES
                    ('Frau', 'woman', 1, 1),
                    ('Frauen', 'women directly', 1, 1),
                    ('Haus', 'house | home', 1, 1),
                    ('Kind', 'child', 1, 1),
                    ('gehen', 'go', 1, 1),
                    ('sein', 'be', 1, 1),
                    ('groß', 'large', 1, 1),
                    ('Band', 'volume | ribbon', 1, 1),
                    ('binden', 'bind', 1, 1),
                    ('aufstehen', 'get up', 1, 1);
            )SQL");
        } else {
            execute(db, R"SQL(
                INSERT INTO simple_translation VALUES
                    ('hablar', 'speak | talk', 1, 1),
                    ('hablaron', 'they spoke directly', 1, 1),
                    ('mujer', 'woman', 1, 1),
                    ('señor', 'lord', 1, 1);
            )SQL");
        }
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
}

bool containsLanguage(const std::vector<std::string>& languages,
                      const std::string& language) {
    return std::find(languages.begin(), languages.end(), language) !=
           languages.end();
}

std::string scanIssues(const verdad::WikDictScanReport& report) {
    std::string issues;
    for (const auto& issue : report.issues) {
        if (!issues.empty()) issues += "; ";
        issues += issue.fileName + ": " + issue.message;
    }
    return issues;
}

void testProviders(const fs::path& spanishPath, const fs::path& germanPath) {
    verdad::SqliteMorphologyProvider spanish(spanishPath);
    require(spanish.isOpen(), "Spanish provider did not open: " + spanish.error());
    require(spanish.supportsLanguage("es-MX"),
            "Spanish provider did not normalize language code");
    auto hablo = spanish.analyze("es", "habló");
    require(hablo.size() == 1 && hablo[0].lemma == "hablar",
            "habló did not resolve to hablar");

    verdad::SqliteMorphologyProvider german(germanPath);
    require(german.isOpen(), "German provider did not open: " + german.error());
    require(german.supportsLanguage("de-DE"),
            "German provider did not normalize de-DE");
    auto hauser = german.analyze("de-DE", "Häuser");
    require(hauser.size() == 1 && hauser[0].lemma == "Haus",
            "Häuser did not resolve to Haus");
    auto decomposed = german.analyze("de", "gro\xcc\x88\xc3\x9f" "eren");
    require(decomposed.size() == 1 && decomposed[0].lemma == "groß",
            "decomposed größeren did not normalize");
}

void testEndToEnd(const fs::path& root) {
    verdad::WikDictManager manager;
    verdad::WikDictScanReport report = manager.scan(root.string());
    require(report.issues.empty(),
            "Test dictionaries produced scan issues: " + scanIssues(report));
    require(report.pairs.size() == 2, "WikDict pairs were not discovered");
    require(containsLanguage(report.morphologyLanguages, "es") &&
                containsLanguage(report.morphologyLanguages, "de"),
            "Spanish and German morphology packs were not discovered");

    auto hablo = manager.lookup("es", "en", "habló");
    require(hablo.has_value() && hablo->morphologyDerived,
            "habló end-to-end lookup failed");
    require(hablo->morphologyAnalyses.size() == 1 &&
                hablo->morphologyAnalyses[0].lemma == "hablar",
            "habló result did not expose hablar");

    auto frauen = manager.lookup("de-DE", "en-US", "frauen");
    require(frauen.has_value() && frauen->morphologyDerived,
            "German language normalization or lemma fallback failed");
    require(frauen->morphologyAnalyses.size() == 1,
            "Frauen variants were not grouped under one analysis");
    require(frauen->morphologyAnalyses[0].lemma == "Frau" &&
                frauen->morphologyAnalyses[0].partOfSpeech == "noun",
            "Frauen lemma and POS were not associated");
    require(frauen->morphologyAnalyses[0].grammaticalVariants.size() == 2,
            "Frauen grammatical variants were flattened or lost");
    require(frauen->morphologyAnalyses[0].glosses[0] == "woman",
            "Frauen did not resolve through Frau's WikDict gloss");

    auto direct = manager.lookup("de", "en", "Frauen");
    require(direct.has_value() && !direct->morphologyDerived,
            "direct German translation did not win over morphology");
    require(direct->glosses[0] == "women directly",
            "direct German translation returned the wrong gloss");

    auto hauser = manager.lookup("de", "en", "Häuser");
    require(hauser.has_value() &&
                hauser->morphologyAnalyses[0].lemma == "Haus",
            "Häuser did not resolve through Haus");
    auto kindern = manager.lookup("de", "en", "Kindern");
    require(kindern.has_value() && kindern->glosses[0] == "child",
            "Kindern did not resolve through Kind");
    auto gegangen = manager.lookup("de", "en", "gegangen");
    require(gegangen.has_value() && gegangen->glosses[0] == "go",
            "gegangen did not resolve through gehen");
    auto war = manager.lookup("de", "en", "war");
    require(war.has_value() && war->glosses[0] == "be",
            "war did not resolve through sein");
    auto gross = manager.lookup("de", "en", "größeren");
    require(gross.has_value() && gross->glosses[0] == "large",
            "größeren did not resolve through groß");

    auto ambiguous = manager.lookup("de", "en", "band");
    require(ambiguous.has_value() &&
                ambiguous->morphologyAnalyses.size() == 2,
            "German ambiguity was not preserved as two groups");
    const auto& noun = ambiguous->morphologyAnalyses[0];
    const auto& verb = ambiguous->morphologyAnalyses[1];
    require(noun.lemma == "Band" && noun.partOfSpeech == "noun" &&
                noun.glosses[0] == "volume",
            "Band noun metadata was mixed with another analysis");
    require(verb.lemma == "binden" && verb.partOfSpeech == "verb" &&
                verb.glosses[0] == "bind",
            "binden verb metadata was mixed with another analysis");

    require(!manager.lookup("de", "en", "fehlten").has_value(),
            "morphology lemma without a de-en gloss was displayed");
}

void testCorruptPack(const fs::path& root) {
    fs::create_directories(root / "morphology");
    createTranslationDatabase(root / "wikdict" / "de-en.sqlite3", true);
    std::ofstream(root / "morphology" / "de-morph-apertium.sqlite3")
        << "not a sqlite database";

    verdad::WikDictManager manager;
    verdad::WikDictScanReport report = manager.scan(root.string());
    require(report.issues.size() == 1,
            "corrupt morphology pack did not produce one scan issue");
    require(report.morphologyLanguages.empty(),
            "corrupt morphology pack was reported as available");
    require(manager.lookup("de", "en", "Frauen").has_value(),
            "exact WikDict lookup failed with a corrupt morphology pack");
}

void testDuplicatePack(const fs::path& root) {
    createTranslationDatabase(root / "wikdict" / "de-en.sqlite3", true);
    createGermanMorphologyDatabase(root / "de-morph-apertium.sqlite3");
    createGermanMorphologyDatabase(
        root / "morphology" / "de-morph-apertium.sqlite3");

    verdad::WikDictManager manager;
    verdad::WikDictScanReport report = manager.scan(root.string());
    require(report.issues.size() == 1 &&
                report.issues[0].message.find("Duplicate morphology pack") !=
                    std::string::npos,
            "duplicate normalized German pack was not reported");
    require(report.morphologyLanguages.size() == 1 &&
                report.morphologyLanguages[0] == "de",
            "duplicate German pack changed availability reporting");
}

void testMissingGermanMorphology(const fs::path& root) {
    fs::remove(root / "morphology" / "de-morph-apertium.sqlite3");
    verdad::WikDictManager manager;
    verdad::WikDictScanReport report = manager.scan(root.string());
    require(!containsLanguage(report.morphologyLanguages, "de"),
            "missing German morphology pack was reported as available");
    require(manager.lookup("de", "en", "Frauen").has_value(),
            "exact WikDict lookup failed without German morphology");
    require(!manager.lookup("de", "en", "frauen").has_value(),
            "inflected German form resolved without a morphology pack");
}

} // namespace

int main() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    const fs::path base = fs::temp_directory_path() /
                          ("verdad-morphology-tests-" +
                           std::to_string(suffix));
    try {
        const fs::path mainRoot = base / "main";
        const fs::path spanishPath =
            mainRoot / "morphology" / "es-morph-apertium.sqlite3";
        const fs::path germanPath =
            mainRoot / "morphology" / "de-morph-apertium.sqlite3";
        createSpanishMorphologyDatabase(spanishPath);
        createGermanMorphologyDatabase(germanPath);
        createTranslationDatabase(
            mainRoot / "wikdict" / "es-en.sqlite3", false);
        createTranslationDatabase(
            mainRoot / "wikdict" / "de-en.sqlite3", true);

        testProviders(spanishPath, germanPath);
        testEndToEnd(mainRoot);
        testCorruptPack(base / "corrupt");
        testDuplicatePack(base / "duplicate");
        testMissingGermanMorphology(mainRoot);
        require(verdad::normalizeLookupToken("¿Sen\xcc\x83or?") == "señor",
                "shared normalization did not compose decomposed ñ");
    } catch (const std::exception& error) {
        std::cerr << "Morphology test failure: " << error.what() << '\n';
        std::error_code ec;
        fs::remove_all(base, ec);
        return 1;
    }

    std::error_code ec;
    fs::remove_all(base, ec);
    std::cout << "Morphology tests passed\n";
    return 0;
}
