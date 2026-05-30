#ifndef VERDAD_TAG_MANAGER_H
#define VERDAD_TAG_MANAGER_H

#include <map>
#include <set>
#include <string>
#include <vector>

struct sqlite3;

namespace verdad {

/// A tag that can be applied to verses
struct Tag {
    std::string name;
    std::string color;   // hex color like "#4a86c8"
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
    std::string displayLabel() const;
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
    bool createTag(const std::string& name, const std::string& color = "#4a86c8");

    /// Delete a tag and remove it from all verses
    bool deleteTag(const std::string& name);

    /// Rename a tag
    bool renameTag(const std::string& oldName, const std::string& newName);

    /// Set tag color
    void setTagColor(const std::string& name, const std::string& color);

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

    /// Get tags for a specific verse
    std::vector<Tag> getTagsForVerse(const std::string& verseKey) const;

    /// Get tags for a specific tagged item.
    std::vector<Tag> getTagsForTarget(const TagTarget& target) const;

    /// Get all verses with a specific tag
    std::vector<std::string> getVersesWithTag(const std::string& tagName) const;

    /// Get all tagged items with a specific tag.
    std::vector<TagTarget> getTargetsWithTag(const std::string& tagName) const;

    /// Check if a verse has a specific tag
    bool verseHasTag(const std::string& verseKey, const std::string& tagName) const;

    /// Check if a tagged item has a specific tag.
    bool targetHasTag(const TagTarget& target, const std::string& tagName) const;

    /// Get the number of verses with a given tag
    int getTagCount(const std::string& tagName) const;

private:
    bool openDatabase(const std::string& filepath);
    void closeDatabase();
    bool loadFromDatabase();
    bool persistToDatabase();
    bool hasStoredData() const;
    bool importLegacyFile(const std::string& legacyPath);

    void addToInvertedIndex(const std::string& tagName, const TagTarget& target);
    void removeFromInvertedIndex(const std::string& tagName, const TagTarget& target);

    static std::string targetKey(const TagTarget& target);
    static bool parseTargetKey(const std::string& key, TagTarget& targetOut);

    std::string filepath_;
    sqlite3* db_ = nullptr;
    std::map<std::string, Tag> tags_;                        // tagName -> Tag
    std::map<std::string, std::set<std::string>> targetTags_; // targetKey -> set of tag names
    std::map<std::string, std::set<std::string>> tagTargets_; // tagName -> set of target keys
    std::map<std::string, TagTarget> targets_;                // targetKey -> target metadata
    bool dirty_ = false;
};

} // namespace verdad

#endif // VERDAD_TAG_MANAGER_H
