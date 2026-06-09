#ifndef VERDAD_TRANSLATION_NORMALIZATION_H
#define VERDAD_TRANSLATION_NORMALIZATION_H

#include <string>

namespace verdad {

std::string normalizeLanguageCode(const std::string& language);
std::string lowercaseUtf8(const std::string& text);
std::string normalizeLookupToken(const std::string& token);

} // namespace verdad

#endif // VERDAD_TRANSLATION_NORMALIZATION_H
