#ifndef VERDAD_READING_PLAN_MANAGER_H
#define VERDAD_READING_PLAN_MANAGER_H

#include <string>
#include <unordered_set>
#include <vector>

struct sqlite3;

namespace verdad {

struct ReadingPlanPassage {
    int id = 0;
    std::string reference;
};

struct ReadingPlanDay {
    int id = 0;
    int sequenceNumber = 0;
    std::string dateIso;
    std::string title;
    std::vector<ReadingPlanPassage> passages;
    bool completed = false;
};

struct ReadingPlanSummary {
    int id = 0;
    std::string name;
    std::string description;
    std::string color;
    std::string startDateIso;
    int totalDays = 0;
    int completedDays = 0;
};

struct ReadingPlan {
    ReadingPlanSummary summary;
    std::vector<ReadingPlanDay> days;
};

class ReadingPlanManager {
public:
    ReadingPlanManager();
    ~ReadingPlanManager();

    bool load(const std::string& filepath);
    bool save();

    std::vector<ReadingPlanSummary> listPlans() const;
    bool getPlan(int planId, ReadingPlan& out) const;
    bool createPlan(const ReadingPlan& plan, int* createdId = nullptr);
    bool updatePlan(const ReadingPlan& plan);
    bool deletePlan(int planId);

    bool setDayCompleted(int planId, const std::string& dateIso, bool completed);
    bool setSwordDayCompleted(const std::string& moduleName,
                              const std::string& dateIso,
                              bool completed);
    bool swordDayCompleted(const std::string& moduleName,
                           const std::string& dateIso) const;
    std::unordered_set<std::string> swordCompletedDates(
        const std::string& moduleName) const;
    bool rescheduleDay(int planId,
                       const std::string& fromDateIso,
                       const std::string& toDateIso);

private:
    bool openDatabase(const std::string& filepath);
    void closeDatabase();
    bool ensureSchema();

    bool loadPlanDays(sqlite3* db,
                      int planId,
                      const std::string& startDateIso,
                      std::vector<ReadingPlanDay>& out) const;
    bool replacePlanDays(sqlite3* db,
                         int planId,
                         const std::string& startDateIso,
                         const std::vector<ReadingPlanDay>& days) const;
    bool loadSinglePlan(sqlite3* db, int planId, ReadingPlan& out) const;

    std::string filepath_;
    sqlite3* db_ = nullptr;
};

} // namespace verdad

#endif // VERDAD_READING_PLAN_MANAGER_H
