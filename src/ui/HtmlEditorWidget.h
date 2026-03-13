#ifndef VERDAD_HTML_EDITOR_WIDGET_H
#define VERDAD_HTML_EDITOR_WIDGET_H

#include <FL/Fl_Group.H>
#include <FL/Enumerations.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Button;
class Fl_Choice;
class Fl_Text_Buffer;

namespace verdad {

class HtmlEditorTextArea;

/// Lightweight rich-text editor backed by a constrained HTML subset.
class HtmlEditorWidget : public Fl_Group {
public:
    enum class Mode {
        Commentary,
        Document,
    };

    using VerseTextProvider = std::function<std::string(const std::string&)>;

    struct CharFormat {
        bool bold = false;
        bool italic = false;
        unsigned char size = 3; // relative size level, 3=default
        unsigned char align = 0; // 0=left, 1=center
        bool displayPad = false; // internal-only leading spaces for centered display

        bool operator==(const CharFormat& other) const {
            return bold == other.bold &&
                   italic == other.italic &&
                   size == other.size &&
                   align == other.align &&
                   displayPad == other.displayPad;
        }
    };

    struct Snapshot {
        std::string text;
        std::vector<CharFormat> formats;
        int insertPos = 0;
        int selStart = 0;
        int selEnd = 0;
        bool modified = false;
    };

    HtmlEditorWidget(int X, int Y, int W, int H, const char* label = nullptr);
    ~HtmlEditorWidget() override;

    void setMode(Mode mode);
    Mode mode() const { return mode_; }

    void setHtml(const std::string& html);
    std::string html() const;
    std::string odtHtml() const;
    void setIndentWidth(int width);
    void setLineHeight(double lineHeight);
    void setTextFont(Fl_Font regularFont, Fl_Font boldFont, int size);
    int indentWidth() const { return indentWidth_; }
    double lineHeight() const { return lineHeight_; }

    void clearDocument();

    bool isModified() const { return modified_; }
    void setModified(bool modified);

    bool undo();
    bool redo();

    void cut();
    void copy();
    void paste();

    void toggleBold();
    void toggleItalic();
    void toggleCenterAlignment();
    void increaseTextSize();
    void decreaseTextSize();
    void toggleUnorderedList();
    void toggleOrderedList();
    void insertHorizontalRule();

    void focusEditor();
    void setVerseTextProvider(VerseTextProvider provider) {
        verseTextProvider_ = std::move(provider);
    }

    void setChangeCallback(std::function<void()> cb) {
        changeCallback_ = std::move(cb);
    }

    // Internal editor hooks used by the embedded text area.
    void prepareForUserEdit();
    void finalizeUserEditAttempt();
    bool handleEnterKey();
    bool handleTabKey(bool outdent);
    void syncToolbarState();

protected:
    void resize(int X, int Y, int W, int H) override;

private:
    friend class HtmlEditorTextArea;

    Mode mode_ = Mode::Commentary;
    Fl_Group* toolbar_ = nullptr;
    Fl_Button* undoButton_ = nullptr;
    Fl_Button* redoButton_ = nullptr;
    Fl_Button* boldButton_ = nullptr;
    Fl_Button* italicButton_ = nullptr;
    Fl_Choice* sizeChoice_ = nullptr;
    Fl_Button* centerButton_ = nullptr;
    Fl_Button* unorderedListButton_ = nullptr;
    Fl_Button* orderedListButton_ = nullptr;
    Fl_Button* ruleButton_ = nullptr;
    HtmlEditorTextArea* editor_ = nullptr;
    Fl_Text_Buffer* textBuffer_ = nullptr;
    std::vector<CharFormat> formats_;
    std::vector<Snapshot> undoStack_;
    std::vector<Snapshot> redoStack_;
    Snapshot pendingUserEdit_;
    bool pendingUserEditValid_ = false;
    bool suppressCallbacks_ = false;
    bool modified_ = false;
    int indentWidth_ = 4;
    double lineHeight_ = 1.2;
    Fl_Font textFont_ = FL_HELVETICA;
    Fl_Font boldTextFont_ = FL_HELVETICA_BOLD;
    Fl_Font italicTextFont_ = FL_HELVETICA_ITALIC;
    Fl_Font boldItalicTextFont_ = FL_HELVETICA_BOLD_ITALIC;
    int textSize_ = 14;
    std::function<void()> changeCallback_;
    VerseTextProvider verseTextProvider_;

    void buildToolbar();
    void layoutChildren();
    void rebuildStyleBuffer();
    void refreshToolbarState();
    void populateSizeChoice();
    void normalizeCenteredDisplay();
    void emitChanged();
    void renumberOrderedListBlocks();

    Snapshot captureSnapshot() const;
    void restoreSnapshot(const Snapshot& snapshot);
    void clearHistory();
    void pushUndoSnapshot(const Snapshot& snapshot);
    void discardPendingUserEdit();
    bool insertHtmlFragmentAt(int start, int end, const std::string& html);

    std::string bufferText() const;
    void setBufferText(const std::string& text);
    Fl_Font fontForFormat(const CharFormat& format) const;
    int displayFontSizeForFormat(const CharFormat& format) const;
    int editorLineAdvance() const;
    int minimumSafeLineAdvance() const;
    double measureStyledRangeWidth(const std::string& text, int start, int end) const;
    double spaceWidthForFormat(const CharFormat& format) const;
    void replaceAll(const std::string& text,
                    const std::vector<CharFormat>& formats,
                    bool markModified);

    bool selectionRange(int& start, int& end) const;
    bool selectionOrWordRange(int& start, int& end) const;
    void setTextSizeLevel(unsigned char level);
    void applyInlineTransform(const std::function<void(CharFormat&)>& transform);
    void applyLinePrefixes(const std::function<std::string(const std::string&, int)>& formatter,
                           bool removeMatching,
                           bool removeNumbered);
    void restoreCaretAfterLinePrefixEdit(int lineStart, int contentOffset);

    static void onToolbarButton(Fl_Widget* w, void* data);
    static void onTextModified(int pos,
                               int nInserted,
                               int nDeleted,
                               int nRestyled,
                               const char* deletedText,
                               void* data);
};

} // namespace verdad

#endif // VERDAD_HTML_EDITOR_WIDGET_H
