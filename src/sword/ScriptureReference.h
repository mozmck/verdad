#ifndef VERDAD_SCRIPTURE_REFERENCE_H
#define VERDAD_SCRIPTURE_REFERENCE_H

#include <string>
#include <utility>
#include <vector>

namespace verdad {

class SwordManager;

namespace scripture {

std::string normalizeBookLookupKey(const std::string& text);
std::string ordinalBookLookupKey(const std::string& text);
std::vector<std::string> bookLookupKeys(const std::string& text);

std::string canonicalBookLabelForModule(SwordManager& manager,
                                        const std::string& moduleName,
                                        const std::string& book);

std::string normalizeSingleLinkedVerseRef(const std::string& rawRef);
std::string normalizeLinkedVerseRef(const std::string& rawRef);

std::vector<std::pair<int, int>> verseReferenceRanges(const std::string& text);
std::string verseReferenceAtPosition(const std::string& text,
                                     int pos,
                                     int* startOut = nullptr,
                                     int* endOut = nullptr);

} // namespace scripture
} // namespace verdad

#endif // VERDAD_SCRIPTURE_REFERENCE_H
