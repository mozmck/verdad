#ifndef VERDAD_RIGHT_PANE_H
#define VERDAD_RIGHT_PANE_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Tree.H>
#include <FL/Enumerations.H>
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
class FilterableChoiceWidget;

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

    /// Set the current commentary module.
    /// When activateCurrentVerse is true, load the module at the active Bible verse.
    void setCommentaryModule(const std::string& moduleName,
                             bool activateCurrentVerse = false);

    /// Set the current dictionary module
    void setDictionaryModule(const std::string& moduleName,
                             bool loadKeys = true);

    /// Set the current general book module
    void setGeneralBookModule(const std::string& moduleName,
                              bool loadToc = true);

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
    bool openDocument(const std::string& path, bool activateTab);
    bool saveDocument();
    bool exportDocumentToOdt();
    bool maybeSaveDocumentChanges();

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
                       const std::string& dictionaryKey);

    /// Capture currently rendered text/scroll for fast restore.
    DisplayBuffer captureDisplayBuffer() const;

    /// Move currently rendered text/scroll out of widgets for zero-copy tab switching.
    DisplayBuffer takeDisplayBuffer();

    /// Restore previously captured rendered text/scroll.
    void restoreDisplayBuffer(const DisplayBuffer& buffer);

    /// Restore previously captured rendered text/scroll (move).
    void restoreDisplayBuffer(DisplayBuffer&& buffer);

    /// Redraw tabs/chrome during live layout changes.
    void redrawChrome();

    /// Refresh display
    void refresh();

    /// Apply runtime HTML style overrides to all right-pane HTML widgets.
    void setHtmlStyleOverride(const std::string& css);
    void setEditorIndentWidth(int width);
    void setEditorLineHeight(double lineHeight);
    void setEditorTextFont(Fl_Font regularFont, Fl_Font boldFont, int size);

protected:
    void resize(int X, int Y, int W, int H) override;
    int handle(int event) override;

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
    std::vector<std::string> commentaryChoiceModules_;
    std::vector<std::string> commentaryChoiceLabels_;
    std::string currentCommentary_;
    std::string currentCommentaryRef_;
    std::string loadedCommentaryModule_;
    std::string loadedCommentaryChapterKey_;
    std::unordered_map<std::string, std::string> commentaryChapterCache_;
    std::deque<std::string> commentaryChapterCacheOrder_;
    size_t commentaryChapterCacheBytes_ = 0;
    static constexpr size_t kCommentaryChapterCacheLimit = 8;
    static constexpr size_t kCommentaryChapterCacheByteLimit = 16 * 1024 * 1024;
    int highlightedCommentaryVerse_ = 0;
    std::string htmlStyleOverrideCss_;
    bool commentaryEditing_ = false;
    std::string commentaryEditModule_;
    std::string commentaryEditReference_;

    // Dictionary pane (bottom pane)
    Fl_Group* dictionaryPaneGroup_;
    Fl_Button* dictionaryBackButton_;
    FilterableChoiceWidget* dictionaryKeyInput_;
    Fl_Button* dictionaryForwardButton_;
    Fl_Choice* dictionaryChoice_;
    HtmlWidget* dictionaryHtml_;
    std::vector<std::string> dictionaryChoiceModules_;
    std::vector<std::string> dictionaryChoiceLabels_;
    std::shared_ptr<const std::vector<std::string>> dictionaryKeys_;
    std::string dictionaryKeysModule_;
    std::string currentDictionary_;
    std::string currentDictKey_;

    // General books tab (top pane)
    Fl_Group* generalBooksGroup_;
    Fl_Choice* generalBookChoice_;
    Fl_Button* generalBookBackButton_;
    Fl_Button* generalBookForwardButton_;
    Fl_Button* generalBookContentsButton_;
    HtmlWidget* generalBookHtml_;
    Fl_Group* generalBookTocPanel_;
    Fl_Box* generalBookTocPanelHeader_;
    Fl_Tree* generalBookTocTree_;
    std::vector<std::string> generalBookChoiceModules_;
    std::vector<std::string> generalBookChoiceLabels_;
    std::string currentGeneralBook_;
    std::string currentGeneralBookKey_;
    std::vector<GeneralBookTocEntry> generalBookToc_;
    std::unordered_map<const Fl_Tree_Item*, int> generalBookTreeItemIndices_;
    std::unordered_map<std::string, std::string> generalBookSectionCache_;
    std::deque<std::string> generalBookSectionCacheOrder_;
    int generalBookLoadedStart_ = -1;
    int generalBookLoadedEnd_ = -1;
    bool generalBookTocVisible_ = false;
    bool generalBookTreeSyncing_ = false;
    static constexpr size_t kGeneralBookSectionCacheLimit = 48;

    // Studypad tab (global, not tied to study tabs)
    Fl_Group* documentsGroup_;
    Fl_Choice* documentChoice_;
    Fl_Button* documentNewButton_;
    Fl_Button* documentSaveButton_;
    Fl_Button* documentExportButton_;
    Fl_Button* documentDeleteButton_;
    HtmlEditorWidget* documentsEditor_;
    std::vector<std::string> documentChoicePaths_;
    std::string currentDocumentPath_;
    bool documentChoiceSyncing_ = false;
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
    void populateDictionaryModules(bool eagerKeyLoad);
    void ensureDictionaryKeysLoaded();
    void populateDictionaryKeyChoices();
    void updateDictionaryNavigationChrome();
    int currentDictionaryKeyIndex() const;
    void showAdjacentDictionaryEntry(int delta);
    void showDictionaryEntryInternal(const std::string& moduleName,
                                     const std::string& key);

    /// Populate general book module choices
    void populateGeneralBookModules(bool eagerLoad);
    void populateGeneralBookToc();
    void rebuildGeneralBookTocTree();
    void syncGeneralBookTreeSelection();
    void showGeneralBookTocOverlay(bool show);
    void toggleGeneralBookTocOverlay();
    void updateGeneralBookNavigationChrome();
    void showAdjacentGeneralBookEntry(int delta);
    void rebuildGeneralBookWindow(int preserveIndex,
                                  bool alignPreserveToTop = false);
    void ensureGeneralBookViewportFilled();
    int currentGeneralBookTocIndex() const;
    std::string generalBookSectionHtml(int tocIndex);
    std::string buildGeneralBookWindowHtml();
    void restoreGeneralBookLoadedRangeFromHtml(const std::string& html);

    void layoutTopTabContents(int tabsX, int tabsY, int tabsW, int tabsH);
    TopTab visibleTopTab() const;
    void updateCommentaryEditorChrome();
    void updateDocumentChrome();
    void refreshDocumentChoices();
    bool beginCommentaryEdit();
    bool saveCommentaryEdit(bool exitEditMode);
    void cancelCommentaryEdit();
    bool deleteCurrentDocument();
    bool saveDocumentAs();
    bool saveDocumentToPath(const std::string& path);
    bool exportDocumentToOdtPath(const std::string& path);
    bool isManagedStudypadPath(const std::string& path) const;
    void loadCommentaryEditorForCurrentEntry();
    void applyCommentaryStyleOverride();
    void updateCommentarySelection(int verse);
    std::string activeBibleReference() const;
    void onHtmlLink(const std::string& url, bool commentarySource);

    // Callbacks
    static void onCommentaryModuleChange(Fl_Widget* w, void* data);
    static void onCommentaryEdit(Fl_Widget* w, void* data);
    static void onCommentarySave(Fl_Widget* w, void* data);
    static void onCommentaryCancel(Fl_Widget* w, void* data);
    static void onDictionaryModuleChange(Fl_Widget* w, void* data);
    static void onDictionaryKeyInput(Fl_Widget* w, void* data);
    static void onDictionaryBack(Fl_Widget* w, void* data);
    static void onDictionaryForward(Fl_Widget* w, void* data);
    static void onTopTabChange(Fl_Widget* w, void* data);
    static void onGeneralBookModuleChange(Fl_Widget* w, void* data);
    static void onGeneralBookBack(Fl_Widget* w, void* data);
    static void onGeneralBookForward(Fl_Widget* w, void* data);
    static void onGeneralBookContents(Fl_Widget* w, void* data);
    static void onGeneralBookTreeSelect(Fl_Widget* w, void* data);
    static void onDocumentChoiceChange(Fl_Widget* w, void* data);
    static void onDocumentNew(Fl_Widget* w, void* data);
    static void onDocumentSave(Fl_Widget* w, void* data);
    static void onDocumentExportOdt(Fl_Widget* w, void* data);
    static void onDocumentDelete(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_RIGHT_PANE_H
