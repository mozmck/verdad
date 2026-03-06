#ifndef VERDAD_VERSE_CONTEXT_H
#define VERDAD_VERSE_CONTEXT_H

#include <FL/Fl_Menu_Button.H>
#include <string>
#include <vector>

namespace verdad {

class VerdadApp;

/// Right-click context menu for verse/word interactions.
/// Provides options like:
/// - Search for Strong's number
/// - Add/remove tag
/// - Copy verse text
class VerseContext {
public:
    VerseContext(VerdadApp* app);
    ~VerseContext();

    /// Show context menu at position for a word/verse
    /// @param word      The word that was right-clicked
    /// @param href      The href attribute (may contain strongs: link)
    /// @param strong    Strong's token(s) from data-strong attribute
    /// @param morph     Morph token(s) from data-morph attribute
    /// @param module    Source module for this hit (parallel-aware)
    /// @param verseKey  Current verse reference
    /// @param screenX   Screen X position
    /// @param screenY   Screen Y position
    void show(const std::string& word, const std::string& href,
              const std::string& strong, const std::string& morph,
              const std::string& module,
              const std::string& verseKey,
              int screenX, int screenY);

private:
    VerdadApp* app_;
    std::string currentWord_;
    std::string currentHref_;
    std::string currentStrong_;
    std::string currentMorph_;
    std::string currentContextModule_;
    std::string currentVerseKey_;
    std::string currentDictionaryLookupKey_;

    struct StrongsMenuAction {
        VerseContext* owner = nullptr;
        std::string strongsNumber;
        bool dictionaryLookup = false;
    };
    std::vector<StrongsMenuAction> strongActions_;

    /// Extract all Strong's numbers from href/data-strong payload.
    std::vector<std::string> extractStrongsNumbers(
        const std::string& href,
        const std::string& strong = "") const;

    // Menu action callbacks
    static void onStrongsAction(Fl_Widget* w, void* data);
    static void onAddTag(Fl_Widget* w, void* data);
    static void onCopyVerse(Fl_Widget* w, void* data);
    static void onCopyWord(Fl_Widget* w, void* data);
    static void onSearchWord(Fl_Widget* w, void* data);
    static void onLookupDictionary(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_VERSE_CONTEXT_H
