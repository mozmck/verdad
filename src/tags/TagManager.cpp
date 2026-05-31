#include "tags/TagManager.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace verdad {
namespace {

constexpr const char* kDefaultTagColor = "#4a86c8";

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

std::string kindToken(TagTarget::Kind kind) {
    switch (kind) {
    case TagTarget::Kind::Verse:
        return "verse";
    case TagTarget::Kind::Commentary:
        return "commentary";
    case TagTarget::Kind::GeneralBook:
        return "general_book";
    }
    return "verse";
}

TagTarget::Kind kindFromToken(const std::string& token) {
    if (token == "commentary") return TagTarget::Kind::Commentary;
    if (token == "general_book") return TagTarget::Kind::GeneralBook;
    return TagTarget::Kind::Verse;
}

std::string encodeSizedField(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

bool decodeSizedField(const std::string& text, size_t& pos, std::string& out) {
    size_t colon = text.find(':', pos);
    if (colon == std::string::npos) return false;

    size_t len = 0;
    try {
        len = static_cast<size_t>(std::stoul(text.substr(pos, colon - pos)));
    } catch (...) {
        return false;
    }

    size_t valuePos = colon + 1;
    if (valuePos + len > text.size()) return false;
    out = text.substr(valuePos, len);
    pos = valuePos + len;
    return true;
}

bool ensureSchema(sqlite3* db) {
    static const char* kSchemaSql = R"SQL(
        CREATE TABLE IF NOT EXISTS tags (
            name TEXT PRIMARY KEY,
            color TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS tag_items (
            resource_kind TEXT NOT NULL,
            module_name TEXT NOT NULL,
            source_key TEXT NOT NULL,
            selection_text TEXT NOT NULL,
            tag_name TEXT NOT NULL,
            PRIMARY KEY (resource_kind, module_name, source_key, selection_text, tag_name),
            FOREIGN KEY (tag_name) REFERENCES tags(name)
                ON DELETE CASCADE
                ON UPDATE CASCADE
        );

        CREATE TABLE IF NOT EXISTS verse_tags (
            verse_key TEXT NOT NULL,
            tag_name TEXT NOT NULL,
            PRIMARY KEY (verse_key, tag_name)
        );

        CREATE INDEX IF NOT EXISTS idx_tag_items_tag_name
            ON tag_items(tag_name, resource_kind, module_name, source_key, selection_text);

        CREATE INDEX IF NOT EXISTS idx_verse_tags_tag_name
            ON verse_tags(tag_name, verse_key);
    )SQL";

    return execSql(db, kSchemaSql) &&
           (userVersion(db) >= 2 || setUserVersion(db, 2));
}

void applyPragmas(sqlite3* db) {
    if (!db) return;
    sqlite3_busy_timeout(db, 5000);
    execSql(db, "PRAGMA foreign_keys=ON;");
    execSql(db, "PRAGMA journal_mode=DELETE;");
    execSql(db, "PRAGMA synchronous=NORMAL;");
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string displayLabelForTarget(const TagTarget& target) {
    if (target.kind == TagTarget::Kind::Verse) {
        return trimCopy(target.sourceKey);
    }

    std::ostringstream label;
    label << (target.kind == TagTarget::Kind::Commentary ? "Commentary" : "General Book");
    if (!trimCopy(target.moduleName).empty()) {
        label << ": " << target.moduleName;
    }
    if (!trimCopy(target.sourceKey).empty()) {
        label << " / " << target.sourceKey;
    }
    if (!trimCopy(target.selectionText).empty()) {
        label << " - " << target.selectionText;
    }
    return label.str();
}

} // namespace

TagTarget TagTarget::verse(const std::string& verseKey) {
    TagTarget target;
    target.kind = Kind::Verse;
    target.sourceKey = trimCopy(verseKey);
    return target;
}

TagTarget TagTarget::commentary(const std::string& moduleName,
                                const std::string& sourceKey,
                                const std::string& selectionText) {
    TagTarget target;
    target.kind = Kind::Commentary;
    target.moduleName = trimCopy(moduleName);
    target.sourceKey = trimCopy(sourceKey);
    target.selectionText = trimCopy(selectionText);
    return target;
}

TagTarget TagTarget::generalBook(const std::string& moduleName,
                                 const std::string& sourceKey,
                                 const std::string& selectionText) {
    TagTarget target;
    target.kind = Kind::GeneralBook;
    target.moduleName = trimCopy(moduleName);
    target.sourceKey = trimCopy(sourceKey);
    target.selectionText = trimCopy(selectionText);
    return target;
}

std::string TagTarget::displayLabel() const {
    return displayLabelForTarget(*this);
}

TagManager::TagManager() = default;

TagManager::~TagManager() {
    if (dirty_ && !filepath_.empty()) {
        save();
    }
    closeDatabase();
}

bool TagManager::load(const std::string& filepath) {
    if (db_ && filepath_ == filepath) {
        closeDatabase();
    }

    tags_.clear();
    targetTags_.clear();
    tagTargets_.clear();
    targets_.clear();
    dirty_ = false;

    if (!openDatabase(filepath)) {
        return false;
    }

    if (!hasStoredData()) {
        std::filesystem::path legacyPath(filepath);
        legacyPath.replace_extension(".dat");
        if (fileExists(legacyPath.string()) && !importLegacyFile(legacyPath.string())) {
            return false;
        }
    }

    return loadFromDatabase();
}

bool TagManager::save(const std::string& filepath) {
    bool pathChanged = filepath_ != filepath || db_ == nullptr;
    if (!openDatabase(filepath)) {
        return false;
    }
    if (pathChanged) {
        dirty_ = true;
    }
    return persistToDatabase();
}

bool TagManager::save() {
    if (filepath_.empty()) return false;
    if (!openDatabase(filepath_)) {
        return false;
    }
    return persistToDatabase();
}

bool TagManager::checkpoint() {
    if (!db_) return false;
    return execSql(db_, "PRAGMA wal_checkpoint(TRUNCATE);");
}

bool TagManager::createTag(const std::string& name, const std::string& color) {
    if (tags_.find(name) != tags_.end()) return false;

    Tag tag;
    tag.name = name;
    tag.color = color;
    tags_[name] = tag;
    dirty_ = true;
    return true;
}

bool TagManager::deleteTag(const std::string& name) {
    auto it = tags_.find(name);
    if (it == tags_.end()) return false;

    tags_.erase(it);

    auto ttIt = tagTargets_.find(name);
    if (ttIt != tagTargets_.end()) {
        for (const auto& targetKey : ttIt->second) {
            auto targetTagsIt = targetTags_.find(targetKey);
            if (targetTagsIt != targetTags_.end()) {
                targetTagsIt->second.erase(name);
                if (targetTagsIt->second.empty()) {
                    targetTags_.erase(targetTagsIt);
                    targets_.erase(targetKey);
                }
            }
        }
        tagTargets_.erase(ttIt);
    }

    dirty_ = true;
    return true;
}

bool TagManager::renameTag(const std::string& oldName, const std::string& newName) {
    auto it = tags_.find(oldName);
    if (it == tags_.end()) return false;
    if (tags_.find(newName) != tags_.end()) return false;

    Tag tag = it->second;
    tag.name = newName;
    tags_.erase(it);
    tags_[newName] = tag;

    auto ttIt = tagTargets_.find(oldName);
    std::set<std::string> targets;
    if (ttIt != tagTargets_.end()) {
        targets = std::move(ttIt->second);
        tagTargets_.erase(ttIt);
    }
    if (!targets.empty()) {
        tagTargets_[newName] = std::move(targets);
    }

    for (auto& pair : targetTags_) {
        if (pair.second.erase(oldName) > 0) {
            pair.second.insert(newName);
        }
    }

    dirty_ = true;
    return true;
}

void TagManager::setTagColor(const std::string& name, const std::string& color) {
    auto it = tags_.find(name);
    if (it != tags_.end() && it->second.color != color) {
        it->second.color = color;
        dirty_ = true;
    }
}

void TagManager::tagVerse(const std::string& verseKey, const std::string& tagName) {
    tagTarget(TagTarget::verse(verseKey), tagName);
}

void TagManager::tagTarget(const TagTarget& target, const std::string& tagName) {
    if (tags_.find(tagName) == tags_.end()) {
        createTag(tagName);
    }

    const std::string key = targetKey(target);
    targets_[key] = target;
    if (targetTags_[key].insert(tagName).second) {
        tagTargets_[tagName].insert(key);
        dirty_ = true;
    }
}

void TagManager::untagVerse(const std::string& verseKey, const std::string& tagName) {
    untagTarget(TagTarget::verse(verseKey), tagName);
}

void TagManager::untagTarget(const TagTarget& target, const std::string& tagName) {
    const std::string key = targetKey(target);
    auto it = targetTags_.find(key);
    if (it == targetTags_.end()) return;

    if (it->second.erase(tagName) > 0) {
        auto ttIt = tagTargets_.find(tagName);
        if (ttIt != tagTargets_.end()) {
            ttIt->second.erase(key);
            if (ttIt->second.empty()) {
                tagTargets_.erase(ttIt);
            }
        }
        dirty_ = true;
    }

    if (it->second.empty()) {
        targetTags_.erase(it);
        targets_.erase(key);
    }
}

std::vector<Tag> TagManager::getAllTags() const {
    std::vector<Tag> result;
    for (const auto& pair : tags_) {
        result.push_back(pair.second);
    }
    std::sort(result.begin(), result.end(),
              [](const Tag& a, const Tag& b) { return a.name < b.name; });
    return result;
}

std::vector<Tag> TagManager::getTagsForVerse(const std::string& verseKey) const {
    return getTagsForTarget(TagTarget::verse(verseKey));
}

std::vector<Tag> TagManager::getTagsForTarget(const TagTarget& target) const {
    std::vector<Tag> result;
    auto it = targetTags_.find(targetKey(target));
    if (it != targetTags_.end()) {
        for (const auto& tagName : it->second) {
            auto tagIt = tags_.find(tagName);
            if (tagIt != tags_.end()) {
                result.push_back(tagIt->second);
            }
        }
    }
    std::sort(result.begin(), result.end(),
              [](const Tag& a, const Tag& b) { return a.name < b.name; });
    return result;
}

std::vector<std::string> TagManager::getVersesWithTag(const std::string& tagName) const {
    std::vector<std::string> result;
    auto it = tagTargets_.find(tagName);
    if (it == tagTargets_.end()) return result;

    for (const auto& key : it->second) {
        auto targetIt = targets_.find(key);
        if (targetIt != targets_.end() &&
            targetIt->second.kind == TagTarget::Kind::Verse) {
            result.push_back(targetIt->second.sourceKey);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::vector<TagTarget> TagManager::getTargetsWithTag(const std::string& tagName) const {
    std::vector<TagTarget> result;
    auto it = tagTargets_.find(tagName);
    if (it == tagTargets_.end()) return result;

    for (const auto& key : it->second) {
        auto targetIt = targets_.find(key);
        if (targetIt != targets_.end()) {
            result.push_back(targetIt->second);
        }
    }
    return result;
}

bool TagManager::verseHasTag(const std::string& verseKey,
                             const std::string& tagName) const {
    return targetHasTag(TagTarget::verse(verseKey), tagName);
}

bool TagManager::targetHasTag(const TagTarget& target, const std::string& tagName) const {
    auto it = targetTags_.find(targetKey(target));
    if (it != targetTags_.end()) {
        return it->second.count(tagName) > 0;
    }
    return false;
}

int TagManager::getTagCount(const std::string& tagName) const {
    auto it = tagTargets_.find(tagName);
    if (it == tagTargets_.end()) return 0;
    return static_cast<int>(it->second.size());
}

bool TagManager::openDatabase(const std::string& filepath) {
    if (filepath.empty()) return false;
    if (db_ && filepath_ == filepath) return true;

    sqlite3* newDb = nullptr;
    int rc = sqlite3_open_v2(
        filepath.c_str(), &newDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open tags database: " << filepath
                  << " (" << (newDb ? sqlite3_errmsg(newDb) : "unknown error")
                  << ")\n";
        if (newDb) sqlite3_close(newDb);
        return false;
    }

    applyPragmas(newDb);
    if (!ensureSchema(newDb)) {
        sqlite3_close(newDb);
        return false;
    }

    closeDatabase();
    db_ = newDb;
    filepath_ = filepath;
    return true;
}

void TagManager::closeDatabase() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool TagManager::loadFromDatabase() {
    if (!db_) return false;

    tags_.clear();
    targetTags_.clear();
    tagTargets_.clear();
    targets_.clear();

    sqlite3_stmt* tagStmt = nullptr;
    sqlite3_stmt* itemStmt = nullptr;
    sqlite3_stmt* verseStmt = nullptr;
    bool ok = true;

    if (sqlite3_prepare_v2(
            db_, "SELECT name, color FROM tags ORDER BY name;", -1, &tagStmt, nullptr) != SQLITE_OK) {
        ok = false;
    }

    int rc = SQLITE_OK;
    while (ok && (rc = sqlite3_step(tagStmt)) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(tagStmt, 0));
        const char* color = reinterpret_cast<const char*>(sqlite3_column_text(tagStmt, 1));
        if (!name || !*name) continue;

        Tag tag;
        tag.name = name;
        tag.color = color ? color : kDefaultTagColor;
        tags_[tag.name] = tag;
    }
    if (ok && rc != SQLITE_DONE) {
        ok = false;
    }

    bool loadedAnyItem = false;
    if (ok &&
        sqlite3_prepare_v2(
            db_,
            "SELECT resource_kind, module_name, source_key, selection_text, tag_name "
            "FROM tag_items ORDER BY resource_kind, module_name, source_key, selection_text, tag_name;",
            -1, &itemStmt, nullptr) == SQLITE_OK) {
        while (ok && (rc = sqlite3_step(itemStmt)) == SQLITE_ROW) {
            const char* kind = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt, 0));
            const char* module = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt, 1));
            const char* sourceKey = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt, 2));
            const char* selection = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt, 3));
            const char* tagName = reinterpret_cast<const char*>(sqlite3_column_text(itemStmt, 4));
            if (!kind || !sourceKey || !tagName) continue;
            if (tags_.find(tagName) == tags_.end()) continue;

            TagTarget target;
            target.kind = kindFromToken(kind);
            target.moduleName = module ? module : "";
            target.sourceKey = sourceKey;
            target.selectionText = selection ? selection : "";
            const std::string key = targetKey(target);
            targets_[key] = target;
            targetTags_[key].insert(tagName);
            tagTargets_[tagName].insert(key);
            loadedAnyItem = true;
        }
        if (rc != SQLITE_DONE) {
            ok = false;
        }
    }

    if (!loadedAnyItem &&
        ok &&
        sqlite3_prepare_v2(
            db_, "SELECT verse_key, tag_name FROM verse_tags ORDER BY verse_key, tag_name;",
            -1, &verseStmt, nullptr) == SQLITE_OK) {
        while (ok && (rc = sqlite3_step(verseStmt)) == SQLITE_ROW) {
            const char* verseKey = reinterpret_cast<const char*>(sqlite3_column_text(verseStmt, 0));
            const char* tagName = reinterpret_cast<const char*>(sqlite3_column_text(verseStmt, 1));
            if (!verseKey || !tagName) continue;
            if (tags_.find(tagName) == tags_.end()) continue;

            TagTarget target = TagTarget::verse(verseKey);
            const std::string key = targetKey(target);
            targets_[key] = target;
            targetTags_[key].insert(tagName);
            tagTargets_[tagName].insert(key);
        }
        if (rc != SQLITE_DONE) {
            ok = false;
        }
    }

    if (tagStmt) sqlite3_finalize(tagStmt);
    if (itemStmt) sqlite3_finalize(itemStmt);
    if (verseStmt) sqlite3_finalize(verseStmt);

    if (!ok) {
        std::cerr << "Failed to load tag data from database.\n";
        return false;
    }

    dirty_ = false;
    return true;
}

void TagManager::addToInvertedIndex(const std::string& tagName, const TagTarget& target) {
    tagTargets_[tagName].insert(targetKey(target));
}

void TagManager::removeFromInvertedIndex(const std::string& tagName, const TagTarget& target) {
    const std::string key = targetKey(target);
    auto it = tagTargets_.find(tagName);
    if (it != tagTargets_.end()) {
        it->second.erase(key);
        if (it->second.empty()) tagTargets_.erase(it);
    }
}

bool TagManager::persistToDatabase() {
    if (!db_) return false;
    if (!dirty_) return true;

    if (!execSql(db_, "BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    sqlite3_stmt* insertTag = nullptr;
    sqlite3_stmt* insertItem = nullptr;
    sqlite3_stmt* insertVerseTag = nullptr;
    bool ok = true;

    if (!execSql(db_, "CREATE TABLE IF NOT EXISTS tags_new(name TEXT PRIMARY KEY, color TEXT);")) ok = false;
    if (ok && !execSql(db_, "CREATE TABLE IF NOT EXISTS tag_items_new("
                             "resource_kind TEXT, module_name TEXT, source_key TEXT, "
                             "selection_text TEXT, tag_name TEXT);")) ok = false;
    if (ok && !execSql(db_, "CREATE TABLE IF NOT EXISTS verse_tags_new(verse_key TEXT, tag_name TEXT);")) ok = false;
    if (ok && !execSql(db_, "DELETE FROM tags_new;")) ok = false;
    if (ok && !execSql(db_, "DELETE FROM tag_items_new;")) ok = false;
    if (ok && !execSql(db_, "DELETE FROM verse_tags_new;")) ok = false;

    if (ok &&
        sqlite3_prepare_v2(
            db_, "INSERT INTO tags_new(name, color) VALUES(?, ?);", -1, &insertTag, nullptr) != SQLITE_OK) {
        ok = false;
    }
    if (ok &&
        sqlite3_prepare_v2(
            db_, "INSERT INTO tag_items_new(resource_kind, module_name, source_key, selection_text, tag_name) "
                 "VALUES(?, ?, ?, ?, ?);",
            -1, &insertItem, nullptr) != SQLITE_OK) {
        ok = false;
    }
    if (ok &&
        sqlite3_prepare_v2(
            db_, "INSERT INTO verse_tags_new(verse_key, tag_name) VALUES(?, ?);",
            -1, &insertVerseTag, nullptr) != SQLITE_OK) {
        ok = false;
    }

    for (const auto& pair : tags_) {
        if (!ok) break;
        sqlite3_reset(insertTag);
        sqlite3_clear_bindings(insertTag);
        ok = bindText(insertTag, 1, pair.second.name) &&
             bindText(insertTag, 2, pair.second.color) &&
             sqlite3_step(insertTag) == SQLITE_DONE;
    }

    for (const auto& pair : targetTags_) {
        if (!ok) break;
        auto targetIt = targets_.find(pair.first);
        if (targetIt == targets_.end()) continue;
        const TagTarget& target = targetIt->second;

        for (const auto& tagName : pair.second) {
            if (tags_.find(tagName) == tags_.end()) continue;

            sqlite3_reset(insertItem);
            sqlite3_clear_bindings(insertItem);
            ok = bindText(insertItem, 1, kindToken(target.kind)) &&
                 bindText(insertItem, 2, target.moduleName) &&
                 bindText(insertItem, 3, target.sourceKey) &&
                 bindText(insertItem, 4, target.selectionText) &&
                 bindText(insertItem, 5, tagName) &&
                 sqlite3_step(insertItem) == SQLITE_DONE;
            if (!ok) break;

            if (target.kind == TagTarget::Kind::Verse) {
                sqlite3_reset(insertVerseTag);
                sqlite3_clear_bindings(insertVerseTag);
                ok = bindText(insertVerseTag, 1, target.sourceKey) &&
                     bindText(insertVerseTag, 2, tagName) &&
                     sqlite3_step(insertVerseTag) == SQLITE_DONE;
                if (!ok) break;
            }
        }
    }

    if (insertTag) sqlite3_finalize(insertTag);
    if (insertItem) sqlite3_finalize(insertItem);
    if (insertVerseTag) sqlite3_finalize(insertVerseTag);

    if (ok) ok = execSql(db_, "DROP TABLE IF EXISTS verse_tags;");
    if (ok) ok = execSql(db_, "DROP TABLE IF EXISTS tag_items;");
    if (ok) ok = execSql(db_, "DROP TABLE IF EXISTS tags;");
    if (ok) ok = execSql(db_, "ALTER TABLE tags_new RENAME TO tags;");
    if (ok) ok = execSql(db_, "ALTER TABLE tag_items_new RENAME TO tag_items;");
    if (ok) ok = execSql(db_, "ALTER TABLE verse_tags_new RENAME TO verse_tags;");

    if (!ok) {
        execSql(db_, "ROLLBACK;");
        execSql(db_, "DROP TABLE IF EXISTS tags_new;");
        execSql(db_, "DROP TABLE IF EXISTS tag_items_new;");
        execSql(db_, "DROP TABLE IF EXISTS verse_tags_new;");
        std::cerr << "Failed to save tag data to database.\n";
        return false;
    }

    if (!execSql(db_, "COMMIT;")) {
        execSql(db_, "ROLLBACK;");
        execSql(db_, "DROP TABLE IF EXISTS tags_new;");
        execSql(db_, "DROP TABLE IF EXISTS tag_items_new;");
        execSql(db_, "DROP TABLE IF EXISTS verse_tags_new;");
        return false;
    }

    dirty_ = false;
    return true;
}

bool TagManager::hasStoredData() const {
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    bool hasData = false;

    if (sqlite3_prepare_v2(
            db_,
            "SELECT EXISTS(SELECT 1 FROM tags LIMIT 1) "
            "OR EXISTS(SELECT 1 FROM tag_items LIMIT 1) "
            "OR EXISTS(SELECT 1 FROM verse_tags LIMIT 1);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        hasData = sqlite3_column_int(stmt, 0) != 0;
    }

    sqlite3_finalize(stmt);
    return hasData;
}

bool TagManager::importLegacyFile(const std::string& legacyPath) {
    std::ifstream file(legacyPath);
    if (!file.is_open()) return false;

    tags_.clear();
    targetTags_.clear();
    tagTargets_.clear();
    targets_.clear();

    std::string line;
    std::string section;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        if (line == "[tags]") {
            section = "tags";
            continue;
        }
        if (line == "[verses]") {
            section = "verses";
            continue;
        }

        if (section == "tags") {
            size_t sep = line.find('|');
            if (sep == std::string::npos) continue;

            Tag tag;
            tag.name = line.substr(0, sep);
            tag.color = line.substr(sep + 1);
            if (tag.color.empty()) {
                tag.color = kDefaultTagColor;
            }
            if (!tag.name.empty()) {
                tags_[tag.name] = tag;
            }
            continue;
        }

        if (section == "verses") {
            size_t sep = line.find('|');
            if (sep == std::string::npos) continue;

            std::string verseKey = line.substr(0, sep);
            std::string tagList = line.substr(sep + 1);
            if (verseKey.empty()) continue;

            TagTarget target = TagTarget::verse(verseKey);
            const std::string key = targetKey(target);
            targets_[key] = target;

            std::istringstream iss(tagList);
            std::string tagName;
            while (std::getline(iss, tagName, ',')) {
                if (tagName.empty()) continue;
                if (tags_.find(tagName) == tags_.end()) {
                    tags_[tagName] = Tag{tagName, kDefaultTagColor};
                }
                targetTags_[key].insert(tagName);
                tagTargets_[tagName].insert(key);
            }
        }
    }

    dirty_ = true;
    return persistToDatabase();
}

std::string TagManager::targetKey(const TagTarget& target) {
    std::ostringstream out;
    out << kindToken(target.kind) << '|'
        << encodeSizedField(trimCopy(target.moduleName)) << '|'
        << encodeSizedField(trimCopy(target.sourceKey)) << '|'
        << encodeSizedField(trimCopy(target.selectionText));
    return out.str();
}

bool TagManager::parseTargetKey(const std::string& key, TagTarget& targetOut) {
    size_t pos = 0;
    size_t kindSep = key.find('|', pos);
    if (kindSep == std::string::npos) return false;
    targetOut.kind = kindFromToken(key.substr(pos, kindSep - pos));
    pos = kindSep + 1;

    std::string module;
    std::string sourceKey;
    std::string selectionText;
    if (!decodeSizedField(key, pos, module)) return false;
    if (pos >= key.size() || key[pos] != '|') return false;
    ++pos;
    if (!decodeSizedField(key, pos, sourceKey)) return false;
    if (pos >= key.size() || key[pos] != '|') return false;
    ++pos;
    if (!decodeSizedField(key, pos, selectionText)) return false;
    if (pos != key.size()) return false;

    targetOut.moduleName = std::move(module);
    targetOut.sourceKey = std::move(sourceKey);
    targetOut.selectionText = std::move(selectionText);
    return true;
}

} // namespace verdad
