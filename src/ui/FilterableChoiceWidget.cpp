#include "ui/FilterableChoiceWidget.h"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Menu_Button.H>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace verdad {

class PopupListWindow : public Fl_Double_Window {
public:
    PopupListWindow(FilterableChoiceWidget* owner, int X, int Y, int W, int H)
        : Fl_Double_Window(X, Y, W, H)
        , owner_(owner) {
        box(FL_NO_BOX);
        clear_border();
    }

    ~PopupListWindow() override {
        if (owner_) owner_->onPopupDestroyed();
    }

private:
    FilterableChoiceWidget* owner_ = nullptr;
};

namespace {

constexpr int kPopupMaxRows = 10;
constexpr int kPopupMinWidth = 120;

char lowerAsciiChar(char c) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
}

std::string_view trimView(std::string_view text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

bool equalsNoCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;

    for (size_t i = 0; i < left.size(); ++i) {
        if (lowerAsciiChar(left[i]) != lowerAsciiChar(right[i])) {
            return false;
        }
    }
    return true;
}

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;

    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char left, char right) {
                              return lowerAsciiChar(left) == lowerAsciiChar(right);
                          });
    return it != haystack.end();
}

std::string browserLineLabel(const std::string& label) {
    return "@." + label;
}

std::pair<int, int> rootPosition(const Fl_Widget* widget) {
    if (!widget) return {0, 0};

    int rootX = widget->x();
    int rootY = widget->y();
    const Fl_Window* top = widget->window();
    if (top) {
        rootX += top->x_root();
        rootY += top->y_root();
    }

    return {rootX, rootY};
}

bool rootPointInsideWidget(const Fl_Widget* widget, int rootX, int rootY) {
    if (!widget || !widget->window() || !widget->visible_r()) return false;

    const auto [widgetX, widgetY] = rootPosition(widget);
    return rootX >= widgetX &&
           rootX < widgetX + widget->w() &&
           rootY >= widgetY &&
           rootY < widgetY + widget->h();
}

std::vector<FilterableChoiceWidget*>& liveWidgets() {
    static std::vector<FilterableChoiceWidget*> widgets;
    return widgets;
}

FilterableChoiceWidget*& openPopupOwner() {
    static FilterableChoiceWidget* owner = nullptr;
    return owner;
}

bool& globalHandlerRegistered() {
    static bool registered = false;
    return registered;
}

bool& dispatchRegistered() {
    static bool registered = false;
    return registered;
}

Fl_Event_Dispatch& previousDispatch() {
    static Fl_Event_Dispatch dispatch = nullptr;
    return dispatch;
}

} // namespace

FilterableChoiceWidget::FilterableChoiceWidget(int X, int Y, int W, int H,
                                               const char* label)
    : Fl_Input_Choice(X, Y, W, H, label) {
    input()->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    input()->callback(onInputChanged, this);
    clearMenu();

    liveWidgets().push_back(this);
    registerGlobalHandler();
}

FilterableChoiceWidget::~FilterableChoiceWidget() {
    hidePopup();
    destroyPopup();

    auto& widgets = liveWidgets();
    widgets.erase(std::remove(widgets.begin(), widgets.end(), this), widgets.end());
    unregisterGlobalHandler();
}

int FilterableChoiceWidget::handle(int event) {
    if (event == FL_PUSH && eventInMenuButton()) {
        showPopup();
        if (input()) input()->take_focus();
        return 1;
    }

    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        Fl::focus() == menubutton()) {
        const int key = Fl::event_key();
        if (key == ' ' || key == FL_Down || key == FL_Up) {
            showPopup();
            if (input()) input()->take_focus();
            if (popupVisible() && key == FL_Up) {
                movePopupSelection(-1);
            } else if (popupVisible() && key == FL_Down) {
                movePopupSelection(1);
            }
            return 1;
        }
    }

    return Fl_Input_Choice::handle(event);
}

void FilterableChoiceWidget::resize(int X, int Y, int W, int H) {
    Fl_Input_Choice::resize(X, Y, W, H);
    if (popupVisible()) updatePopupGeometry();
}

void FilterableChoiceWidget::setItems(const std::vector<std::string>& items) {
    ownedItems_ = items;
    items_ = &ownedItems_;
    if (!selectedItem_.empty() && exactItemMatch(selectedItem_).empty()) {
        selectedItem_.clear();
    }
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setItemsView(const std::vector<std::string>* items) {
    items_ = items ? items : &ownedItems_;
    if (!selectedItem_.empty() && exactItemMatch(selectedItem_).empty()) {
        selectedItem_.clear();
    }
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setSelectedValue(const std::string& value) {
    selectedItem_ = exactItemMatch(value);
    committedValue_ = value;
    Fl_Input_Choice::value(value.c_str());
    hidePopup();
    refreshMenu(value);
}

void FilterableChoiceWidget::setDisplayedValue(const std::string& value) {
    selectedItem_ = exactItemMatch(value);
    committedValue_ = value;
    Fl_Input_Choice::value(value.c_str());
    hidePopup();
    clearMenu();
}

std::string FilterableChoiceWidget::selectedValue() const {
    std::string typed = Fl_Input_Choice::value() ? Fl_Input_Choice::value() : "";
    std::string exact = exactItemMatch(typed);
    if (!exact.empty()) return exact;
    return selectedItem_;
}

void FilterableChoiceWidget::setNoMatchesLabel(const std::string& label) {
    noMatchesLabel_ = label;
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setShowAllWhenFilterEmpty(bool showAll) {
    showAllWhenFilterEmpty_ = showAll;
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setMaxVisibleItems(size_t maxItems) {
    if (maxItems == 0) {
        maxVisibleItems_ = std::numeric_limits<size_t>::max();
    } else {
        maxVisibleItems_ = maxItems;
    }
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setEnsureItemsCallback(std::function<void()> callback) {
    ensureItemsCallback_ = std::move(callback);
}

const std::vector<std::string>& FilterableChoiceWidget::items() const {
    return items_ ? *items_ : ownedItems_;
}

void FilterableChoiceWidget::clearMenu() {
    Fl_Menu_Button* menuButton = menubutton();
    if (!menuButton) return;

    menuButton->clear();
    menuButton->value(-1);
}

void FilterableChoiceWidget::destroyPopup() {
    if (!popupWindow_) return;

    Fl_Widget* popup = popupWindow_;
    popupBrowser_ = nullptr;
    popupWindow_ = nullptr;
    if (popup->parent()) {
        popup->parent()->remove(*popup);
    }
    delete popup;
}

void FilterableChoiceWidget::refreshMenu(const std::string& filter) {
    clearMenu();
    if (popupVisible()) refreshPopupContents(filter);
}

void FilterableChoiceWidget::ensureItemsLoaded() {
    if (items().empty() && ensureItemsCallback_) {
        ensureItemsCallback_();
    }
}

void FilterableChoiceWidget::ensurePopupCreated() {
    if (popupWindow_) return;

    Fl_Window* hostWindow = window();
    if (!hostWindow) return;

    Fl_Group* savedCurrent = Fl_Group::current();
    Fl_Group::current(hostWindow);
    popupWindow_ = new PopupListWindow(this, x(), y() + h() - 1, w(), 1);
    popupWindow_->begin();
    popupBrowser_ = new Fl_Hold_Browser(0, 0, w(), 1);
    popupBrowser_->box(FL_DOWN_BOX);
    popupBrowser_->when(FL_WHEN_CHANGED);
    popupBrowser_->callback(onPopupSelected, this);
    popupWindow_->resizable(popupBrowser_);
    popupWindow_->end();
    popupWindow_->hide();
    Fl_Group::current(savedCurrent);
}

void FilterableChoiceWidget::refreshPopupContents(const std::string& filter) {
    if (!popupBrowser_) return;

    popupBrowser_->textfont(input()->textfont());
    popupBrowser_->textsize(input()->textsize());
    popupBrowser_->clear();
    popupItems_.clear();

    std::string_view normalizedFilter = trimView(filter);
    if (normalizedFilter.empty() && !showAllWhenFilterEmpty_) {
        if (!emptyFilterPrompt_.empty()) {
            popupBrowser_->add(browserLineLabel(emptyFilterPrompt_).c_str());
        }
        popupBrowser_->value(0);
        updatePopupGeometry();
        return;
    }

    for (const auto& item : items()) {
        if (normalizedFilter.empty() ||
            containsNoCase(item, normalizedFilter)) {
            if (popupItems_.size() >= maxVisibleItems_) break;
            popupItems_.push_back(item);
            popupBrowser_->add(browserLineLabel(item).c_str());
        }
    }

    if (popupItems_.empty()) {
        if (!noMatchesLabel_.empty()) {
            popupBrowser_->add(browserLineLabel(noMatchesLabel_).c_str());
        }
        popupBrowser_->value(0);
        updatePopupGeometry();
        return;
    }

    std::string exact = exactItemMatch(
        input() && input()->value() ? input()->value() : "");

    int selectedLine = 1;
    const std::string& preferred = !exact.empty() ? exact : selectedItem_;
    if (!preferred.empty()) {
        auto it = std::find(popupItems_.begin(), popupItems_.end(), preferred);
        if (it != popupItems_.end()) {
            selectedLine = static_cast<int>(std::distance(popupItems_.begin(), it)) + 1;
        }
    }
    popupBrowser_->value(selectedLine);
    popupBrowser_->show(selectedLine);
    updatePopupGeometry();
}

std::string FilterableChoiceWidget::exactItemMatch(const std::string& value) const {
    std::string_view wanted = trimView(value);
    if (wanted.empty()) return "";

    for (const auto& item : items()) {
        if (equalsNoCase(item, wanted)) {
            return item;
        }
    }
    return "";
}

void FilterableChoiceWidget::updatePopupGeometry() {
    if (!popupWindow_ || !popupBrowser_ || !window()) return;

    const int lineCount = std::max(1, popupBrowser_->size());
    const int visibleRows = std::min(lineCount, kPopupMaxRows);
    const int lineHeight = std::max(20, static_cast<int>(popupBrowser_->textsize()) + 8);
    int popupW = std::max(w(), kPopupMinWidth);
    int popupH = std::max(lineHeight + 4, (visibleRows * lineHeight) + 4);

    const int hostW = std::max(1, window()->w());
    const int hostH = std::max(1, window()->h());
    popupW = std::min(popupW, hostW);

    const int popupBelowY = y() + h() - 1;
    const int spaceBelow = std::max(0, hostH - popupBelowY);
    const int spaceAbove = std::max(0, y());
    const bool placeBelow = (spaceBelow >= popupH) || (spaceBelow >= spaceAbove);
    if (placeBelow && spaceBelow > 0) {
        popupH = std::min(popupH, spaceBelow);
    } else if (!placeBelow && spaceAbove > 0) {
        popupH = std::min(popupH, spaceAbove);
    }
    popupH = std::max(lineHeight + 4, popupH);

    const int maxPopupX = std::max(0, hostW - popupW);
    const int popupX = std::clamp(x(), 0, maxPopupX);
    const int popupY = placeBelow ? popupBelowY : y() - popupH + 1;

    popupWindow_->resize(popupX, popupY, popupW, popupH);
    popupBrowser_->resize(0, 0, popupW, popupH);
}

bool FilterableChoiceWidget::showPopup() {
    if (!window() || !visible_r() || !active_r()) return false;

    ensureItemsLoaded();
    ensurePopupCreated();
    if (!popupWindow_) return false;
    if (openPopupOwner() && openPopupOwner() != this) {
        openPopupOwner()->hidePopup();
    }

    refreshPopupContents(input() && input()->value() ? input()->value() : "");
    if (!popupBrowser_ || popupBrowser_->size() <= 0) {
        hidePopup();
        return false;
    }

    updatePopupGeometry();
    openPopupOwner() = this;
    if (popupWindow_->parent() == window()) {
        window()->remove(*popupWindow_);
        window()->add(*popupWindow_);
    }
    popupWindow_->show();
    if (input()) input()->take_focus();
    popupWindow_->redraw();
    if (window()) window()->redraw();
    return true;
}

void FilterableChoiceWidget::hidePopup() {
    if (openPopupOwner() == this) {
        openPopupOwner() = nullptr;
    }
    if (popupWindow_ && popupWindow_->shown()) {
        popupWindow_->hide();
        if (window()) window()->redraw();
    }
}

void FilterableChoiceWidget::cancelEditing() {
    if (!popupVisible() && !hasPendingEdits()) return;

    hidePopup();
    Fl_Input_Choice::value(committedValue_.c_str());
    if (input()) {
        const int cursor = static_cast<int>(committedValue_.size());
        input()->insert_position(cursor);
        input()->mark(cursor);
        input()->redraw();
    }
}

void FilterableChoiceWidget::onPopupDestroyed() {
    popupBrowser_ = nullptr;
    popupWindow_ = nullptr;
}

bool FilterableChoiceWidget::popupVisible() const {
    return popupWindow_ && popupWindow_->shown() && openPopupOwner() == this;
}

int FilterableChoiceWidget::popupSelectionLine() const {
    if (!popupBrowser_ || popupItems_.empty()) return 0;

    int line = popupBrowser_->value();
    if (line <= 0 || line > static_cast<int>(popupItems_.size())) return 0;
    return line;
}

void FilterableChoiceWidget::movePopupSelection(int delta) {
    if (!popupBrowser_ || popupItems_.empty() || delta == 0) return;

    int line = popupSelectionLine();
    if (line <= 0) {
        line = (delta > 0) ? 1 : static_cast<int>(popupItems_.size());
    } else {
        line = std::clamp(line + delta,
                          1,
                          static_cast<int>(popupItems_.size()));
    }

    popupBrowser_->value(line);
    popupBrowser_->show(line);
    popupBrowser_->redraw();
}

void FilterableChoiceWidget::selectPopupLine(int line, bool invokeCallback) {
    if (line <= 0 || line > static_cast<int>(popupItems_.size())) return;

    const std::string selected = popupItems_[static_cast<size_t>(line - 1)];
    selectedItem_ = selected;
    Fl_Input_Choice::value(selected.c_str());
    hidePopup();

    if (input()) {
        input()->insert_position(static_cast<int>(selected.size()));
        input()->mark(static_cast<int>(selected.size()));
        input()->take_focus();
    }

    if (invokeCallback) {
        do_callback(FL_REASON_CHANGED);
    }
}

bool FilterableChoiceWidget::handlePopupKey(int key) {
    switch (key) {
    case FL_Escape:
        cancelEditing();
        return true;
    case FL_Down:
        movePopupSelection(1);
        return true;
    case FL_Up:
        movePopupSelection(-1);
        return true;
    case FL_Page_Down:
        movePopupSelection(kPopupMaxRows);
        return true;
    case FL_Page_Up:
        movePopupSelection(-kPopupMaxRows);
        return true;
    case FL_Home:
        if (!popupItems_.empty()) movePopupSelection(-static_cast<int>(popupItems_.size()));
        return true;
    case FL_End:
        if (!popupItems_.empty()) movePopupSelection(static_cast<int>(popupItems_.size()));
        return true;
    case FL_Enter:
    case FL_KP_Enter: {
        int line = popupSelectionLine();
        if (line <= 0 && !popupItems_.empty()) line = 1;
        if (line > 0) {
            selectPopupLine(line, true);
        } else {
            hidePopup();
            do_callback(FL_REASON_ENTER_KEY);
        }
        return true;
    }
    default:
        return false;
    }
}

bool FilterableChoiceWidget::hasPendingEdits() const {
    const char* currentValue = Fl_Input_Choice::value();
    const std::string current = currentValue ? currentValue : "";
    return current != committedValue_;
}

bool FilterableChoiceWidget::ownsFocus() {
    Fl_Widget* focus = Fl::focus();
    return focus == this || focus == input() || focus == menubutton();
}

bool FilterableChoiceWidget::containsRootPoint(int rootX, int rootY) const {
    return rootPointInsideWidget(this, rootX, rootY);
}

bool FilterableChoiceWidget::popupContainsRootPoint(int rootX, int rootY) const {
    if (!popupWindow_ || !popupWindow_->shown()) return false;

    return rootX >= popupWindow_->x_root() &&
           rootX < popupWindow_->x_root() + popupWindow_->w() &&
           rootY >= popupWindow_->y_root() &&
           rootY < popupWindow_->y_root() + popupWindow_->h();
}

bool FilterableChoiceWidget::menuButtonContainsRootPoint(int rootX, int rootY) {
    return rootPointInsideWidget(menubutton(), rootX, rootY);
}

bool FilterableChoiceWidget::eventInMenuButton() {
    const int rootX = Fl::event_x_root();
    const int rootY = Fl::event_y_root();
    return popupVisible() ? menuButtonContainsRootPoint(rootX, rootY)
                          : rootPointInsideWidget(menubutton(), rootX, rootY);
}

FilterableChoiceWidget* FilterableChoiceWidget::widgetAtRootPoint(int rootX, int rootY) {
    auto& widgets = liveWidgets();
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
        FilterableChoiceWidget* widget = *it;
        if (!widget || !widget->active_r() || !widget->visible_r()) continue;
        if (widget->containsRootPoint(rootX, rootY)) return widget;
    }
    return nullptr;
}

FilterableChoiceWidget* FilterableChoiceWidget::widgetWithFocus() {
    Fl_Widget* focus = Fl::focus();
    if (!focus) return nullptr;

    auto& widgets = liveWidgets();
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
        FilterableChoiceWidget* widget = *it;
        if (!widget || !widget->active_r() || !widget->visible_r()) continue;
        if (focus == widget || focus == widget->input() || focus == widget->menubutton()) {
            return widget;
        }
    }
    return nullptr;
}

int FilterableChoiceWidget::dispatchEvent(int event, Fl_Window* window) {
    FilterableChoiceWidget* openWidget = openPopupOwner();
    if (event == FL_PUSH && openWidget && openWidget->popupVisible()) {
        const int rootX = Fl::event_x_root();
        const int rootY = Fl::event_y_root();
        if (!openWidget->containsRootPoint(rootX, rootY) &&
            !openWidget->popupContainsRootPoint(rootX, rootY)) {
            openWidget->cancelEditing();
        }
    }

    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        openWidget && openWidget->popupVisible() && openWidget->ownsFocus()) {
        const int key = Fl::event_key();
        if (key == FL_Tab) {
            openWidget->cancelEditing();
        } else if (openWidget->handlePopupKey(key)) {
            return 1;
        }
    }

    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) && !openWidget) {
        FilterableChoiceWidget* focusedWidget = widgetWithFocus();
        if (focusedWidget && Fl::event_key() == FL_Escape &&
            focusedWidget->hasPendingEdits()) {
            focusedWidget->cancelEditing();
            return 1;
        }
        if (focusedWidget && (Fl::event_key() == FL_Down || Fl::event_key() == FL_Up)) {
            if (focusedWidget->showPopup()) {
                focusedWidget->movePopupSelection(Fl::event_key() == FL_Down ? 1 : -1);
                return 1;
            }
        }
    }

    Fl_Event_Dispatch dispatch = previousDispatch();
    return dispatch ? dispatch(event, window) : Fl::handle_(event, window);
}

void FilterableChoiceWidget::registerGlobalHandler() {
    if (globalHandlerRegistered()) return;
    Fl::add_handler(handleGlobalEvent);
    globalHandlerRegistered() = true;
    if (!dispatchRegistered()) {
        previousDispatch() = Fl::event_dispatch();
        Fl::event_dispatch(dispatchEvent);
        dispatchRegistered() = true;
    }
}

void FilterableChoiceWidget::unregisterGlobalHandler() {
    if (!globalHandlerRegistered() || !liveWidgets().empty()) return;
    Fl::remove_handler(handleGlobalEvent);
    globalHandlerRegistered() = false;
    if (dispatchRegistered() && Fl::event_dispatch() == dispatchEvent) {
        Fl::event_dispatch(previousDispatch());
    }
    previousDispatch() = nullptr;
    dispatchRegistered() = false;
}

int FilterableChoiceWidget::handleGlobalEvent(int event) {
    if (event == FL_PUSH) {
        const int rootX = Fl::event_x_root();
        const int rootY = Fl::event_y_root();
        FilterableChoiceWidget* openWidget = openPopupOwner();
        if (openWidget && openWidget->popupContainsRootPoint(rootX, rootY)) {
            return 0;
        }

        FilterableChoiceWidget* target = widgetAtRootPoint(rootX, rootY);
        if (!target || Fl::event_button() != FL_LEFT_MOUSE) return 0;

        target->showPopup();
        if (target->menuButtonContainsRootPoint(rootX, rootY)) {
            if (target->input()) target->input()->take_focus();
            return 1;
        }
        return 0;
    }

    if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
        FilterableChoiceWidget* openWidget = openPopupOwner();
        if (!openWidget || !openWidget->popupVisible()) return 0;

        const int key = Fl::event_key();
        if (key == FL_Escape && openWidget->ownsFocus()) {
            openWidget->cancelEditing();
            return 1;
        }
        if (key == FL_Tab) {
            openWidget->cancelEditing();
        }
    }

    return 0;
}

void FilterableChoiceWidget::onInputChanged(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<FilterableChoiceWidget*>(data);
    if (!self) return;

    self->ensureItemsLoaded();

    std::string filter = self->input() && self->input()->value()
        ? self->input()->value()
        : "";
    if (self->popupVisible()) {
        self->refreshPopupContents(filter);
    } else if (self->ownsFocus()) {
        self->showPopup();
    }

    const int key = Fl::event_key();
    if (Fl::event() == FL_KEYBOARD &&
        (key == FL_Enter || key == FL_KP_Enter)) {
        if (self->popupVisible() && !self->popupItems_.empty()) {
            int line = self->popupSelectionLine();
            if (line <= 0) line = 1;
            self->selectPopupLine(line, true);
        } else {
            self->hidePopup();
            self->do_callback(FL_REASON_ENTER_KEY);
        }
    }
}

void FilterableChoiceWidget::onPopupSelected(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<FilterableChoiceWidget*>(data);
    if (!self || !self->popupBrowser_) return;

    self->selectPopupLine(self->popupBrowser_->value(), true);
}

} // namespace verdad
