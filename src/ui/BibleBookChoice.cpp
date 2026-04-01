#include "ui/BibleBookChoice.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <utility>

namespace verdad {

namespace {

constexpr int kPopupPadding = 6;
constexpr int kPopupGap = 6;
constexpr int kPopupHeaderH = 22;
constexpr int kPopupMinColumnW = 110;
constexpr int kPopupScrollbarAllowance = 24;
constexpr int kPopupScreenEdgeMargin = 24;

int clampBrowserLineSpacing(int pixels) {
    return std::clamp(pixels, 0, 16);
}

std::pair<int, int> rootPosition(const Fl_Widget* widget) {
    if (!widget) return {0, 0};

    int rootX = 0;
    int rootY = 0;
    Fl_Window* top = widget->top_window_offset(rootX, rootY);
    if (top) {
        rootX += top->x_root();
        rootY += top->y_root();
    }
    return {rootX, rootY};
}

int measureColumnWidth(const std::vector<std::string>& items,
                       const char* header,
                       Fl_Font font,
                       Fl_Fontsize size) {
    fl_font(font, size);

    double widest = header ? fl_width(header) : 0.0;
    for (const auto& item : items) {
        widest = std::max(widest, fl_width(item.c_str()));
    }

    return std::max(kPopupMinColumnW,
                    static_cast<int>(std::ceil(widest)) + kPopupScrollbarAllowance);
}

BibleBookChoice*& openPopupOwner() {
    static BibleBookChoice* owner = nullptr;
    return owner;
}

} // namespace

class BibleBookChoicePopupWindow : public Fl_Double_Window {
public:
    BibleBookChoicePopupWindow(BibleBookChoice* owner, int X, int Y, int W, int H)
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
                owner_->hidePopup();
                return 1;
            }

            if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
                const int key = Fl::event_key();
                if (key == FL_Escape || key == FL_Tab) {
                    owner_->hidePopup();
                    owner_->take_focus();
                    return 1;
                }
            }
        }

        return Fl_Double_Window::handle(event);
    }

private:
    BibleBookChoice* owner_ = nullptr;
};

class BibleBookChoiceBrowser : public Fl_Hold_Browser {
public:
    BibleBookChoiceBrowser(BibleBookChoice* owner,
                           int columnIndex,
                           int X, int Y, int W, int H)
        : Fl_Hold_Browser(X, Y, W, H)
        , owner_(owner)
        , columnIndex_(columnIndex) {
        when(FL_WHEN_RELEASE | FL_WHEN_NOT_CHANGED);
        callback(BibleBookChoice::onPopupPicked, owner_);
        box(FL_BORDER_BOX);
    }

    int preferredWidgetHeight() const {
        const Fl_Boxtype borderBox = box() ? box() : FL_DOWN_BOX;
        return std::max(0, full_height()) + Fl::box_dh(borderBox);
    }

    int minimumWidgetHeight() const {
        const Fl_Boxtype borderBox = box() ? box() : FL_DOWN_BOX;
        return std::max(1, incr_height()) + Fl::box_dh(borderBox);
    }

    int handle(int event) override {
        if (owner_ && (event == FL_KEYBOARD || event == FL_SHORTCUT)) {
            switch (Fl::event_key()) {
            case FL_Left:
                owner_->focusColumn(columnIndex_ - 1, value());
                return 1;
            case FL_Right:
                owner_->focusColumn(columnIndex_ + 1, value());
                return 1;
            case FL_Enter:
            case FL_KP_Enter:
                owner_->selectBrowserLine(this, value());
                return 1;
            case FL_Escape:
                owner_->hidePopup();
                owner_->take_focus();
                return 1;
            default:
                break;
            }
        }

        return Fl_Hold_Browser::handle(event);
    }

private:
    BibleBookChoice* owner_ = nullptr;
    int columnIndex_ = 0;
};

BibleBookChoice::BibleBookChoice(int X, int Y, int W, int H, const char* label)
    : Fl_Choice(X, Y, W, H, label) {}

BibleBookChoice::~BibleBookChoice() {
    hidePopup();
    destroyPopup();
}

int BibleBookChoice::handle(int event) {
    const bool keyboardOpen =
        (event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        (Fl::event_key() == ' ' || Fl::event_key() == FL_Down || Fl::event_key() == FL_Up) &&
        !(Fl::event_state() & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META));

    if (event == FL_PUSH || keyboardOpen) {
        if (Fl::visible_focus()) Fl::focus(this);
        showPopup();
        return 1;
    }

    return Fl_Choice::handle(event);
}

void BibleBookChoice::resize(int X, int Y, int W, int H) {
    Fl_Choice::resize(X, Y, W, H);
    if (popupVisible()) {
        updatePopupGeometry();
    }
}

void BibleBookChoice::setBrowserLineSpacing(int pixels) {
    const int clamped = clampBrowserLineSpacing(pixels);
    if (browserLineSpacing_ == clamped) return;

    browserLineSpacing_ = clamped;
    if (oldTestamentBrowser_ || newTestamentBrowser_) {
        refreshPopupContents();
    }
    if (popupVisible()) {
        updatePopupGeometry();
        popupWindow_->redraw();
    }
}

void BibleBookChoice::setBookColumns(const std::vector<std::string>& oldTestament,
                                     const std::vector<std::string>& newTestament) {
    oldTestamentBooks_ = oldTestament;
    newTestamentBooks_ = newTestament;

    if (oldTestamentBooks_.empty() && newTestamentBooks_.empty()) {
        hidePopup();
        return;
    }

    if (popupVisible()) {
        refreshPopupContents();
        updatePopupGeometry();
    }
}

void BibleBookChoice::destroyPopup() {
    if (!popupWindow_) return;

    if (Fl::grab() == popupWindow_) {
        Fl::grab(nullptr);
    }
    if (openPopupOwner() == this) {
        openPopupOwner() = nullptr;
    }

    delete popupWindow_;
    popupWindow_ = nullptr;
    oldTestamentHeader_ = nullptr;
    newTestamentHeader_ = nullptr;
    oldTestamentBrowser_ = nullptr;
    newTestamentBrowser_ = nullptr;
}

void BibleBookChoice::ensurePopupCreated() {
    if (popupWindow_) return;

    Fl_Group* savedCurrent = Fl_Group::current();
    Fl_Group::current(nullptr);
    popupWindow_ = new BibleBookChoicePopupWindow(this, 0, 0, 1, 1);
    popupWindow_->begin();

    oldTestamentHeader_ = new Fl_Box(0, 0, 1, 1, "Old Testament");
    oldTestamentHeader_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    oldTestamentHeader_->labelfont(FL_HELVETICA_BOLD);
    oldTestamentHeader_->labelsize(std::max(12, static_cast<int>(textsize())));

    newTestamentHeader_ = new Fl_Box(0, 0, 1, 1, "New Testament");
    newTestamentHeader_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    newTestamentHeader_->labelfont(FL_HELVETICA_BOLD);
    newTestamentHeader_->labelsize(std::max(12, static_cast<int>(textsize())));

    oldTestamentBrowser_ = new BibleBookChoiceBrowser(this, 0, 0, 0, 1, 1);
    newTestamentBrowser_ = new BibleBookChoiceBrowser(this, 1, 0, 0, 1, 1);

    popupWindow_->end();
    popupWindow_->hide();
    Fl_Group::current(savedCurrent);
}

void BibleBookChoice::refreshPopupContents() {
    if (!oldTestamentBrowser_ || !newTestamentBrowser_) return;

    oldTestamentBrowser_->textfont(textfont());
    oldTestamentBrowser_->textsize(textsize());
    oldTestamentBrowser_->linespacing(browserLineSpacing_);
    newTestamentBrowser_->textfont(textfont());
    newTestamentBrowser_->textsize(textsize());
    newTestamentBrowser_->linespacing(browserLineSpacing_);

    oldTestamentBrowser_->clear();
    for (const auto& book : oldTestamentBooks_) {
        oldTestamentBrowser_->add(book.c_str());
    }

    newTestamentBrowser_->clear();
    for (const auto& book : newTestamentBooks_) {
        newTestamentBrowser_->add(book.c_str());
    }

    oldTestamentBrowser_->deselect();
    newTestamentBrowser_->deselect();

    const int selectedIndex = value();
    if (selectedIndex >= 0) {
        const int oldCount = static_cast<int>(oldTestamentBooks_.size());
        if (selectedIndex < oldCount && selectedIndex < oldTestamentBrowser_->size()) {
            oldTestamentBrowser_->value(selectedIndex + 1);
            oldTestamentBrowser_->middleline(selectedIndex + 1);
        } else {
            const int newLine = selectedIndex - oldCount + 1;
            if (newLine > 0 && newLine <= newTestamentBrowser_->size()) {
                newTestamentBrowser_->value(newLine);
                newTestamentBrowser_->middleline(newLine);
            }
        }
    }

    if (oldTestamentBooks_.empty()) oldTestamentBrowser_->deactivate();
    else oldTestamentBrowser_->activate();

    if (newTestamentBooks_.empty()) newTestamentBrowser_->deactivate();
    else newTestamentBrowser_->activate();
}

void BibleBookChoice::updatePopupGeometry() {
    if (!popupWindow_ || !oldTestamentBrowser_ || !newTestamentBrowser_) return;

    int anchorX = 0;
    int anchorY = 0;
    Fl_Window* top = top_window_offset(anchorX, anchorY);
    const int topRootX = top ? top->x_root() : 0;
    const int topRootY = top ? top->y_root() : 0;
    const int rootX = anchorX + topRootX;
    const int rootY = anchorY + topRootY;
    auto* oldBrowser = static_cast<BibleBookChoiceBrowser*>(oldTestamentBrowser_);
    auto* newBrowser = static_cast<BibleBookChoiceBrowser*>(newTestamentBrowser_);
    const int leftColumnW = measureColumnWidth(oldTestamentBooks_, "Old Testament",
                                               textfont(), textsize());
    const int rightColumnW = measureColumnWidth(newTestamentBooks_, "New Testament",
                                                textfont(), textsize());
    const int desiredW = (kPopupPadding * 2) + kPopupGap + leftColumnW + rightColumnW;
    // Use the browsers' exact laid-out content height so popup sizing matches FLTK.
    const int desiredBodyH = std::max(oldBrowser->preferredWidgetHeight(),
                                      newBrowser->preferredWidgetHeight());
    const int minimumBodyH = std::max(oldBrowser->minimumWidgetHeight(),
                                      newBrowser->minimumWidgetHeight());
    const int desiredH = (kPopupPadding * 2) + kPopupHeaderH + kPopupGap +
                         std::max(desiredBodyH, minimumBodyH);
    const int minimumH = (kPopupPadding * 2) + kPopupHeaderH + kPopupGap + minimumBodyH;

    const int screenNum = window() ? window()->screen_num() : Fl::screen_num(rootX, rootY);
    int workX = 0;
    int workY = 0;
    int workW = 0;
    int workH = 0;
    Fl::screen_work_area(workX, workY, workW, workH, screenNum);

    const int popupBelowY = rootY + h() - 1;
    const int workBottom = workY + workH;
    const int spaceBelow = std::max(0, workBottom - popupBelowY - kPopupScreenEdgeMargin);
    const int spaceAbove = std::max(0, rootY - workY - kPopupScreenEdgeMargin);
    const bool canFitBelow = spaceBelow >= desiredH;
    const bool canFitAbove = spaceAbove >= desiredH;
    const bool placeBelow = canFitBelow || (!canFitAbove && spaceBelow >= spaceAbove);

    int popupW = std::min(std::max(w(), desiredW), workW);
    const int availableHeight = placeBelow ? spaceBelow : spaceAbove;
    const int maxPopupH = std::max(minimumH, workH - (kPopupScreenEdgeMargin * 2));
    int popupH = desiredH;
    if (availableHeight > 0) {
        popupH = std::min(popupH, availableHeight);
    }
    popupH = std::clamp(popupH, minimumH, maxPopupH);

    const int popupX = std::clamp(rootX, workX, workX + workW - popupW);
    const int verticalEdgeMargin = std::min(
        kPopupScreenEdgeMargin,
        std::max(0, (workH - popupH) / 2));
    int popupY = placeBelow ? popupBelowY : rootY - popupH + 1;
    popupY = std::clamp(popupY,
                        workY + verticalEdgeMargin,
                        workBottom - verticalEdgeMargin - popupH);

    popupWindow_->resize(popupX, popupY, popupW, popupH);
    layoutPopupContents();
}

void BibleBookChoice::layoutPopupContents() {
    if (!popupWindow_ || !oldTestamentHeader_ || !newTestamentHeader_ ||
        !oldTestamentBrowser_ || !newTestamentBrowser_) {
        return;
    }

    const int innerX = kPopupPadding;
    const int innerY = kPopupPadding;
    const int bodyY = innerY + kPopupHeaderH + kPopupGap;
    const int bodyH = std::max(20, popupWindow_->h() - bodyY - kPopupPadding);
    const int bodyW = std::max(40, popupWindow_->w() - (kPopupPadding * 2) - kPopupGap);
    const int leftW = bodyW / 2;
    const int rightW = bodyW - leftW;
    const int rightX = innerX + leftW + kPopupGap;

    oldTestamentHeader_->resize(innerX, innerY, leftW, kPopupHeaderH);
    newTestamentHeader_->resize(rightX, innerY, rightW, kPopupHeaderH);
    oldTestamentBrowser_->resize(innerX, bodyY, leftW, bodyH);
    newTestamentBrowser_->resize(rightX, bodyY, rightW, bodyH);
}

bool BibleBookChoice::showPopup(int preferredColumn) {
    if (size() <= 1 || (oldTestamentBooks_.empty() && newTestamentBooks_.empty())) {
        return false;
    }

    if (openPopupOwner() && openPopupOwner() != this) {
        openPopupOwner()->hidePopup();
    }

    ensurePopupCreated();
    refreshPopupContents();
    updatePopupGeometry();

    openPopupOwner() = this;
    // Grab before show so FLTK maps the popup like a menu, not a normal window.
    Fl::grab(popupWindow_);
    popupWindow_->show();
    focusColumn(preferredColumn >= 0 ? preferredColumn : currentPopupColumn());
    popupWindow_->redraw();
    redraw();
    return true;
}

void BibleBookChoice::hidePopup() {
    if (Fl::grab() == popupWindow_) {
        Fl::grab(nullptr);
    }
    if (openPopupOwner() == this) {
        openPopupOwner() = nullptr;
    }
    if (popupWindow_ && popupWindow_->shown()) {
        popupWindow_->hide();
        redraw();
    }
}

bool BibleBookChoice::popupVisible() const {
    return popupWindow_ && popupWindow_->shown() && openPopupOwner() == this;
}

void BibleBookChoice::focusColumn(int columnIndex, int preferredLine) {
    if (!popupVisible() || !oldTestamentBrowser_ || !newTestamentBrowser_) return;

    Fl_Hold_Browser* target = (columnIndex > 0) ? newTestamentBrowser_ : oldTestamentBrowser_;
    Fl_Hold_Browser* fallback = (target == oldTestamentBrowser_)
        ? newTestamentBrowser_
        : oldTestamentBrowser_;

    if (!target->active_r() || target->size() <= 0) {
        target = fallback;
    }
    if (!target || !target->active_r() || target->size() <= 0) return;

    int line = preferredLine;
    if (line <= 0) {
        line = target->value();
    }
    if (line <= 0) {
        line = 1;
    }
    line = std::clamp(line, 1, target->size());

    target->value(line);
    target->middleline(line);
    target->take_focus();

    Fl_Hold_Browser* other = (target == oldTestamentBrowser_)
        ? newTestamentBrowser_
        : oldTestamentBrowser_;
    if (other) other->deselect();
}

bool BibleBookChoice::popupContainsRootPoint(int rootX, int rootY) const {
    if (!popupWindow_ || !popupWindow_->shown()) return false;

    return rootX >= popupWindow_->x_root() &&
           rootX < popupWindow_->x_root() + popupWindow_->w() &&
           rootY >= popupWindow_->y_root() &&
           rootY < popupWindow_->y_root() + popupWindow_->h();
}

int BibleBookChoice::currentPopupColumn() const {
    const int selectedIndex = value();
    if (selectedIndex < 0) return 0;
    return selectedIndex < static_cast<int>(oldTestamentBooks_.size()) ? 0 : 1;
}

void BibleBookChoice::selectBrowserLine(Fl_Hold_Browser* browser, int line) {
    const int menuIndex = menuIndexForSelection(browser, line);
    if (menuIndex < 0 || !menu() || menuIndex >= size() - 1) return;

    const Fl_Menu_Item* item = &menu()[menuIndex];
    hidePopup();
    picked(item);
}

int BibleBookChoice::menuIndexForSelection(const Fl_Hold_Browser* browser, int line) const {
    if (line <= 0) return -1;

    if (browser == oldTestamentBrowser_) {
        return line - 1;
    }
    if (browser == newTestamentBrowser_) {
        return static_cast<int>(oldTestamentBooks_.size()) + line - 1;
    }
    return -1;
}

void BibleBookChoice::onPopupPicked(Fl_Widget* widget, void* data) {
    auto* self = static_cast<BibleBookChoice*>(data);
    auto* browser = static_cast<Fl_Hold_Browser*>(widget);
    if (!self || !browser) return;

    self->selectBrowserLine(browser, browser->value());
}

} // namespace verdad
