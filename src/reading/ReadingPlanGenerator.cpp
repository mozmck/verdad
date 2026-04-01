#include "reading/ReadingPlanGenerator.h"

#include "reading/DateUtils.h"
#include "reading/ReadingPlanUtils.h"
#include "sword/SwordManager.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace verdad {
namespace {

struct ChapterSpec {
    std::string book;
    int chapter = 0;
    int verseCount = 0;
};

struct Segment {
    std::string book;
    int chapter = 0;
    int verseStart = 0;
    int verseEnd = 0;
    int maxVerse = 0;
    int weight = 0;
    bool wholeChapter = false;
};

std::string lowerTrimmed(const std::string& text) {
    std::string out = reading::trimCopy(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return out;
}

reading::Date addMonthsClamped(const reading::Date& date, int deltaMonths) {
    std::tm value = reading::toTm(date);
    const int originalDay = date.day;
    value.tm_mon += deltaMonths;
    value.tm_mday = 1;
    std::mktime(&value);

    reading::Date shifted = reading::fromTm(value);
    shifted.day = std::min(originalDay, reading::daysInMonth(shifted.year, shifted.month));
    return reading::normalizeDate(shifted);
}

int inclusiveDaySpan(const reading::Date& start, const reading::Date& end) {
    std::tm startTm = reading::toTm(start);
    std::tm endTm = reading::toTm(end);
    const double seconds = std::difftime(std::mktime(&endTm), std::mktime(&startTm));
    return static_cast<int>(std::llround(seconds / (60.0 * 60.0 * 24.0))) + 1;
}

reading::Date resolveEndDate(const ReadingPlanGenerationRequest& request,
                             const reading::Date& startDate,
                             std::string* errorOut) {
    switch (request.timeframeKind) {
    case ReadingPlanTimeframeKind::Days:
        if (request.timeframeValue <= 0) {
            if (errorOut) *errorOut = "Enter a positive number of days.";
            return {};
        }
        return reading::addDays(startDate, request.timeframeValue - 1);
    case ReadingPlanTimeframeKind::Weeks:
        if (request.timeframeValue <= 0) {
            if (errorOut) *errorOut = "Enter a positive number of weeks.";
            return {};
        }
        return reading::addDays(startDate, (request.timeframeValue * 7) - 1);
    case ReadingPlanTimeframeKind::Months:
        if (request.timeframeValue <= 0) {
            if (errorOut) *errorOut = "Enter a positive number of months.";
            return {};
        }
        return reading::addDays(addMonthsClamped(startDate, request.timeframeValue), -1);
    case ReadingPlanTimeframeKind::OneYear:
        return reading::addDays(addMonthsClamped(startDate, 12), -1);
    case ReadingPlanTimeframeKind::TwoYears:
        return reading::addDays(addMonthsClamped(startDate, 24), -1);
    case ReadingPlanTimeframeKind::Custom: {
        reading::Date endDate{};
        if (!reading::parseIsoDate(request.customEndDateIso, endDate)) {
            if (errorOut) *errorOut = "Enter a valid end date in YYYY-MM-DD format.";
            return {};
        }
        if (reading::compareDates(endDate, startDate) < 0) {
            if (errorOut) *errorOut = "The end date must be on or after the start date.";
            return {};
        }
        return endDate;
    }
    }

    if (errorOut) *errorOut = "Unsupported time frame.";
    return {};
}

bool appendRuleBooks(std::vector<std::string>& orderedBooks,
                     const ReadingPlanScopeRule& rule,
                     const std::vector<std::string>& allBooks,
                     const std::vector<std::string>& oldTestamentBooks,
                     const std::vector<std::string>& newTestamentBooks,
                     std::string* errorOut) {
    std::vector<std::string> ruleBooks;
    switch (rule.kind) {
    case ReadingPlanScopeKind::WholeBible:
        ruleBooks = allBooks;
        break;
    case ReadingPlanScopeKind::OldTestament:
        ruleBooks = oldTestamentBooks;
        break;
    case ReadingPlanScopeKind::NewTestament:
        ruleBooks = newTestamentBooks;
        break;
    case ReadingPlanScopeKind::Books: {
        if (rule.books.empty()) {
            if (errorOut) *errorOut = "Add at least one custom book.";
            return false;
        }

        std::unordered_map<std::string, std::string> lookup;
        for (const auto& book : allBooks) {
            lookup.emplace(lowerTrimmed(book), book);
        }
        for (const auto& rawBook : rule.books) {
            const auto it = lookup.find(lowerTrimmed(rawBook));
            if (it == lookup.end()) {
                if (errorOut) {
                    *errorOut = "Could not find the book \"" + rawBook +
                                "\" in the selected Bible module.";
                }
                return false;
            }
            ruleBooks.push_back(it->second);
        }
        break;
    }
    }

    if (ruleBooks.empty()) {
        if (errorOut) *errorOut = "The selected reading scope has no books.";
        return false;
    }

    for (int repeat = 0; repeat < rule.repeatCount; ++repeat) {
        orderedBooks.insert(orderedBooks.end(), ruleBooks.begin(), ruleBooks.end());
    }
    return true;
}

std::vector<ChapterSpec> collectChapters(SwordManager& swordManager,
                                         const std::string& moduleName,
                                         const std::vector<std::string>& orderedBooks) {
    std::vector<ChapterSpec> chapters;
    for (const auto& book : orderedBooks) {
        const int chapterCount = swordManager.getChapterCount(moduleName, book);
        for (int chapter = 1; chapter <= chapterCount; ++chapter) {
            const int verseCount = swordManager.getVerseCount(moduleName, book, chapter);
            if (verseCount <= 0) continue;
            chapters.push_back(ChapterSpec{book, chapter, verseCount});
        }
    }
    return chapters;
}

std::vector<Segment> buildSegments(const std::vector<ChapterSpec>& chapters,
                                   ReadingPlanSplitMode splitMode,
                                   int readingDayCount) {
    std::vector<Segment> segments;
    if (chapters.empty()) return segments;

    int totalVerses = 0;
    for (const auto& chapter : chapters) {
        totalVerses += chapter.verseCount;
    }

    if (splitMode == ReadingPlanSplitMode::Verse) {
        segments.reserve(static_cast<size_t>(totalVerses));
        for (const auto& chapter : chapters) {
            for (int verse = 1; verse <= chapter.verseCount; ++verse) {
                segments.push_back(
                    Segment{chapter.book, chapter.chapter, verse, verse, chapter.verseCount, 1, false});
            }
        }
        return segments;
    }

    const double targetWeight =
        (readingDayCount > 0) ? static_cast<double>(totalVerses) / readingDayCount : 0.0;
    for (const auto& chapter : chapters) {
        int partCount = 1;
        if (targetWeight > 0.0 &&
            chapter.verseCount > std::max(45.0, targetWeight * 1.35)) {
            partCount = std::max(2, static_cast<int>(
                                        std::round(chapter.verseCount / targetWeight)));
        }

        if (partCount <= 1) {
            segments.push_back(Segment{chapter.book,
                                       chapter.chapter,
                                       1,
                                       chapter.verseCount,
                                       chapter.verseCount,
                                       chapter.verseCount,
                                       true});
            continue;
        }

        for (int part = 0; part < partCount; ++part) {
            const int startVerse = ((part * chapter.verseCount) / partCount) + 1;
            const int endVerse = ((part + 1) * chapter.verseCount) / partCount;
            if (endVerse < startVerse) continue;
            segments.push_back(Segment{chapter.book,
                                       chapter.chapter,
                                       startVerse,
                                       endVerse,
                                       chapter.verseCount,
                                       endVerse - startVerse + 1,
                                       false});
        }
    }

    return segments;
}

std::vector<std::vector<Segment>> partitionSegments(const std::vector<Segment>& segments,
                                                    int dayCount) {
    std::vector<std::vector<Segment>> groups;
    if (segments.empty() || dayCount <= 0) return groups;

    groups.resize(static_cast<size_t>(dayCount));
    int remainingWeight = 0;
    for (const auto& segment : segments) {
        remainingWeight += segment.weight;
    }

    size_t index = 0;
    for (int day = 0; day < dayCount; ++day) {
        const int daysLeft = dayCount - day;
        const int nextDayCount = daysLeft - 1;
        const double target =
            (daysLeft > 0) ? static_cast<double>(remainingWeight) / daysLeft : 0.0;

        double currentWeight = 0.0;
        size_t dayStart = index;
        while (index < segments.size()) {
            const size_t remainingAfterTaking = segments.size() - (index + 1);
            if (index > dayStart && remainingAfterTaking < static_cast<size_t>(nextDayCount)) {
                break;
            }

            const double withCurrent = currentWeight + segments[index].weight;
            if (index > dayStart && currentWeight > 0.0 &&
                std::fabs(withCurrent - target) > std::fabs(currentWeight - target)) {
                break;
            }

            currentWeight = withCurrent;
            ++index;
            if (index >= segments.size()) break;
            if ((segments.size() - index) == static_cast<size_t>(nextDayCount)) break;
        }

        if (index == dayStart && index < segments.size()) {
            currentWeight += segments[index].weight;
            ++index;
        }

        for (size_t i = dayStart; i < index; ++i) {
            groups[static_cast<size_t>(day)].push_back(segments[i]);
        }

        remainingWeight -= static_cast<int>(currentWeight);
    }

    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const std::vector<Segment>& group) {
                                    return group.empty();
                                }),
                 groups.end());
    return groups;
}

std::string formatSegmentReference(const Segment& segment) {
    std::ostringstream out;
    out << segment.book << " " << segment.chapter;
    if (!segment.wholeChapter) {
        out << ":" << segment.verseStart;
        if (segment.verseEnd > segment.verseStart) {
            out << "-" << segment.verseEnd;
        }
    }
    return out.str();
}

std::vector<ReadingPlanPassage> passagesForGroup(const std::vector<Segment>& group) {
    std::vector<ReadingPlanPassage> passages;
    if (group.empty()) return passages;

    Segment current = group.front();
    for (size_t i = 1; i < group.size(); ++i) {
        const Segment& next = group[i];
        const bool canMerge =
            !current.wholeChapter &&
            !next.wholeChapter &&
            current.book == next.book &&
            current.chapter == next.chapter &&
            current.verseEnd + 1 == next.verseStart;
        if (canMerge) {
            current.verseEnd = next.verseEnd;
            continue;
        }

        passages.push_back(ReadingPlanPassage{0, formatSegmentReference(current)});
        current = next;
    }

    passages.push_back(ReadingPlanPassage{0, formatSegmentReference(current)});
    return passages;
}

} // namespace

bool generateReadingPlan(SwordManager& swordManager,
                         const ReadingPlanGenerationRequest& request,
                         ReadingPlan& out,
                         std::string* errorOut) {
    if (request.name.empty()) {
        if (errorOut) *errorOut = "Enter a plan name.";
        return false;
    }
    if (request.moduleName.empty()) {
        if (errorOut) *errorOut = "No Bible module is available for reading-plan generation.";
        return false;
    }

    reading::Date startDate{};
    if (!reading::parseIsoDate(request.startDateIso, startDate)) {
        if (errorOut) *errorOut = "Enter a valid start date in YYYY-MM-DD format.";
        return false;
    }

    reading::Date endDate = resolveEndDate(request, startDate, errorOut);
    if (!endDate.valid()) return false;

    const int totalDateSlots = inclusiveDaySpan(startDate, endDate);
    if (totalDateSlots <= 0) {
        if (errorOut) *errorOut = "The selected date range is empty.";
        return false;
    }

    const std::vector<std::string> allBooks = swordManager.getBookNames(request.moduleName);
    const std::vector<std::string> oldTestamentBooks =
        swordManager.getBookNamesForTestament(request.moduleName, 1);
    const std::vector<std::string> newTestamentBooks =
        swordManager.getBookNamesForTestament(request.moduleName, 2);
    if (allBooks.empty()) {
        if (errorOut) *errorOut = "The selected Bible module does not expose any books.";
        return false;
    }

    std::vector<std::string> orderedBooks;
    for (const auto& rule : request.scopeRules) {
        if (rule.repeatCount <= 0) continue;
        if (!appendRuleBooks(orderedBooks,
                             rule,
                             allBooks,
                             oldTestamentBooks,
                             newTestamentBooks,
                             errorOut)) {
            return false;
        }
    }

    if (orderedBooks.empty()) {
        if (errorOut) *errorOut = "Select at least one range to generate.";
        return false;
    }

    const std::vector<ChapterSpec> chapters =
        collectChapters(swordManager, request.moduleName, orderedBooks);
    if (chapters.empty()) {
        if (errorOut) *errorOut = "The selected range did not produce any readable chapters.";
        return false;
    }

    int baseSegmentCount = 0;
    if (request.splitMode == ReadingPlanSplitMode::Verse) {
        for (const auto& chapter : chapters) {
            baseSegmentCount += chapter.verseCount;
        }
    } else {
        baseSegmentCount = static_cast<int>(chapters.size());
    }
    if (baseSegmentCount <= 0) {
        if (errorOut) *errorOut = "The selected range is empty.";
        return false;
    }

    const int readingDayCount = std::max(1, std::min(totalDateSlots, baseSegmentCount));
    const std::vector<Segment> segments =
        buildSegments(chapters, request.splitMode, readingDayCount);
    const std::vector<std::vector<Segment>> groups =
        partitionSegments(segments, readingDayCount);
    if (groups.empty()) {
        if (errorOut) *errorOut = "Failed to divide the selected range into reading days.";
        return false;
    }

    ReadingPlan plan;
    plan.summary.name = request.name;
    plan.summary.description = request.description;
    plan.summary.color = request.color;
    plan.summary.startDateIso = request.startDateIso;

    for (size_t i = 0; i < groups.size(); ++i) {
        const int slotIndex = (groups.size() <= 1 || totalDateSlots <= 1)
                                  ? 0
                                  : static_cast<int>(std::lround(
                                        (static_cast<double>(i) * (totalDateSlots - 1)) /
                                        (groups.size() - 1)));
        ReadingPlanDay day;
        day.sequenceNumber = static_cast<int>(i) + 1;
        day.dateIso = reading::formatIsoDate(reading::addDays(startDate, slotIndex));
        day.passages = passagesForGroup(groups[i]);
        plan.days.push_back(std::move(day));
    }

    reading::normalizeReadingPlanDays(plan.days);
    if (plan.days.empty()) {
        if (errorOut) *errorOut = "The generated plan did not contain any passages.";
        return false;
    }

    plan.summary.totalDays = static_cast<int>(plan.days.size());
    plan.summary.completedDays = 0;
    out = std::move(plan);
    return true;
}

} // namespace verdad
