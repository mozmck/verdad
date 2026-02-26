#ifndef VERDAD_RIGHT_PANE_H
#define VERDAD_RIGHT_PANE_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Choice.H>
#include <string>
#include <vector>

namespace verdad {

class VerdadApp;
class HtmlWidget;

/// Right pane for commentary and dictionary display.
/// Uses tabs for multiple commentaries/dictionaries.
class RightPane : public Fl_Group {
public:
    struct DisplayBuffer {
        std::string commentaryHtml;
        int commentaryScrollY = 0;
        bool hasCommentary = false;
        std::string dictionaryHtml;
        int dictionaryScrollY = 0;
        bool hasDictionary = false;
    };

    RightPane(VerdadApp* app, int X, int Y, int W, int H);
    ~RightPane() override;

    /// Show commentary for a verse reference
    void showCommentary(const std::string& reference);

    /// Show commentary using a specific module
    void showCommentary(const std::string& moduleName, const std::string& reference);

    /// Show a dictionary/lexicon entry
    void showDictionaryEntry(const std::string& key);

    /// Show a dictionary entry in a specific module
    void showDictionaryEntry(const std::string& moduleName, const std::string& key);

    /// Set the current commentary module
    void setCommentaryModule(const std::string& moduleName);

    /// Set the current dictionary module
    void setDictionaryModule(const std::string& moduleName);

    /// Current commentary module
    const std::string& currentCommentaryModule() const { return currentCommentary_; }

    /// Current commentary reference
    const std::string& currentCommentaryReference() const { return currentCommentaryRef_; }

    /// Current dictionary module
    const std::string& currentDictionaryModule() const { return currentDictionary_; }

    /// Current dictionary key
    const std::string& currentDictionaryKey() const { return currentDictKey_; }

    /// Returns true if dictionary tab is active, false for commentary tab.
    bool isDictionaryTabActive() const;

    /// Select the visible tab: true = dictionary, false = commentary.
    void setDictionaryTabActive(bool dictionaryActive);

    /// Apply modules/keys/tab selection without rendering new text.
    void setStudyState(const std::string& commentaryModule,
                       const std::string& commentaryReference,
                       const std::string& dictionaryModule,
                       const std::string& dictionaryKey,
                       bool dictionaryActive);

    /// Capture currently rendered text/scroll for fast restore.
    DisplayBuffer captureDisplayBuffer() const;

    /// Restore previously captured rendered text/scroll.
    void restoreDisplayBuffer(const DisplayBuffer& buffer, bool dictionaryActive);

    /// Redraw tabs/chrome during live layout changes.
    void redrawChrome();

    /// Refresh display
    void refresh();

protected:
    void resize(int X, int Y, int W, int H) override;

private:
    VerdadApp* app_;

    // Tabs for commentary and dictionary
    Fl_Tabs* tabs_;

    // Commentary tab
    Fl_Group* commentaryGroup_;
    Fl_Choice* commentaryChoice_;
    HtmlWidget* commentaryHtml_;
    std::string currentCommentary_;
    std::string currentCommentaryRef_;

    // Dictionary tab
    Fl_Group* dictionaryGroup_;
    Fl_Choice* dictionaryChoice_;
    HtmlWidget* dictionaryHtml_;
    std::string currentDictionary_;
    std::string currentDictKey_;

    /// Populate commentary module choices
    void populateCommentaryModules();

    /// Populate dictionary module choices
    void populateDictionaryModules();

    // Callbacks
    static void onCommentaryModuleChange(Fl_Widget* w, void* data);
    static void onDictionaryModuleChange(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_RIGHT_PANE_H
