#ifndef VERDAD_HTML_EDITOR_WIDGET_H
#define VERDAD_HTML_EDITOR_WIDGET_H

#include <FL/Fl_Group.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Button;
class Fl_Text_Buffer;
class Fl_Text_Editor;

namespace verdad {

/// Lightweight rich-text editor backed by a constrained HTML subset.
class HtmlEditorWidget : public Fl_Group {
public:
    enum class Mode {
        Commentary,
        Document,
    };

    struct CharFormat {
        bool bold = false;
        bool italic = false;
        unsigned char size = 1; // 0=small, 1=normal, 2=large

        bool operator==(const CharFormat& other) const {
            return bold == other.bold &&
                   italic == other.italic &&
                   size == other.size;
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
    void setIndentWidth(int width);
    int indentWidth() const { return indentWidth_; }

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
    void increaseTextSize();
    void decreaseTextSize();
    void toggleUnorderedList();
    void toggleOrderedList();
    void insertHorizontalRule();

    void focusEditor();

    void setChangeCallback(std::function<void()> cb) {
        changeCallback_ = std::move(cb);
    }

    // Internal editor hooks used by the embedded text area.
    void prepareForUserEdit();
    void finalizeUserEditAttempt();
    bool handleEnterKey();
    bool handleTabKey(bool outdent);

protected:
    void resize(int X, int Y, int W, int H) override;

private:
    Mode mode_ = Mode::Commentary;
    Fl_Group* toolbar_ = nullptr;
    Fl_Button* undoButton_ = nullptr;
    Fl_Button* redoButton_ = nullptr;
    Fl_Button* boldButton_ = nullptr;
    Fl_Button* italicButton_ = nullptr;
    Fl_Button* smallerButton_ = nullptr;
    Fl_Button* largerButton_ = nullptr;
    Fl_Button* unorderedListButton_ = nullptr;
    Fl_Button* orderedListButton_ = nullptr;
    Fl_Button* ruleButton_ = nullptr;
    Fl_Text_Editor* editor_ = nullptr;
    Fl_Text_Buffer* textBuffer_ = nullptr;
    Fl_Text_Buffer* styleBuffer_ = nullptr;
    std::vector<CharFormat> formats_;
    std::vector<Snapshot> undoStack_;
    std::vector<Snapshot> redoStack_;
    Snapshot pendingUserEdit_;
    bool pendingUserEditValid_ = false;
    bool suppressCallbacks_ = false;
    bool modified_ = false;
    int indentWidth_ = 4;
    std::function<void()> changeCallback_;

    void buildToolbar();
    void layoutChildren();
    void rebuildStyleBuffer();
    void refreshToolbarState();
    void emitChanged();
    void renumberOrderedListBlocks();

    Snapshot captureSnapshot() const;
    void restoreSnapshot(const Snapshot& snapshot);
    void clearHistory();
    void pushUndoSnapshot(const Snapshot& snapshot);
    void discardPendingUserEdit();

    std::string bufferText() const;
    void setBufferText(const std::string& text);
    void replaceAll(const std::string& text,
                    const std::vector<CharFormat>& formats,
                    bool markModified);

    bool selectionRange(int& start, int& end) const;
    bool selectionOrWordRange(int& start, int& end) const;
    void applyInlineTransform(const std::function<void(CharFormat&)>& transform);
    void applyLinePrefixes(const std::function<std::string(const std::string&, int)>& formatter,
                           bool removeMatching,
                           bool removeNumbered);

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
