#include "tags/TagManager.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void checkRange(const std::string& text, const std::string& expected) {
    verdad::VerseRange range;
    const bool ok = verdad::VerseRange::parse(text, range);
    if (expected.empty()) {
        expect(!ok, "should not parse: " + text);
        return;
    }
    expect(ok, "should parse: " + text);
    if (ok) {
        expect(range.toString() == expected,
               "parse " + text + " -> " + range.toString() + " (expected " + expected + ")");
    }
}

std::vector<std::string> names(const std::vector<verdad::Tag>& tags) {
    std::vector<std::string> out;
    for (const auto& tag : tags) out.push_back(tag.name);
    return out;
}

bool execSql(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}
}

int main() {
    using verdad::TagManager;
    using verdad::TagTarget;
    using verdad::VerseRange;

    // Reference parsing and formatting.
    checkRange("Genesis 1:1", "Genesis 1:1");
    checkRange("Genesis 1:1-5", "Genesis 1:1-5");
    checkRange("Genesis 1:5-3", "Genesis 1:3-5");
    checkRange("Genesis 1:1-1", "Genesis 1:1");
    checkRange("Genesis 1:30-2:3", "Genesis 1:30-2:3");
    checkRange("Genesis 1:1 - 5", "Genesis 1:1-5");
    checkRange("Genesis 1:1\xE2\x80\x93" "5", "Genesis 1:1-5");
    checkRange("1 John 3:16", "1 John 3:16");
    checkRange("Song of Solomon 2:1-4", "Song of Solomon 2:1-4");
    checkRange("John 3", "John 3");
    checkRange("John 3-4", "John 3-4");
    checkRange("John", "");
    checkRange("3:16", "");
    checkRange("Grace", "");
    checkRange("Genesis 1:x", "");

    VerseRange range;
    VerseRange::parse("John 3:14-18", range);
    expect(range.contains("john", 3, 16), "range contains inner verse");
    expect(!range.contains("john", 3, 19), "range excludes later verse");
    VerseRange chapter;
    VerseRange::parse("John 3", chapter);
    expect(chapter.overlaps(range), "chapter overlaps range");
    VerseRange other;
    VerseRange::parse("John 4:1", other);
    expect(!chapter.overlaps(other), "chapter does not overlap next chapter");

    expect(TagTarget::verse("Genesis 1:3-1").sourceKey == "Genesis 1:1-3",
           "verse target canonicalizes reversed range");
    expect(TagTarget::verse("  Genesis 1:1 ").sourceKey == "Genesis 1:1",
           "single verse keys stay as given");
    expect(TagTarget::verse("Genesis 1:1-3").isVerseRange(), "range target detected");
    expect(!TagTarget::verse("Genesis 1:1").isVerseRange(), "single target is not a range");

    const std::filesystem::path dbPath =
        std::filesystem::temp_directory_path() / "verdad-tag-manager-test.db";
    std::filesystem::remove(dbPath);

    {
        TagManager mgr;
        expect(mgr.load(dbPath.string()), "load empty database");

        // Verse ranges.
        mgr.tagVerse("John 3:14-18", "Salvation");
        mgr.tagVerse("John 3:16", "Love");
        auto coverage = mgr.getVerseTagCoverage("John 3:16");
        expect(names(coverage.starting) == std::vector<std::string>{"Love"},
               "verse 16 starts Love");
        expect(names(coverage.continuing) == std::vector<std::string>{"Salvation"},
               "verse 16 continues Salvation");
        coverage = mgr.getVerseTagCoverage("John 3:14");
        expect(names(coverage.starting) == std::vector<std::string>{"Salvation"},
               "verse 14 starts Salvation");
        expect(coverage.startingRanges == std::vector<std::string>{"John 3:14-18"},
               "verse 14 reports its range");
        expect(mgr.getTagsForVerse("John 3:19").empty(), "verse 19 untagged");
        expect(mgr.getTagsForVerse("John 3:17").size() == 1, "verse 17 covered");

        // Hierarchy.
        expect(mgr.createTag("Theology"), "create Theology");
        expect(mgr.createTag("Grace", "#ff0000", "Theology"), "create Grace under Theology");
        expect(mgr.setParent("Salvation", "Grace"), "move Salvation under Grace");
        expect(!mgr.setParent("Theology", "Salvation"), "cycle rejected");
        expect(!mgr.setParent("Theology", "Theology"), "self-parent rejected");
        expect(mgr.tagPath("Salvation") == "Theology / Grace / Salvation", "tag path");
        expect(mgr.childrenOf("") == std::vector<std::string>({"Love", "Theology"}),
               "top-level tags");
        expect(mgr.descendantsOf("Theology") ==
                   std::vector<std::string>({"Grace", "Salvation"}),
               "descendants");
        mgr.tagVerse("Romans 3:24", "Grace");
        expect(mgr.getTagCount("Theology") == 0, "Theology has no own items");
        expect(mgr.getTagCount("Theology", true) == 2, "Theology rolls up items");
        expect(mgr.getTargetsWithTag("Grace", true).size() == 2, "Grace with descendants");

        expect(mgr.renameTag("Grace", "Grace of God"), "rename parent");
        expect(mgr.parentOf("Salvation") == "Grace of God", "child follows rename");
        expect(mgr.parentOf("Grace of God") == "Theology", "renamed keeps parent");
        expect(mgr.save(), "save");
    }

    {
        TagManager mgr;
        expect(mgr.load(dbPath.string()), "reload database");
        expect(mgr.tagPath("Salvation") == "Theology / Grace of God / Salvation",
               "hierarchy persisted");
        expect(mgr.getTagsForVerse("John 3:18").size() == 1, "range persisted");

        // Deleting a middle tag keeps its children by moving them up.
        expect(mgr.deleteTag("Grace of God"), "delete middle tag");
        expect(mgr.parentOf("Salvation") == "Theology", "child moved up");
        expect(mgr.getTagCount("Theology", true) == 1, "deleted tag's items removed");

        mgr.createTag("Sub", "#000000", "Salvation");
        expect(mgr.deleteTag("Theology", true), "delete subtree");
        expect(!mgr.hasTag("Salvation") && !mgr.hasTag("Sub"), "subtree deleted");
        expect(mgr.getTagsForVerse("John 3:17").empty(), "range removed with tag");

        mgr.createTag("Parent");
        mgr.createTag("Child", "#000000", "Parent");
        expect(mgr.save(), "save after deletes");
    }

    // Simulate an older build rewriting only its own tables: the hierarchy
    // table must survive, and stale rows must be ignored.
    {
        sqlite3* db = nullptr;
        expect(sqlite3_open(dbPath.string().c_str(), &db) == SQLITE_OK, "open raw db");
        expect(execSql(db, "INSERT INTO tag_tree(tag_name, parent_name) VALUES('Ghost', 'Parent');"),
               "insert stale row");
        expect(execSql(db, "CREATE TABLE tags_new(name TEXT PRIMARY KEY, color TEXT);"
                           "INSERT INTO tags_new SELECT name, color FROM tags;"
                           "DROP TABLE tags; ALTER TABLE tags_new RENAME TO tags;"),
               "rewrite tags table");
        sqlite3_close(db);

        TagManager mgr;
        expect(mgr.load(dbPath.string()), "reload after old-build rewrite");
        expect(mgr.parentOf("Child") == "Parent", "hierarchy survives old-build rewrite");
        expect(!mgr.hasTag("Ghost"), "stale hierarchy row ignored");
    }

    std::filesystem::remove(dbPath);

    if (failures) {
        std::cerr << failures << " tag manager test(s) failed\n";
        return 1;
    }
    std::cout << "Tag manager tests passed\n";
    return 0;
}
