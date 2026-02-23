#ifndef VERDAD_SWORD_MANAGER_H
#define VERDAD_SWORD_MANAGER_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <mutex>

// Forward declarations for SWORD library types
namespace sword {
    class SWMgr;
    class SWModule;
    class SWKey;
    class VerseKey;
    class InstallMgr;
}

namespace verdad {

/// Information about a SWORD module
struct ModuleInfo {
    std::string name;           // Module abbreviation (e.g. "KJV")
    std::string description;    // Full description
    std::string type;           // "Biblical Texts", "Commentaries", "Lexicons / Dictionaries", etc.
    std::string language;       // Language code
    bool hasStrongs = false;
    bool hasMorph = false;
};

/// A single search result
struct SearchResult {
    std::string key;            // e.g. "Genesis 1:1"
    std::string text;           // Preview text
    std::string module;         // Source module name
};

/// Strongs/morphology info for a word
struct WordInfo {
    std::string word;           // The actual word
    std::string strongsNumber;  // e.g. "H1234" or "G5678"
    std::string strongsDef;     // Strong's definition text
    std::string morphCode;      // Morphology code
    std::string morphDef;       // Morphology definition
    std::string lemma;          // Original language lemma
};

/// Manages all SWORD library interactions
class SwordManager {
public:
    SwordManager();
    ~SwordManager();

    // Prevent copying
    SwordManager(const SwordManager&) = delete;
    SwordManager& operator=(const SwordManager&) = delete;

    /// Initialize the SWORD manager. Returns true on success.
    bool initialize();

    /// Get list of all available modules
    std::vector<ModuleInfo> getModules() const;

    /// Get modules filtered by type
    std::vector<ModuleInfo> getModulesByType(const std::string& type) const;

    /// Get list of available Bible modules
    std::vector<ModuleInfo> getBibleModules() const;

    /// Get list of available commentary modules
    std::vector<ModuleInfo> getCommentaryModules() const;

    /// Get list of available dictionary/lexicon modules
    std::vector<ModuleInfo> getDictionaryModules() const;

    /// Get rendered XHTML text for a verse/passage
    /// @param moduleName  Module to use (e.g. "KJV")
    /// @param key         Verse reference (e.g. "Genesis 1:1" or "Gen 1:1-5")
    /// @return XHTML string
    std::string getVerseText(const std::string& moduleName, const std::string& key);

    /// Get rendered XHTML for an entire chapter
    /// @param moduleName    Module to use
    /// @param book          Book name (e.g. "Genesis")
    /// @param chapter       Chapter number
    /// @param paragraphMode If true, display verses inline (paragraph style);
    ///                      if false (default), display one verse per line
    /// @return XHTML string with all verses
    std::string getChapterText(const std::string& moduleName,
                               const std::string& book, int chapter,
                               bool paragraphMode = false);

    /// Get rendered XHTML for parallel Bibles showing the same chapter
    /// @param moduleNames    List of modules to show in parallel
    /// @param book           Book name
    /// @param chapter        Chapter number
    /// @param paragraphMode  If true, display verses inline (paragraph style);
    ///                       if false (default), display one verse per line
    /// @return XHTML table with parallel columns
    std::string getParallelText(const std::vector<std::string>& moduleNames,
                                const std::string& book, int chapter,
                                bool paragraphMode = false);

    /// Get commentary text for a given verse reference
    std::string getCommentaryText(const std::string& moduleName,
                                  const std::string& key);

    /// Get dictionary/lexicon entry
    std::string getDictionaryEntry(const std::string& moduleName,
                                   const std::string& key);

    /// Search a module for text
    /// @param moduleName   Module to search
    /// @param searchText   Text to search for
    /// @param searchType   0=regex, 1=phrase, -1=multi-word, -2=entry attribute (e.g. Strong's)
    /// @param scope        Optional scope (e.g. "Gen-Rev")
    /// @param callback     Progress callback (0.0 to 1.0)
    /// @return Vector of search results
    std::vector<SearchResult> search(const std::string& moduleName,
                                     const std::string& searchText,
                                     int searchType = -1,
                                     const std::string& scope = "",
                                     std::function<void(float)> callback = nullptr);

    /// Search for a Strong's number across a module
    std::vector<SearchResult> searchStrongs(const std::string& moduleName,
                                            const std::string& strongsNumber);

    /// Get word information (Strong's, morphology) for a given position in rendered text
    /// This parses SWORD entry attributes after rendering
    WordInfo getWordInfo(const std::string& moduleName,
                         const std::string& verseKey,
                         const std::string& word);

    /// Get Strong's definition from a lexicon
    std::string getStrongsDefinition(const std::string& strongsNumber);

    /// Get morphology definition
    std::string getMorphDefinition(const std::string& morphCode);

    /// Get list of book names for a module
    std::vector<std::string> getBookNames(const std::string& moduleName);

    /// Get the number of chapters in a book
    int getChapterCount(const std::string& moduleName, const std::string& book);

    /// Get the number of verses in a chapter
    int getVerseCount(const std::string& moduleName, const std::string& book, int chapter);

    /// Get a module's description
    std::string getModuleDescription(const std::string& moduleName) const;

    /// Check if module has Strong's numbers
    bool moduleHasStrongs(const std::string& moduleName) const;

    /// Check if module has morphology
    bool moduleHasMorph(const std::string& moduleName) const;

    /// Parse a verse key string into components
    struct VerseRef {
        std::string book;
        int chapter = 0;
        int verse = 0;
        int verseEnd = 0; // 0 if single verse
    };
    static VerseRef parseVerseRef(const std::string& ref);

    /// Get all entry attributes for the last rendered verse
    /// Used for extracting Strong's and morph data from rendered text
    std::map<std::string, std::map<std::string, std::string>>
        getEntryAttributes(const std::string& moduleName);

private:
    std::unique_ptr<sword::SWMgr> mgr_;
    mutable std::mutex mutex_;

    /// Get a SWORD module by name (returns nullptr if not found)
    sword::SWModule* getModule(const std::string& name) const;

    /// Configure module filters for XHTML output
    void configureFilters(sword::SWModule* mod);

    /// Build ModuleInfo from a SWORD module
    ModuleInfo buildModuleInfo(sword::SWModule* mod) const;

    /// Post-process SWORD XHTML output: strip visible Strong's/morph codes and
    /// wrap words with data-strong/data-morph attributes for hover (Mag) support.
    std::string postProcessHtml(const std::string& html) const;
};

} // namespace verdad

#endif // VERDAD_SWORD_MANAGER_H
