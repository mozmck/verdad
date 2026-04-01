#include "ui/ChoicePopupUtils.h"
#include "ui/WrappingChoice.h"

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>

#include <algorithm>
#include <cmath>
#include <utility>

namespace verdad {

namespace {

constexpr int kPopupPadding = 4;
constexpr int kPopupGap = 4;
constexpr int kPopupScrollbarAllowance = 24;
constexpr int kPopupLinePadding = 14;
constexpr int kPopupBrowserBorder = 4;
constexpr int kPopupScreenEdgeMargin = 12;

int browserLineHeightForChoice(const WrappingChoice* choice) {
    if (!choice) return 20;

    return std::max(20,
                    static_cast<int>(choice->textsize()) +
                        std::max(4, Fl::menu_linespacing()) + 4);
}

int measureEntryWidth(const WrappingChoice* choice, int menuIndex) {
    if (!choice || !choice->menu() ||
        menuIndex < 0 || menuIndex >= choice->size() - 1) {
        return choice_popup::kPopupDefaultMinColumnW;
    }

    const Fl_Menu_Item& item = choice->menu()[menuIndex];
    const std::string label = choice_popup::displayLabelForMenuText(item.label());
    return std::max(choice_popup::kPopupDefaultMinColumnW,
                    choice_popup::measureLabelWidth(choice->textfont(),
                                                    choice->textsize(),
                                                    label) +
                        kPopupScrollbarAllowance + kPopupLinePadding);
}

} // namespace

class WrappingChoicePopupWindow : public Fl_Double_Window {
public:
    WrappingChoicePopupWindow(WrappingChoice* owner, int X, int Y, int W, int H)
        : Fl_Double_Window(X, Y, W, H)
        , owner_(owner) {
        box(FL_UP_BOX);
        color(FL_BACKGROUND2_COLOR);
        set_menu_window();
        clear_border();
    }

    int handle(int event) override {
        if (owner_) {
            if (event == FL_PUSH &&
                !owner_->popupContainsRootPoint(Fl::event_x_root(), Fl::event_y_root())) {
                choice_popup::PopupOwner* target =
                    choice_popup::popupOwnerAtRootPoint(Fl::event_x_root(), Fl::event_y_root());
                owner_->hidePopup();
                if (target && target != owner_) {
                    if (Fl::visible_focus()) Fl::focus(target->popupAnchorWidget());
                    target->showOwnedPopup();
                } else if (target == owner_) {
                    owner_->takeOwnedFocus();
                }
                return 1;
            }

            if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
                const int key = Fl::event_key();
                if (key == FL_Escape || key == FL_Tab) {
                    owner_->hidePopup();
                    owner_->takeOwnedFocus();
                    return 1;
                }
            }
        }

        return Fl_Double_Window::handle(event);
    }

private:
    WrappingChoice* owner_ = nullptr;
};

class WrappingChoiceColumnBrowser : public Fl_Hold_Browser {
public:
    WrappingChoiceColumnBrowser(WrappingChoice* owner,
                                int columnIndex,
                                int X, int Y, int W, int H)
        : Fl_Hold_Browser(X, Y, W, H)
        , owner_(owner)
        , columnIndex_(columnIndex) {
        when(FL_WHEN_RELEASE | FL_WHEN_NOT_CHANGED);
        callback(WrappingChoice::onPopupPicked, owner_);
        box(FL_BORDER_BOX);
        has_scrollbar(Fl_Browser_::VERTICAL);
    }

    int preferredWidgetHeight() const {
        const Fl_Boxtype borderBox = box() ? box() : FL_DOWN_BOX;
        return std::max(0, full_height()) + Fl::box_dh(borderBox);
    }

    int minimumWidgetHeight() const {
        const Fl_Boxtype borderBox = box() ? box() : FL_DOWN_BOX;
        return std::max(1, incr_height()) + Fl::box_dh(borderBox);
    }

    int heightForRows(int rows) const {
        const Fl_Boxtype borderBox = box() ? box() : FL_DOWN_BOX;
        return (std::max(1, rows) * std::max(1, incr_height())) +
               Fl::box_dh(borderBox);
    }

    int handle(int event) override {
        if (owner_ && (event == FL_KEYBOARD || event == FL_SHORTCUT)) {
            switch (Fl::event_key()) {
            case FL_Left:
                owner_->moveFocus(-1);
                return 1;
            case FL_Right:
                owner_->moveFocus(1);
                return 1;
            case FL_Enter:
            case FL_KP_Enter:
                owner_->selectBrowserLine(this, value());
                return 1;
            case FL_Escape:
            case FL_Tab:
                owner_->hidePopup();
                owner_->takeOwnedFocus();
                return 1;
            default:
                break;
            }
        }

        return Fl_Hold_Browser::handle(event);
    }

private:
    WrappingChoice* owner_ = nullptr;
    int columnIndex_ = 0;
};

WrappingChoice::WrappingChoice(int X, int Y, int W, int H, const char* label)
    : Fl_Choice(X, Y, W, H, label) {
    choice_popup::registerPopupOwner(this);
}

WrappingChoice::~WrappingChoice() {
    hidePopup();
    destroyPopup();
    choice_popup::unregisterPopupOwner(this);
}

int WrappingChoice::handle(int event) {
    if (!supportsCustomPopup()) {
        return Fl_Choice::handle(event);
    }

    const bool keyboardOpen =
        (event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        (Fl::event_key() == ' ' || Fl::event_key() == FL_Down || Fl::event_key() == FL_Up) &&
        !(Fl::event_state() & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META));

    const Fl_Menu_Item* shortcutItem = nullptr;

    switch (event) {
    case FL_ENTER:
    case FL_LEAVE:
        return 1;
    case FL_PUSH:
        if (Fl::visible_focus()) Fl::focus(this);
        showPopup();
        return 1;
    case FL_KEYBOARD:
        if (!keyboardOpen) return 0;
        if (Fl::visible_focus()) Fl::focus(this);
        showPopup();
        return 1;
    case FL_SHORTCUT:
        if (keyboardOpen) {
            if (Fl::visible_focus()) Fl::focus(this);
            showPopup();
            return 1;
        }
        if (Fl_Widget::test_shortcut()) {
            if (Fl::visible_focus()) Fl::focus(this);
            showPopup();
            return 1;
        }
        shortcutItem = menu()->test_shortcut();
        if (!shortcutItem || shortcutItem->submenu()) return 0;
        if (shortcutItem != mvalue()) redraw();
        picked(shortcutItem);
        return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
        if (Fl::visible_focus()) {
            redraw();
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

void WrappingChoice::resize(int X, int Y, int W, int H) {
    Fl_Choice::resize(X, Y, W, H);
    if (popupVisible()) {
        hidePopup();
    }
}

void WrappingChoice::destroyPopup() {
    if (!popupWindow_) return;

    if (Fl::grab() == popupWindow_) {
        Fl::grab(nullptr);
    }
    if (choice_popup::activePopupOwner() == this) {
        choice_popup::activePopupOwner() = nullptr;
    }

    delete popupWindow_;
    popupWindow_ = nullptr;
    popupBrowsers_.clear();
    popupColumnEntries_.clear();
    popupEntries_.clear();
}

bool WrappingChoice::supportsCustomPopup() const {
    if (!menu() || size() <= 1) return false;

    for (int i = 0; i < size() - 1; ++i) {
        const Fl_Menu_Item& item = menu()[i];
        if (!item.label() || !item.visible()) continue;
        if (item.submenu()) return false;
        if (item.labeltype() != FL_NORMAL_LABEL) return false;
    }

    return true;
}

bool WrappingChoice::buildPopup() {
    menu_end();
    popupEntries_.clear();

    for (int i = 0; i < size() - 1; ++i) {
        const Fl_Menu_Item& item = menu()[i];
        if (!item.label() || !item.visible() || item.submenu()) continue;

        PopupEntry entry;
        entry.menuIndex = i;
        entry.label = choice_popup::displayLabelForMenuText(item.label());
        entry.active = item.active();
        popupEntries_.push_back(std::move(entry));
    }

    if (popupEntries_.empty()) return false;

    int anchorX = 0;
    int anchorY = 0;
    Fl_Window* top = top_window_offset(anchorX, anchorY);
    const int topRootX = top ? top->x_root() : 0;
    const int topRootY = top ? top->y_root() : 0;
    const int rootX = anchorX + topRootX;
    const int rootY = anchorY + topRootY;
    const int screenNum = window() ? window()->screen_num() : Fl::screen_num(rootX, rootY);
    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    Fl::screen_work_area(workX, workY, workW, workH, screenNum);

    const int popupY = rootY + h() - 1;
    const int workRight = workX + workW;
    const int workBottom = workY + workH;
    const int availableHeight = std::max(
        browserLineHeightForChoice(this) + kPopupBrowserBorder + (kPopupPadding * 2),
        workBottom - popupY - kPopupScreenEdgeMargin);
    const int availableWidth = std::max(
        w(),
        workW - (kPopupScreenEdgeMargin * 2));

    const int lineHeight = browserLineHeightForChoice(this);
    const int maxVisibleRows = std::max(
        1,
        (availableHeight - (kPopupPadding * 2) - kPopupBrowserBorder) /
            std::max(1, lineHeight));
    const int idealColumns = std::max(
        1,
        static_cast<int>(std::ceil(
            static_cast<double>(popupEntries_.size()) /
            static_cast<double>(maxVisibleRows))));

    struct Layout {
        int columnCount = 1;
        std::vector<std::vector<int>> entries;
        std::vector<int> widths;
        int visibleRows = 1;
        int popupW = 0;
    };

    Layout chosen;
    for (int columnCount = idealColumns; columnCount >= 1; --columnCount) {
        Layout attempt;
        attempt.columnCount = columnCount;
        attempt.entries.assign(static_cast<size_t>(columnCount), {});
        attempt.widths.assign(static_cast<size_t>(columnCount),
                              choice_popup::kPopupDefaultMinColumnW);

        const int baseItemsPerColumn =
            static_cast<int>(popupEntries_.size()) / columnCount;
        const int extraItems =
            static_cast<int>(popupEntries_.size()) % columnCount;
        size_t entryIndex = 0;
        int widestTotal = kPopupPadding * 2 + (kPopupGap * std::max(0, columnCount - 1));
        int maxColumnItems = 0;

        for (int column = 0; column < columnCount; ++column) {
            auto& columnEntries = attempt.entries[static_cast<size_t>(column)];
            const int targetItems = baseItemsPerColumn + (column < extraItems ? 1 : 0);
            while (entryIndex < popupEntries_.size() &&
                    static_cast<int>(columnEntries.size()) < targetItems) {
                columnEntries.push_back(static_cast<int>(entryIndex));
                const int desiredWidth =
                    measureEntryWidth(this, popupEntries_[entryIndex].menuIndex);
                attempt.widths[static_cast<size_t>(column)] = std::max(
                    attempt.widths[static_cast<size_t>(column)],
                    choice_popup::clampPopupColumnWidth(desiredWidth,
                                                        workW,
                                                        choice_popup::kPopupDefaultMinColumnW));
                ++entryIndex;
            }

            widestTotal += attempt.widths[static_cast<size_t>(column)];
            maxColumnItems = std::max(maxColumnItems,
                                      static_cast<int>(columnEntries.size()));
        }

        attempt.visibleRows = std::max(1, std::min(maxVisibleRows, maxColumnItems));
        attempt.popupW = widestTotal;

        chosen = std::move(attempt);
        if (chosen.popupW <= availableWidth) {
            break;
        }
    }

    if (chosen.popupW > availableWidth && chosen.widths.size() == 1) {
        chosen.widths[0] = std::max(1, availableWidth - (kPopupPadding * 2));
        chosen.popupW = availableWidth;
    }

    const int popupW = std::min(chosen.popupW, availableWidth);
    const int minPopupX = workX + std::min(kPopupScreenEdgeMargin, std::max(0, workW - popupW));
    const int maxPopupX = std::max(minPopupX, workRight - kPopupScreenEdgeMargin - popupW);
    const int popupX = std::clamp(rootX, minPopupX, maxPopupX);
    Fl_Group* savedCurrent = Fl_Group::current();
    Fl_Group::current(nullptr);
    popupWindow_ = new WrappingChoicePopupWindow(this, popupX, popupY, popupW, 1);
    popupWindow_->begin();

    popupColumnEntries_ = std::move(chosen.entries);
    popupBrowsers_.reserve(popupColumnEntries_.size());

    int columnX = kPopupPadding;
    for (size_t column = 0; column < popupColumnEntries_.size(); ++column) {
        const int columnW = chosen.widths[column];
        auto* browser = new WrappingChoiceColumnBrowser(
            this,
            static_cast<int>(column),
            columnX,
            kPopupPadding,
            columnW,
            1);
        browser->textfont(textfont());
        browser->textsize(textsize());
        browser->selection_color(selection_color());

        for (int entryIndex : popupColumnEntries_[column]) {
            browser->add(popupEntries_[static_cast<size_t>(entryIndex)].label.c_str());
        }

        const int line = selectedLineForColumn(static_cast<int>(column));
        if (line > 0 && line <= browser->size()) {
            browser->value(line);
        }

        popupBrowsers_.push_back(browser);
        columnX += columnW + kPopupGap;
    }

    int desiredBodyH = 0;
    int minimumBodyH = 0;
    for (auto* browser : popupBrowsers_) {
        auto* popupBrowser = static_cast<WrappingChoiceColumnBrowser*>(browser);
        if (!popupBrowser) continue;

        const int visibleBodyH =
            chosen.visibleRows >= popupBrowser->size()
                ? popupBrowser->preferredWidgetHeight()
                : popupBrowser->heightForRows(chosen.visibleRows);
        desiredBodyH = std::max(
            desiredBodyH,
            visibleBodyH);
        minimumBodyH = std::max(minimumBodyH, popupBrowser->minimumWidgetHeight());
    }

    const int desiredPopupH = (kPopupPadding * 2) + std::max(desiredBodyH, minimumBodyH);
    const int minimumPopupH = (kPopupPadding * 2) + minimumBodyH;
    int popupH = desiredPopupH;
    if (availableHeight > 0) {
        popupH = std::min(popupH, availableHeight);
    }
    popupH = std::max(minimumPopupH, popupH);
    const int bodyH = std::max(minimumBodyH, popupH - (kPopupPadding * 2));

    popupWindow_->resize(popupX, popupY, popupW, popupH);
    for (size_t i = 0; i < popupBrowsers_.size(); ++i) {
        auto* browser = popupBrowsers_[i];
        if (!browser) continue;
        browser->resize(browser->x(), kPopupPadding, browser->w(), bodyH);
        choice_popup::positionBrowserForOpen(
            browser,
            selectedLineForColumn(static_cast<int>(i)));
    }

    popupWindow_->end();
    popupWindow_->hide();
    Fl_Group::current(savedCurrent);
    return true;
}

bool WrappingChoice::showPopup() {
    if (!window() || !visible_r() || !active_r() || !supportsCustomPopup()) {
        return false;
    }

    if (choice_popup::activePopupOwner() &&
        choice_popup::activePopupOwner() != this) {
        choice_popup::activePopupOwner()->hideOwnedPopup();
    }

    destroyPopup();
    if (!buildPopup() || !popupWindow_ || popupBrowsers_.empty()) {
        destroyPopup();
        return false;
    }

    choice_popup::activePopupOwner() = this;
    // Grab before show so FLTK maps the popup like a menu, not a normal window.
    Fl::grab(popupWindow_);
    popupWindow_->show();
    focusColumn(currentPopupColumn());
    popupWindow_->redraw();
    redraw();
    return true;
}

void WrappingChoice::hidePopup() {
    if (Fl::grab() == popupWindow_) {
        Fl::grab(nullptr);
    }
    if (choice_popup::activePopupOwner() == this) {
        choice_popup::activePopupOwner() = nullptr;
    }
    if (popupWindow_ && popupWindow_->shown()) {
        popupWindow_->hide();
        redraw();
    }
}

bool WrappingChoice::popupVisible() const {
    return popupWindow_ && popupWindow_->shown() &&
           choice_popup::activePopupOwner() == this;
}

bool WrappingChoice::popupContainsRootPoint(int rootX, int rootY) const {
    if (!popupWindow_ || !popupWindow_->shown()) return false;

    return rootX >= popupWindow_->x_root() &&
           rootX < popupWindow_->x_root() + popupWindow_->w() &&
           rootY >= popupWindow_->y_root() &&
           rootY < popupWindow_->y_root() + popupWindow_->h();
}

int WrappingChoice::currentPopupColumn() const {
    const int currentIndex = value();
    if (currentIndex < 0) return 0;

    for (size_t column = 0; column < popupColumnEntries_.size(); ++column) {
        const auto& entries = popupColumnEntries_[column];
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&](int entryIndex) {
                                   return popupEntries_[static_cast<size_t>(entryIndex)].menuIndex ==
                                          currentIndex;
                               });
        if (it != entries.end()) {
            return static_cast<int>(column);
        }
    }

    return 0;
}

int WrappingChoice::selectedLineForColumn(int columnIndex) const {
    if (columnIndex < 0 ||
        columnIndex >= static_cast<int>(popupColumnEntries_.size())) {
        return 0;
    }

    const int currentIndex = value();
    if (currentIndex < 0) {
        return popupColumnEntries_[static_cast<size_t>(columnIndex)].empty() ? 0 : 1;
    }

    const auto& entries = popupColumnEntries_[static_cast<size_t>(columnIndex)];
    for (size_t line = 0; line < entries.size(); ++line) {
        if (popupEntries_[static_cast<size_t>(entries[line])].menuIndex == currentIndex) {
            return static_cast<int>(line) + 1;
        }
    }

    return entries.empty() ? 0 : 1;
}

void WrappingChoice::focusColumn(int columnIndex, int preferredLine) {
    if (!popupVisible() || popupBrowsers_.empty()) return;

    columnIndex = std::clamp(columnIndex, 0, static_cast<int>(popupBrowsers_.size()) - 1);
    Fl_Hold_Browser* target = popupBrowsers_[static_cast<size_t>(columnIndex)];
    if (!target) return;

    int line = preferredLine;
    if (line <= 0) {
        line = selectedLineForColumn(columnIndex);
    }
    if (line <= 0) {
        line = 1;
    }
    line = std::clamp(line, 1, std::max(1, target->size()));

    for (size_t i = 0; i < popupBrowsers_.size(); ++i) {
        if (static_cast<int>(i) != columnIndex && popupBrowsers_[i]) {
            popupBrowsers_[i]->deselect();
        }
    }

    target->value(line);
    choice_popup::positionBrowserForOpen(target, line);
    target->take_focus();
}

void WrappingChoice::moveFocus(int deltaColumns) {
    if (!popupVisible() || popupBrowsers_.empty() || deltaColumns == 0) return;

    int currentColumn = currentPopupColumn();
    Fl_Widget* focus = Fl::focus();
    for (size_t i = 0; i < popupBrowsers_.size(); ++i) {
        if (popupBrowsers_[i] == focus) {
            currentColumn = static_cast<int>(i);
            break;
        }
    }

    const int currentLine =
        (currentColumn >= 0 &&
         currentColumn < static_cast<int>(popupBrowsers_.size()) &&
         popupBrowsers_[static_cast<size_t>(currentColumn)])
            ? popupBrowsers_[static_cast<size_t>(currentColumn)]->value()
            : 0;
    const int targetColumn = std::clamp(currentColumn + deltaColumns,
                                        0,
                                        static_cast<int>(popupBrowsers_.size()) - 1);
    focusColumn(targetColumn, currentLine);
}

void WrappingChoice::selectBrowserLine(Fl_Hold_Browser* browser, int line) {
    const int columnIndex = columnIndexForBrowser(browser);
    if (columnIndex < 0 || line <= 0 ||
        columnIndex >= static_cast<int>(popupColumnEntries_.size())) {
        return;
    }

    const auto& entries = popupColumnEntries_[static_cast<size_t>(columnIndex)];
    if (line > static_cast<int>(entries.size())) return;

    const PopupEntry& entry =
        popupEntries_[static_cast<size_t>(entries[static_cast<size_t>(line - 1)])];
    if (!entry.active || !menu() ||
        entry.menuIndex < 0 || entry.menuIndex >= size() - 1) {
        return;
    }

    const Fl_Menu_Item* item = &menu()[entry.menuIndex];
    hidePopup();
    picked(item);
}

int WrappingChoice::columnIndexForBrowser(const Fl_Hold_Browser* browser) const {
    for (size_t i = 0; i < popupBrowsers_.size(); ++i) {
        if (popupBrowsers_[i] == browser) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void WrappingChoice::onPopupPicked(Fl_Widget* widget, void* data) {
    auto* self = static_cast<WrappingChoice*>(data);
    auto* browser = static_cast<Fl_Hold_Browser*>(widget);
    if (!self || !browser) return;

    self->selectBrowserLine(browser, browser->value());
}

Fl_Widget* WrappingChoice::popupAnchorWidget() const {
    return const_cast<WrappingChoice*>(this);
}

void WrappingChoice::showOwnedPopup() {
    showPopup();
}

void WrappingChoice::hideOwnedPopup() {
    hidePopup();
}

bool WrappingChoice::popupIsVisible() const {
    return popupVisible();
}

void WrappingChoice::takeOwnedFocus() {
    take_focus();
}

} // namespace verdad
