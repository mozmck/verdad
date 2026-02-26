#ifndef VERDAD_VERSE_CONTEXT_H
#define VERDAD_VERSE_CONTEXT_H

#include <FL/Fl_Menu_Button.H>
#include <string>
#include <functional>

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
    /// @param verseKey  Current verse reference
    /// @param screenX   Screen X position
    /// @param screenY   Screen Y position
    void show(const std::string& word, const std::string& href,
              const std::string& verseKey,
              int screenX, int screenY);

private:
    VerdadApp* app_;
    std::string currentWord_;
    std::string currentHref_;
    std::string currentVerseKey_;

    /// Extract Strong's number from href or word info
    std::string extractStrongsNumber(const std::string& href) const;

    // Menu action callbacks
    static void onSearchStrongs(Fl_Widget* w, void* data);
    static void onAddTag(Fl_Widget* w, void* data);
    static void onCopyVerse(Fl_Widget* w, void* data);
    static void onCopyWord(Fl_Widget* w, void* data);
    static void onLookupDictionary(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_VERSE_CONTEXT_H
