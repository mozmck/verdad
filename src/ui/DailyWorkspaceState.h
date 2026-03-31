#ifndef VERDAD_DAILY_WORKSPACE_STATE_H
#define VERDAD_DAILY_WORKSPACE_STATE_H

#include <string>

namespace verdad {

enum class DailyWorkspaceMode {
    Devotionals,
    ReadingPlans,
};

enum class DailyReadingPlanSource {
    Editable,
    SwordModule,
};

struct DailyWorkspaceState {
    bool tabActive = false;
    DailyWorkspaceMode mode = DailyWorkspaceMode::Devotionals;
    std::string devotionalModule;
    DailyReadingPlanSource readingPlanSource = DailyReadingPlanSource::Editable;
    int readingPlanId = 0;
    std::string swordReadingPlanModule;
    std::string selectedDateIso;
    bool calendarVisible = false;
};

inline const char* dailyWorkspaceModeToken(DailyWorkspaceMode mode) {
    switch (mode) {
    case DailyWorkspaceMode::Devotionals:
        return "devotionals";
    case DailyWorkspaceMode::ReadingPlans:
        return "reading_plans";
    }
    return "devotionals";
}

inline DailyWorkspaceMode dailyWorkspaceModeFromToken(const std::string& text) {
    if (text == "reading_plans" || text == "plans") {
        return DailyWorkspaceMode::ReadingPlans;
    }
    return DailyWorkspaceMode::Devotionals;
}

inline const char* dailyReadingPlanSourceToken(DailyReadingPlanSource source) {
    switch (source) {
    case DailyReadingPlanSource::Editable:
        return "editable";
    case DailyReadingPlanSource::SwordModule:
        return "sword_module";
    }
    return "editable";
}

inline DailyReadingPlanSource dailyReadingPlanSourceFromToken(
    const std::string& text) {
    if (text == "sword_module" || text == "sword") {
        return DailyReadingPlanSource::SwordModule;
    }
    return DailyReadingPlanSource::Editable;
}

} // namespace verdad

#endif // VERDAD_DAILY_WORKSPACE_STATE_H
