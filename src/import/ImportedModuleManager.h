#ifndef VERDAD_IMPORTED_MODULE_MANAGER_H
#define VERDAD_IMPORTED_MODULE_MANAGER_H

#include "sword/SwordManager.h"

#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace verdad {

class ImportedModuleManager {
public:
    struct Entry {
        std::string key;
        std::string title;
        std::string html;
        std::string plainText;
        int depth = 0;
        bool hasChildren = false;
        int orderIndex = 0;
    };

    struct ModuleRecord {
        std::string moduleName;
        std::string displayName;
        std::string sourcePath;
        std::string storedPath;
        std::string fileType;
        std::string contentHash;
        std::string tags;
        std::string notes;
        bool copiedToLibrary = false;
        long long fileSize = 0;
        long long modifiedTime = 0;
        ModuleInfo info;
        std::vector<Entry> entries;
    };

    struct ImportOptions {
        bool copyFiles = false;
        std::string tags;
        std::string notes;
    };

    struct FileImportResult {
        std::string path;
        std::string moduleName;
        std::string message;
        bool imported = false;
        bool updated = false;
        bool skipped = false;
    };

    struct BatchImportResult {
        std::vector<FileImportResult> files;

        int importedCount() const;
        int updatedCount() const;
        int skippedCount() const;
        int failureCount() const;
    };

    ImportedModuleManager();
    ~ImportedModuleManager();

    bool load(const std::string& dbPath,
              const std::string& storageDir);
    void close();

    bool isLoaded() const { return db_ != nullptr; }

    std::vector<ModuleInfo> modules() const;
    bool hasModule(const std::string& moduleName) const;
    ModuleInfo moduleInfo(const std::string& moduleName) const;
    std::vector<GeneralBookTocEntry> toc(const std::string& moduleName) const;
    std::string entryHtml(const std::string& moduleName,
                          const std::string& key) const;
    std::vector<Entry> indexEntries(const std::string& moduleName) const;
    std::vector<ModuleRecord> records() const;

    BatchImportResult importPaths(const std::vector<std::string>& paths,
                                  const ImportOptions& options);
    bool removeModule(const std::string& moduleName,
                      std::string* errorMessage = nullptr);

private:
    bool openDatabase(const std::string& dbPath);
    bool ensureSchema();
    bool reloadCache();

    std::string dbPath_;
    std::string storageDir_;
    sqlite3* db_ = nullptr;
    std::unordered_map<std::string, ModuleRecord> modulesByName_;
};

} // namespace verdad

#endif // VERDAD_IMPORTED_MODULE_MANAGER_H
