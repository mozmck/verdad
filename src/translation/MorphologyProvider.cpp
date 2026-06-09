#include "translation/MorphologyProvider.h"

#include <algorithm>
#include <set>

namespace verdad {

void MorphologyService::clear() {
    providers_.clear();
}

void MorphologyService::addProvider(
        std::unique_ptr<MorphologyProvider> provider) {
    if (provider) providers_.push_back(std::move(provider));
}

bool MorphologyService::supportsLanguage(const std::string& language) const {
    return std::any_of(
        providers_.begin(), providers_.end(),
        [&](const std::unique_ptr<MorphologyProvider>& provider) {
            return provider && provider->supportsLanguage(language);
        });
}

std::vector<MorphAnalysis> MorphologyService::analyze(
        const std::string& language,
        const std::string& token) const {
    std::vector<MorphAnalysis> analyses;
    std::set<std::string> seen;
    for (const auto& provider : providers_) {
        if (!provider || !provider->supportsLanguage(language)) continue;
        for (MorphAnalysis analysis : provider->analyze(language, token)) {
            const std::string key = analysis.lemma + "\n" + analysis.pos +
                                    "\n" + analysis.features;
            if (seen.insert(key).second) analyses.push_back(std::move(analysis));
        }
    }

    std::stable_sort(
        analyses.begin(), analyses.end(),
        [](const MorphAnalysis& left, const MorphAnalysis& right) {
            if (left.confidence != right.confidence) {
                return left.confidence > right.confidence;
            }
            if (left.pos.empty() != right.pos.empty()) return !left.pos.empty();
            return left.lemma < right.lemma;
        });
    return analyses;
}

} // namespace verdad
