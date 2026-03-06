#include "ui/StyledTabs.h"

#include <algorithm>

#include <FL/Fl.H>
#include <FL/Enumerations.H>
#include <FL/fl_draw.H>

namespace verdad {

namespace {

constexpr int kCloseGap = 6;
constexpr int kCloseEdgePad = 5;
constexpr int kSelectedTabWidthExtra = 12;
constexpr int kLabelWidthSlack = 2;
constexpr int kSelectedTabRole = 2;

} // namespace

StyledTabs::StyledTabs(int X, int Y, int W, int H, const char* L)
    : Fl_Tabs(X, Y, W, H, L) {}

void StyledTabs::draw() {
    closeBtnValid_ = false;

    if (fillParentBackground_) {
        fl_push_clip(x(), y(), w(), h());
        Fl_Color bg = parent() ? parent()->color() : FL_BACKGROUND_COLOR;
        fl_color(bg);
        fl_rectf(x(), y(), w(), h());
        fl_pop_clip();
    }

    applySelectedTabFonts();
    applyCloseFlags();
    Fl_Tabs::draw();
}

void StyledTabs::resize(int X, int Y, int W, int H) {
    Fl_Tabs::resize(X, Y, W, H);
    closeBtnValid_ = false;
    damage(FL_DAMAGE_ALL);
    redraw();
}

int StyledTabs::handle(int event) {
    Fl_Widget* beforeSelected = value();

    if (event == FL_RELEASE && showClose_ && children() > 1) {
        Fl_Widget* selected = value();
        if (selected &&
            (selected->when() & FL_WHEN_CLOSED) &&
            hit_close(selected, Fl::event_x(), Fl::event_y())) {
            if (closeCb_) closeCb_(selected);
            return 1;
        }
    }

    if (showClose_ && children() > 1 && value()) {
        switch (event) {
        case FL_DRAG:
        case FL_MOVE:
        case FL_ENTER: {
            bool hover = hit_close(value(), Fl::event_x(), Fl::event_y()) != 0;
            if (hover != closeHovered_) {
                closeHovered_ = hover;
                redraw_tabs();
            }
            break;
        }
        case FL_LEAVE:
            if (closeHovered_) {
                closeHovered_ = false;
                redraw_tabs();
            }
            break;
        default:
            break;
        }
    }
    int handled = Fl_Tabs::handle(event);

    if (handled &&
        selectionCb_ &&
        (event == FL_RELEASE || event == FL_KEYBOARD || event == FL_SHORTCUT)) {
        Fl_Widget* afterSelected = value();
        if (afterSelected && afterSelected != beforeSelected) {
            selectionCb_(afterSelected);
        }
    }

    return handled;
}

void StyledTabs::applySelectedTabFonts() {
    Fl_Widget* selected = value();
    for (int i = 0; i < children(); ++i) {
        Fl_Widget* child = this->child(i);
        if (!child) continue;
        child->labelfont(child == selected ? boldLabelFont_ : baseLabelFont_);
    }
}

void StyledTabs::applyCloseFlags() {
    if (!showClose_ || children() <= 1) {
        for (int i = 0; i < children(); ++i) {
            Fl_Widget* child = this->child(i);
            if (child) {
                child->when(child->when() & ~FL_WHEN_CLOSED);
            }
        }
        return;
    }

    Fl_Widget* selected = value();
    for (int i = 0; i < children(); ++i) {
        Fl_Widget* child = this->child(i);
        if (!child) continue;
        if (child == selected) {
            child->when(child->when() | FL_WHEN_CLOSED);
        } else {
            child->when(child->when() & ~FL_WHEN_CLOSED);
        }
    }
}

bool StyledTabs::hasActiveCloseButton(const Fl_Widget* tab) {
    return showClose_ && children() > 1 && tab && tab == value() &&
           (tab->when() & FL_WHEN_CLOSED);
}

int StyledTabs::closeButtonSize() const {
    return std::max(12, labelsize() / 2 + 4);
}

int StyledTabs::tab_positions() {
    int selected = Fl_Tabs::tab_positions();
    if (selected < 0 || kSelectedTabWidthExtra <= 0) return selected;

    Fl_Widget* selectedTab = value();
    if (!hasActiveCloseButton(selectedTab)) return selected;

    for (int i = 0; i < children(); ++i) {
        if (child(i) != selectedTab) continue;
        tab_width[i] += kSelectedTabWidthExtra;
        for (int j = i + 1; j <= children(); ++j) {
            tab_pos[j] += kSelectedTabWidthExtra;
        }
        break;
    }
    return selected;
}

bool StyledTabs::computeCloseButtonRect(Fl_Widget* tab,
                                        int H,
                                        int& outX,
                                        int& outY,
                                        int& outW,
                                        int& outH) {
    if (!hasActiveCloseButton(tab)) return false;
    tab_positions();

    for (int i = 0; i < children(); ++i) {
        if (child(i) != tab) continue;
        if (tab_flags[i] & 1) return false;

        int tabX = x() + tab_pos[i] + tab_offset;
        int tabW = tab_width[i];
        int tabH = H >= 0 ? H + Fl::box_dh(box()) : -H + Fl::box_dh(box());
        int tabY = H >= 0 ? y() : y() + h() - tabH;

        int labelW = 0;
        int labelH = 0;
        Fl_Labeltype oldType = tab->labeltype();
        Fl_Align oldAlign = tab->align();
        if (oldType == FL_NO_LABEL) tab->labeltype(FL_NORMAL_LABEL);
        tab->align(tab_align());
        tab->measure_label(labelW, labelH);
        tab->labeltype(oldType);
        tab->align(oldAlign);
        (void)labelH;

        int buttonSize = closeButtonSize();
        int contentW = labelW + kLabelWidthSlack + kCloseGap + buttonSize;
        int minLeft = tabX + kCloseEdgePad;
        int maxRight = tabX + tabW - kCloseEdgePad;
        int contentX = tabX + std::max(0, (tabW - contentW) / 2);
        contentX = std::max(minLeft, std::min(contentX, maxRight - contentW));

        int labelBoxW = std::max(0, maxRight - contentX - buttonSize - kCloseGap);
        outW = buttonSize;
        outH = buttonSize;
        outX = contentX + labelBoxW + kCloseGap;
        outY = tabY + (tabH - buttonSize) / 2;
        return true;
    }
    return false;
}

void StyledTabs::draw_tab(int x1, int x2, int W, int H, Fl_Widget* o, int flags, int what) {
    (void)x2;
    (void)flags;

    if (!hasActiveCloseButton(o) || what != kSelectedTabRole) {
        Fl_Tabs::draw_tab(x1, x2, W, H, o, flags, what);
        return;
    }

    x1 += tab_offset;

    int dh = Fl::box_dh(box());
    char prevDrawShortcut = fl_draw_shortcut;
    fl_draw_shortcut = 1;

    Fl_Color oldLabelColor = o->labelcolor();
    Fl_Labeltype oldLabelType = o->labeltype();
    if (oldLabelType == FL_NO_LABEL) o->labeltype(FL_NORMAL_LABEL);

    int labelW = 0;
    int labelH = 0;
    Fl_Align oldAlign = o->align();
    o->align(tab_align());
    o->measure_label(labelW, labelH);
    o->align(oldAlign);
    (void)labelH;

    Fl_Boxtype boxType = box();
    Fl_Color tabColor = selection_color();
    o->labelcolor(labelcolor());
    closeBtnValid_ = false;

    auto drawCloseButton = [&](int tabY, int tabH) {
        int buttonSize = closeButtonSize();
        int contentW = labelW + kLabelWidthSlack + kCloseGap + buttonSize;
        int minLeft = x1 + kCloseEdgePad;
        int maxRight = x1 + W - kCloseEdgePad;
        int contentX = x1 + std::max(0, (W - contentW) / 2);
        contentX = std::max(minLeft, std::min(contentX, maxRight - contentW));

        int labelBoxW = std::max(0, maxRight - contentX - buttonSize - kCloseGap);
        int labelAlign = FL_ALIGN_LEFT | FL_ALIGN_CLIP | FL_ALIGN_INSIDE;
        o->draw_label(contentX, tabY, labelBoxW, tabH, labelAlign);

        closeBtnW_ = buttonSize;
        closeBtnH_ = buttonSize;
        closeBtnX_ = contentX + labelBoxW + kCloseGap;
        closeBtnY_ = tabY + (tabH - buttonSize) / 2;
        closeBtnValid_ = true;

        Fl_Color buttonFill = tabColor;
        if (closeHovered_) {
            buttonFill = fl_color_average(tabColor, FL_FOREGROUND_COLOR, 0.88f);
        }
        Fl_Color buttonOutline = fl_color_average(tabColor, FL_FOREGROUND_COLOR, 0.58f);
        Fl_Color glyphColor = fl_contrast(FL_FOREGROUND_COLOR, buttonFill);
        if (!active_r()) {
            buttonFill = fl_inactive(buttonFill);
            buttonOutline = fl_inactive(buttonOutline);
            glyphColor = fl_inactive(glyphColor);
        }

        fl_color(buttonFill);
        fl_pie(closeBtnX_, closeBtnY_, closeBtnW_, closeBtnH_, 0.0, 360.0);
        fl_color(buttonOutline);
        fl_arc(closeBtnX_, closeBtnY_, closeBtnW_, closeBtnH_, 0.0, 360.0);

        int glyphInset = std::max(2, closeBtnW_ / 5);
        int lineStartX = closeBtnX_ + glyphInset;
        int lineEndX = closeBtnX_ + closeBtnW_ - glyphInset - 1;
        int lineStartY = closeBtnY_ + glyphInset;
        int lineEndY = closeBtnY_ + closeBtnH_ - glyphInset - 1;
        fl_color(glyphColor);
        fl_line_style(FL_SOLID, 2);
        fl_line(lineStartX, lineStartY, lineEndX, lineEndY);
        fl_line(lineStartX, lineEndY, lineEndX, lineStartY);
        fl_line_style(0);
    };

    if (H >= 0) {
        H += dh;
        draw_box(boxType, x1, y(), W, H + 10, tabColor);
        drawCloseButton(y(), H);
        if (Fl::focus() == this && o->visible()) {
            draw_focus(boxType, x1, y(), W, H, tabColor);
        }
    } else {
        H = -H;
        H += dh;
        int tabY = y() + h() - H;
        draw_box(boxType, x1, y() + h() - H - 10, W, H + 10, tabColor);
        drawCloseButton(tabY, H);
        if (Fl::focus() == this && o->visible()) {
            draw_focus(boxType, x1, tabY + 1, W, H, tabColor);
        }
    }

    fl_draw_shortcut = prevDrawShortcut;
    o->labelcolor(oldLabelColor);
    o->labeltype(oldLabelType);
}

int StyledTabs::hit_close(Fl_Widget* o, int eventX, int eventY) {
    int btnX = 0;
    int btnY = 0;
    int btnW = 0;
    int btnH = 0;
    if (!computeCloseButtonRect(o, tab_height(), btnX, btnY, btnW, btnH)) {
        return 0;
    }
    return eventX >= btnX &&
           eventX < btnX + btnW &&
           eventY >= btnY &&
           eventY < btnY + btnH;
}

void StyledTabs::updateCloseButtons() {
    showClose_ = children() > 1;
    closeHovered_ = false;
    closeBtnValid_ = false;
    redraw();
}

} // namespace verdad
