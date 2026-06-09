#ifndef VERDAD_SQLITE_MORPHOLOGY_PROVIDER_H
#define VERDAD_SQLITE_MORPHOLOGY_PROVIDER_H

#include "translation/MorphologyProvider.h"

#include <filesystem>
#include <memory>
#include <string>

namespace verdad {

class SqliteMorphologyProvider final : public MorphologyProvider {
public:
    explicit SqliteMorphologyProvider(std::filesystem::path dbPath);
    ~SqliteMorphologyProvider() override;

    SqliteMorphologyProvider(const SqliteMorphologyProvider&) = delete;
    SqliteMorphologyProvider& operator=(const SqliteMorphologyProvider&) = delete;
    SqliteMorphologyProvider(SqliteMorphologyProvider&&) noexcept;
    SqliteMorphologyProvider& operator=(SqliteMorphologyProvider&&) noexcept;

    bool isOpen() const;
    const std::string& error() const;
    bool supportsLanguage(const std::string& language) const override;
    std::vector<MorphAnalysis> analyze(
        const std::string& language,
        const std::string& token) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace verdad

#endif // VERDAD_SQLITE_MORPHOLOGY_PROVIDER_H
