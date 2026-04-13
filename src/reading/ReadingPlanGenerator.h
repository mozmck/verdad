#ifndef VERDAD_READING_PLAN_GENERATOR_H
#define VERDAD_READING_PLAN_GENERATOR_H

#include "reading/ReadingPlanManager.h"

#include <string>
#include <vector>

namespace verdad {

class SwordManager;

enum class ReadingPlanTimeframeKind {
    Days,
    Weeks,
    Months,
    OneYear,
    TwoYears,
};

enum class ReadingPlanSplitMode {
    Chapter,
    Verse,
};

enum class ReadingPlanScopeKind {
    WholeBible,
    OldTestament,
    NewTestament,
    Books,
};

struct ReadingPlanScopeRule {
    ReadingPlanScopeKind kind = ReadingPlanScopeKind::Books;
    int repeatCount = 0;
    std::vector<std::string> books;
};

struct ReadingPlanGenerationRequest {
    std::string moduleName;
    std::string name;
    std::string description;
    ReadingPlanTimeframeKind timeframeKind = ReadingPlanTimeframeKind::OneYear;
    int timeframeValue = 0;
    ReadingPlanSplitMode splitMode = ReadingPlanSplitMode::Chapter;
    std::vector<ReadingPlanScopeRule> scopeRules;
};

std::string defaultReadingPlanDisplayStartDateIso();

bool buildReadingPlanTemplateDates(const ReadingPlanGenerationRequest& request,
                                   std::vector<std::string>& outDates,
                                   std::string* errorOut = nullptr);

bool generateReadingPlan(SwordManager& swordManager,
                         const ReadingPlanGenerationRequest& request,
                         ReadingPlan& out,
                         std::string* errorOut = nullptr);

} // namespace verdad

#endif // VERDAD_READING_PLAN_GENERATOR_H
