#ifndef VERDAD_MORPHOLOGY_PROVIDER_H
#define VERDAD_MORPHOLOGY_PROVIDER_H

#include <memory>
#include <string>
#include <vector>

namespace verdad {

struct MorphAnalysis {
    std::string surface;
    std::string lemma;
    std::string pos;
    std::string features;
    std::string provider;
    int confidence = 100;
};

class MorphologyProvider {
public:
    virtual ~MorphologyProvider() = default;

    virtual bool supportsLanguage(const std::string& language) const = 0;
    virtual std::vector<MorphAnalysis> analyze(
        const std::string& language,
        const std::string& token) const = 0;
};

class MorphologyService {
public:
    void clear();
    void addProvider(std::unique_ptr<MorphologyProvider> provider);
    bool supportsLanguage(const std::string& language) const;
    std::vector<MorphAnalysis> analyze(
        const std::string& language,
        const std::string& token) const;

private:
    std::vector<std::unique_ptr<MorphologyProvider>> providers_;
};

} // namespace verdad

#endif // VERDAD_MORPHOLOGY_PROVIDER_H
