#include "reading/ReadingPlanManager.h"
#include "reading/DateUtils.h"

#include <sqlite3.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

namespace verdad {
namespace {

struct ScheduleOffset {
    int sequenceNumber = 0;
    std::string dateIso;
};

bool execSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite error: "
                  << (err ? err : sqlite3_errmsg(db))
                  << "\n";
    }
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

void applyPragmas(sqlite3* db) {
    if (!db) return;
    sqlite3_busy_timeout(db, 5000);
    execSql(db, "PRAGMA foreign_keys=ON;");
    execSql(db, "PRAGMA journal_mode=WAL;");
    execSql(db, "PRAGMA synchronous=NORMAL;");
}

bool beginTransaction(sqlite3* db) {
    return execSql(db, "BEGIN IMMEDIATE TRANSACTION;");
}

bool commitTransaction(sqlite3* db) {
    return execSql(db, "COMMIT;");
}

void rollbackTransaction(sqlite3* db) {
    execSql(db, "ROLLBACK;");
}

bool columnExists(sqlite3* db,
                  const char* tableName,
                  const char* columnName) {
    if (!db || !tableName || !columnName) return false;

    std::string sql = "PRAGMA table_info(" + std::string(tableName) + ");";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK;
    bool found = false;
    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::string(name) == columnName) {
            found = true;
            break;
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return found;
}

std::vector<ReadingPlanDay> sortedDays(std::vector<ReadingPlanDay> days) {
    std::sort(days.begin(), days.end(),
              [](const ReadingPlanDay& lhs, const ReadingPlanDay& rhs) {
                  const bool lhsHasSequence = lhs.sequenceNumber > 0;
                  const bool rhsHasSequence = rhs.sequenceNumber > 0;
                  if (lhsHasSequence || rhsHasSequence) {
                      if (lhs.sequenceNumber != rhs.sequenceNumber) {
                          return lhs.sequenceNumber < rhs.sequenceNumber;
                      }
                  }
                  if (lhs.dateIso != rhs.dateIso) return lhs.dateIso < rhs.dateIso;
                  return lhs.id < rhs.id;
              });
    for (size_t i = 0; i < days.size(); ++i) {
        days[i].sequenceNumber = static_cast<int>(i) + 1;
    }
    return days;
}

std::vector<ScheduleOffset> sortedOffsets(std::vector<ScheduleOffset> offsets) {
    std::sort(offsets.begin(), offsets.end(),
              [](const ScheduleOffset& lhs, const ScheduleOffset& rhs) {
                  if (lhs.sequenceNumber != rhs.sequenceNumber) {
                      return lhs.sequenceNumber < rhs.sequenceNumber;
                  }
                  return lhs.dateIso < rhs.dateIso;
              });
    offsets.erase(std::remove_if(offsets.begin(), offsets.end(),
                                 [](const ScheduleOffset& offset) {
                                     return offset.sequenceNumber <= 1 ||
                                            !reading::isIsoDateInRange(offset.dateIso);
                                 }),
                  offsets.end());
    return offsets;
}

std::vector<ScheduleOffset> compactOffsets(const std::string& startDateIso,
                                          const std::vector<ScheduleOffset>& offsets) {
    std::vector<ScheduleOffset> compacted;
    if (!reading::isIsoDateInRange(startDateIso)) return compacted;

    reading::Date anchorDate{};
    reading::parseIsoDate(startDateIso, anchorDate);
    int anchorSequence = 1;

    for (const auto& offset : sortedOffsets(offsets)) {
        const std::string expectedDate = reading::formatIsoDate(
            reading::addDays(anchorDate, offset.sequenceNumber - anchorSequence));
        if (expectedDate == offset.dateIso) continue;

        compacted.push_back(offset);
        reading::parseIsoDate(offset.dateIso, anchorDate);
        anchorSequence = offset.sequenceNumber;
    }

    return compacted;
}

std::string effectiveDateForSequence(const std::string& startDateIso,
                                     const std::vector<ScheduleOffset>& offsets,
                                     int sequenceNumber) {
    if (!reading::isIsoDateInRange(startDateIso) || sequenceNumber <= 0) return "";

    reading::Date anchorDate{};
    reading::parseIsoDate(startDateIso, anchorDate);
    int anchorSequence = 1;
    for (const auto& offset : offsets) {
        if (offset.sequenceNumber > sequenceNumber) break;
        reading::Date offsetDate{};
        if (!reading::parseIsoDate(offset.dateIso, offsetDate)) continue;
        anchorDate = offsetDate;
        anchorSequence = offset.sequenceNumber;
    }

    return reading::formatIsoDate(
        reading::addDays(anchorDate, sequenceNumber - anchorSequence));
}

std::vector<ScheduleOffset> buildOffsetsFromDays(const std::string& startDateIso,
                                                 const std::vector<ReadingPlanDay>& days) {
    std::vector<ScheduleOffset> offsets;
    if (!reading::isIsoDateInRange(startDateIso)) return offsets;

    reading::Date anchorDate{};
    reading::parseIsoDate(startDateIso, anchorDate);
    int anchorSequence = 1;

    for (const auto& day : days) {
        if (day.sequenceNumber <= 1 || !reading::isIsoDateInRange(day.dateIso)) {
            continue;
        }

        const std::string expectedDate = reading::formatIsoDate(
            reading::addDays(anchorDate, day.sequenceNumber - anchorSequence));
        if (expectedDate == day.dateIso) continue;

        offsets.push_back(ScheduleOffset{day.sequenceNumber, day.dateIso});
        reading::parseIsoDate(day.dateIso, anchorDate);
        anchorSequence = day.sequenceNumber;
    }

    return offsets;
}

int findSequenceForDate(const std::vector<ReadingPlanDay>& days,
                        const std::string& dateIso) {
    for (const auto& day : days) {
        if (day.dateIso == dateIso) return day.sequenceNumber;
    }
    return -1;
}

bool setPlanStartDate(sqlite3* db,
                      int planId,
                      const std::string& startDateIso) {
    if (!db || planId <= 0 || !reading::isIsoDateInRange(startDateIso)) return false;

    static const char* kSql = "UPDATE reading_plans SET start_date = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, startDateIso);
    if (ok) ok = sqlite3_bind_int(stmt, 2, planId) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool replacePlanOffsets(sqlite3* db,
                        int planId,
                        int totalDays,
                        const std::vector<ScheduleOffset>& offsets) {
    if (!db || planId <= 0) return false;

    static const char* kDeleteSql =
        "DELETE FROM reading_plan_offsets WHERE plan_id = ?;";
    static const char* kInsertSql = R"SQL(
        INSERT INTO reading_plan_offsets (plan_id, day_number, start_date)
        VALUES (?, ?, ?);
    )SQL";

    sqlite3_stmt* deleteStmt = nullptr;
    sqlite3_stmt* insertStmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kDeleteSql, -1, &deleteStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_prepare_v2(db, kInsertSql, -1, &insertStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(deleteStmt, 1, planId) == SQLITE_OK;
    if (ok) ok = sqlite3_step(deleteStmt) == SQLITE_DONE;

    for (const auto& offset : offsets) {
        if (!ok) break;
        if (offset.sequenceNumber <= 1 || offset.sequenceNumber > totalDays ||
            !reading::isIsoDateInRange(offset.dateIso)) {
            continue;
        }

        sqlite3_reset(insertStmt);
        sqlite3_clear_bindings(insertStmt);
        ok = sqlite3_bind_int(insertStmt, 1, planId) == SQLITE_OK;
        if (ok) ok = sqlite3_bind_int(insertStmt, 2, offset.sequenceNumber) == SQLITE_OK;
        if (ok) ok = bindText(insertStmt, 3, offset.dateIso);
        if (ok) ok = sqlite3_step(insertStmt) == SQLITE_DONE;
    }

    if (deleteStmt) sqlite3_finalize(deleteStmt);
    if (insertStmt) sqlite3_finalize(insertStmt);
    return ok;
}

bool loadPlanOffsets(sqlite3* db,
                     int planId,
                     std::vector<ScheduleOffset>& out) {
    if (!db || planId <= 0) return false;

    static const char* kSql = R"SQL(
        SELECT day_number, start_date
          FROM reading_plan_offsets
         WHERE plan_id = ?
         ORDER BY day_number, id;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, planId) == SQLITE_OK;

    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        ScheduleOffset offset;
        offset.sequenceNumber = sqlite3_column_int(stmt, 0);
        const char* dateIso = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        offset.dateIso = dateIso ? dateIso : "";
        out.push_back(std::move(offset));
    }

    if (stmt) sqlite3_finalize(stmt);
    out = sortedOffsets(std::move(out));
    return ok;
}

} // namespace

ReadingPlanManager::ReadingPlanManager() = default;

ReadingPlanManager::~ReadingPlanManager() {
    save();
    closeDatabase();
}

bool ReadingPlanManager::load(const std::string& filepath) {
    return openDatabase(filepath);
}

bool ReadingPlanManager::save() {
    return db_ != nullptr;
}

std::vector<ReadingPlanSummary> ReadingPlanManager::listPlans() const {
    std::vector<ReadingPlanSummary> plans;
    if (!db_) return plans;

    static const char* kSql = R"SQL(
        SELECT p.id, p.name, p.description, p.color, p.start_date,
               COUNT(d.id) AS total_days,
               COALESCE(SUM(CASE WHEN d.completed != 0 THEN 1 ELSE 0 END), 0) AS completed_days
        FROM reading_plans p
        LEFT JOIN reading_plan_days d ON d.plan_id = p.id
        GROUP BY p.id, p.name, p.description, p.color, p.start_date
        ORDER BY LOWER(p.name), p.id;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return plans;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ReadingPlanSummary summary;
        summary.id = sqlite3_column_int(stmt, 0);
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* color = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* startDate = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        summary.name = name ? name : "";
        summary.description = description ? description : "";
        summary.color = color ? color : "";
        summary.startDateIso = startDate ? startDate : "";
        summary.totalDays = sqlite3_column_int(stmt, 5);
        summary.completedDays = sqlite3_column_int(stmt, 6);
        plans.push_back(std::move(summary));
    }

    sqlite3_finalize(stmt);
    return plans;
}

bool ReadingPlanManager::getPlan(int planId, ReadingPlan& out) const {
    if (!db_) return false;
    return loadSinglePlan(db_, planId, out);
}

bool ReadingPlanManager::createPlan(const ReadingPlan& plan, int* createdId) {
    if (!db_ || plan.summary.name.empty() ||
        !reading::isIsoDateInRange(plan.summary.startDateIso)) {
        return false;
    }

    if (!beginTransaction(db_)) return false;

    static const char* kInsertSql = R"SQL(
        INSERT INTO reading_plans (name, description, color, start_date)
        VALUES (?, ?, ?, ?);
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, plan.summary.name);
    if (ok) ok = bindText(stmt, 2, plan.summary.description);
    if (ok) ok = bindText(stmt, 3, plan.summary.color);
    if (ok) ok = bindText(stmt, 4, plan.summary.startDateIso);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);

    int planId = ok ? static_cast<int>(sqlite3_last_insert_rowid(db_)) : 0;
    if (ok) {
        ok = replacePlanDays(db_, planId, plan.summary.startDateIso, sortedDays(plan.days));
    }

    if (ok) {
        ok = commitTransaction(db_);
    } else {
        rollbackTransaction(db_);
    }

    if (ok && createdId) *createdId = planId;
    return ok;
}

bool ReadingPlanManager::updatePlan(const ReadingPlan& plan) {
    if (!db_ || plan.summary.id <= 0 || plan.summary.name.empty() ||
        !reading::isIsoDateInRange(plan.summary.startDateIso)) {
        return false;
    }

    if (!beginTransaction(db_)) return false;

    static const char* kUpdateSql = R"SQL(
        UPDATE reading_plans
           SET name = ?, description = ?, color = ?, start_date = ?
         WHERE id = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kUpdateSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, plan.summary.name);
    if (ok) ok = bindText(stmt, 2, plan.summary.description);
    if (ok) ok = bindText(stmt, 3, plan.summary.color);
    if (ok) ok = bindText(stmt, 4, plan.summary.startDateIso);
    if (ok) ok = sqlite3_bind_int(stmt, 5, plan.summary.id) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (stmt) sqlite3_finalize(stmt);

    if (ok) {
        ok = replacePlanDays(db_,
                             plan.summary.id,
                             plan.summary.startDateIso,
                             sortedDays(plan.days));
    }

    if (ok) {
        ok = commitTransaction(db_);
    } else {
        rollbackTransaction(db_);
    }
    return ok;
}

bool ReadingPlanManager::deletePlan(int planId) {
    if (!db_ || planId <= 0) return false;

    static const char* kSql = "DELETE FROM reading_plans WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, planId) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool ReadingPlanManager::setDayCompleted(int planId,
                                         const std::string& dateIso,
                                         bool completed) {
    if (!db_ || planId <= 0 || !reading::isIsoDateInRange(dateIso)) return false;

    ReadingPlan plan;
    if (!getPlan(planId, plan)) return false;

    const int sequenceNumber = findSequenceForDate(plan.days, dateIso);
    if (sequenceNumber <= 0) return false;

    static const char* kSql =
        "UPDATE reading_plan_days SET completed = ? WHERE plan_id = ? AND day_number = ?;";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, completed ? 1 : 0) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 2, planId) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 3, sequenceNumber) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool ReadingPlanManager::setSwordDayCompleted(const std::string& moduleName,
                                              const std::string& dateIso,
                                              bool completed) {
    if (!db_ || dateIso.empty() || moduleName.empty() ||
        !reading::isIsoDateInRange(dateIso)) {
        return false;
    }

    static const char* kSql = R"SQL(
        INSERT INTO sword_reading_plan_progress (module_name, day_date, completed)
        VALUES (?, ?, ?)
        ON CONFLICT(module_name, day_date)
        DO UPDATE SET completed = excluded.completed;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);
    if (ok) ok = bindText(stmt, 2, dateIso);
    if (ok) ok = sqlite3_bind_int(stmt, 3, completed ? 1 : 0) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool ReadingPlanManager::swordDayCompleted(const std::string& moduleName,
                                           const std::string& dateIso) const {
    if (!db_ || dateIso.empty() || moduleName.empty() ||
        !reading::isIsoDateInRange(dateIso)) {
        return false;
    }

    static const char* kSql = R"SQL(
        SELECT completed
          FROM sword_reading_plan_progress
         WHERE module_name = ? AND day_date = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);
    if (ok) ok = bindText(stmt, 2, dateIso);
    bool completed = false;
    if (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        completed = sqlite3_column_int(stmt, 0) != 0;
    }
    if (stmt) sqlite3_finalize(stmt);
    return completed;
}

std::unordered_set<std::string> ReadingPlanManager::swordCompletedDates(
    const std::string& moduleName) const {
    std::unordered_set<std::string> dates;
    if (!db_ || moduleName.empty()) return dates;

    static const char* kSql = R"SQL(
        SELECT day_date
          FROM sword_reading_plan_progress
         WHERE module_name = ? AND completed != 0
         ORDER BY day_date;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);

    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* dateIso = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (dateIso && reading::isIsoDateInRange(dateIso)) {
            dates.emplace(dateIso);
        }
    }

    if (stmt) sqlite3_finalize(stmt);
    return dates;
}

bool ReadingPlanManager::rescheduleDay(int planId,
                                       const std::string& fromDateIso,
                                       const std::string& toDateIso) {
    if (!db_ || planId <= 0 ||
        !reading::isIsoDateInRange(fromDateIso) ||
        !reading::isIsoDateInRange(toDateIso)) {
        return false;
    }

    ReadingPlan plan;
    if (!getPlan(planId, plan)) return false;

    const int sequenceNumber = findSequenceForDate(plan.days, fromDateIso);
    if (sequenceNumber <= 0) return false;

    reading::Date fromDate{};
    reading::Date toDate{};
    reading::parseIsoDate(fromDateIso, fromDate);
    reading::parseIsoDate(toDateIso, toDate);
    std::tm fromTm = reading::toTm(fromDate);
    std::tm toTm = reading::toTm(toDate);
    const int dayDelta = static_cast<int>(
        std::difftime(std::mktime(&toTm), std::mktime(&fromTm)) / (60 * 60 * 24));

    if (!beginTransaction(db_)) return false;

    std::vector<ScheduleOffset> offsets;
    bool ok = loadPlanOffsets(db_, planId, offsets);
    std::string newStartDateIso = plan.summary.startDateIso;

    if (sequenceNumber == 1) {
        newStartDateIso = toDateIso;
        for (auto& offset : offsets) {
            reading::Date current{};
            if (!reading::parseIsoDate(offset.dateIso, current)) continue;
            offset.dateIso = reading::formatIsoDate(reading::addDays(current, dayDelta));
        }
    } else {
        bool foundSequenceOffset = false;
        for (auto& offset : offsets) {
            if (offset.sequenceNumber == sequenceNumber) {
                offset.dateIso = toDateIso;
                foundSequenceOffset = true;
            } else if (offset.sequenceNumber > sequenceNumber) {
                reading::Date current{};
                if (!reading::parseIsoDate(offset.dateIso, current)) continue;
                offset.dateIso = reading::formatIsoDate(reading::addDays(current, dayDelta));
            }
        }
        if (!foundSequenceOffset) {
            offsets.push_back(ScheduleOffset{sequenceNumber, toDateIso});
        }
    }

    offsets = compactOffsets(newStartDateIso, offsets);

    if (ok) ok = setPlanStartDate(db_, planId, newStartDateIso);
    if (ok) {
        ok = replacePlanOffsets(db_,
                                planId,
                                static_cast<int>(plan.days.size()),
                                offsets);
    }

    if (ok) {
        ok = commitTransaction(db_);
    } else {
        rollbackTransaction(db_);
    }
    return ok;
}

bool ReadingPlanManager::openDatabase(const std::string& filepath) {
    if (filepath.empty()) return false;
    if (db_ && filepath_ == filepath) return true;

    closeDatabase();

    sqlite3* newDb = nullptr;
    int rc = sqlite3_open_v2(filepath.c_str(),
                             &newDb,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             nullptr);
    if (rc != SQLITE_OK || !newDb) {
        if (newDb) sqlite3_close(newDb);
        return false;
    }

    db_ = newDb;
    filepath_ = filepath;
    applyPragmas(db_);
    if (!ensureSchema()) {
        closeDatabase();
        return false;
    }
    return true;
}

void ReadingPlanManager::closeDatabase() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    filepath_.clear();
}

bool ReadingPlanManager::ensureSchema() {
    if (!db_) return false;

    static const char* kSchemaSql = R"SQL(
        CREATE TABLE IF NOT EXISTS reading_plans (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT '',
            color TEXT NOT NULL DEFAULT '',
            start_date TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS reading_plan_days (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plan_id INTEGER NOT NULL,
            day_date TEXT NOT NULL DEFAULT '',
            title TEXT NOT NULL DEFAULT '',
            completed INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (plan_id) REFERENCES reading_plans(id)
                ON DELETE CASCADE
                ON UPDATE CASCADE,
            UNIQUE(plan_id, day_date)
        );

        CREATE TABLE IF NOT EXISTS reading_plan_day_passages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            day_id INTEGER NOT NULL,
            sort_order INTEGER NOT NULL DEFAULT 0,
            reference TEXT NOT NULL,
            FOREIGN KEY (day_id) REFERENCES reading_plan_days(id)
                ON DELETE CASCADE
                ON UPDATE CASCADE
        );

        CREATE TABLE IF NOT EXISTS reading_plan_offsets (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plan_id INTEGER NOT NULL,
            day_number INTEGER NOT NULL,
            start_date TEXT NOT NULL,
            FOREIGN KEY (plan_id) REFERENCES reading_plans(id)
                ON DELETE CASCADE
                ON UPDATE CASCADE,
            UNIQUE(plan_id, day_number)
        );

        CREATE INDEX IF NOT EXISTS idx_reading_plan_day_passages_day_sort
            ON reading_plan_day_passages(day_id, sort_order);

        CREATE INDEX IF NOT EXISTS idx_reading_plan_offsets_plan_day
            ON reading_plan_offsets(plan_id, day_number);

        CREATE TABLE IF NOT EXISTS sword_reading_plan_progress (
            module_name TEXT NOT NULL,
            day_date TEXT NOT NULL,
            completed INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(module_name, day_date)
        );

        CREATE INDEX IF NOT EXISTS idx_sword_reading_plan_progress_module_date
            ON sword_reading_plan_progress(module_name, day_date);
    )SQL";

    if (!execSql(db_, kSchemaSql)) return false;

    if (!columnExists(db_, "reading_plan_days", "day_number") &&
        !execSql(db_,
                 "ALTER TABLE reading_plan_days ADD COLUMN day_number INTEGER NOT NULL DEFAULT 0;")) {
        return false;
    }

    if (!execSql(db_,
                 "CREATE INDEX IF NOT EXISTS idx_reading_plan_days_plan_day_number "
                 "ON reading_plan_days(plan_id, day_number);")) {
        return false;
    }

    if (!execSql(db_, R"SQL(
        UPDATE reading_plan_days AS target
           SET day_number = (
               SELECT COUNT(*)
                 FROM reading_plan_days AS prior
                WHERE prior.plan_id = target.plan_id
                  AND (
                      prior.day_date < target.day_date OR
                      (prior.day_date = target.day_date AND prior.id <= target.id)
                  )
           )
         WHERE target.day_number <= 0;
    )SQL")) {
        return false;
    }

    return execSql(db_, "PRAGMA user_version = 3;");
}

bool ReadingPlanManager::loadPlanDays(sqlite3* db,
                                      int planId,
                                      const std::string& startDateIso,
                                      std::vector<ReadingPlanDay>& out) const {
    if (!db || planId <= 0) return false;

    static const char* kDaySql = R"SQL(
        SELECT id, day_number, day_date, title, completed
          FROM reading_plan_days
         WHERE plan_id = ?
         ORDER BY day_number, id;
    )SQL";
    static const char* kPassageSql = R"SQL(
        SELECT id, reference
          FROM reading_plan_day_passages
         WHERE day_id = ?
         ORDER BY sort_order, id;
    )SQL";

    sqlite3_stmt* dayStmt = nullptr;
    sqlite3_stmt* passageStmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kDaySql, -1, &dayStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_prepare_v2(db, kPassageSql, -1, &passageStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(dayStmt, 1, planId) == SQLITE_OK;

    std::vector<ReadingPlanDay> rawDays;
    while (ok && sqlite3_step(dayStmt) == SQLITE_ROW) {
        ReadingPlanDay day;
        day.id = sqlite3_column_int(dayStmt, 0);
        day.sequenceNumber = sqlite3_column_int(dayStmt, 1);
        const char* legacyDate = reinterpret_cast<const char*>(sqlite3_column_text(dayStmt, 2));
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(dayStmt, 3));
        day.dateIso = legacyDate ? legacyDate : "";
        day.title = title ? title : "";
        day.completed = sqlite3_column_int(dayStmt, 4) != 0;

        sqlite3_reset(passageStmt);
        sqlite3_clear_bindings(passageStmt);
        ok = sqlite3_bind_int(passageStmt, 1, day.id) == SQLITE_OK;
        while (ok && sqlite3_step(passageStmt) == SQLITE_ROW) {
            ReadingPlanPassage passage;
            passage.id = sqlite3_column_int(passageStmt, 0);
            const char* ref = reinterpret_cast<const char*>(sqlite3_column_text(passageStmt, 1));
            passage.reference = ref ? ref : "";
            day.passages.push_back(std::move(passage));
        }
        if (ok) rawDays.push_back(std::move(day));
    }

    if (dayStmt) sqlite3_finalize(dayStmt);
    if (passageStmt) sqlite3_finalize(passageStmt);
    if (!ok) return false;

    std::vector<ScheduleOffset> offsets;
    ok = loadPlanOffsets(db, planId, offsets);
    if (!ok) return false;

    const bool hasLegacyScheduledDates = std::any_of(
        rawDays.begin(), rawDays.end(),
        [](const ReadingPlanDay& day) { return reading::isIsoDateInRange(day.dateIso); });
    const bool useDerivedSchedule =
        reading::isIsoDateInRange(startDateIso) &&
        (!offsets.empty() || !hasLegacyScheduledDates);

    out.clear();
    out.reserve(rawDays.size());
    for (auto& day : rawDays) {
        if (day.sequenceNumber <= 0) {
            day.sequenceNumber = static_cast<int>(out.size()) + 1;
        }
        if (useDerivedSchedule) {
            day.dateIso = effectiveDateForSequence(startDateIso, offsets, day.sequenceNumber);
        }
        out.push_back(std::move(day));
    }
    return true;
}

bool ReadingPlanManager::replacePlanDays(sqlite3* db,
                                         int planId,
                                         const std::string& startDateIso,
                                         const std::vector<ReadingPlanDay>& days) const {
    if (!db || planId <= 0 || !reading::isIsoDateInRange(startDateIso)) return false;

    static const char* kDeleteSql =
        "DELETE FROM reading_plan_days WHERE plan_id = ?;";
    static const char* kInsertDaySql = R"SQL(
        INSERT INTO reading_plan_days (plan_id, day_number, day_date, title, completed)
        VALUES (?, ?, ?, ?, ?);
    )SQL";
    static const char* kInsertPassageSql = R"SQL(
        INSERT INTO reading_plan_day_passages (day_id, sort_order, reference)
        VALUES (?, ?, ?);
    )SQL";

    std::vector<ReadingPlanDay> orderedDays = sortedDays(days);
    std::vector<ScheduleOffset> offsets = buildOffsetsFromDays(startDateIso, orderedDays);

    sqlite3_stmt* deleteStmt = nullptr;
    sqlite3_stmt* dayStmt = nullptr;
    sqlite3_stmt* passageStmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kDeleteSql, -1, &deleteStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_prepare_v2(db, kInsertDaySql, -1, &dayStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_prepare_v2(db, kInsertPassageSql, -1, &passageStmt, nullptr) == SQLITE_OK;

    if (ok) ok = sqlite3_bind_int(deleteStmt, 1, planId) == SQLITE_OK;
    if (ok) ok = sqlite3_step(deleteStmt) == SQLITE_DONE;

    for (size_t i = 0; ok && i < orderedDays.size(); ++i) {
        const ReadingPlanDay& day = orderedDays[i];
        const int sequenceNumber = static_cast<int>(i) + 1;

        sqlite3_reset(dayStmt);
        sqlite3_clear_bindings(dayStmt);
        ok = sqlite3_bind_int(dayStmt, 1, planId) == SQLITE_OK;
        if (ok) ok = sqlite3_bind_int(dayStmt, 2, sequenceNumber) == SQLITE_OK;
        if (ok) ok = bindText(dayStmt, 3, "__seq_" + std::to_string(sequenceNumber));
        if (ok) ok = bindText(dayStmt, 4, day.title);
        if (ok) ok = sqlite3_bind_int(dayStmt, 5, day.completed ? 1 : 0) == SQLITE_OK;
        if (ok) ok = sqlite3_step(dayStmt) == SQLITE_DONE;
        const int dayId = ok ? static_cast<int>(sqlite3_last_insert_rowid(db)) : 0;

        for (size_t passageIndex = 0; ok && passageIndex < day.passages.size(); ++passageIndex) {
            sqlite3_reset(passageStmt);
            sqlite3_clear_bindings(passageStmt);
            ok = sqlite3_bind_int(passageStmt, 1, dayId) == SQLITE_OK;
            if (ok) {
                ok = sqlite3_bind_int(passageStmt,
                                      2,
                                      static_cast<int>(passageIndex)) == SQLITE_OK;
            }
            if (ok) ok = bindText(passageStmt, 3, day.passages[passageIndex].reference);
            if (ok) ok = sqlite3_step(passageStmt) == SQLITE_DONE;
        }
    }

    if (ok) {
        ok = replacePlanOffsets(db, planId, static_cast<int>(orderedDays.size()), offsets);
    }

    if (deleteStmt) sqlite3_finalize(deleteStmt);
    if (dayStmt) sqlite3_finalize(dayStmt);
    if (passageStmt) sqlite3_finalize(passageStmt);
    return ok;
}

bool ReadingPlanManager::loadSinglePlan(sqlite3* db,
                                        int planId,
                                        ReadingPlan& out) const {
    if (!db || planId <= 0) return false;

    static const char* kSql =
        "SELECT id, name, description, color, start_date FROM reading_plans WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, planId) == SQLITE_OK;
    if (!ok) {
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const char* color = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    const char* startDate = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    out.summary.id = sqlite3_column_int(stmt, 0);
    out.summary.name = name ? name : "";
    out.summary.description = description ? description : "";
    out.summary.color = color ? color : "";
    out.summary.startDateIso = startDate ? startDate : "";
    sqlite3_finalize(stmt);

    out.days.clear();
    ok = loadPlanDays(db, planId, out.summary.startDateIso, out.days);
    if (ok && !reading::isIsoDateInRange(out.summary.startDateIso) && !out.days.empty()) {
        out.summary.startDateIso = out.days.front().dateIso;
    }
    if (ok) {
        out.summary.totalDays = static_cast<int>(out.days.size());
        out.summary.completedDays = static_cast<int>(
            std::count_if(out.days.begin(), out.days.end(),
                          [](const ReadingPlanDay& day) { return day.completed; }));
    }
    return ok;
}

} // namespace verdad
