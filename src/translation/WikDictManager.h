#ifndef VERDAD_WIKDICT_MANAGER_H
#define VERDAD_WIKDICT_MANAGER_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace verdad {

struct OfflineTranslationSettings {
    bool enabled = false;
    std::string dictionaryDirectory;
};

struct TranslationLanguagePair {
    std::string sourceLanguage;
    std::string targetLanguage;
};

struct OfflineTranslationResult {
    std::string sourceWord;
    std::string sourceLanguage;
    std::string targetLanguage;
    std::vector<std::string> glosses;
    std::vector<std::string> lemmas;
    std::vector<std::string> partsOfSpeech;
    std::vector<std::string> grammaticalDetails;
    bool inferredAnalysis = false;
    std::string attribution;
};

struct WikDictScanIssue {
    std::string fileName;
    std::string message;
};

struct WikDictScanReport {
    std::vector<TranslationLanguagePair> pairs;
    std::vector<std::string> analysisLanguages;
    std::vector<WikDictScanIssue> issues;
};

class WikDictManager {
public:
    WikDictManager();
    ~WikDictManager();

    WikDictManager(const WikDictManager&) = delete;
    WikDictManager& operator=(const WikDictManager&) = delete;

    WikDictManager(WikDictManager&&) noexcept;
    WikDictManager& operator=(WikDictManager&&) noexcept;

    WikDictScanReport scan(const std::string& directory);
    std::vector<TranslationLanguagePair> availablePairs() const;
    bool hasPair(const std::string& sourceLanguage,
                 const std::string& targetLanguage) const;
    std::optional<OfflineTranslationResult> lookup(
        const std::string& sourceLanguage,
        const std::string& targetLanguage,
        const std::string& word);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace verdad

#endif // VERDAD_WIKDICT_MANAGER_H
