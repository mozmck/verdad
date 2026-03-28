#ifndef VERDAD_BIBLE_BOOK_CHOICE_H
#define VERDAD_BIBLE_BOOK_CHOICE_H

#include <FL/Fl_Choice.H>

#include <string>
#include <vector>

class Fl_Box;
class Fl_Double_Window;
class Fl_Hold_Browser;

namespace verdad {

class BibleBookChoice : public Fl_Choice {
public:
    BibleBookChoice(int X, int Y, int W, int H, const char* label = nullptr);
    ~BibleBookChoice() override;

    int handle(int event) override;
    void resize(int X, int Y, int W, int H) override;

    void setBrowserLineSpacing(int pixels);
    void setBookColumns(const std::vector<std::string>& oldTestament,
                        const std::vector<std::string>& newTestament);

private:
    friend class BibleBookChoicePopupWindow;
    friend class BibleBookChoiceBrowser;

    std::vector<std::string> oldTestamentBooks_;
    std::vector<std::string> newTestamentBooks_;
    Fl_Double_Window* popupWindow_ = nullptr;
    Fl_Box* oldTestamentHeader_ = nullptr;
    Fl_Box* newTestamentHeader_ = nullptr;
    Fl_Hold_Browser* oldTestamentBrowser_ = nullptr;
    Fl_Hold_Browser* newTestamentBrowser_ = nullptr;
    int browserLineSpacing_ = 0;

    void destroyPopup();
    void ensurePopupCreated();
    void refreshPopupContents();
    void updatePopupGeometry();
    void layoutPopupContents();
    bool showPopup(int preferredColumn = -1);
    void hidePopup();
    bool popupVisible() const;
    void focusColumn(int columnIndex, int preferredLine = 0);
    bool popupContainsRootPoint(int rootX, int rootY) const;
    int currentPopupColumn() const;
    void selectBrowserLine(Fl_Hold_Browser* browser, int line);
    int menuIndexForSelection(const Fl_Hold_Browser* browser, int line) const;

    static void onPopupPicked(Fl_Widget* widget, void* data);
};

} // namespace verdad

#endif // VERDAD_BIBLE_BOOK_CHOICE_H
