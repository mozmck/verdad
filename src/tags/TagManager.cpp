#include "tags/TagManager.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <tuple>

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

std::string toLowerCopy(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return out;
}

bool lessNoCase(const std::string& a, const std::string& b) {
    const std::string la = toLowerCopy(a);
    const std::string lb = toLowerCopy(b);
    if (la != lb) return la < lb;
    return a < b;
}

void sortTagsByName(std::vector<Tag>& tags) {
    std::sort(tags.begin(), tags.end(),
              [](const Tag& a, const Tag& b) { return lessNoCase(a.name, b.name); });
}

bool parsePositiveInt(const std::string& text, int& out) {
    if (text.empty() || text.size() > 6) return false;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    out = std::stoi(text);
    return out > 0;
}

/// Parse "C:V" or "C". verse is 0 when only a chapter is given.
bool parseChapterVerse(const std::string& text, int& chapter, int& verse) {
    verse = 0;
    size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return parsePositiveInt(text, chapter);
    }
    return parsePositiveInt(text.substr(0, colon), chapter) &&
           parsePositiveInt(text.substr(colon + 1), verse);
}

/// Collapse en/em dashes and whitespace around separators so
/// "Genesis 1:1 – 5" parses like "Genesis 1:1-5".
std::string normalizeReferenceText(const std::string& ref) {
    std::string text = trimCopy(ref);
    const std::string dashes[] = {"\xE2\x80\x93", "\xE2\x80\x94"};
    for (const auto& dash : dashes) {
        size_t pos = 0;
        while ((pos = text.find(dash, pos)) != std::string::npos) {
            text.replace(pos, dash.size(), "-");
            ++pos;
        }
    }

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            size_t next = i;
            while (next < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[next]))) {
                ++next;
            }
            bool nearSeparator =
                (!out.empty() && (out.back() == '-' || out.back() == ':')) ||
                (next < text.size() && (text[next] == '-' || text[next] == ':'));
            if (!nearSeparator) out.push_back(' ');
            i = next - 1;
            continue;
        }
        out.push_back(c);
    }
    return out;
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

        CREATE TABLE IF NOT EXISTS tag_tree (
            tag_name TEXT PRIMARY KEY,
            parent_name TEXT NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_tag_items_tag_name
            ON tag_items(tag_name, resource_kind, module_name, source_key, selection_text);

        CREATE INDEX IF NOT EXISTS idx_verse_tags_tag_name
            ON verse_tags(tag_name, verse_key);
    )SQL";

    return execSql(db, kSchemaSql) &&
           (userVersion(db) >= 3 || setUserVersion(db, 3));
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

bool VerseRange::isSingleVerse() const {
    return startChapter == endChapter && startVerse == endVerse;
}

bool VerseRange::contains(const std::string& otherBookKey, int chapter, int verse) const {
    if (otherBookKey != bookKey) return false;
    auto point = std::make_tuple(chapter, verse);
    return point >= std::make_tuple(startChapter, startVerse) &&
           point <= std::make_tuple(endChapter, endVerse);
}

bool VerseRange::overlaps(const VerseRange& other) const {
    if (other.bookKey != bookKey) return false;
    return !(std::make_tuple(endChapter, endVerse) <
                 std::make_tuple(other.startChapter, other.startVerse) ||
             std::make_tuple(other.endChapter, other.endVerse) <
                 std::make_tuple(startChapter, startVerse));
}

bool VerseRange::startsAt(int chapter, int verse) const {
    return startChapter == chapter && startVerse == verse;
}

std::string VerseRange::toString() const {
    std::ostringstream out;
    out << book << ' ' << startChapter;
    const bool wholeChapters = startVerse == 1 && endVerse == kChapterEnd;
    if (wholeChapters) {
        if (endChapter != startChapter) out << '-' << endChapter;
        return out.str();
    }

    out << ':' << startVerse;
    if (endChapter != startChapter) {
        out << '-' << endChapter << ':' << endVerse;
    } else if (endVerse != startVerse) {
        out << '-' << endVerse;
    }
    return out.str();
}

bool VerseRange::parse(const std::string& ref, VerseRange& out) {
    const std::string text = normalizeReferenceText(ref);
    size_t lastSpace = text.rfind(' ');
    if (lastSpace == std::string::npos || lastSpace == 0) return false;

    VerseRange range;
    range.book = trimCopy(text.substr(0, lastSpace));
    range.bookKey = normalizeBookKey(range.book);
    if (range.bookKey.empty() ||
        !std::any_of(range.book.begin(), range.book.end(),
                     [](unsigned char c) { return std::isalpha(c); })) {
        return false;
    }

    const std::string spec = text.substr(lastSpace + 1);
    const size_t dash = spec.find('-');
    const std::string startText = spec.substr(0, dash);
    const std::string endText =
        dash == std::string::npos ? "" : spec.substr(dash + 1);
    if (dash != std::string::npos && endText.find('-') != std::string::npos) {
        return false;
    }

    int startChapter = 0;
    int startVerse = 0;
    if (!parseChapterVerse(startText, startChapter, startVerse)) return false;
    const bool startHasVerse = startVerse > 0;

    int endChapter = startChapter;
    int endVerse = startHasVerse ? startVerse : kChapterEnd;
    if (dash != std::string::npos) {
        int a = 0;
        int b = 0;
        if (!parseChapterVerse(endText, a, b)) return false;
        if (b > 0) {
            endChapter = a;
            endVerse = b;
        } else if (startHasVerse) {
            endVerse = a;
        } else {
            endChapter = a;
            endVerse = kChapterEnd;
        }
    }
    if (!startHasVerse) startVerse = 1;

    if (std::make_tuple(endChapter, endVerse) < std::make_tuple(startChapter, startVerse)) {
        if (startHasVerse && endVerse != kChapterEnd) {
            std::swap(startChapter, endChapter);
            std::swap(startVerse, endVerse);
        } else {
            return false;
        }
    }

    range.startChapter = startChapter;
    range.startVerse = startVerse;
    range.endChapter = endChapter;
    range.endVerse = endVerse;
    out = std::move(range);
    return true;
}

std::string VerseRange::normalizeBookKey(const std::string& book) {
    std::string out;
    out.reserve(book.size());
    for (char c : book) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || uc >= 0x80) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
}

TagTarget TagTarget::verse(const std::string& verseKey) {
    TagTarget target;
    target.kind = Kind::Verse;
    target.sourceKey = trimCopy(verseKey);

    // Canonicalize ranges ("Gen 1:5-3" -> "Gen 1:3-5", "Gen 1:1-1" -> "Gen 1:1")
    // while leaving plain single-verse keys exactly as given.
    VerseRange range;
    if (target.sourceKey.find_first_of("-\xE2") != std::string::npos &&
        VerseRange::parse(target.sourceKey, range)) {
        target.sourceKey = range.toString();
    }
    return target;
}

bool TagTarget::verseRange(VerseRange& out) const {
    return kind == Kind::Verse && VerseRange::parse(sourceKey, out);
}

bool TagTarget::isVerseRange() const {
    VerseRange range;
    return verseRange(range) && !range.isSingleVerse();
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

std::string TagTarget::identityKey() const {
    std::ostringstream out;
    out << kindToken(kind) << '|'
        << encodeSizedField(trimCopy(moduleName)) << '|'
        << encodeSizedField(trimCopy(sourceKey)) << '|'
        << encodeSizedField(trimCopy(selectionText));
    return out.str();
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
    parents_.clear();
    invalidateCaches();
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

bool TagManager::createTag(const std::string& name,
                           const std::string& color,
                           const std::string& parentName) {
    if (name.empty() || tags_.find(name) != tags_.end()) return false;

    Tag tag;
    tag.name = name;
    tag.color = color.empty() ? kDefaultTagColor : color;
    tags_[name] = tag;
    if (!parentName.empty() && parentName != name &&
        tags_.find(parentName) != tags_.end()) {
        parents_[name] = parentName;
    }
    invalidateCaches();
    dirty_ = true;
    return true;
}

void TagManager::removeTagInternal(const std::string& name) {
    tags_.erase(name);
    parents_.erase(name);

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
}

bool TagManager::deleteTag(const std::string& name, bool deleteDescendants) {
    if (tags_.find(name) == tags_.end()) return false;

    if (deleteDescendants) {
        for (const auto& descendant : descendantsOf(name)) {
            removeTagInternal(descendant);
        }
    } else {
        const std::string newParent = parentOf(name);
        for (const auto& child : childrenOf(name)) {
            if (newParent.empty()) {
                parents_.erase(child);
            } else {
                parents_[child] = newParent;
            }
        }
    }

    removeTagInternal(name);
    invalidateCaches();
    dirty_ = true;
    return true;
}

bool TagManager::renameTag(const std::string& oldName, const std::string& newName) {
    auto it = tags_.find(oldName);
    if (it == tags_.end()) return false;
    if (newName.empty() || tags_.find(newName) != tags_.end()) return false;

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

    auto parentIt = parents_.find(oldName);
    if (parentIt != parents_.end()) {
        std::string parent = parentIt->second;
        parents_.erase(parentIt);
        parents_[newName] = parent;
    }
    for (auto& pair : parents_) {
        if (pair.second == oldName) pair.second = newName;
    }

    invalidateCaches();
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

bool TagManager::hasTag(const std::string& name) const {
    return tags_.find(name) != tags_.end();
}

bool TagManager::getTag(const std::string& name, Tag& out) const {
    auto it = tags_.find(name);
    if (it == tags_.end()) return false;
    out = it->second;
    return true;
}

std::string TagManager::parentOf(const std::string& name) const {
    auto it = parents_.find(name);
    return it != parents_.end() ? it->second : std::string();
}

void TagManager::rebuildChildrenCache() const {
    childrenCache_.clear();
    for (const auto& pair : tags_) {
        childrenCache_[parentOf(pair.first)].push_back(pair.first);
    }
    for (auto& pair : childrenCache_) {
        std::sort(pair.second.begin(), pair.second.end(), lessNoCase);
    }
    childrenCacheDirty_ = false;
}

std::vector<std::string> TagManager::childrenOf(const std::string& parentName) const {
    if (childrenCacheDirty_) rebuildChildrenCache();
    auto it = childrenCache_.find(parentName);
    return it != childrenCache_.end() ? it->second : std::vector<std::string>{};
}

bool TagManager::hasChildren(const std::string& name) const {
    if (childrenCacheDirty_) rebuildChildrenCache();
    auto it = childrenCache_.find(name);
    return it != childrenCache_.end() && !it->second.empty();
}

std::vector<std::string> TagManager::descendantsOf(const std::string& name) const {
    std::vector<std::string> result;
    std::function<void(const std::string&)> visit = [&](const std::string& parent) {
        for (const auto& child : childrenOf(parent)) {
            result.push_back(child);
            visit(child);
        }
    };
    if (!name.empty()) visit(name);
    return result;
}

bool TagManager::isDescendantOf(const std::string& name, const std::string& ancestor) const {
    if (ancestor.empty()) return false;
    std::string current = parentOf(name);
    size_t guard = 0;
    while (!current.empty() && guard++ <= tags_.size()) {
        if (current == ancestor) return true;
        current = parentOf(current);
    }
    return false;
}

bool TagManager::setParent(const std::string& name, const std::string& parentName) {
    if (tags_.find(name) == tags_.end()) return false;
    if (parentName == parentOf(name)) return true;
    if (!parentName.empty()) {
        if (parentName == name) return false;
        if (tags_.find(parentName) == tags_.end()) return false;
        if (isDescendantOf(parentName, name)) return false;
        parents_[name] = parentName;
    } else {
        parents_.erase(name);
    }
    invalidateCaches();
    dirty_ = true;
    return true;
}

std::string TagManager::tagPath(const std::string& name, const std::string& separator) const {
    std::vector<std::string> parts{name};
    std::string current = parentOf(name);
    size_t guard = 0;
    while (!current.empty() && guard++ <= tags_.size()) {
        parts.push_back(current);
        current = parentOf(current);
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) path += separator;
        path += *it;
    }
    return path;
}

void TagManager::validateHierarchy() {
    for (auto it = parents_.begin(); it != parents_.end();) {
        if (tags_.find(it->first) == tags_.end() ||
            tags_.find(it->second) == tags_.end() ||
            it->first == it->second) {
            it = parents_.erase(it);
        } else {
            ++it;
        }
    }

    // Break any cycles by detaching the tag where the cycle is detected.
    for (const auto& pair : tags_) {
        std::set<std::string> seen{pair.first};
        std::string current = parentOf(pair.first);
        std::string child = pair.first;
        while (!current.empty()) {
            if (!seen.insert(current).second) {
                parents_.erase(child);
                break;
            }
            child = current;
            current = parentOf(current);
        }
    }
    invalidateCaches();
}

void TagManager::invalidateCaches() {
    childrenCacheDirty_ = true;
    rangeIndexDirty_ = true;
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
        rangeIndexDirty_ = true;
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
    rangeIndexDirty_ = true;
}

std::vector<Tag> TagManager::getAllTags() const {
    std::vector<Tag> result;
    for (const auto& pair : tags_) {
        result.push_back(pair.second);
    }
    sortTagsByName(result);
    return result;
}

std::vector<Tag> TagManager::getTagsForVerse(const std::string& verseKey) const {
    VerseTagCoverage coverage = getVerseTagCoverage(verseKey);
    if (coverage.empty()) return getTagsForTarget(TagTarget::verse(verseKey));

    std::vector<Tag> result = std::move(coverage.starting);
    result.insert(result.end(), coverage.continuing.begin(), coverage.continuing.end());
    sortTagsByName(result);
    return result;
}

void TagManager::rebuildRangeIndex() const {
    rangeIndex_.clear();
    for (const auto& pair : targets_) {
        VerseRange range;
        if (!pair.second.verseRange(range)) continue;
        rangeIndex_[range.bookKey].push_back(RangeIndexEntry{range, pair.first});
    }
    rangeIndexDirty_ = false;
}

VerseTagCoverage TagManager::getVerseTagCoverage(const std::string& verseKey) const {
    VerseTagCoverage coverage;
    VerseRange verse;
    if (!VerseRange::parse(verseKey, verse)) return coverage;

    if (rangeIndexDirty_) rebuildRangeIndex();
    auto bookIt = rangeIndex_.find(verse.bookKey);
    if (bookIt == rangeIndex_.end()) return coverage;

    std::set<std::string> startingNames;
    std::set<std::string> continuingNames;
    std::set<std::string> startingRanges;
    for (const auto& entry : bookIt->second) {
        if (!entry.range.contains(verse.bookKey, verse.startChapter, verse.startVerse)) {
            continue;
        }
        auto tagsIt = targetTags_.find(entry.targetKey);
        if (tagsIt == targetTags_.end()) continue;

        const bool starts = entry.range.startsAt(verse.startChapter, verse.startVerse);
        if (starts && !entry.range.isSingleVerse()) {
            auto targetIt = targets_.find(entry.targetKey);
            if (targetIt != targets_.end()) startingRanges.insert(targetIt->second.sourceKey);
        }
        for (const auto& tagName : tagsIt->second) {
            (starts ? startingNames : continuingNames).insert(tagName);
        }
    }

    for (const auto& name : startingNames) {
        auto it = tags_.find(name);
        if (it != tags_.end()) coverage.starting.push_back(it->second);
    }
    for (const auto& name : continuingNames) {
        if (startingNames.count(name)) continue;
        auto it = tags_.find(name);
        if (it != tags_.end()) coverage.continuing.push_back(it->second);
    }
    sortTagsByName(coverage.starting);
    sortTagsByName(coverage.continuing);
    coverage.startingRanges.assign(startingRanges.begin(), startingRanges.end());
    return coverage;
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
    sortTagsByName(result);
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

std::vector<TagTarget> TagManager::getTargetsWithTag(const std::string& tagName,
                                                     bool includeDescendants) const {
    std::vector<TagTarget> result;
    std::set<std::string> seen;
    auto collect = [&](const std::string& name) {
        auto it = tagTargets_.find(name);
        if (it == tagTargets_.end()) return;
        for (const auto& key : it->second) {
            if (!seen.insert(key).second) continue;
            auto targetIt = targets_.find(key);
            if (targetIt != targets_.end()) {
                result.push_back(targetIt->second);
            }
        }
    };

    collect(tagName);
    if (includeDescendants) {
        for (const auto& descendant : descendantsOf(tagName)) {
            collect(descendant);
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

int TagManager::getTagCount(const std::string& tagName, bool includeDescendants) const {
    if (!includeDescendants) {
        auto it = tagTargets_.find(tagName);
        if (it == tagTargets_.end()) return 0;
        return static_cast<int>(it->second.size());
    }

    std::set<std::string> keys;
    auto collect = [&](const std::string& name) {
        auto it = tagTargets_.find(name);
        if (it != tagTargets_.end()) keys.insert(it->second.begin(), it->second.end());
    };
    collect(tagName);
    for (const auto& descendant : descendantsOf(tagName)) {
        collect(descendant);
    }
    return static_cast<int>(keys.size());
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
    parents_.clear();
    invalidateCaches();

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

    sqlite3_stmt* treeStmt = nullptr;
    if (ok &&
        sqlite3_prepare_v2(
            db_, "SELECT tag_name, parent_name FROM tag_tree ORDER BY tag_name;",
            -1, &treeStmt, nullptr) == SQLITE_OK) {
        while (ok && (rc = sqlite3_step(treeStmt)) == SQLITE_ROW) {
            const char* tagName = reinterpret_cast<const char*>(sqlite3_column_text(treeStmt, 0));
            const char* parentName = reinterpret_cast<const char*>(sqlite3_column_text(treeStmt, 1));
            if (!tagName || !*tagName || !parentName || !*parentName) continue;
            parents_[tagName] = parentName;
        }
        if (rc != SQLITE_DONE) {
            ok = false;
        }
    }

    if (tagStmt) sqlite3_finalize(tagStmt);
    if (itemStmt) sqlite3_finalize(itemStmt);
    if (verseStmt) sqlite3_finalize(verseStmt);
    if (treeStmt) sqlite3_finalize(treeStmt);

    if (!ok) {
        std::cerr << "Failed to load tag data from database.\n";
        return false;
    }

    // Drop hierarchy rows that reference missing tags (for example after an
    // older build rewrote the tag tables) and break any cycles.
    validateHierarchy();

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

    // The hierarchy lives in its own table so older builds, which rewrite
    // only tags/tag_items/verse_tags, leave it intact.
    sqlite3_stmt* insertTree = nullptr;
    if (ok) ok = execSql(db_, "CREATE TABLE IF NOT EXISTS tag_tree("
                              "tag_name TEXT PRIMARY KEY, parent_name TEXT NOT NULL);");
    if (ok) ok = execSql(db_, "DELETE FROM tag_tree;");
    if (ok &&
        sqlite3_prepare_v2(
            db_, "INSERT INTO tag_tree(tag_name, parent_name) VALUES(?, ?);",
            -1, &insertTree, nullptr) != SQLITE_OK) {
        ok = false;
    }
    for (const auto& pair : parents_) {
        if (!ok) break;
        if (tags_.find(pair.first) == tags_.end() ||
            tags_.find(pair.second) == tags_.end()) {
            continue;
        }
        sqlite3_reset(insertTree);
        sqlite3_clear_bindings(insertTree);
        ok = bindText(insertTree, 1, pair.first) &&
             bindText(insertTree, 2, pair.second) &&
             sqlite3_step(insertTree) == SQLITE_DONE;
    }
    if (insertTree) sqlite3_finalize(insertTree);

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
    parents_.clear();
    invalidateCaches();

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
    return target.identityKey();
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
