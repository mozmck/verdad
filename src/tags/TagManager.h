#ifndef VERDAD_TAG_MANAGER_H
#define VERDAD_TAG_MANAGER_H

#include <map>
#include <set>
#include <string>
#include <vector>

struct sqlite3;

namespace verdad {

/// A tag that can be applied to verses. Tags form a tree: any tag can have
/// a parent tag and child tags in addition to its own tagged items.
struct Tag {
    std::string name;
    std::string color;   // hex color like "#4a86c8"; empty = use the app default
};

/// A parsed Bible reference span such as "Genesis 1:1", "Genesis 1:1-5",
/// "Genesis 1:30-2:3", or a whole chapter "Genesis 1". Single verses have
/// start == end. Whole chapters end at kChapterEnd.
struct VerseRange {
    static constexpr int kChapterEnd = 9999;

    std::string book;     // book name as written
    std::string bookKey;  // normalized book name for comparisons
    int startChapter = 0;
    int startVerse = 0;
    int endChapter = 0;
    int endVerse = 0;

    bool isSingleVerse() const;
    bool contains(const std::string& otherBookKey, int chapter, int verse) const;
    bool overlaps(const VerseRange& other) const;
    bool startsAt(int chapter, int verse) const;

    /// Format as "Book C:V", "Book C:V-V2", "Book C:V-C2:V2", "Book C", or "Book C-C2".
    std::string toString() const;

    /// Parse a reference. Returns false when the text is not a Bible reference.
    static bool parse(const std::string& ref, VerseRange& out);

    /// Normalize a book name for comparison (lowercase alphanumerics only).
    static std::string normalizeBookKey(const std::string& book);
};

/// A tagged target item.
struct TagTarget {
    enum class Kind {
        Verse,
        Commentary,
        GeneralBook,
    };

    Kind kind = Kind::Verse;
    std::string moduleName;
    std::string sourceKey;
    std::string selectionText;

    static TagTarget verse(const std::string& verseKey);
    static TagTarget commentary(const std::string& moduleName,
                                const std::string& sourceKey,
                                const std::string& selectionText = "");
    static TagTarget generalBook(const std::string& moduleName,
                                 const std::string& sourceKey,
                                 const std::string& selectionText = "");

    bool isVerse() const { return kind == Kind::Verse; }
    bool isResource() const { return kind != Kind::Verse; }

    /// True for verse targets covering more than one verse.
    bool isVerseRange() const;

    /// Parse a verse target's reference. Returns false for resources or
    /// unparseable references.
    bool verseRange(VerseRange& out) const;

    std::string displayLabel() const;

    /// Stable identity string for this target.
    std::string identityKey() const;
};

/// Tags touching a single verse, split by whether a tagged range starts on
/// that verse or merely continues through it.
struct VerseTagCoverage {
    std::vector<Tag> starting;    // tagged items whose reference starts here
    std::vector<Tag> continuing;  // tagged ranges that began on an earlier verse
    std::vector<std::string> startingRanges; // multi-verse refs starting here

    bool empty() const { return starting.empty() && continuing.empty(); }
};

/// Manages verse tags persisted in an SQLite database
class TagManager {
public:
    TagManager();
    ~TagManager();

    /// Load tags from database. Returns true on success.
    bool load(const std::string& filepath);

    /// Save tags to database. Returns true on success.
    bool save(const std::string& filepath);

    /// Save to the last loaded database
    bool save();

    /// Checkpoint pending SQLite journal data before copying the database file.
    bool checkpoint();

    /// Create a new tag. Returns true if created (false if already exists).
    /// An empty parent creates a top-level tag.
    bool createTag(const std::string& name,
                   const std::string& color = "",
                   const std::string& parentName = "");

    /// Delete a tag and remove it from all items. When deleteDescendants is
    /// false, child tags move up to the deleted tag's parent.
    bool deleteTag(const std::string& name, bool deleteDescendants = false);

    /// Rename a tag
    bool renameTag(const std::string& oldName, const std::string& newName);

    /// Set tag color
    void setTagColor(const std::string& name, const std::string& color);

    /// Check whether a tag exists.
    bool hasTag(const std::string& name) const;

    /// Look up a tag. Returns false if it does not exist.
    bool getTag(const std::string& name, Tag& out) const;

    /// Parent tag name, or "" for top-level tags.
    std::string parentOf(const std::string& name) const;

    /// Direct child tags, sorted by name. An empty name returns top-level tags.
    std::vector<std::string> childrenOf(const std::string& parentName) const;

    bool hasChildren(const std::string& name) const;

    /// All descendants in depth-first, name-sorted order.
    std::vector<std::string> descendantsOf(const std::string& name) const;

    /// True when ancestor is a (strict) ancestor of name.
    bool isDescendantOf(const std::string& name, const std::string& ancestor) const;

    /// Move a tag under a new parent ("" for top level). Fails on cycles.
    bool setParent(const std::string& name, const std::string& parentName);

    /// Full path such as "Theology / Grace / Justification".
    std::string tagPath(const std::string& name, const std::string& separator = " / ") const;

    /// Add a tag to a verse
    /// @param verseKey  Canonical verse reference (e.g. "Genesis 1:1")
    /// @param tagName   Tag name
    void tagVerse(const std::string& verseKey, const std::string& tagName);

    /// Add a tag to a tagged item.
    void tagTarget(const TagTarget& target, const std::string& tagName);

    /// Remove a tag from a verse
    void untagVerse(const std::string& verseKey, const std::string& tagName);

    /// Remove a tag from a tagged item.
    void untagTarget(const TagTarget& target, const std::string& tagName);

    /// Get all tags
    std::vector<Tag> getAllTags() const;

    /// Get tags for a specific verse, including tagged ranges covering it
    std::vector<Tag> getTagsForVerse(const std::string& verseKey) const;

    /// Get tags covering a verse, split into ranges starting there and
    /// ranges continuing from earlier verses.
    VerseTagCoverage getVerseTagCoverage(const std::string& verseKey) const;

    /// Get tags for a specific tagged item.
    std::vector<Tag> getTagsForTarget(const TagTarget& target) const;

    /// Get all verses with a specific tag
    std::vector<std::string> getVersesWithTag(const std::string& tagName) const;

    /// Get all tagged items with a specific tag, optionally including items
    /// tagged with any descendant tag.
    std::vector<TagTarget> getTargetsWithTag(const std::string& tagName,
                                             bool includeDescendants = false) const;

    /// Check if a verse has a specific tag
    bool verseHasTag(const std::string& verseKey, const std::string& tagName) const;

    /// Check if a tagged item has a specific tag.
    bool targetHasTag(const TagTarget& target, const std::string& tagName) const;

    /// Get the number of items with a given tag, optionally including
    /// items tagged with descendant tags (each item counted once).
    int getTagCount(const std::string& tagName, bool includeDescendants = false) const;

private:
    bool openDatabase(const std::string& filepath);
    void closeDatabase();
    bool loadFromDatabase();
    bool persistToDatabase();
    bool hasStoredData() const;
    bool importLegacyFile(const std::string& legacyPath);

    void addToInvertedIndex(const std::string& tagName, const TagTarget& target);
    void removeFromInvertedIndex(const std::string& tagName, const TagTarget& target);
    void removeTagInternal(const std::string& name);
    void validateHierarchy();
    void invalidateCaches();
    void rebuildChildrenCache() const;
    void rebuildRangeIndex() const;

    static std::string targetKey(const TagTarget& target);
    static bool parseTargetKey(const std::string& key, TagTarget& targetOut);

    std::string filepath_;
    sqlite3* db_ = nullptr;
    std::map<std::string, Tag> tags_;                        // tagName -> Tag
    std::map<std::string, std::set<std::string>> targetTags_; // targetKey -> set of tag names
    std::map<std::string, std::set<std::string>> tagTargets_; // tagName -> set of target keys
    std::map<std::string, TagTarget> targets_;                // targetKey -> target metadata
    std::map<std::string, std::string> parents_;              // tagName -> parent tag name
    bool dirty_ = false;

    struct RangeIndexEntry {
        VerseRange range;
        std::string targetKey;
    };
    mutable std::map<std::string, std::vector<RangeIndexEntry>> rangeIndex_; // bookKey -> ranges
    mutable bool rangeIndexDirty_ = true;
    mutable std::map<std::string, std::vector<std::string>> childrenCache_;  // parent -> children
    mutable bool childrenCacheDirty_ = true;
};

} // namespace verdad

#endif // VERDAD_TAG_MANAGER_H
