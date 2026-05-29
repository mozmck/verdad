#ifndef VERDAD_SCRIPTURE_LINK_H
#define VERDAD_SCRIPTURE_LINK_H

#include <string>

namespace verdad {
namespace scripture {

std::string urlEncode(const std::string& text);
std::string urlDecode(const std::string& text);
std::string extractQueryValue(const std::string& url, const std::string& key);

bool isReadingPlanOpenUrl(const std::string& url);
std::string readingPlanOpenUrl(const std::string& reference);
std::string readingPlanOpenReference(const std::string& url);
std::string firstReadingListItem(const std::string& reference);

} // namespace scripture
} // namespace verdad

#endif // VERDAD_SCRIPTURE_LINK_H
