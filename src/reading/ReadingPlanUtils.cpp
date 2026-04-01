#include "reading/ReadingPlanUtils.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace verdad {
namespace reading {

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

std::vector<std::string> splitPlanLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trimCopy(line);
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::string joinPlanPassages(const std::vector<ReadingPlanPassage>& passages,
                             const char* separator) {
    std::ostringstream out;
    for (size_t i = 0; i < passages.size(); ++i) {
        if (i) out << separator;
        out << passages[i].reference;
    }
    return out.str();
}

namespace {

bool compareDaysByDate(const ReadingPlanDay& lhs, const ReadingPlanDay& rhs) {
    const bool lhsHasSequence = lhs.sequenceNumber > 0;
    const bool rhsHasSequence = rhs.sequenceNumber > 0;
    if (lhsHasSequence && rhsHasSequence) {
        if (lhs.sequenceNumber != rhs.sequenceNumber) {
            return lhs.sequenceNumber < rhs.sequenceNumber;
        }
    }
    if (lhs.dateIso != rhs.dateIso) return lhs.dateIso < rhs.dateIso;
    if (lhs.title != rhs.title) return lhs.title < rhs.title;
    return joinPlanPassages(lhs.passages, ";") < joinPlanPassages(rhs.passages, ";");
}

} // namespace

std::string formatReadingPlanDayLabel(const ReadingPlanDay& day) {
    std::ostringstream label;
    label << day.dateIso;
    if (!trimCopy(day.title).empty()) {
        label << "  |  " << day.title;
    }
    if (!day.passages.empty()) {
        label << "  |  " << joinPlanPassages(day.passages, "; ");
    } else {
        label << "  |  (No passages)";
    }
    return label.str();
}

void normalizeReadingPlanDays(std::vector<ReadingPlanDay>& days) {
    for (auto& day : days) {
        day.dateIso = trimCopy(day.dateIso);
        day.title = trimCopy(day.title);

        std::vector<ReadingPlanPassage> cleaned;
        for (auto passage : day.passages) {
            passage.reference = trimCopy(passage.reference);
            if (!passage.reference.empty()) {
                passage.id = 0;
                cleaned.push_back(std::move(passage));
            }
        }
        day.passages = std::move(cleaned);
        day.id = 0;
    }

    days.erase(std::remove_if(days.begin(), days.end(),
                              [](const ReadingPlanDay& day) {
                                  return day.dateIso.empty();
                              }),
               days.end());
    std::sort(days.begin(), days.end(), compareDaysByDate);

    std::vector<ReadingPlanDay> merged;
    for (auto& day : days) {
        const bool sameSequence =
            !merged.empty() &&
            merged.back().sequenceNumber > 0 &&
            day.sequenceNumber > 0 &&
            merged.back().sequenceNumber == day.sequenceNumber;
        const bool sameDate = !merged.empty() && merged.back().dateIso == day.dateIso;
        if (sameSequence || sameDate) {
            if (merged.back().title.empty()) merged.back().title = day.title;
            merged.back().completed = merged.back().completed || day.completed;
            merged.back().passages.insert(merged.back().passages.end(),
                                          day.passages.begin(),
                                          day.passages.end());
        } else {
            merged.push_back(std::move(day));
        }
    }
    for (size_t i = 0; i < merged.size(); ++i) {
        merged[i].sequenceNumber = static_cast<int>(i) + 1;
    }
    days = std::move(merged);
}

} // namespace reading
} // namespace verdad
