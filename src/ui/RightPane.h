#ifndef VERDAD_RIGHT_PANE_H
#define VERDAD_RIGHT_PANE_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>

#include "sword/SwordManager.h"

namespace verdad {

class VerdadApp;
class HtmlWidget;
class HtmlEditorWidget;

/// Right pane for commentary, dictionary, and general books.
/// Commentary/General Books are top tabs; dictionary is a resizable bottom pane.
class RightPane : public Fl_Group {
public:
    enum class TopTab {
        Commentary,
        GeneralBooks,
        Documents,
    };

    struct HtmlState {
        std::shared_ptr<void> doc;
        std::string html;
        std::string baseUrl;
        int scrollY = 0;
        int contentHeight = 0;
        int renderWidth = 0;
        bool scrollbarVisible = false;
        bool valid = false;
    };

    struct DisplayBuffer {
        HtmlState commentary;
        HtmlState dictionary;
        HtmlState generalBook;
    };

    RightPane(VerdadApp* app, int X, int Y, int W, int H);
    ~RightPane() override;

    /// Show commentary for a verse reference
    void showCommentary(const std::string& reference);

    /// Show commentary using a specific module
    void showCommentary(const std::string& moduleName, const std::string& reference);

    /// Show a dictionary/lexicon entry
    void showDictionaryEntry(const std::string& key);

    /// Show a dictionary entry using the best default for a source module.
    void showDictionaryLookup(const std::string& key,
                              const std::string& contextModule);

    /// Show a dictionary entry in a specific module
    void showDictionaryEntry(const std::string& moduleName, const std::string& key);

    /// Show a general book entry
    void showGeneralBookEntry(const std::string& key);

    /// Show a general book entry in a specific module
    void showGeneralBookEntry(const std::string& moduleName, const std::string& key);

    /// Set the current commentary module
    void setCommentaryModule(const std::string& moduleName);

    /// Set the current dictionary module
    void setDictionaryModule(const std::string& moduleName);

    /// Set the current general book module
    void setGeneralBookModule(const std::string& moduleName);

    /// Current commentary module
    const std::string& currentCommentaryModule() const { return currentCommentary_; }

    /// Current commentary reference
    const std::string& currentCommentaryReference() const { return currentCommentaryRef_; }

    /// Current dictionary module
    const std::string& currentDictionaryModule() const { return currentDictionary_; }

    /// Current dictionary key
    const std::string& currentDictionaryKey() const { return currentDictKey_; }

    /// Current general book module
    const std::string& currentGeneralBookModule() const { return currentGeneralBook_; }

    /// Current general book key
    const std::string& currentGeneralBookKey() const { return currentGeneralBookKey_; }

    /// Legacy naming kept for session compatibility.
    /// Returns true if the secondary tab is active (now General Books), false for Commentary.
    bool isDictionaryTabActive() const;

    bool isDocumentsTabActive() const;
    void setDocumentsTabActive(bool active);
    const std::string& currentDocumentPath() const { return currentDocumentPath_; }

    bool newDocument();
    bool openDocument();
    bool openDocument(const std::string& path, bool activateTab);
    bool saveDocument();
    bool closeDocument();

    /// Legacy naming kept for session compatibility.
    /// Select visible tab: true = General Books, false = Commentary.
    void setDictionaryTabActive(bool dictionaryActive);

    /// Current dictionary pane height in pixels.
    int dictionaryPaneHeight() const;

    /// Current commentary scroll Y.
    int commentaryScrollY() const;

    /// Restore commentary scroll Y.
    void setCommentaryScrollY(int y);

    /// Set dictionary pane height in pixels (clamped to valid splitter range).
    void setDictionaryPaneHeight(int height);

    /// Apply modules/keys/tab selection without rendering new text.
    void setStudyState(const std::string& commentaryModule,
                       const std::string& commentaryReference,
                       const std::string& dictionaryModule,
                       const std::string& dictionaryKey,
                       const std::string& generalBookModule,
                       const std::string& generalBookKey,
                       bool dictionaryActive);

    /// Capture currently rendered text/scroll for fast restore.
    DisplayBuffer captureDisplayBuffer() const;

    /// Move currently rendered text/scroll out of widgets for zero-copy tab switching.
    DisplayBuffer takeDisplayBuffer();

    /// Restore previously captured rendered text/scroll.
    void restoreDisplayBuffer(const DisplayBuffer& buffer, bool dictionaryActive);

    /// Restore previously captured rendered text/scroll (move).
    void restoreDisplayBuffer(DisplayBuffer&& buffer, bool dictionaryActive);

    /// Redraw tabs/chrome during live layout changes.
    void redrawChrome();

    /// Refresh display
    void refresh();

    /// Apply runtime HTML style overrides to all right-pane HTML widgets.
    void setHtmlStyleOverride(const std::string& css);

protected:
    void resize(int X, int Y, int W, int H) override;

private:
    VerdadApp* app_;

    // Main vertical split: top tabs + bottom dictionary pane.
    Fl_Tile* contentTile_;
    Fl_Box* contentResizeBox_;

    // Tabs for commentary and general books (top pane)
    Fl_Tabs* tabs_;

    // Commentary tab (top pane)
    Fl_Group* commentaryGroup_;
    Fl_Choice* commentaryChoice_;
    Fl_Button* commentaryEditButton_;
    Fl_Button* commentarySaveButton_;
    Fl_Button* commentaryCancelButton_;
    HtmlWidget* commentaryHtml_;
    HtmlEditorWidget* commentaryEditor_;
    std::string currentCommentary_;
    std::string currentCommentaryRef_;
    std::string loadedCommentaryModule_;
    std::string loadedCommentaryChapterKey_;
    std::unordered_map<std::string, std::string> commentaryChapterCache_;
    std::deque<std::string> commentaryChapterCacheOrder_;
    static constexpr size_t kCommentaryChapterCacheLimit = 64;
    bool commentaryEditing_ = false;
    std::string commentaryEditModule_;
    std::string commentaryEditReference_;

    // Dictionary pane (bottom pane)
    Fl_Group* dictionaryPaneGroup_;
    Fl_Input* dictionaryKeyInput_;
    Fl_Choice* dictionaryChoice_;
    HtmlWidget* dictionaryHtml_;
    std::string currentDictionary_;
    std::string currentDictKey_;

    // General books tab (top pane)
    Fl_Group* generalBooksGroup_;
    Fl_Choice* generalBookChoice_;
    Fl_Choice* generalBookTocChoice_;
    HtmlWidget* generalBookHtml_;
    std::string currentGeneralBook_;
    std::string currentGeneralBookKey_;
    std::vector<GeneralBookTocEntry> generalBookToc_;

    // Documents tab (global, not tied to study tabs)
    Fl_Group* documentsGroup_;
    Fl_Button* documentNewButton_;
    Fl_Button* documentOpenButton_;
    Fl_Button* documentSaveButton_;
    Fl_Button* documentCloseButton_;
    Fl_Box* documentPathLabel_;
    HtmlEditorWidget* documentsEditor_;
    std::string currentDocumentPath_;
    TopTab activeTopTab_ = TopTab::Commentary;
    bool secondaryTabIsGeneralBooks_ = false;

    /// Populate commentary module choices
    void populateCommentaryModules();

    /// Build normalized "Book Chapter" key from a verse reference.
    std::string commentaryChapterKeyForReference(const std::string& reference,
                                                 int* verseOut = nullptr) const;

    /// Commentary chapter cache helpers.
    bool lookupCommentaryCache(const std::string& cacheKey, std::string& htmlOut);
    void storeCommentaryCache(const std::string& cacheKey, const std::string& html);
    void invalidateCommentaryCache(const std::string& moduleName,
                                   const std::string& reference);

    /// Populate dictionary module choices
    void populateDictionaryModules();

    /// Populate general book module choices
    void populateGeneralBookModules();
    void populateGeneralBookToc();

    void layoutTopTabContents(int tabsX, int tabsY, int tabsW, int tabsH);
    TopTab visibleTopTab() const;
    void updateCommentaryEditorChrome();
    void updateDocumentChrome();
    bool beginCommentaryEdit();
    bool saveCommentaryEdit(bool exitEditMode);
    void cancelCommentaryEdit();
    bool maybeSaveDocumentChanges();
    bool saveDocumentAs();
    bool saveDocumentToPath(const std::string& path);
    void loadCommentaryEditorForCurrentEntry();
    void onHtmlLink(const std::string& url, bool commentarySource);

    // Callbacks
    static void onCommentaryModuleChange(Fl_Widget* w, void* data);
    static void onCommentaryEdit(Fl_Widget* w, void* data);
    static void onCommentarySave(Fl_Widget* w, void* data);
    static void onCommentaryCancel(Fl_Widget* w, void* data);
    static void onDictionaryModuleChange(Fl_Widget* w, void* data);
    static void onDictionaryKeyInput(Fl_Widget* w, void* data);
    static void onTopTabChange(Fl_Widget* w, void* data);
    static void onGeneralBookModuleChange(Fl_Widget* w, void* data);
    static void onGeneralBookTocChange(Fl_Widget* w, void* data);
    static void onDocumentNew(Fl_Widget* w, void* data);
    static void onDocumentOpen(Fl_Widget* w, void* data);
    static void onDocumentSave(Fl_Widget* w, void* data);
    static void onDocumentClose(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_RIGHT_PANE_H
