#include "translation/WikDictManager.h"

#include <FL/fl_utf8.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace verdad {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxWordBytes = 256;
constexpr size_t kMaxGlossBytes = 512;
constexpr size_t kMaxAttributionBytes = 256;
constexpr size_t kMaxGlosses = 5;

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

std::string normalizeLanguageCode(const std::string& language) {
    std::string code = trimCopy(language);
    size_t separator = code.find_first_of("-_");
    if (separator != std::string::npos) code.resize(separator);

    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return code;
}

std::string lowercaseUtf8(const std::string& text) {
    if (text.empty()) return "";

    std::string lowered(text.size() * 4 + 4, '\0');
    int loweredLength = fl_utf_tolower(
        reinterpret_cast<const unsigned char*>(text.data()),
        static_cast<int>(text.size()),
        lowered.data());
    if (loweredLength <= 0) return text;

    lowered.resize(static_cast<size_t>(loweredLength));
    return lowered;
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

bool hasAnalysisSchema(sqlite3* db) {
    static const std::set<std::string> entryColumns = {
        "lexentry", "vocable", "written_rep", "part_of_speech", "gender"};
    static const std::set<std::string> importanceColumns = {
        "vocable", "score", "written_rep_guess"};
    static const std::set<std::string> relativeImportanceColumns = {
        "vocable", "score", "rel_score", "written_rep_guess"};
    return tableExists(db, "entry") &&
           tableExists(db, "importance") &&
           tableExists(db, "rel_importance") &&
           tableHasColumns(db, "entry", entryColumns) &&
           tableHasColumns(db, "importance", importanceColumns) &&
           tableHasColumns(db, "rel_importance", relativeImportanceColumns);
}

std::string pairKey(const std::string& sourceLanguage,
                    const std::string& targetLanguage) {
    return sourceLanguage + "-" + targetLanguage;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string withoutSuffix(const std::string& text,
                          const std::string& suffix) {
    return text.substr(0, text.size() - suffix.size());
}

std::string removeSpanishAcuteAccents(const std::string& text) {
    std::string result = text;
    const std::pair<const char*, const char*> replacements[] = {
        {"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"},
        {"Á", "A"}, {"É", "E"}, {"Í", "I"}, {"Ó", "O"}, {"Ú", "U"},
    };
    for (const auto& replacement : replacements) {
        size_t offset = 0;
        while ((offset = result.find(replacement.first, offset)) !=
               std::string::npos) {
            result.replace(offset,
                           std::strlen(replacement.first),
                           replacement.second);
            offset += std::strlen(replacement.second);
        }
    }
    return result;
}

struct LemmaCandidate {
    std::string lemma;
    std::string partOfSpeech;
    std::string grammaticalDetail;
};

struct WordMetadata {
    std::vector<std::string> partsOfSpeech;
    std::vector<std::string> genders;
};

void appendUnique(std::vector<std::string>& values, const std::string& value) {
    std::string cleaned = trimCopy(value);
    if (cleaned.empty()) return;
    if (std::find(values.begin(), values.end(), cleaned) == values.end()) {
        values.push_back(std::move(cleaned));
    }
}

void addLemmaCandidate(std::vector<LemmaCandidate>& candidates,
                       std::unordered_set<std::string>& seen,
                       std::string lemma,
                       const std::string& partOfSpeech,
                       const std::string& detail) {
    lemma = trimCopy(lemma);
    if (lemma.size() < 2) return;

    std::string key = lemma + "\n" + partOfSpeech + "\n" + detail;
    if (seen.insert(key).second) {
        candidates.push_back({std::move(lemma), partOfSpeech, detail});
    }
}

void addVerbSuffixCandidate(std::vector<LemmaCandidate>& candidates,
                            std::unordered_set<std::string>& seen,
                            const std::string& word,
                            const std::string& suffix,
                            const std::vector<std::string>& infinitiveEndings,
                            const std::string& detail) {
    if (!endsWith(word, suffix) || word.size() <= suffix.size() + 1) return;
    const std::string stem = withoutSuffix(word, suffix);
    for (const auto& ending : infinitiveEndings) {
        addLemmaCandidate(candidates, seen, stem + ending, "verb", detail);
    }
}

void addFutureOrConditionalCandidate(
        std::vector<LemmaCandidate>& candidates,
        std::unordered_set<std::string>& seen,
        const std::string& word,
        const std::string& suffix,
        const std::string& detail) {
    if (!endsWith(word, suffix) || word.size() <= suffix.size() + 2) return;
    std::string lemma = withoutSuffix(word, suffix);
    if (endsWith(lemma, "ar") || endsWith(lemma, "er") ||
        endsWith(lemma, "ir")) {
        addLemmaCandidate(candidates, seen, lemma, "verb", detail);
    }
}

void addIrregularSpanishCandidates(
        const std::string& word,
        std::vector<LemmaCandidate>& candidates,
        std::unordered_set<std::string>& seen) {
    auto formDetail = [](const std::string& form,
                         const std::string& fallback) {
        static const std::unordered_set<std::string> gerunds = {
            "siendo", "yendo", "habiendo", "teniendo", "haciendo", "diciendo",
            "viniendo", "viendo", "dando", "poniendo", "pudiendo", "queriendo",
            "sabiendo"};
        if (gerunds.find(form) != gerunds.end()) return std::string("gerund");

        static const std::unordered_set<std::string> participles = {
            "sido", "ido", "habido", "tenido", "hecho", "dicho", "venido",
            "visto", "dado", "puesto", "podido", "querido", "sabido", "bendito",
            "escrito", "abierto", "cubierto", "muerto", "roto", "vuelto"};
        if (participles.find(form) != participles.end()) {
            return std::string("past participle");
        }

        static const std::unordered_map<std::string, std::string> preterites = {
            {"fui", "preterite, first-person singular"},
            {"fuiste", "preterite, second-person singular"},
            {"fue", "preterite, third-person singular"},
            {"fuimos", "preterite, first-person plural"},
            {"fuisteis", "preterite, second-person plural"},
            {"fueron", "preterite, third-person plural"},
            {"di", "preterite, first-person singular"},
            {"diste", "preterite, second-person singular"},
            {"dio", "preterite, third-person singular"},
            {"dimos", "preterite, first-person plural"},
            {"disteis", "preterite, second-person plural"},
            {"dieron", "preterite, third-person plural"},
        };
        auto preterite = preterites.find(form);
        if (preterite != preterites.end()) return preterite->second;
        if (endsWith(form, "ieron")) return std::string("preterite, third-person plural");
        if (endsWith(form, "isteis")) return std::string("preterite, second-person plural");
        if (endsWith(form, "iste")) return std::string("preterite, second-person singular");

        static const std::unordered_set<std::string> subjunctives = {
            "sea", "seas", "seamos", "seáis", "sean", "vaya", "vayas", "vayamos",
            "vayáis", "vayan", "esté", "estés", "estemos", "estéis", "estén",
            "haya", "hayas", "hayamos", "hayáis", "hayan", "tenga", "tengas",
            "tengamos", "tengáis", "tengan", "haga", "hagas", "hagamos", "hagáis",
            "hagan", "diga", "digas", "digamos", "digáis", "digan", "venga",
            "vengas", "vengamos", "vengáis", "vengan", "vea", "veas", "veamos",
            "veáis", "vean", "dé", "des", "demos", "deis", "den", "ponga",
            "pongas", "pongamos", "pongáis", "pongan", "pueda", "puedas", "podamos",
            "podáis", "puedan", "quiera", "quieras", "queramos", "queráis", "quieran",
            "sepa", "sepas", "sepamos", "sepáis", "sepan"};
        if (subjunctives.find(form) != subjunctives.end()) {
            return std::string("present subjunctive or imperative");
        }

        return fallback;
    };

    struct IrregularSet {
        const char* lemma;
        const char* detail;
        std::vector<std::string> forms;
    };
    static const std::vector<IrregularSet> sets = {
        {"ser", "irregular form of ser",
         {"soy", "eres", "es", "somos", "sois", "son", "era", "eras",
          "éramos", "erais", "eran", "sea", "seas", "seamos", "seáis",
          "sean", "fuera", "fueras", "fuéramos", "fuerais", "fueran",
          "fuese", "fueses", "fuésemos", "fueseis", "fuesen", "siendo",
          "sido", "fui", "fuiste", "fue", "fuimos", "fuisteis", "fueron"}},
        {"ir", "irregular form of ir",
         {"voy", "vas", "va", "vamos", "vais", "van", "iba", "ibas",
          "íbamos", "ibais", "iban", "vaya", "vayas", "vayamos", "vayáis",
          "vayan", "yendo", "ido", "fui", "fuiste", "fue", "fuimos",
          "fuisteis", "fueron"}},
        {"estar", "irregular form of estar",
         {"estoy", "estás", "está", "estamos", "estáis", "están", "estuve",
          "estuviste", "estuvo", "estuvimos", "estuvisteis", "estuvieron",
          "esté", "estés", "estemos", "estéis", "estén"}},
        {"haber", "irregular form of haber",
         {"he", "has", "ha", "hemos", "habéis", "han", "hay", "había",
          "habías", "habíamos", "habíais", "habían", "hubo", "hube",
          "hubiste", "hubimos", "hubisteis", "hubieron", "haya", "hayas",
          "hayamos", "hayáis", "hayan", "habido", "habiendo"}},
        {"tener", "irregular form of tener",
         {"tengo", "tienes", "tiene", "tenemos", "tenéis", "tienen", "tuve",
          "tuviste", "tuvo", "tuvimos", "tuvisteis", "tuvieron", "tenga",
          "tengas", "tengamos", "tengáis", "tengan", "tenido", "teniendo"}},
        {"hacer", "irregular form of hacer",
         {"hago", "haces", "hace", "hacemos", "hacéis", "hacen", "hice",
          "hiciste", "hizo", "hicimos", "hicisteis", "hicieron", "haga",
          "hagas", "hagamos", "hagáis", "hagan", "hecho", "haciendo"}},
        {"decir", "irregular form of decir",
         {"digo", "dices", "dice", "decimos", "decís", "dicen", "dije",
          "dijiste", "dijo", "dijimos", "dijisteis", "dijeron", "diga",
          "digas", "digamos", "digáis", "digan", "dicho", "diciendo"}},
        {"venir", "irregular form of venir",
         {"vengo", "vienes", "viene", "venimos", "venís", "vienen", "vine",
          "viniste", "vino", "vinimos", "vinisteis", "vinieron", "venga",
          "vengas", "vengamos", "vengáis", "vengan", "venido", "viniendo"}},
        {"ver", "irregular form of ver",
         {"veo", "ves", "ve", "vemos", "veis", "ven", "vi", "viste", "vio",
          "vimos", "visteis", "vieron", "vea", "veas", "veamos", "veáis",
          "vean", "visto", "viendo"}},
        {"dar", "irregular form of dar",
         {"doy", "das", "da", "damos", "dais", "dan", "di", "diste", "dio",
          "dimos", "disteis", "dieron", "dé", "des", "demos", "deis", "den",
          "dado", "dando"}},
        {"poner", "irregular form of poner",
         {"pongo", "pones", "pone", "ponemos", "ponéis", "ponen", "puse",
          "pusiste", "puso", "pusimos", "pusisteis", "pusieron", "ponga",
          "pongas", "pongamos", "pongáis", "pongan", "puesto", "poniendo"}},
        {"poder", "irregular form of poder",
         {"puedo", "puedes", "puede", "podemos", "podéis", "pueden", "pude",
          "pudiste", "pudo", "pudimos", "pudisteis", "pudieron", "pueda",
          "puedas", "podamos", "podáis", "puedan", "podido", "pudiendo"}},
        {"querer", "irregular form of querer",
         {"quiero", "quieres", "quiere", "queremos", "queréis", "quieren",
          "quise", "quisiste", "quiso", "quisimos", "quisisteis", "quisieron",
          "quiera", "quieras", "queramos", "queráis", "quieran", "querido",
          "queriendo"}},
        {"saber", "irregular form of saber",
         {"sé", "sabes", "sabe", "sabemos", "sabéis", "saben", "supe",
          "supiste", "supo", "supimos", "supisteis", "supieron", "sepa",
          "sepas", "sepamos", "sepáis", "sepan", "sabido", "sabiendo"}},
        {"bendecir", "irregular participle of bendecir", {"bendito"}},
        {"escribir", "irregular participle of escribir", {"escrito"}},
        {"abrir", "irregular participle of abrir", {"abierto"}},
        {"cubrir", "irregular participle of cubrir", {"cubierto"}},
        {"morir", "irregular participle of morir", {"muerto"}},
        {"romper", "irregular participle of romper", {"roto"}},
        {"volver", "irregular participle of volver", {"vuelto"}},
    };

    for (const auto& set : sets) {
        if (std::find(set.forms.begin(), set.forms.end(), word) !=
            set.forms.end()) {
            addLemmaCandidate(candidates,
                              seen,
                              set.lemma,
                              "verb",
                              formDetail(word, set.detail));
        }
    }
}

std::vector<LemmaCandidate> spanishLemmaCandidates(const std::string& spelling) {
    std::vector<LemmaCandidate> candidates;
    std::unordered_set<std::string> seen;
    std::string word = lowercaseUtf8(spelling);
    if (word.empty()) return candidates;

    addIrregularSpanishCandidates(word, candidates, seen);

    std::string uncliticized = word;
    static const std::vector<std::string> clitics = {
        "melos", "melas", "telos", "telas", "selos", "selas",
        "melo", "mela", "telo", "tela", "selo", "sela",
        "les", "los", "las", "nos", "me", "te", "se", "lo", "la", "le"};
    for (const auto& clitic : clitics) {
        if (endsWith(uncliticized, clitic) &&
            uncliticized.size() > clitic.size() + 3) {
            uncliticized = removeSpanishAcuteAccents(
                withoutSuffix(uncliticized, clitic));
            break;
        }
    }
    if (uncliticized != word) {
        addIrregularSpanishCandidates(uncliticized, candidates, seen);
    }

    const std::string& form = uncliticized;
    addVerbSuffixCandidate(candidates, seen, form, "ando", {"ar"}, "gerund");
    addVerbSuffixCandidate(candidates, seen, form, "iendo", {"er", "ir"}, "gerund");
    addVerbSuffixCandidate(candidates, seen, form, "yendo", {"er", "ir"}, "gerund");

    addVerbSuffixCandidate(candidates, seen, form, "ados", {"ar"},
                           "past participle, masculine plural");
    addVerbSuffixCandidate(candidates, seen, form, "adas", {"ar"},
                           "past participle, feminine plural");
    addVerbSuffixCandidate(candidates, seen, form, "idos", {"er", "ir"},
                           "past participle, masculine plural");
    addVerbSuffixCandidate(candidates, seen, form, "idas", {"er", "ir"},
                           "past participle, feminine plural");
    addVerbSuffixCandidate(candidates, seen, form, "ado", {"ar"}, "past participle");
    addVerbSuffixCandidate(candidates, seen, form, "ada", {"ar"},
                           "past participle, feminine singular");
    addVerbSuffixCandidate(candidates, seen, form, "ido", {"er", "ir"},
                           "past participle");
    addVerbSuffixCandidate(candidates, seen, form, "ida", {"er", "ir"},
                           "past participle, feminine singular");

    struct VerbRule {
        const char* suffix;
        std::vector<std::string> endings;
        const char* detail;
    };
    static const std::vector<VerbRule> verbRules = {
        {"asteis", {"ar"}, "preterite, second-person plural"},
        {"isteis", {"er", "ir"}, "preterite, second-person plural"},
        {"aron", {"ar"}, "preterite, third-person plural"},
        {"ieron", {"er", "ir"}, "preterite, third-person plural"},
        {"ábamos", {"ar"}, "imperfect, first-person plural"},
        {"íamos", {"er", "ir"}, "imperfect, first-person plural"},
        {"aban", {"ar"}, "imperfect, third-person plural"},
        {"ían", {"er", "ir"}, "imperfect, third-person plural"},
        {"abas", {"ar"}, "imperfect, second-person singular"},
        {"ías", {"er", "ir"}, "imperfect, second-person singular"},
        {"aba", {"ar"}, "imperfect, first- or third-person singular"},
        {"ía", {"er", "ir"}, "imperfect, first- or third-person singular"},
        {"aste", {"ar"}, "preterite, second-person singular"},
        {"iste", {"er", "ir"}, "preterite, second-person singular"},
        {"amos", {"ar"}, "present or preterite, first-person plural"},
        {"emos", {"er"}, "present, first-person plural"},
        {"imos", {"ir"}, "present or preterite, first-person plural"},
        {"áis", {"ar"}, "present, second-person plural"},
        {"éis", {"er"}, "present, second-person plural"},
        {"ís", {"ir"}, "present, second-person plural"},
        {"an", {"ar"}, "present, third-person plural"},
        {"en", {"er", "ir"}, "present, third-person plural"},
        {"as", {"ar"}, "present, second-person singular"},
        {"es", {"er", "ir"}, "present, second-person singular"},
        {"é", {"ar"}, "preterite, first-person singular"},
        {"ó", {"ar"}, "preterite, third-person singular"},
        {"í", {"er", "ir"}, "preterite, first-person singular"},
        {"ió", {"er", "ir"}, "preterite, third-person singular"},
        {"o", {"ar", "er", "ir"}, "present, first-person singular"},
        {"a", {"ar"}, "present, third-person singular"},
        {"e", {"er", "ir"}, "present, third-person singular"},
    };
    for (const auto& rule : verbRules) {
        addVerbSuffixCandidate(candidates,
                               seen,
                               form,
                               rule.suffix,
                               rule.endings,
                               rule.detail);
    }

    static const std::vector<std::pair<std::string, std::string>> futureRules = {
        {"emos", "future, first-person plural"},
        {"éis", "future, second-person plural"},
        {"án", "future, third-person plural"},
        {"ás", "future, second-person singular"},
        {"á", "future, third-person singular"},
        {"é", "future, first-person singular"},
    };
    for (const auto& rule : futureRules) {
        addFutureOrConditionalCandidate(
            candidates, seen, form, rule.first, rule.second);
    }
    static const std::vector<std::pair<std::string, std::string>> conditionalRules = {
        {"íamos", "conditional, first-person plural"},
        {"íais", "conditional, second-person plural"},
        {"ían", "conditional, third-person plural"},
        {"ías", "conditional, second-person singular"},
        {"ía", "conditional, first- or third-person singular"},
    };
    for (const auto& rule : conditionalRules) {
        addFutureOrConditionalCandidate(
            candidates, seen, form, rule.first, rule.second);
    }

    if (endsWith(form, "ces") && form.size() > 4) {
        addLemmaCandidate(candidates,
                          seen,
                          withoutSuffix(form, "ces") + "z",
                          "noun",
                          "plural");
    }
    if (endsWith(form, "s") && form.size() > 3) {
        addLemmaCandidate(candidates,
                          seen,
                          withoutSuffix(form, "s"),
                          "noun",
                          "plural");
        addLemmaCandidate(candidates,
                          seen,
                          withoutSuffix(form, "s"),
                          "adjective",
                          "plural");
    }
    if (endsWith(form, "os") && form.size() > 4) {
        addLemmaCandidate(candidates,
                          seen,
                          withoutSuffix(form, "os") + "o",
                          "adjective",
                          "masculine plural");
    }
    if (endsWith(form, "as") && form.size() > 4) {
        addLemmaCandidate(candidates,
                          seen,
                          withoutSuffix(form, "as") + "o",
                          "adjective",
                          "feminine plural");
    }
    if (endsWith(form, "a") && form.size() > 3) {
        addLemmaCandidate(candidates,
                          seen,
                          withoutSuffix(form, "a") + "o",
                          "adjective",
                          "feminine singular");
    }

    return candidates;
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

    struct AnalysisConnection {
        sqlite3* db = nullptr;
        sqlite3_stmt* vocableStatement = nullptr;
        sqlite3_stmt* entryStatement = nullptr;

        AnalysisConnection() = default;
        AnalysisConnection(const AnalysisConnection&) = delete;
        AnalysisConnection& operator=(const AnalysisConnection&) = delete;

        ~AnalysisConnection() {
            if (entryStatement) sqlite3_finalize(entryStatement);
            if (vocableStatement) sqlite3_finalize(vocableStatement);
            if (db) sqlite3_close(db);
        }
    };

    std::map<std::string, std::unique_ptr<Connection>> connections;
    std::map<std::string, std::unique_ptr<AnalysisConnection>> analysisConnections;
    std::unordered_map<std::string, std::vector<std::string>> lookupCache;
    std::unordered_map<std::string, WordMetadata> metadataCache;

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
        if (sqlite3_bind_text(statement,
                              1,
                              spelling.data(),
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

        if (stepResult != SQLITE_DONE && stepResult != SQLITE_ROW) {
            return {};
        }

        lookupCache.emplace(cacheKey, glosses);
        return glosses;
    }

    WordMetadata queryMetadata(const std::string& language,
                               const std::string& spelling) {
        const std::string cacheKey = language + "\n" + spelling;
        auto cached = metadataCache.find(cacheKey);
        if (cached != metadataCache.end()) return cached->second;

        WordMetadata metadata;
        auto connectionIt = analysisConnections.find(language);
        if (connectionIt == analysisConnections.end() ||
            !connectionIt->second ||
            !connectionIt->second->vocableStatement ||
            !connectionIt->second->entryStatement) {
            metadataCache.emplace(cacheKey, metadata);
            return metadata;
        }

        AnalysisConnection& connection = *connectionIt->second;
        sqlite3_stmt* vocableStatement = connection.vocableStatement;
        sqlite3_reset(vocableStatement);
        sqlite3_clear_bindings(vocableStatement);
        if (sqlite3_bind_text(vocableStatement,
                              1,
                              spelling.data(),
                              static_cast<int>(spelling.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            sqlite3_reset(vocableStatement);
            return metadata;
        }

        while (sqlite3_step(vocableStatement) == SQLITE_ROW) {
            const unsigned char* rawVocable =
                sqlite3_column_text(vocableStatement, 0);
            if (!rawVocable) continue;

            std::string vocable = reinterpret_cast<const char*>(rawVocable);
            std::string rangeStart = vocable + "__";
            std::string rangeEnd = vocable + "_`";

            sqlite3_stmt* entryStatement = connection.entryStatement;
            sqlite3_reset(entryStatement);
            sqlite3_clear_bindings(entryStatement);
            sqlite3_bind_text(entryStatement,
                              1,
                              rangeStart.data(),
                              static_cast<int>(rangeStart.size()),
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(entryStatement,
                              2,
                              rangeEnd.data(),
                              static_cast<int>(rangeEnd.size()),
                              SQLITE_TRANSIENT);
            while (sqlite3_step(entryStatement) == SQLITE_ROW) {
                const unsigned char* pos = sqlite3_column_text(entryStatement, 0);
                const unsigned char* gender = sqlite3_column_text(entryStatement, 1);
                if (pos) {
                    appendUnique(metadata.partsOfSpeech,
                                 reinterpret_cast<const char*>(pos));
                }
                if (gender) {
                    appendUnique(metadata.genders,
                                 reinterpret_cast<const char*>(gender));
                }
            }
            sqlite3_reset(entryStatement);
        }
        sqlite3_reset(vocableStatement);

        metadataCache.emplace(cacheKey, metadata);
        return metadata;
    }
};

WikDictManager::WikDictManager()
    : impl_(std::make_unique<Impl>()) {}

WikDictManager::~WikDictManager() = default;
WikDictManager::WikDictManager(WikDictManager&&) noexcept = default;
WikDictManager& WikDictManager::operator=(WikDictManager&&) noexcept = default;

WikDictScanReport WikDictManager::scan(const std::string& directory) {
    impl_->connections.clear();
    impl_->analysisConnections.clear();
    impl_->lookupCache.clear();
    impl_->metadataCache.clear();

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
    std::vector<fs::path> analysisCandidates;
    fs::directory_iterator iterator(directoryPath, ec);
    fs::directory_iterator end;
    if (ec) {
        report.issues.push_back({directoryPath.filename().string(),
                                 "Cannot read dictionary folder: " + ec.message()});
        return report;
    }

    static const std::regex filePattern(
        R"(^([A-Za-z]{2,3})-([A-Za-z]{2,3})\.sqlite3$)");
    static const std::regex analysisFilePattern(
        R"(^([A-Za-z]{2,3})\.sqlite3$)");
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

        std::smatch match;
        const std::string fileName = iterator->path().filename().string();
        if (std::regex_match(fileName, match, filePattern)) {
            candidates.push_back(iterator->path());
        } else if (std::regex_match(fileName, match, analysisFilePattern)) {
            analysisCandidates.push_back(iterator->path());
        }
    }
    if (ec) {
        report.issues.push_back({directoryPath.filename().string(),
                                 "Dictionary folder scan stopped: " + ec.message()});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.filename().string() < right.filename().string();
              });
    std::sort(analysisCandidates.begin(), analysisCandidates.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.filename().string() < right.filename().string();
              });

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
        if (sqlite3_exec(connection->db,
                         "PRAGMA query_only=ON",
                         nullptr,
                         nullptr,
                         &pragmaError) != SQLITE_OK) {
            std::string message = pragmaError ? pragmaError : "query_only failed";
            sqlite3_free(pragmaError);
            report.issues.push_back({fileName,
                                     "Cannot enable read-only query mode: " + message});
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
        if (sqlite3_prepare_v2(connection->db,
                              lookupSql,
                              -1,
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

    for (const fs::path& path : analysisCandidates) {
        std::smatch match;
        const std::string fileName = path.filename().string();
        if (!std::regex_match(fileName, match, analysisFilePattern)) continue;

        const std::string language = normalizeLanguageCode(match[1].str());
        if (impl_->analysisConnections.find(language) !=
            impl_->analysisConnections.end()) {
            report.issues.push_back({fileName,
                                     "Duplicate normalized analysis language: " +
                                         language});
            continue;
        }

        auto connection = std::make_unique<Impl::AnalysisConnection>();
        int openResult = sqlite3_open_v2(path.string().c_str(),
                                         &connection->db,
                                         SQLITE_OPEN_READONLY,
                                         nullptr);
        if (openResult != SQLITE_OK) {
            report.issues.push_back({fileName,
                                     "Cannot open analysis database: " +
                                         sqliteMessage(connection->db,
                                                       "SQLite open failed")});
            continue;
        }

        sqlite3_enable_load_extension(connection->db, 0);
        char* pragmaError = nullptr;
        if (sqlite3_exec(connection->db,
                         "PRAGMA query_only=ON",
                         nullptr,
                         nullptr,
                         &pragmaError) != SQLITE_OK) {
            std::string message = pragmaError ? pragmaError : "query_only failed";
            sqlite3_free(pragmaError);
            report.issues.push_back({fileName,
                                     "Cannot enable read-only query mode: " + message});
            continue;
        }

        if (!hasAnalysisSchema(connection->db)) {
            report.issues.push_back({fileName,
                                     "Unsupported WikDict analysis schema"});
            continue;
        }

        const char* vocableSql =
            "SELECT vocable FROM rel_importance "
            "WHERE written_rep_guess = ?1 "
            "ORDER BY rel_score DESC, score DESC LIMIT 8";
        if (sqlite3_prepare_v2(connection->db,
                              vocableSql,
                              -1,
                              &connection->vocableStatement,
                              nullptr) != SQLITE_OK) {
            report.issues.push_back({fileName,
                                     "Cannot prepare word analysis: " +
                                         sqliteMessage(connection->db,
                                                       "SQLite prepare failed")});
            continue;
        }

        const char* entrySql =
            "SELECT part_of_speech, gender FROM entry "
            "WHERE lexentry >= ?1 AND lexentry < ?2";
        if (sqlite3_prepare_v2(connection->db,
                              entrySql,
                              -1,
                              &connection->entryStatement,
                              nullptr) != SQLITE_OK) {
            report.issues.push_back({fileName,
                                     "Cannot prepare entry metadata lookup: " +
                                         sqliteMessage(connection->db,
                                                       "SQLite prepare failed")});
            continue;
        }

        impl_->analysisConnections.emplace(language, std::move(connection));
        report.analysisLanguages.push_back(language);
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

bool metadataHasPartOfSpeech(const WordMetadata& metadata,
                             const std::string& expected) {
    if (expected.empty()) return !metadata.partsOfSpeech.empty();
    return std::find(metadata.partsOfSpeech.begin(),
                     metadata.partsOfSpeech.end(),
                     expected) != metadata.partsOfSpeech.end();
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

    const std::string lowercase = lowercaseUtf8(word);
    std::vector<std::string> glosses = impl_->queryCandidate(key, word);
    if (glosses.empty()) {
        if (lowercase != word) {
            glosses = impl_->queryCandidate(key, lowercase);
        }
    }
    const bool hadDirectGloss = !glosses.empty();

    WordMetadata sourceMetadata = impl_->queryMetadata(source, word);
    if (sourceMetadata.partsOfSpeech.empty() && lowercase != word) {
        sourceMetadata = impl_->queryMetadata(source, lowercase);
    }

    std::vector<LemmaCandidate> acceptedCandidates;
    if (source == "es" &&
        impl_->analysisConnections.find(source) !=
            impl_->analysisConnections.end()) {
        std::vector<LemmaCandidate> candidates = spanishLemmaCandidates(lowercase);
        if (!sourceMetadata.partsOfSpeech.empty()) {
            std::stable_sort(
                candidates.begin(),
                candidates.end(),
                [&](const LemmaCandidate& left, const LemmaCandidate& right) {
                    const bool leftMatches =
                        metadataHasPartOfSpeech(sourceMetadata, left.partOfSpeech);
                    const bool rightMatches =
                        metadataHasPartOfSpeech(sourceMetadata, right.partOfSpeech);
                    return leftMatches && !rightMatches;
                });
        }

        std::unordered_set<std::string> acceptedLemmas;
        for (const auto& candidate : candidates) {
            if (candidate.lemma == lowercase ||
                acceptedLemmas.find(candidate.lemma) != acceptedLemmas.end()) {
                continue;
            }

            WordMetadata lemmaMetadata =
                impl_->queryMetadata(source, candidate.lemma);
            if (!metadataHasPartOfSpeech(lemmaMetadata,
                                         candidate.partOfSpeech)) {
                continue;
            }

            std::vector<std::string> candidateGlosses =
                impl_->queryCandidate(key, candidate.lemma);
            if (candidateGlosses.empty()) continue;

            acceptedLemmas.insert(candidate.lemma);
            acceptedCandidates.push_back(candidate);
            if (!hadDirectGloss) {
                for (const auto& candidateGloss : candidateGlosses) {
                    appendUnique(glosses, candidateGloss);
                    if (glosses.size() >= kMaxGlosses) break;
                }
            }
            if (acceptedCandidates.size() >= 3 ||
                glosses.size() >= kMaxGlosses) {
                break;
            }
        }
    }

    if (glosses.empty()) return std::nullopt;

    OfflineTranslationResult result;
    result.sourceWord = word;
    result.sourceLanguage = source;
    result.targetLanguage = target;
    result.glosses = std::move(glosses);
    result.partsOfSpeech = sourceMetadata.partsOfSpeech;
    for (const auto& gender : sourceMetadata.genders) {
        appendUnique(result.grammaticalDetails, "Gender: " + gender);
    }
    for (const auto& candidate : acceptedCandidates) {
        appendUnique(result.lemmas, candidate.lemma);
        appendUnique(result.partsOfSpeech, candidate.partOfSpeech);
        if (!candidate.grammaticalDetail.empty()) {
            appendUnique(result.grammaticalDetails,
                         "Form: " + candidate.grammaticalDetail);
        }
    }
    result.inferredAnalysis = !acceptedCandidates.empty();
    result.attribution = truncateUtf8(
        "WikDict " + source + " -> " + target +
            " - CC BY-SA - Wiktionary via DBnary",
        kMaxAttributionBytes);
    return result;
}

} // namespace verdad
