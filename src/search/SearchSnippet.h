#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace verdad::search_snippet {

inline constexpr size_t kDefaultContextWords = 6;
inline constexpr size_t kDefaultFallbackWords = kDefaultContextWords * 2 + 1;

std::string collapseWhitespace(std::string_view text);

std::string truncateWords(std::string_view text,
                          size_t maxWords = kDefaultFallbackWords);

std::string buildPlainText(std::string_view text,
                           const std::vector<bool>& mask,
                           size_t contextWords = kDefaultContextWords);

std::string buildHighlightedMarkup(std::string_view text,
                                   const std::vector<bool>& mask,
                                   size_t contextWords = kDefaultContextWords);

} // namespace verdad::search_snippet
