#include "reading/ReadingPlanManager.h"

#include "reading/DateUtils.h"
#include "reading/ReadingPlanUtils.h"

#include <sqlite3.h>

#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

namespace verdad {
namespace {

struct ScheduleOffset {
    int sequenceNumber = 0;
    std::string dateIso;
};

struct SwordScheduleMigrationRecord {
    std::string moduleName;
    std::string startDateIso;
    std::vector<ScheduleOffset> offsets;
};

struct SwordProgressMigrationRecord {
    std::string moduleName;
    int dayNumber = 0;
    bool completed = false;
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
    execSql(db, "PRAGMA journal_mode=DELETE;");
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

bool tableExists(sqlite3* db, const char* tableName) {
    if (!db || !tableName || !tableName[0]) return false;

    static const char* kSql = R"SQL(
        SELECT 1
          FROM sqlite_master
         WHERE type = 'table' AND name = ?
         LIMIT 1;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, tableName);
    const bool found = ok && sqlite3_step(stmt) == SQLITE_ROW;
    if (stmt) sqlite3_finalize(stmt);
    return found;
}

bool columnExists(sqlite3* db, const char* tableName, const char* columnName) {
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

int userVersion(sqlite3* db) {
    if (!db) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

bool setUserVersion(sqlite3* db, int version) {
    return execSql(db, ("PRAGMA user_version = " + std::to_string(version) + ";").c_str());
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
                  if (lhs.title != rhs.title) return lhs.title < rhs.title;
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
    offsets.erase(std::unique(offsets.begin(), offsets.end(),
                              [](const ScheduleOffset& lhs, const ScheduleOffset& rhs) {
                                  return lhs.sequenceNumber == rhs.sequenceNumber;
                              }),
                  offsets.end());
    return offsets;
}

std::string effectiveDateForSequence(const std::string& startDateIso,
                                     const std::vector<ScheduleOffset>& offsets,
                                     int sequenceNumber) {
    if (!reading::isIsoDateInRange(startDateIso) || sequenceNumber <= 0) return "";

    reading::Date anchorDate{};
    if (!reading::parseIsoDate(startDateIso, anchorDate)) return "";

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

int sequenceForScheduledDate(const std::string& startDateIso,
                             const std::vector<ScheduleOffset>& offsets,
                             const std::string& dateIso) {
    if (!reading::isIsoDateInRange(startDateIso) ||
        !reading::isIsoDateInRange(dateIso)) {
        return -1;
    }

    std::string anchorDateIso = startDateIso;
    int anchorSequence = 1;

    for (const auto& offset : offsets) {
        if (!reading::isIsoDateInRange(offset.dateIso)) continue;

        if (dateIso < offset.dateIso) {
            const int delta = reading::dayDifference(anchorDateIso, dateIso);
            const int sequenceNumber = anchorSequence + delta;
            if (delta >= 0 && sequenceNumber < offset.sequenceNumber) {
                return sequenceNumber;
            }
            return -1;
        }

        if (dateIso == offset.dateIso) {
            return offset.sequenceNumber;
        }

        anchorDateIso = offset.dateIso;
        anchorSequence = offset.sequenceNumber;
    }

    const int delta = reading::dayDifference(anchorDateIso, dateIso);
    if (delta < 0) return -1;
    return anchorSequence + delta;
}

std::vector<ScheduleOffset> compactOffsets(const std::string& startDateIso,
                                           const std::vector<ScheduleOffset>& offsets) {
    std::vector<ScheduleOffset> compacted;
    if (!reading::isIsoDateInRange(startDateIso)) return compacted;

    reading::Date anchorDate{};
    if (!reading::parseIsoDate(startDateIso, anchorDate)) return compacted;

    int anchorSequence = 1;
    for (const auto& offset : sortedOffsets(offsets)) {
        const std::string expectedDateIso = reading::formatIsoDate(
            reading::addDays(anchorDate, offset.sequenceNumber - anchorSequence));
        if (expectedDateIso == offset.dateIso) continue;

        compacted.push_back(offset);
        reading::parseIsoDate(offset.dateIso, anchorDate);
        anchorSequence = offset.sequenceNumber;
    }

    return compacted;
}

std::vector<ScheduleOffset> buildOffsetsFromDays(const std::string& startDateIso,
                                                 const std::vector<ReadingPlanDay>& days) {
    std::vector<ScheduleOffset> offsets;
    if (!reading::isIsoDateInRange(startDateIso)) return offsets;

    reading::Date anchorDate{};
    if (!reading::parseIsoDate(startDateIso, anchorDate)) return offsets;

    int anchorSequence = 1;
    for (const auto& day : days) {
        if (day.sequenceNumber <= 1 || !reading::isIsoDateInRange(day.dateIso)) {
            continue;
        }

        const std::string expectedDateIso = reading::formatIsoDate(
            reading::addDays(anchorDate, day.sequenceNumber - anchorSequence));
        if (expectedDateIso == day.dateIso) continue;

        offsets.push_back(ScheduleOffset{day.sequenceNumber, day.dateIso});
        reading::parseIsoDate(day.dateIso, anchorDate);
        anchorSequence = day.sequenceNumber;
    }

    return compactOffsets(startDateIso, offsets);
}

bool looksLikeLegacyTemplateDate(const std::string& dateIso) {
    reading::Date date{};
    if (!reading::parseIsoDate(dateIso, date)) return false;
    return date.year >= 1996 && date.year <= 2001;
}

std::string migratedScheduleStartDate(const std::string& legacyStartDateIso,
                                      const std::vector<ReadingPlanDay>& legacyDays) {
    reading::Date base{};
    if (!reading::parseIsoDate(legacyStartDateIso, base)) {
        for (const auto& day : legacyDays) {
            if (reading::parseIsoDate(day.dateIso, base)) {
                break;
            }
        }
    }

    if (!base.valid()) return reading::formatIsoDate(reading::today());

    reading::Date today = reading::today();
    reading::Date migrated{
        today.year,
        base.month,
        std::min(base.day, reading::daysInMonth(today.year, base.month)),
    };
    return reading::formatIsoDate(migrated);
}

bool loadPlanScheduleStartDate(sqlite3* db,
                               int planId,
                               std::string& startDateIsoOut) {
    if (!db || planId <= 0) return false;

    static const char* kSql = R"SQL(
        SELECT start_date
          FROM reading_plan_schedules
         WHERE plan_id = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, planId) == SQLITE_OK;

    startDateIsoOut.clear();
    if (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* startDate =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        startDateIsoOut = startDate ? startDate : "";
    }

    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool setPlanScheduleStartDate(sqlite3* db,
                              int planId,
                              const std::string& startDateIso) {
    if (!db || planId <= 0 || !reading::isIsoDateInRange(startDateIso)) return false;

    static const char* kSql = R"SQL(
        INSERT INTO reading_plan_schedules (plan_id, start_date)
        VALUES (?, ?)
        ON CONFLICT(plan_id)
        DO UPDATE SET start_date = excluded.start_date;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, planId) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 2, startDateIso);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool loadPlanOffsets(sqlite3* db,
                     int planId,
                     std::vector<ScheduleOffset>& out) {
    out.clear();
    if (!db || planId <= 0) return false;

    static const char* kSql = R"SQL(
        SELECT day_number, start_date
          FROM reading_plan_schedule_offsets
         WHERE plan_id = ?
         ORDER BY day_number, id;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, planId) == SQLITE_OK;

    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        ScheduleOffset offset;
        offset.sequenceNumber = sqlite3_column_int(stmt, 0);
        const char* startDate =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        offset.dateIso = startDate ? startDate : "";
        out.push_back(std::move(offset));
    }

    if (stmt) sqlite3_finalize(stmt);
    out = sortedOffsets(std::move(out));
    return ok;
}

bool replacePlanOffsets(sqlite3* db,
                        int planId,
                        int totalDays,
                        const std::vector<ScheduleOffset>& offsets) {
    if (!db || planId <= 0) return false;

    static const char* kDeleteSql =
        "DELETE FROM reading_plan_schedule_offsets WHERE plan_id = ?;";
    static const char* kInsertSql = R"SQL(
        INSERT INTO reading_plan_schedule_offsets (plan_id, day_number, start_date)
        VALUES (?, ?, ?);
    )SQL";

    sqlite3_stmt* deleteStmt = nullptr;
    sqlite3_stmt* insertStmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kDeleteSql, -1, &deleteStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_prepare_v2(db, kInsertSql, -1, &insertStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(deleteStmt, 1, planId) == SQLITE_OK;
    if (ok) ok = sqlite3_step(deleteStmt) == SQLITE_DONE;

    for (const auto& offset : sortedOffsets(offsets)) {
        if (!ok) break;
        if (offset.sequenceNumber <= 1 ||
            (totalDays > 0 && offset.sequenceNumber > totalDays) ||
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

bool loadSwordScheduleStartDate(sqlite3* db,
                                const std::string& moduleName,
                                std::string& startDateIsoOut) {
    if (!db || moduleName.empty()) return false;

    static const char* kSql = R"SQL(
        SELECT start_date
          FROM sword_reading_plan_schedules
         WHERE module_name = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);

    startDateIsoOut.clear();
    if (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* startDate =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        startDateIsoOut = startDate ? startDate : "";
    }

    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool setSwordScheduleStartDate(sqlite3* db,
                               const std::string& moduleName,
                               const std::string& startDateIso) {
    if (!db || moduleName.empty() || !reading::isIsoDateInRange(startDateIso)) {
        return false;
    }

    static const char* kSql = R"SQL(
        INSERT INTO sword_reading_plan_schedules (module_name, start_date)
        VALUES (?, ?)
        ON CONFLICT(module_name)
        DO UPDATE SET start_date = excluded.start_date;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);
    if (ok) ok = bindText(stmt, 2, startDateIso);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool loadSwordOffsets(sqlite3* db,
                      const std::string& moduleName,
                      std::vector<ScheduleOffset>& out) {
    out.clear();
    if (!db || moduleName.empty()) return false;

    static const char* kSql = R"SQL(
        SELECT day_number, start_date
          FROM sword_reading_plan_schedule_offsets
         WHERE module_name = ?
         ORDER BY day_number, id;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);

    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        ScheduleOffset offset;
        offset.sequenceNumber = sqlite3_column_int(stmt, 0);
        const char* startDate =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        offset.dateIso = startDate ? startDate : "";
        out.push_back(std::move(offset));
    }

    if (stmt) sqlite3_finalize(stmt);
    out = sortedOffsets(std::move(out));
    return ok;
}

bool replaceSwordOffsets(sqlite3* db,
                         const std::string& moduleName,
                         const std::vector<ScheduleOffset>& offsets) {
    if (!db || moduleName.empty()) return false;

    static const char* kDeleteSql =
        "DELETE FROM sword_reading_plan_schedule_offsets WHERE module_name = ?;";
    static const char* kInsertSql = R"SQL(
        INSERT INTO sword_reading_plan_schedule_offsets (module_name, day_number, start_date)
        VALUES (?, ?, ?);
    )SQL";

    sqlite3_stmt* deleteStmt = nullptr;
    sqlite3_stmt* insertStmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kDeleteSql, -1, &deleteStmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_prepare_v2(db, kInsertSql, -1, &insertStmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(deleteStmt, 1, moduleName);
    if (ok) ok = sqlite3_step(deleteStmt) == SQLITE_DONE;

    for (const auto& offset : sortedOffsets(offsets)) {
        if (!ok) break;

        sqlite3_reset(insertStmt);
        sqlite3_clear_bindings(insertStmt);
        ok = bindText(insertStmt, 1, moduleName);
        if (ok) ok = sqlite3_bind_int(insertStmt, 2, offset.sequenceNumber) == SQLITE_OK;
        if (ok) ok = bindText(insertStmt, 3, offset.dateIso);
        if (ok) ok = sqlite3_step(insertStmt) == SQLITE_DONE;
    }

    if (deleteStmt) sqlite3_finalize(deleteStmt);
    if (insertStmt) sqlite3_finalize(insertStmt);
    return ok;
}

bool loadSwordScheduleState(sqlite3* db,
                            const std::string& moduleName,
                            std::string& startDateIsoOut,
                            std::vector<ScheduleOffset>& offsetsOut) {
    if (!db || moduleName.empty()) return false;

    bool ok = loadSwordScheduleStartDate(db, moduleName, startDateIsoOut);
    if (!ok) return false;

    ok = loadSwordOffsets(db, moduleName, offsetsOut);
    if (!ok) return false;

    if (!reading::isIsoDateInRange(startDateIsoOut)) {
        startDateIsoOut = reading::formatIsoDate(reading::today());
    }
    return true;
}

bool upsertSwordProgress(sqlite3* db,
                         const std::string& moduleName,
                         int dayNumber,
                         bool completed) {
    if (!db || moduleName.empty() || dayNumber <= 0) return false;

    static const char* kSql = R"SQL(
        INSERT INTO sword_reading_plan_progress (module_name, day_number, completed)
        VALUES (?, ?, ?)
        ON CONFLICT(module_name, day_number)
        DO UPDATE SET completed = excluded.completed;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);
    if (ok) ok = sqlite3_bind_int(stmt, 2, dayNumber) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 3, completed ? 1 : 0) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool createSchemaV5(sqlite3* db) {
    if (!db) return false;

    static const char* kSchemaSql = R"SQL(
        CREATE TABLE IF NOT EXISTS reading_plans (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT '',
            color TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS reading_plan_days (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plan_id INTEGER NOT NULL,
            day_number INTEGER NOT NULL,
            title TEXT NOT NULL DEFAULT '',
            completed INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (plan_id) REFERENCES reading_plans(id)
                ON DELETE CASCADE
                ON UPDATE CASCADE,
            UNIQUE(plan_id, day_number)
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

        CREATE TABLE IF NOT EXISTS reading_plan_schedules (
            plan_id INTEGER PRIMARY KEY,
            start_date TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (plan_id) REFERENCES reading_plans(id)
                ON DELETE CASCADE
                ON UPDATE CASCADE
        );

        CREATE TABLE IF NOT EXISTS reading_plan_schedule_offsets (
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

        CREATE INDEX IF NOT EXISTS idx_reading_plan_schedule_offsets_plan_day
            ON reading_plan_schedule_offsets(plan_id, day_number);

        CREATE TABLE IF NOT EXISTS sword_reading_plan_schedules (
            module_name TEXT PRIMARY KEY,
            start_date TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS sword_reading_plan_schedule_offsets (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            module_name TEXT NOT NULL,
            day_number INTEGER NOT NULL,
            start_date TEXT NOT NULL,
            UNIQUE(module_name, day_number)
        );

        CREATE INDEX IF NOT EXISTS idx_sword_reading_plan_schedule_offsets_module_day
            ON sword_reading_plan_schedule_offsets(module_name, day_number);

        CREATE TABLE IF NOT EXISTS sword_reading_plan_progress (
            module_name TEXT NOT NULL,
            day_number INTEGER NOT NULL,
            completed INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(module_name, day_number)
        );
    )SQL";

    return execSql(db, kSchemaSql);
}

bool dropAllReadingPlanTables(sqlite3* db) {
    if (!db) return false;

    static const char* kDropSql = R"SQL(
        DROP TABLE IF EXISTS reading_plan_day_passages;
        DROP TABLE IF EXISTS reading_plan_schedule_offsets;
        DROP TABLE IF EXISTS reading_plan_offsets;
        DROP TABLE IF EXISTS reading_plan_days;
        DROP TABLE IF EXISTS reading_plan_schedules;
        DROP TABLE IF EXISTS reading_plans;

        DROP TABLE IF EXISTS sword_reading_plan_schedule_offsets;
        DROP TABLE IF EXISTS sword_reading_plan_offsets;
        DROP TABLE IF EXISTS sword_reading_plan_progress_v2;
        DROP TABLE IF EXISTS sword_reading_plan_progress;
        DROP TABLE IF EXISTS sword_reading_plan_schedules;
    )SQL";

    return execSql(db, kDropSql);
}

bool loadLegacyPlanOffsets(sqlite3* db,
                           int planId,
                           std::vector<ScheduleOffset>& out) {
    out.clear();
    if (!db || planId <= 0 || !tableExists(db, "reading_plan_offsets")) return true;

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
        const char* startDate =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        offset.dateIso = startDate ? startDate : "";
        out.push_back(std::move(offset));
    }

    if (stmt) sqlite3_finalize(stmt);
    out = sortedOffsets(std::move(out));
    return ok;
}

bool loadLegacyPlanDays(sqlite3* db,
                        int planId,
                        std::vector<ReadingPlanDay>& out) {
    out.clear();
    if (!db || planId <= 0 || !tableExists(db, "reading_plan_days")) return false;

    const bool hasDayNumber = columnExists(db, "reading_plan_days", "day_number");
    const bool hasDayDate = columnExists(db, "reading_plan_days", "day_date");
    const std::string daySql =
        "SELECT id, " +
        std::string(hasDayNumber ? "day_number" : "0 AS day_number") +
        ", " +
        std::string(hasDayDate ? "day_date" : "'' AS day_date") +
        ", title, completed "
        "FROM reading_plan_days WHERE plan_id = ? ORDER BY " +
        std::string(hasDayNumber ? "day_number, " : "") +
        (hasDayDate ? "day_date, " : "") +
        "id;";

    sqlite3_stmt* dayStmt = nullptr;
    sqlite3_stmt* passageStmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, daySql.c_str(), -1, &dayStmt, nullptr) == SQLITE_OK;

    const bool hasPassagesTable = tableExists(db, "reading_plan_day_passages");
    if (ok && hasPassagesTable) {
        static const char* kPassageSql = R"SQL(
            SELECT id, reference
              FROM reading_plan_day_passages
             WHERE day_id = ?
             ORDER BY sort_order, id;
        )SQL";
        ok = sqlite3_prepare_v2(db, kPassageSql, -1, &passageStmt, nullptr) == SQLITE_OK;
    }
    if (ok) ok = sqlite3_bind_int(dayStmt, 1, planId) == SQLITE_OK;

    while (ok && sqlite3_step(dayStmt) == SQLITE_ROW) {
        ReadingPlanDay day;
        day.id = sqlite3_column_int(dayStmt, 0);
        day.sequenceNumber = sqlite3_column_int(dayStmt, 1);
        const char* dayDate =
            reinterpret_cast<const char*>(sqlite3_column_text(dayStmt, 2));
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(dayStmt, 3));
        day.dateIso = dayDate ? dayDate : "";
        day.title = title ? title : "";
        day.completed = sqlite3_column_int(dayStmt, 4) != 0;

        if (passageStmt) {
            sqlite3_reset(passageStmt);
            sqlite3_clear_bindings(passageStmt);
            ok = sqlite3_bind_int(passageStmt, 1, day.id) == SQLITE_OK;
            while (ok && sqlite3_step(passageStmt) == SQLITE_ROW) {
                ReadingPlanPassage passage;
                passage.id = sqlite3_column_int(passageStmt, 0);
                const char* reference =
                    reinterpret_cast<const char*>(sqlite3_column_text(passageStmt, 1));
                passage.reference = reference ? reference : "";
                day.passages.push_back(std::move(passage));
            }
        }

        if (ok) out.push_back(std::move(day));
    }

    if (dayStmt) sqlite3_finalize(dayStmt);
    if (passageStmt) sqlite3_finalize(passageStmt);
    if (!ok) return false;

    out = sortedDays(std::move(out));
    return true;
}

bool loadLegacyCustomPlans(sqlite3* db,
                           std::vector<ReadingPlan>& out) {
    out.clear();
    if (!db || !tableExists(db, "reading_plans")) return true;

    const bool hasStartDate = columnExists(db, "reading_plans", "start_date");
    const std::string planSql = hasStartDate
        ? "SELECT id, name, description, color, start_date "
          "FROM reading_plans ORDER BY id;"
        : "SELECT id, name, description, color, '' AS start_date "
          "FROM reading_plans ORDER BY id;";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, planSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK;
    if (!ok) {
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ReadingPlan plan;
        plan.summary.id = sqlite3_column_int(stmt, 0);
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* color = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* startDate = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        plan.summary.name = name ? name : "";
        plan.summary.description = description ? description : "";
        plan.summary.color = color ? color : "";

        std::vector<ReadingPlanDay> legacyDays;
        std::vector<ScheduleOffset> legacyOffsets;
        if (!loadLegacyPlanDays(db, plan.summary.id, legacyDays) ||
            !loadLegacyPlanOffsets(db, plan.summary.id, legacyOffsets)) {
            ok = false;
            break;
        }

        const std::string legacyStartDateIso = startDate ? startDate : "";
        const bool usesTemplateDates =
            looksLikeLegacyTemplateDate(legacyStartDateIso) ||
            std::any_of(legacyDays.begin(), legacyDays.end(),
                        [](const ReadingPlanDay& day) {
                            return looksLikeLegacyTemplateDate(day.dateIso);
                        }) ||
            std::any_of(legacyOffsets.begin(), legacyOffsets.end(),
                        [](const ScheduleOffset& offset) {
                            return looksLikeLegacyTemplateDate(offset.dateIso);
                        });

        plan.summary.startDateIso = usesTemplateDates
            ? migratedScheduleStartDate(legacyStartDateIso, legacyDays)
            : (reading::isIsoDateInRange(legacyStartDateIso)
                   ? legacyStartDateIso
                   : migratedScheduleStartDate(legacyStartDateIso, legacyDays));

        plan.days = legacyDays;
        for (auto& day : plan.days) {
            if (usesTemplateDates) {
                day.dateIso = effectiveDateForSequence(plan.summary.startDateIso,
                                                       {},
                                                       day.sequenceNumber);
            } else if (!legacyOffsets.empty()) {
                day.dateIso = effectiveDateForSequence(plan.summary.startDateIso,
                                                       legacyOffsets,
                                                       day.sequenceNumber);
            } else if (!reading::isIsoDateInRange(day.dateIso)) {
                day.dateIso = effectiveDateForSequence(plan.summary.startDateIso,
                                                       {},
                                                       day.sequenceNumber);
            }
        }

        reading::normalizeReadingPlanDays(plan.days);
        if (!plan.days.empty() && reading::isIsoDateInRange(plan.days.front().dateIso)) {
            plan.summary.startDateIso = plan.days.front().dateIso;
        }
        plan.summary.totalDays = static_cast<int>(plan.days.size());
        plan.summary.completedDays = static_cast<int>(
            std::count_if(plan.days.begin(), plan.days.end(),
                          [](const ReadingPlanDay& day) { return day.completed; }));
        out.push_back(std::move(plan));
    }

    sqlite3_finalize(stmt);
    return ok;
}

bool loadLegacySwordSchedules(sqlite3* db,
                              std::vector<SwordScheduleMigrationRecord>& out) {
    out.clear();
    if (!db || !tableExists(db, "sword_reading_plan_schedules")) return true;

    static const char* kSql = R"SQL(
        SELECT module_name, start_date
          FROM sword_reading_plan_schedules
         ORDER BY module_name;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (!ok) {
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        SwordScheduleMigrationRecord record;
        const char* moduleName =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* startDate =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.moduleName = moduleName ? moduleName : "";
        record.startDateIso = startDate ? startDate : "";

        if (tableExists(db, "sword_reading_plan_offsets")) {
            static const char* kOffsetSql = R"SQL(
                SELECT day_number, start_date
                  FROM sword_reading_plan_offsets
                 WHERE module_name = ?
                 ORDER BY day_number, id;
            )SQL";

            sqlite3_stmt* offsetStmt = nullptr;
            ok = sqlite3_prepare_v2(db, kOffsetSql, -1, &offsetStmt, nullptr) == SQLITE_OK;
            if (ok) ok = bindText(offsetStmt, 1, record.moduleName);

            while (ok && sqlite3_step(offsetStmt) == SQLITE_ROW) {
                ScheduleOffset offset;
                offset.sequenceNumber = sqlite3_column_int(offsetStmt, 0);
                const char* offsetDate =
                    reinterpret_cast<const char*>(sqlite3_column_text(offsetStmt, 1));
                offset.dateIso = offsetDate ? offsetDate : "";
                record.offsets.push_back(std::move(offset));
            }

            if (offsetStmt) sqlite3_finalize(offsetStmt);
            record.offsets = sortedOffsets(std::move(record.offsets));
        }

        if (ok) out.push_back(std::move(record));
    }

    sqlite3_finalize(stmt);
    return ok;
}

bool loadLegacySwordProgress(sqlite3* db,
                             const std::vector<SwordScheduleMigrationRecord>& schedules,
                             std::vector<SwordProgressMigrationRecord>& out) {
    out.clear();
    if (!db) return false;

    if (tableExists(db, "sword_reading_plan_progress_v2")) {
        static const char* kSql = R"SQL(
            SELECT module_name, day_number, completed
              FROM sword_reading_plan_progress_v2
             ORDER BY module_name, day_number;
        )SQL";

        sqlite3_stmt* stmt = nullptr;
        bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
        while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
            SwordProgressMigrationRecord record;
            const char* moduleName =
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            record.moduleName = moduleName ? moduleName : "";
            record.dayNumber = sqlite3_column_int(stmt, 1);
            record.completed = sqlite3_column_int(stmt, 2) != 0;
            out.push_back(std::move(record));
        }
        if (stmt) sqlite3_finalize(stmt);
        if (!ok) return false;
        return true;
    }

    if (!tableExists(db, "sword_reading_plan_progress")) return true;

    static const char* kSql = R"SQL(
        SELECT module_name, day_date, completed
          FROM sword_reading_plan_progress
         ORDER BY module_name, day_date;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* moduleName =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* dayDate =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const std::string module = moduleName ? moduleName : "";
        const std::string dateIso = dayDate ? dayDate : "";
        if (module.empty() || !reading::isIsoDateInRange(dateIso)) continue;

        auto found = std::find_if(
            schedules.begin(), schedules.end(),
            [&module](const SwordScheduleMigrationRecord& record) {
                return record.moduleName == module;
            });
        if (found == schedules.end() ||
            !reading::isIsoDateInRange(found->startDateIso)) {
            continue;
        }

        const int dayNumber = sequenceForScheduledDate(found->startDateIso,
                                                       found->offsets,
                                                       dateIso);
        if (dayNumber <= 0) continue;

        out.push_back(SwordProgressMigrationRecord{
            module,
            dayNumber,
            sqlite3_column_int(stmt, 2) != 0,
        });
    }

    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

} // namespace

ReadingPlanManager::ReadingPlanManager() = default;

ReadingPlanManager::~ReadingPlanManager() {
    save();
    closeDatabase();
}

bool ReadingPlanManager::load(const std::string& filepath) {
    if (db_ && filepath_ == filepath) {
        closeDatabase();
    }
    return openDatabase(filepath);
}

bool ReadingPlanManager::save() {
    return db_ != nullptr;
}

bool ReadingPlanManager::checkpoint() {
    if (!db_) return false;
    return execSql(db_, "PRAGMA wal_checkpoint(TRUNCATE);");
}

std::vector<ReadingPlanSummary> ReadingPlanManager::listPlans() const {
    std::vector<ReadingPlanSummary> plans;
    if (!db_) return plans;

    static const char* kSql = R"SQL(
        SELECT p.id,
               p.name,
               p.description,
               p.color,
               COALESCE(s.start_date, ''),
               COUNT(d.id) AS total_days,
               COALESCE(SUM(CASE WHEN d.completed != 0 THEN 1 ELSE 0 END), 0) AS completed_days
          FROM reading_plans p
          LEFT JOIN reading_plan_schedules s ON s.plan_id = p.id
          LEFT JOIN reading_plan_days d ON d.plan_id = p.id
         GROUP BY p.id, p.name, p.description, p.color, s.start_date
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

bool ReadingPlanManager::resolvePlanDay(int planId,
                                        const std::string& dateIso,
                                        ReadingPlanDay* outDay,
                                        std::string* canonicalDateIsoOut) const {
    if (!db_ || planId <= 0 || !reading::isIsoDateInRange(dateIso)) return false;

    ReadingPlan plan;
    if (!getPlan(planId, plan)) return false;

    auto found = std::find_if(plan.days.begin(), plan.days.end(),
                              [&dateIso](const ReadingPlanDay& day) {
                                  return day.dateIso == dateIso;
                              });
    if (found == plan.days.end()) return false;

    if (outDay) *outDay = *found;
    if (canonicalDateIsoOut) *canonicalDateIsoOut = found->dateIso;
    return true;
}

bool ReadingPlanManager::createPlan(const ReadingPlan& plan, int* createdId) {
    if (!db_ || plan.summary.name.empty() ||
        !reading::isIsoDateInRange(plan.summary.startDateIso)) {
        return false;
    }

    if (!beginTransaction(db_)) return false;

    static const char* kInsertSql = R"SQL(
        INSERT INTO reading_plans (name, description, color)
        VALUES (?, ?, ?);
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, plan.summary.name);
    if (ok) ok = bindText(stmt, 2, plan.summary.description);
    if (ok) ok = bindText(stmt, 3, plan.summary.color);
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);

    const int planId = ok ? static_cast<int>(sqlite3_last_insert_rowid(db_)) : 0;
    if (ok) {
        ok = replacePlanDays(db_, planId, plan.summary.startDateIso, sortedDays(plan.days));
    }

    if (ok) ok = commitTransaction(db_);
    else rollbackTransaction(db_);

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
           SET name = ?, description = ?, color = ?
         WHERE id = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kUpdateSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, plan.summary.name);
    if (ok) ok = bindText(stmt, 2, plan.summary.description);
    if (ok) ok = bindText(stmt, 3, plan.summary.color);
    if (ok) ok = sqlite3_bind_int(stmt, 4, plan.summary.id) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);

    if (ok) {
        ok = replacePlanDays(db_,
                             plan.summary.id,
                             plan.summary.startDateIso,
                             sortedDays(plan.days));
    }

    if (ok) ok = commitTransaction(db_);
    else rollbackTransaction(db_);
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

    auto found = std::find_if(plan.days.begin(), plan.days.end(),
                              [&dateIso](const ReadingPlanDay& day) {
                                  return day.dateIso == dateIso;
                              });
    if (found == plan.days.end()) return false;

    static const char* kSql =
        "UPDATE reading_plan_days SET completed = ? WHERE plan_id = ? AND day_number = ?;";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 1, completed ? 1 : 0) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 2, planId) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int(stmt, 3, found->sequenceNumber) == SQLITE_OK;
    if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool ReadingPlanManager::setSwordDayCompleted(const std::string& moduleName,
                                              const std::string& dateIso,
                                              bool completed) {
    if (!db_ || moduleName.empty() || !reading::isIsoDateInRange(dateIso)) {
        return false;
    }

    if (!beginTransaction(db_)) return false;

    std::string startDateIso;
    std::vector<ScheduleOffset> offsets;
    bool ok = loadSwordScheduleState(db_, moduleName, startDateIso, offsets);
    if (ok && !reading::isIsoDateInRange(startDateIso)) {
        startDateIso = reading::formatIsoDate(reading::today());
        ok = setSwordScheduleStartDate(db_, moduleName, startDateIso);
    }

    const int dayNumber = ok ? sequenceForScheduledDate(startDateIso, offsets, dateIso) : -1;
    if (!ok || dayNumber <= 0) {
        rollbackTransaction(db_);
        return false;
    }

    ok = upsertSwordProgress(db_, moduleName, dayNumber, completed);

    if (ok) ok = commitTransaction(db_);
    else rollbackTransaction(db_);
    return ok;
}

bool ReadingPlanManager::swordDayCompleted(const std::string& moduleName,
                                           const std::string& dateIso) const {
    if (!db_ || moduleName.empty() || !reading::isIsoDateInRange(dateIso)) {
        return false;
    }

    std::string startDateIso;
    std::vector<ScheduleOffset> offsets;
    if (!loadSwordScheduleState(db_, moduleName, startDateIso, offsets)) {
        return false;
    }

    const int dayNumber = sequenceForScheduledDate(startDateIso, offsets, dateIso);
    if (dayNumber <= 0) return false;

    static const char* kSql = R"SQL(
        SELECT completed
          FROM sword_reading_plan_progress
         WHERE module_name = ? AND day_number = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);
    if (ok) ok = sqlite3_bind_int(stmt, 2, dayNumber) == SQLITE_OK;

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

    std::string startDateIso;
    std::vector<ScheduleOffset> offsets;
    if (!loadSwordScheduleState(db_, moduleName, startDateIso, offsets)) {
        return dates;
    }

    static const char* kSql = R"SQL(
        SELECT day_number
          FROM sword_reading_plan_progress
         WHERE module_name = ? AND completed != 0
         ORDER BY day_number;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) ok = bindText(stmt, 1, moduleName);

    while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        const int dayNumber = sqlite3_column_int(stmt, 0);
        const std::string scheduledDateIso =
            effectiveDateForSequence(startDateIso, offsets, dayNumber);
        if (reading::isIsoDateInRange(scheduledDateIso)) {
            dates.emplace(scheduledDateIso);
        }
    }

    if (stmt) sqlite3_finalize(stmt);
    return dates;
}

bool ReadingPlanManager::ensureSwordScheduleInitialized(
    const std::string& moduleName,
    const std::string& preferredStartDateIso) {
    if (!db_ || moduleName.empty()) return false;

    std::string storedStartDateIso;
    if (!loadSwordScheduleStartDate(db_, moduleName, storedStartDateIso)) {
        return false;
    }
    if (reading::isIsoDateInRange(storedStartDateIso)) {
        return true;
    }

    const std::string startDateIso = reading::isIsoDateInRange(preferredStartDateIso)
        ? preferredStartDateIso
        : reading::formatIsoDate(reading::today());
    return setSwordScheduleStartDate(db_, moduleName, startDateIso);
}

std::string ReadingPlanManager::swordScheduledDateForDay(const std::string& moduleName,
                                                         int dayNumber) const {
    if (!db_ || moduleName.empty() || dayNumber <= 0) return "";

    std::string startDateIso;
    std::vector<ScheduleOffset> offsets;
    if (!loadSwordScheduleState(db_, moduleName, startDateIso, offsets)) {
        return "";
    }

    return effectiveDateForSequence(startDateIso, offsets, dayNumber);
}

int ReadingPlanManager::swordDayNumberForDate(const std::string& moduleName,
                                              const std::string& dateIso) const {
    if (!db_ || moduleName.empty() || !reading::isIsoDateInRange(dateIso)) {
        return -1;
    }

    std::string startDateIso;
    std::vector<ScheduleOffset> offsets;
    if (!loadSwordScheduleState(db_, moduleName, startDateIso, offsets)) {
        return -1;
    }

    return sequenceForScheduledDate(startDateIso, offsets, dateIso);
}

bool ReadingPlanManager::rescheduleSwordDay(const std::string& moduleName,
                                            const std::string& fromDateIso,
                                            const std::string& toDateIso) {
    if (!db_ || moduleName.empty() ||
        !reading::isIsoDateInRange(fromDateIso) ||
        !reading::isIsoDateInRange(toDateIso)) {
        return false;
    }

    if (!beginTransaction(db_)) return false;

    std::string startDateIso;
    std::vector<ScheduleOffset> offsets;
    bool ok = loadSwordScheduleState(db_, moduleName, startDateIso, offsets);
    if (ok && !reading::isIsoDateInRange(startDateIso)) {
        startDateIso = reading::formatIsoDate(reading::today());
        ok = setSwordScheduleStartDate(db_, moduleName, startDateIso);
    }

    const int sequenceNumber = ok
        ? sequenceForScheduledDate(startDateIso, offsets, fromDateIso)
        : -1;
    if (!ok || sequenceNumber <= 0) {
        rollbackTransaction(db_);
        return false;
    }

    const int dayDelta = reading::dayDifference(fromDateIso, toDateIso);
    std::string newStartDateIso = startDateIso;

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
    if (ok) ok = setSwordScheduleStartDate(db_, moduleName, newStartDateIso);
    if (ok) ok = replaceSwordOffsets(db_, moduleName, offsets);

    if (ok) ok = commitTransaction(db_);
    else rollbackTransaction(db_);
    return ok;
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
    if (!getPlan(planId, plan) || plan.days.empty()) return false;

    auto found = std::find_if(plan.days.begin(), plan.days.end(),
                              [&fromDateIso](const ReadingPlanDay& day) {
                                  return day.dateIso == fromDateIso;
                              });
    if (found == plan.days.end()) return false;

    const int startIndex = static_cast<int>(found - plan.days.begin());
    const int dayDelta = reading::dayDifference(fromDateIso, toDateIso);

    if (startIndex == 0) {
        plan.summary.startDateIso = toDateIso;
    }

    for (auto it = found; it != plan.days.end(); ++it) {
        reading::Date date{};
        if (!reading::parseIsoDate(it->dateIso, date)) continue;
        it->dateIso = reading::formatIsoDate(reading::addDays(date, dayDelta));
    }

    if (!beginTransaction(db_)) return false;
    const bool ok = replacePlanDays(db_,
                                    planId,
                                    plan.summary.startDateIso,
                                    plan.days);
    if (ok) {
        return commitTransaction(db_);
    }

    rollbackTransaction(db_);
    return false;
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

    const int version = userVersion(db_);
    if (version >= 5) {
        if (!createSchemaV5(db_)) return false;
        return setUserVersion(db_, 5);
    }

    std::vector<ReadingPlan> migratedPlans;
    std::vector<SwordScheduleMigrationRecord> migratedSwordSchedules;
    std::vector<SwordProgressMigrationRecord> migratedSwordProgress;

    if (!loadLegacyCustomPlans(db_, migratedPlans) ||
        !loadLegacySwordSchedules(db_, migratedSwordSchedules) ||
        !loadLegacySwordProgress(db_, migratedSwordSchedules, migratedSwordProgress)) {
        return false;
    }

    if (!beginTransaction(db_)) return false;

    bool ok = dropAllReadingPlanTables(db_);
    if (ok) ok = createSchemaV5(db_);

    for (const auto& plan : migratedPlans) {
        if (!ok) break;

        static const char* kInsertPlanSql = R"SQL(
            INSERT INTO reading_plans (id, name, description, color)
            VALUES (?, ?, ?, ?);
        )SQL";

        sqlite3_stmt* stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, kInsertPlanSql, -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) ok = sqlite3_bind_int(stmt, 1, plan.summary.id) == SQLITE_OK;
        if (ok) ok = bindText(stmt, 2, plan.summary.name);
        if (ok) ok = bindText(stmt, 3, plan.summary.description);
        if (ok) ok = bindText(stmt, 4, plan.summary.color);
        if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (stmt) sqlite3_finalize(stmt);

        if (ok) {
            ok = replacePlanDays(db_,
                                 plan.summary.id,
                                 plan.summary.startDateIso,
                                 plan.days);
        }
    }

    for (const auto& schedule : migratedSwordSchedules) {
        if (!ok) break;
        if (reading::isIsoDateInRange(schedule.startDateIso)) {
            ok = setSwordScheduleStartDate(db_,
                                           schedule.moduleName,
                                           schedule.startDateIso);
        }
        if (ok) {
            ok = replaceSwordOffsets(db_, schedule.moduleName, schedule.offsets);
        }
    }

    for (const auto& progress : migratedSwordProgress) {
        if (!ok) break;
        ok = upsertSwordProgress(db_,
                                 progress.moduleName,
                                 progress.dayNumber,
                                 progress.completed);
    }

    if (ok) ok = setUserVersion(db_, 5);

    if (ok) ok = commitTransaction(db_);
    else rollbackTransaction(db_);

    return ok;
}

bool ReadingPlanManager::loadPlanDays(sqlite3* db,
                                      int planId,
                                      const std::string& startDateIso,
                                      std::vector<ReadingPlanDay>& out) const {
    out.clear();
    if (!db || planId <= 0) return false;

    static const char* kDaySql = R"SQL(
        SELECT id, day_number, title, completed
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

    while (ok && sqlite3_step(dayStmt) == SQLITE_ROW) {
        ReadingPlanDay day;
        day.id = sqlite3_column_int(dayStmt, 0);
        day.sequenceNumber = sqlite3_column_int(dayStmt, 1);
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(dayStmt, 2));
        day.title = title ? title : "";
        day.completed = sqlite3_column_int(dayStmt, 3) != 0;

        sqlite3_reset(passageStmt);
        sqlite3_clear_bindings(passageStmt);
        ok = sqlite3_bind_int(passageStmt, 1, day.id) == SQLITE_OK;
        while (ok && sqlite3_step(passageStmt) == SQLITE_ROW) {
            ReadingPlanPassage passage;
            passage.id = sqlite3_column_int(passageStmt, 0);
            const char* reference =
                reinterpret_cast<const char*>(sqlite3_column_text(passageStmt, 1));
            passage.reference = reference ? reference : "";
            day.passages.push_back(std::move(passage));
        }

        if (day.sequenceNumber <= 0) {
            day.sequenceNumber = static_cast<int>(out.size()) + 1;
        }
        day.dateIso = effectiveDateForSequence(startDateIso, {}, day.sequenceNumber);
        out.push_back(std::move(day));
    }

    if (dayStmt) sqlite3_finalize(dayStmt);
    if (passageStmt) sqlite3_finalize(passageStmt);
    if (!ok) return false;

    std::vector<ScheduleOffset> offsets;
    if (!loadPlanOffsets(db, planId, offsets)) return false;

    for (auto& day : out) {
        day.dateIso = effectiveDateForSequence(startDateIso, offsets, day.sequenceNumber);
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
        INSERT INTO reading_plan_days (plan_id, day_number, title, completed)
        VALUES (?, ?, ?, ?);
    )SQL";
    static const char* kInsertPassageSql = R"SQL(
        INSERT INTO reading_plan_day_passages (day_id, sort_order, reference)
        VALUES (?, ?, ?);
    )SQL";

    std::vector<ReadingPlanDay> orderedDays = sortedDays(days);
    std::string normalizedStartDateIso = startDateIso;
    if (!orderedDays.empty() && reading::isIsoDateInRange(orderedDays.front().dateIso)) {
        normalizedStartDateIso = orderedDays.front().dateIso;
    }

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
        if (ok) ok = bindText(dayStmt, 3, day.title);
        if (ok) ok = sqlite3_bind_int(dayStmt, 4, day.completed ? 1 : 0) == SQLITE_OK;
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
        ok = setPlanScheduleStartDate(db, planId, normalizedStartDateIso);
    }
    if (ok) {
        ok = replacePlanOffsets(db,
                                planId,
                                static_cast<int>(orderedDays.size()),
                                buildOffsetsFromDays(normalizedStartDateIso, orderedDays));
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

    static const char* kSql = R"SQL(
        SELECT p.id, p.name, p.description, p.color, COALESCE(s.start_date, '')
          FROM reading_plans p
          LEFT JOIN reading_plan_schedules s ON s.plan_id = p.id
         WHERE p.id = ?;
    )SQL";

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

    if (!reading::isIsoDateInRange(out.summary.startDateIso)) {
        out.summary.startDateIso = reading::formatIsoDate(reading::today());
    }

    out.days.clear();
    ok = loadPlanDays(db, planId, out.summary.startDateIso, out.days);
    if (!ok) return false;

    if (!out.days.empty() && reading::isIsoDateInRange(out.days.front().dateIso)) {
        out.summary.startDateIso = out.days.front().dateIso;
    }

    out.summary.totalDays = static_cast<int>(out.days.size());
    out.summary.completedDays = static_cast<int>(
        std::count_if(out.days.begin(), out.days.end(),
                      [](const ReadingPlanDay& day) { return day.completed; }));
    return true;
}

} // namespace verdad
