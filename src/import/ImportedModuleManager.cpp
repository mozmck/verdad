#include "import/ImportedModuleManager.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace verdad {
namespace {

namespace fs = std::filesystem;

bool execSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

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

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string collapseWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool prevSpace = true;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc)) {
            if (!prevSpace) out.push_back(' ');
            prevSpace = true;
        } else {
            out.push_back(c);
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t start = 0;
    while (start < out.size() && out[start] == ' ') ++start;
    return out.substr(start);
}

std::string readFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string shellQuote(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string detectExtension(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    return toLowerAscii(ext);
}

bool supportedImportExtension(const std::string& ext) {
    return ext == ".pdf" || ext == ".txt" || ext == ".md" || ext == ".markdown";
}

std::string makeModuleName(const std::string& displayName,
                           const std::unordered_set<std::string>& usedNames) {
    std::string base = trimCopy(displayName);
    if (base.empty()) base = "Imported Document";
    if (usedNames.count(base) == 0) return base;

    for (int suffix = 2; suffix < 10000; ++suffix) {
        std::string candidate = base + " (" + std::to_string(suffix) + ")";
        if (usedNames.count(candidate) == 0) return candidate;
    }
    return base + " " + std::to_string(static_cast<int>(usedNames.size()) + 1);
}

std::string fnv1a64(const std::string& text) {
    unsigned long long hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

std::string sanitizeKeyComponent(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (c == ' ' || c == '-' || c == '_') {
            if (out.empty() || out.back() == '-') continue;
            out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "section" : out;
}

std::string textBlockHtml(const std::string& text) {
    std::ostringstream html;
    html << "<div class=\"general-book imported-document\">\n";

    std::istringstream in(text);
    std::string line;
    std::vector<std::string> paragraph;
    auto flushParagraph = [&]() {
        if (paragraph.empty()) return;
        std::ostringstream joined;
        for (size_t i = 0; i < paragraph.size(); ++i) {
            if (i > 0) joined << ' ';
            joined << trimCopy(paragraph[i]);
        }
        html << "<p>" << htmlEscape(joined.str()) << "</p>\n";
        paragraph.clear();
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trimCopy(line).empty()) {
            flushParagraph();
            continue;
        }
        paragraph.push_back(line);
    }
    flushParagraph();
    html << "</div>\n";
    return html.str();
}

struct MarkdownParseResult {
    std::vector<ImportedModuleManager::Entry> entries;
    std::string fullPlainText;
};

MarkdownParseResult parseMarkdownDocument(const std::string& text) {
    MarkdownParseResult result;
    std::istringstream in(text);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }

    struct SectionBuilder {
        std::string title;
        int depth = 0;
        std::vector<std::string> lines;
        int orderIndex = 0;
    };

    std::vector<SectionBuilder> sections;
    SectionBuilder current;
    current.title = "Document";
    current.depth = 0;
    current.orderIndex = 0;

    bool inCodeBlock = false;
    int orderIndex = 0;
    for (const auto& rawLine : lines) {
        std::string trimmed = trimCopy(rawLine);
        if (trimmed.rfind("```", 0) == 0) {
            inCodeBlock = !inCodeBlock;
            current.lines.push_back(rawLine);
            continue;
        }

        if (!inCodeBlock) {
            size_t hashCount = 0;
            while (hashCount < rawLine.size() && rawLine[hashCount] == '#') {
                ++hashCount;
            }
            if (hashCount > 0 && hashCount <= 6 &&
                hashCount < rawLine.size() &&
                std::isspace(static_cast<unsigned char>(rawLine[hashCount]))) {
                if (!current.lines.empty() || sections.empty()) {
                    sections.push_back(current);
                }
                current = {};
                current.title = trimCopy(rawLine.substr(hashCount));
                current.depth = static_cast<int>(hashCount) - 1;
                current.orderIndex = ++orderIndex;
                current.lines.push_back(rawLine);
                continue;
            }
        }

        current.lines.push_back(rawLine);
    }
    if (!current.lines.empty() || sections.empty()) {
        sections.push_back(current);
    }

    std::vector<int> depths;
    depths.reserve(sections.size());
    for (const auto& section : sections) {
        depths.push_back(section.depth);
    }

    auto markdownLinesToHtml = [](const std::vector<std::string>& sectionLines) {
        std::ostringstream html;
        html << "<div class=\"general-book imported-document markdown-document\">\n";
        bool inCode = false;
        bool inList = false;
        std::vector<std::string> paragraph;

        auto flushParagraph = [&]() {
            if (paragraph.empty()) return;
            std::ostringstream joined;
            for (size_t i = 0; i < paragraph.size(); ++i) {
                if (i > 0) joined << ' ';
                joined << trimCopy(paragraph[i]);
            }
            html << "<p>" << htmlEscape(joined.str()) << "</p>\n";
            paragraph.clear();
        };
        auto closeList = [&]() {
            if (!inList) return;
            html << "</ul>\n";
            inList = false;
        };

        for (const auto& rawLine : sectionLines) {
            std::string trimmed = trimCopy(rawLine);
            if (trimmed.rfind("```", 0) == 0) {
                flushParagraph();
                closeList();
                if (!inCode) {
                    html << "<pre><code>";
                    inCode = true;
                } else {
                    html << "</code></pre>\n";
                    inCode = false;
                }
                continue;
            }
            if (inCode) {
                html << htmlEscape(rawLine) << "\n";
                continue;
            }

            size_t headingCount = 0;
            while (headingCount < rawLine.size() && rawLine[headingCount] == '#') {
                ++headingCount;
            }
            if (headingCount > 0 && headingCount <= 6 &&
                headingCount < rawLine.size() &&
                std::isspace(static_cast<unsigned char>(rawLine[headingCount]))) {
                flushParagraph();
                closeList();
                std::string headingText = trimCopy(rawLine.substr(headingCount));
                html << "<h" << headingCount << ">" << htmlEscape(headingText)
                     << "</h" << headingCount << ">\n";
                continue;
            }

            if (trimmed.empty()) {
                flushParagraph();
                closeList();
                continue;
            }

            if (trimmed.rfind("- ", 0) == 0 || trimmed.rfind("* ", 0) == 0) {
                flushParagraph();
                if (!inList) {
                    html << "<ul>\n";
                    inList = true;
                }
                html << "<li>" << htmlEscape(trimCopy(trimmed.substr(2))) << "</li>\n";
                continue;
            }

            closeList();
            paragraph.push_back(rawLine);
        }

        flushParagraph();
        closeList();
        if (inCode) html << "</code></pre>\n";
        html << "</div>\n";
        return html.str();
    };

    for (size_t i = 0; i < sections.size(); ++i) {
        ImportedModuleManager::Entry entry;
        entry.title = trimCopy(sections[i].title);
        if (entry.title.empty()) entry.title = "Document";
        entry.depth = std::max(0, sections[i].depth);
        entry.orderIndex = static_cast<int>(i);
        entry.key = std::to_string(i + 1) + "-" + sanitizeKeyComponent(entry.title);
        entry.html = markdownLinesToHtml(sections[i].lines);
        std::ostringstream plain;
        for (const auto& sectionLine : sections[i].lines) {
            plain << sectionLine << "\n";
        }
        entry.plainText = collapseWhitespace(plain.str());
        result.entries.push_back(std::move(entry));
    }

    for (size_t i = 0; i < result.entries.size(); ++i) {
        int depth = result.entries[i].depth;
        bool hasChildren = false;
        for (size_t j = i + 1; j < result.entries.size(); ++j) {
            if (result.entries[j].depth <= depth) break;
            hasChildren = true;
            break;
        }
        result.entries[i].hasChildren = hasChildren;
        if (!result.fullPlainText.empty()) result.fullPlainText += "\n";
        result.fullPlainText += result.entries[i].plainText;
    }

    return result;
}

bool runPdftotext(const std::string& path,
                  std::string& outText,
                  std::string& errorText) {
    std::string command = "pdftotext -layout -enc UTF-8 " +
                          shellQuote(path) + " - 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        errorText = "Failed to start pdftotext.";
        return false;
    }

    std::array<char, 4096> buffer{};
    std::ostringstream out;
    while (true) {
        size_t read = std::fread(buffer.data(), 1, buffer.size(), pipe);
        if (read > 0) out.write(buffer.data(), static_cast<std::streamsize>(read));
        if (read < buffer.size()) break;
    }

    int rc = pclose(pipe);
    outText = out.str();
    if (rc != 0 || trimCopy(outText).empty()) {
        errorText = "pdftotext is unavailable or produced no text.";
        return false;
    }
    return true;
}

std::vector<ImportedModuleManager::Entry> parsePdfDocument(const std::string& text) {
    std::vector<ImportedModuleManager::Entry> entries;
    size_t start = 0;
    int pageNumber = 1;
    while (start <= text.size()) {
        size_t end = text.find('\f', start);
        std::string page = (end == std::string::npos)
            ? text.substr(start)
            : text.substr(start, end - start);
        page = trimCopy(page);
        if (!page.empty()) {
            ImportedModuleManager::Entry entry;
            entry.key = "page-" + std::to_string(pageNumber);
            entry.title = "Page " + std::to_string(pageNumber);
            entry.depth = 0;
            entry.orderIndex = static_cast<int>(entries.size());
            entry.plainText = collapseWhitespace(page);
            entry.html = textBlockHtml(page);
            entries.push_back(std::move(entry));
        }
        if (end == std::string::npos) break;
        start = end + 1;
        ++pageNumber;
    }

    if (entries.empty()) {
        ImportedModuleManager::Entry entry;
        entry.key = "document";
        entry.title = "Document";
        entry.depth = 0;
        entry.orderIndex = 0;
        entry.plainText = collapseWhitespace(text);
        entry.html = textBlockHtml(text);
        entries.push_back(std::move(entry));
    }

    return entries;
}

std::vector<ImportedModuleManager::Entry> parsePlainTextDocument(const std::string& text) {
    ImportedModuleManager::Entry entry;
    entry.key = "document";
    entry.title = "Document";
    entry.depth = 0;
    entry.orderIndex = 0;
    entry.plainText = collapseWhitespace(text);
    entry.html = textBlockHtml(text);
    return {std::move(entry)};
}

std::string storageFileName(const std::string& sourcePath,
                            const std::string& contentHash) {
    std::string ext = detectExtension(sourcePath);
    if (ext.empty()) ext = ".bin";
    return contentHash + ext;
}

long long fileSizeOrZero(const std::string& path) {
    std::error_code ec;
    auto size = fs::file_size(fs::path(path), ec);
    return ec ? 0 : static_cast<long long>(size);
}

long long fileMtimeOrZero(const std::string& path) {
    std::error_code ec;
    auto value = fs::last_write_time(fs::path(path), ec);
    if (ec) return 0;
    auto sinceEpoch = value.time_since_epoch().count();
    return static_cast<long long>(sinceEpoch);
}

ModuleInfo buildImportedModuleInfo(const ImportedModuleManager::ModuleRecord& record) {
    ModuleInfo info;
    info.name = record.moduleName;
    info.description = record.displayName;
    info.type = "General Books";
    info.language = "en";
    info.markup = record.fileType;
    info.category = "Imported Files";
    info.version = record.contentHash.substr(0, std::min<size_t>(record.contentHash.size(), 12));
    std::ostringstream about;
    about << "<div class=\"module-preview-about\">";
    about << "<p><b>Source:</b> " << htmlEscape(record.sourcePath) << "</p>";
    if (record.copiedToLibrary && !record.storedPath.empty()) {
        about << "<p><b>Library copy:</b> " << htmlEscape(record.storedPath) << "</p>";
    }
    if (!trimCopy(record.tags).empty()) {
        about << "<p><b>Tags:</b> " << htmlEscape(record.tags) << "</p>";
    }
    if (!trimCopy(record.notes).empty()) {
        about << "<p><b>Notes:</b> " << htmlEscape(record.notes) << "</p>";
    }
    about << "</div>";
    info.aboutHtml = about.str();
    if (!trimCopy(record.tags).empty()) {
        info.featureLabels.push_back("Tags: " + trimCopy(record.tags));
    }
    if (!trimCopy(record.notes).empty()) {
        info.featureLabels.push_back("Has notes");
    }
    return info;
}

} // namespace

int ImportedModuleManager::BatchImportResult::importedCount() const {
    return static_cast<int>(std::count_if(files.begin(), files.end(),
        [](const FileImportResult& item) { return item.imported; }));
}

int ImportedModuleManager::BatchImportResult::updatedCount() const {
    return static_cast<int>(std::count_if(files.begin(), files.end(),
        [](const FileImportResult& item) { return item.updated; }));
}

int ImportedModuleManager::BatchImportResult::skippedCount() const {
    return static_cast<int>(std::count_if(files.begin(), files.end(),
        [](const FileImportResult& item) { return item.skipped; }));
}

int ImportedModuleManager::BatchImportResult::failureCount() const {
    int failed = 0;
    for (const auto& item : files) {
        if (!item.imported && !item.updated && !item.skipped) ++failed;
    }
    return failed;
}

ImportedModuleManager::ImportedModuleManager() = default;

ImportedModuleManager::~ImportedModuleManager() {
    close();
}

bool ImportedModuleManager::load(const std::string& dbPath,
                                 const std::string& storageDir) {
    dbPath_ = dbPath;
    storageDir_ = storageDir;

    std::error_code ec;
    fs::create_directories(fs::path(storageDir_), ec);
    if (!openDatabase(dbPath_)) return false;
    if (!ensureSchema()) return false;
    return reloadCache();
}

void ImportedModuleManager::close() {
    modulesByName_.clear();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool ImportedModuleManager::openDatabase(const std::string& dbPath) {
    if (db_ && dbPath == dbPath_) return true;
    close();

    sqlite3* newDb = nullptr;
    int rc = sqlite3_open_v2(
        dbPath.c_str(), &newDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK || !newDb) {
        if (newDb) sqlite3_close(newDb);
        return false;
    }

    sqlite3_busy_timeout(newDb, 5000);
    execSql(newDb, "PRAGMA foreign_keys=ON;");
    execSql(newDb, "PRAGMA journal_mode=WAL;");
    execSql(newDb, "PRAGMA synchronous=NORMAL;");
    db_ = newDb;
    return true;
}

bool ImportedModuleManager::ensureSchema() {
    if (!db_) return false;
    static const char* kSchema = R"SQL(
        CREATE TABLE IF NOT EXISTS imported_modules (
            module_name TEXT PRIMARY KEY,
            display_name TEXT NOT NULL,
            source_path TEXT NOT NULL,
            stored_path TEXT NOT NULL,
            file_type TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            tags TEXT NOT NULL DEFAULT '',
            notes TEXT NOT NULL DEFAULT '',
            copied_to_library INTEGER NOT NULL DEFAULT 0,
            file_size INTEGER NOT NULL DEFAULT 0,
            modified_time INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS imported_entries (
            module_name TEXT NOT NULL,
            entry_key TEXT NOT NULL,
            title TEXT NOT NULL,
            html TEXT NOT NULL,
            plain_text TEXT NOT NULL,
            depth INTEGER NOT NULL DEFAULT 0,
            has_children INTEGER NOT NULL DEFAULT 0,
            order_index INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (module_name, entry_key),
            FOREIGN KEY (module_name) REFERENCES imported_modules(module_name)
                ON DELETE CASCADE
        );

        CREATE INDEX IF NOT EXISTS idx_imported_entries_module_order
            ON imported_entries(module_name, order_index);
    )SQL";

    return execSql(db_, kSchema);
}

bool ImportedModuleManager::reloadCache() {
    modulesByName_.clear();
    if (!db_) return false;

    sqlite3_stmt* moduleStmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT module_name, display_name, source_path, stored_path, file_type, "
            "content_hash, tags, notes, copied_to_library, file_size, modified_time "
            "FROM imported_modules ORDER BY display_name COLLATE NOCASE, module_name",
            -1, &moduleStmt, nullptr) != SQLITE_OK) {
        return false;
    }

    while (sqlite3_step(moduleStmt) == SQLITE_ROW) {
        ModuleRecord record;
        auto readText = [&](int index) {
            const char* value = reinterpret_cast<const char*>(sqlite3_column_text(moduleStmt, index));
            return std::string(value ? value : "");
        };
        record.moduleName = readText(0);
        record.displayName = readText(1);
        record.sourcePath = readText(2);
        record.storedPath = readText(3);
        record.fileType = readText(4);
        record.contentHash = readText(5);
        record.tags = readText(6);
        record.notes = readText(7);
        record.copiedToLibrary = sqlite3_column_int(moduleStmt, 8) != 0;
        record.fileSize = sqlite3_column_int64(moduleStmt, 9);
        record.modifiedTime = sqlite3_column_int64(moduleStmt, 10);
        record.info = buildImportedModuleInfo(record);
        modulesByName_.emplace(record.moduleName, std::move(record));
    }
    sqlite3_finalize(moduleStmt);

    sqlite3_stmt* entryStmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT module_name, entry_key, title, html, plain_text, depth, has_children, order_index "
            "FROM imported_entries ORDER BY module_name, order_index",
            -1, &entryStmt, nullptr) != SQLITE_OK) {
        return false;
    }

    while (sqlite3_step(entryStmt) == SQLITE_ROW) {
        const char* moduleName = reinterpret_cast<const char*>(sqlite3_column_text(entryStmt, 0));
        if (!moduleName) continue;
        auto moduleIt = modulesByName_.find(moduleName);
        if (moduleIt == modulesByName_.end()) continue;

        Entry entry;
        auto readEntryText = [&](int index) {
            const char* value = reinterpret_cast<const char*>(sqlite3_column_text(entryStmt, index));
            return std::string(value ? value : "");
        };
        entry.key = readEntryText(1);
        entry.title = readEntryText(2);
        entry.html = readEntryText(3);
        entry.plainText = readEntryText(4);
        entry.depth = sqlite3_column_int(entryStmt, 5);
        entry.hasChildren = sqlite3_column_int(entryStmt, 6) != 0;
        entry.orderIndex = sqlite3_column_int(entryStmt, 7);
        moduleIt->second.entries.push_back(std::move(entry));
    }
    sqlite3_finalize(entryStmt);

    return true;
}

std::vector<ModuleInfo> ImportedModuleManager::modules() const {
    std::vector<ModuleInfo> result;
    result.reserve(modulesByName_.size());
    for (const auto& [name, record] : modulesByName_) {
        (void)name;
        result.push_back(record.info);
    }
    std::sort(result.begin(), result.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  std::string left = toLowerAscii(a.name);
                  std::string right = toLowerAscii(b.name);
                  if (left != right) return left < right;
                  return a.name < b.name;
              });
    return result;
}

bool ImportedModuleManager::hasModule(const std::string& moduleName) const {
    return modulesByName_.find(moduleName) != modulesByName_.end();
}

ModuleInfo ImportedModuleManager::moduleInfo(const std::string& moduleName) const {
    auto it = modulesByName_.find(moduleName);
    return it != modulesByName_.end() ? it->second.info : ModuleInfo{};
}

std::vector<GeneralBookTocEntry> ImportedModuleManager::toc(const std::string& moduleName) const {
    std::vector<GeneralBookTocEntry> out;
    auto it = modulesByName_.find(moduleName);
    if (it == modulesByName_.end()) return out;
    out.reserve(it->second.entries.size());
    for (const auto& entry : it->second.entries) {
        GeneralBookTocEntry tocEntry;
        tocEntry.key = entry.key;
        tocEntry.label = entry.title;
        tocEntry.depth = entry.depth;
        tocEntry.hasChildren = entry.hasChildren;
        out.push_back(std::move(tocEntry));
    }
    return out;
}

std::string ImportedModuleManager::entryHtml(const std::string& moduleName,
                                             const std::string& key) const {
    auto it = modulesByName_.find(moduleName);
    if (it == modulesByName_.end()) return "";
    for (const auto& entry : it->second.entries) {
        if (entry.key == key) return entry.html;
    }
    if (!it->second.entries.empty()) return it->second.entries.front().html;
    return "";
}

std::vector<ImportedModuleManager::Entry> ImportedModuleManager::indexEntries(
        const std::string& moduleName) const {
    auto it = modulesByName_.find(moduleName);
    return it != modulesByName_.end() ? it->second.entries : std::vector<Entry>{};
}

std::vector<ImportedModuleManager::ModuleRecord> ImportedModuleManager::records() const {
    std::vector<ModuleRecord> out;
    out.reserve(modulesByName_.size());
    for (const auto& [name, record] : modulesByName_) {
        (void)name;
        out.push_back(record);
    }
    std::sort(out.begin(), out.end(),
              [](const ModuleRecord& a, const ModuleRecord& b) {
                  std::string left = toLowerAscii(a.displayName);
                  std::string right = toLowerAscii(b.displayName);
                  if (left != right) return left < right;
                  return a.moduleName < b.moduleName;
              });
    return out;
}

ImportedModuleManager::BatchImportResult ImportedModuleManager::importPaths(
        const std::vector<std::string>& paths,
        const ImportOptions& options) {
    BatchImportResult batch;
    if (!db_) return batch;

    std::unordered_set<std::string> usedNames;
    for (const auto& [name, record] : modulesByName_) {
        (void)record;
        usedNames.insert(name);
    }

    for (const auto& rawPath : paths) {
        FileImportResult result;
        result.path = rawPath;

        std::error_code ec;
        fs::path fsPath(rawPath);
        fs::path canonical = fs::weakly_canonical(fsPath, ec);
        std::string normalizedPath = (ec ? fsPath.lexically_normal() : canonical).string();
        std::string ext = detectExtension(normalizedPath);

        if (!supportedImportExtension(ext)) {
            result.message = "Unsupported file type.";
            batch.files.push_back(std::move(result));
            continue;
        }
        if (!fs::exists(fsPath, ec) || ec) {
            result.message = "File not found.";
            batch.files.push_back(std::move(result));
            continue;
        }

        std::string sourceText;
        if (ext == ".pdf") {
            std::string error;
            if (!runPdftotext(normalizedPath, sourceText, error)) {
                result.message = error;
                batch.files.push_back(std::move(result));
                continue;
            }
        } else {
            sourceText = readFileText(normalizedPath);
            if (sourceText.empty() && fileSizeOrZero(normalizedPath) > 0) {
                result.message = "Failed to read file contents.";
                batch.files.push_back(std::move(result));
                continue;
            }
        }

        std::vector<Entry> entries;
        if (ext == ".pdf") {
            entries = parsePdfDocument(sourceText);
        } else if (ext == ".md" || ext == ".markdown") {
            auto parsed = parseMarkdownDocument(sourceText);
            entries = std::move(parsed.entries);
        } else {
            entries = parsePlainTextDocument(sourceText);
        }

        if (entries.empty()) {
            result.message = "No importable text was extracted.";
            batch.files.push_back(std::move(result));
            continue;
        }

        std::string contentHash = fnv1a64(sourceText);
        std::string existingModule;
        std::string previousStoredPath;
        bool previousCopied = false;
        bool duplicateHash = false;
        for (const auto& [moduleName, record] : modulesByName_) {
            if (record.sourcePath == normalizedPath) {
                existingModule = moduleName;
                previousStoredPath = record.storedPath;
                previousCopied = record.copiedToLibrary;
                break;
            }
            if (record.contentHash == contentHash &&
                toLowerAscii(fs::path(record.sourcePath).filename().string()) ==
                    toLowerAscii(fs::path(normalizedPath).filename().string())) {
                duplicateHash = true;
            }
        }

        if (existingModule.empty() && duplicateHash) {
            result.skipped = true;
            result.message = "Duplicate content already imported.";
            batch.files.push_back(std::move(result));
            continue;
        }

        std::string displayName = fs::path(normalizedPath).stem().string();
        std::string moduleName = existingModule.empty()
            ? makeModuleName(displayName, usedNames)
            : existingModule;
        usedNames.insert(moduleName);

        std::string storedPath;
        if (options.copyFiles) {
            std::error_code copyEc;
            fs::create_directories(fs::path(storageDir_), copyEc);
            storedPath = (fs::path(storageDir_) /
                         storageFileName(normalizedPath, contentHash)).string();
            fs::copy_file(fs::path(normalizedPath), fs::path(storedPath),
                          fs::copy_options::overwrite_existing, copyEc);
            if (copyEc) {
                result.message = "Failed to copy file into imports directory.";
                batch.files.push_back(std::move(result));
                continue;
            }
        }

        sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

        sqlite3_stmt* deleteEntries = nullptr;
        sqlite3_stmt* upsertModule = nullptr;
        sqlite3_stmt* insertEntry = nullptr;
        bool prepared =
            sqlite3_prepare_v2(db_,
                               "DELETE FROM imported_entries WHERE module_name = ?",
                               -1, &deleteEntries, nullptr) == SQLITE_OK &&
            sqlite3_prepare_v2(
                db_,
                "INSERT OR REPLACE INTO imported_modules("
                "module_name, display_name, source_path, stored_path, file_type, content_hash, "
                "tags, notes, copied_to_library, file_size, modified_time, updated_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)",
                -1, &upsertModule, nullptr) == SQLITE_OK &&
            sqlite3_prepare_v2(
                db_,
                "INSERT INTO imported_entries("
                "module_name, entry_key, title, html, plain_text, depth, has_children, order_index) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                -1, &insertEntry, nullptr) == SQLITE_OK;

        bool ok = prepared;
        if (ok) {
            bindText(deleteEntries, 1, moduleName);
            ok = sqlite3_step(deleteEntries) == SQLITE_DONE;
        }

        if (ok) {
            bindText(upsertModule, 1, moduleName);
            bindText(upsertModule, 2, displayName);
            bindText(upsertModule, 3, normalizedPath);
            bindText(upsertModule, 4, storedPath);
            bindText(upsertModule, 5, ext == ".markdown" ? ".md" : ext);
            bindText(upsertModule, 6, contentHash);
            bindText(upsertModule, 7, options.tags);
            bindText(upsertModule, 8, options.notes);
            sqlite3_bind_int(upsertModule, 9, options.copyFiles ? 1 : 0);
            sqlite3_bind_int64(upsertModule, 10, fileSizeOrZero(normalizedPath));
            sqlite3_bind_int64(upsertModule, 11, fileMtimeOrZero(normalizedPath));
            ok = sqlite3_step(upsertModule) == SQLITE_DONE;
        }

        for (size_t i = 0; ok && i < entries.size(); ++i) {
            const auto& entry = entries[i];
            bindText(insertEntry, 1, moduleName);
            bindText(insertEntry, 2, entry.key);
            bindText(insertEntry, 3, entry.title);
            bindText(insertEntry, 4, entry.html);
            bindText(insertEntry, 5, entry.plainText);
            sqlite3_bind_int(insertEntry, 6, entry.depth);
            sqlite3_bind_int(insertEntry, 7, entry.hasChildren ? 1 : 0);
            sqlite3_bind_int(insertEntry, 8, static_cast<int>(i));
            ok = sqlite3_step(insertEntry) == SQLITE_DONE;
            sqlite3_reset(insertEntry);
            sqlite3_clear_bindings(insertEntry);
        }

        if (deleteEntries) sqlite3_finalize(deleteEntries);
        if (upsertModule) sqlite3_finalize(upsertModule);
        if (insertEntry) sqlite3_finalize(insertEntry);

        if (ok) {
            sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
            if (!existingModule.empty() &&
                previousCopied &&
                !previousStoredPath.empty() &&
                previousStoredPath != storedPath) {
                std::error_code removeEc;
                fs::remove(fs::path(previousStoredPath), removeEc);
            }
            result.moduleName = moduleName;
            result.imported = existingModule.empty();
            result.updated = !existingModule.empty();
            result.message = existingModule.empty()
                ? "Imported successfully."
                : "Updated existing import.";
        } else {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            result.message = "Failed to persist imported content.";
        }

        batch.files.push_back(std::move(result));
    }

    reloadCache();
    return batch;
}

bool ImportedModuleManager::removeModule(const std::string& moduleName,
                                         std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    if (!db_) {
        if (errorMessage) *errorMessage = "Import database is not open.";
        return false;
    }

    auto it = modulesByName_.find(moduleName);
    if (it == modulesByName_.end()) {
        if (errorMessage) *errorMessage = "Imported module not found.";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(
                  db_,
                  "DELETE FROM imported_modules WHERE module_name = ?",
                  -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        bindText(stmt, 1, moduleName);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    if (stmt) sqlite3_finalize(stmt);
    if (!ok) {
        if (errorMessage) *errorMessage = "Failed to remove imported module metadata.";
        return false;
    }

    if (it->second.copiedToLibrary && !it->second.storedPath.empty()) {
        std::error_code ec;
        fs::remove(fs::path(it->second.storedPath), ec);
    }

    reloadCache();
    return true;
}

} // namespace verdad
