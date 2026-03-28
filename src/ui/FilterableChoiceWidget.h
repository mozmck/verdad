#ifndef VERDAD_FILTERABLE_CHOICE_WIDGET_H
#define VERDAD_FILTERABLE_CHOICE_WIDGET_H

#include <FL/Fl_Input_Choice.H>

#include <functional>
#include <limits>
#include <string>
#include <vector>

class Fl_Double_Window;
class Fl_Hold_Browser;

namespace verdad {

class PopupListWindow;

class FilterableChoiceWidget : public Fl_Input_Choice {
public:
    FilterableChoiceWidget(int X, int Y, int W, int H, const char* label = nullptr);
    ~FilterableChoiceWidget() override;

    int handle(int event) override;
    void resize(int X, int Y, int W, int H) override;

    void setItems(const std::vector<std::string>& items);
    void setItemsView(const std::vector<std::string>* items);
    void setSelectedValue(const std::string& value);
    void setDisplayedValue(const std::string& value);
    std::string selectedValue() const;

    void setNoMatchesLabel(const std::string& label);
    void setShowAllWhenFilterEmpty(bool showAll);
    void setMaxVisibleItems(size_t maxItems);
    void setEnsureItemsCallback(std::function<void()> callback);

private:
    friend class PopupListWindow;

    std::vector<std::string> ownedItems_;
    const std::vector<std::string>* items_ = &ownedItems_;
    std::string selectedItem_;
    std::string committedValue_;
    std::string noMatchesLabel_ = "No matches";
    std::string emptyFilterPrompt_ = "Type to filter";
    bool showAllWhenFilterEmpty_ = true;
    size_t maxVisibleItems_ = std::numeric_limits<size_t>::max();
    std::function<void()> ensureItemsCallback_;
    std::vector<std::string> popupItems_;
    Fl_Double_Window* popupWindow_ = nullptr;
    Fl_Hold_Browser* popupBrowser_ = nullptr;

    void refreshMenu(const std::string& filter);
    std::string exactItemMatch(const std::string& value) const;
    const std::vector<std::string>& items() const;
    void clearMenu();
    void destroyPopup();
    void ensureItemsLoaded();
    void ensurePopupCreated();
    void refreshPopupContents(const std::string& filter);
    void updatePopupGeometry();
    bool showPopup();
    void hidePopup();
    void cancelEditing();
    void onPopupDestroyed();
    bool popupVisible() const;
    int popupSelectionLine() const;
    void movePopupSelection(int delta);
    void selectPopupLine(int line, bool invokeCallback);
    bool handlePopupKey(int key);
    bool hasPendingEdits() const;
    bool ownsFocus();
    bool containsRootPoint(int rootX, int rootY) const;
    bool popupContainsRootPoint(int rootX, int rootY) const;
    bool menuButtonContainsRootPoint(int rootX, int rootY);
    bool eventInMenuButton();

    static FilterableChoiceWidget* widgetAtRootPoint(int rootX, int rootY);
    static FilterableChoiceWidget* widgetWithFocus();
    static int dispatchEvent(int event, Fl_Window* window);
    static int handleGlobalEvent(int event);
    static void registerGlobalHandler();
    static void unregisterGlobalHandler();

    static void onInputChanged(Fl_Widget* w, void* data);
    static void onPopupSelected(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_FILTERABLE_CHOICE_WIDGET_H
