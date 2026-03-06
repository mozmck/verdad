#ifndef VERDAD_STYLED_TABS_H
#define VERDAD_STYLED_TABS_H

#include <FL/Fl_Tabs.H>
#include <functional>

namespace verdad {

/// Fl_Tabs subclass with bold selected-tab labels and a custom close button
/// on the active tab.
class StyledTabs : public Fl_Tabs {
public:
    StyledTabs(int X, int Y, int W, int H, const char* L = nullptr);

    void setFillParentBackground(bool fill) { fillParentBackground_ = fill; }

    /// Set the regular and bold fonts used for tab labels.
    void setLabelFonts(Fl_Font regular, Fl_Font bold) {
        baseLabelFont_ = regular;
        boldLabelFont_ = bold;
    }

    /// Set the callback invoked when the close button is clicked.
    /// The argument is the Fl_Group* of the tab being closed.
    void setCloseCallback(std::function<void(Fl_Widget*)> cb) { closeCb_ = std::move(cb); }

    /// Set the callback invoked when the selected tab changes.
    void setSelectionCallback(std::function<void(Fl_Widget*)> cb) { selectionCb_ = std::move(cb); }

    /// Update close-button visibility based on the number of children.
    void updateCloseButtons();

    void draw() override;
    void resize(int X, int Y, int W, int H) override;
    int handle(int event) override;

protected:
    void draw_tab(int x1, int x2, int W, int H, Fl_Widget* o, int flags, int what) override;
    int tab_positions() override;
    int hit_close(Fl_Widget* o, int eventX, int eventY) override;

private:
    void applySelectedTabFonts();
    void applyCloseFlags();
    bool hasActiveCloseButton(const Fl_Widget* tab);
    bool computeCloseButtonRect(Fl_Widget* tab,
                                int H,
                                int& outX,
                                int& outY,
                                int& outW,
                                int& outH);
    int closeButtonSize() const;

    bool fillParentBackground_ = true;
    Fl_Font baseLabelFont_ = FL_HELVETICA;
    Fl_Font boldLabelFont_ = FL_HELVETICA_BOLD;
    bool showClose_ = false;
    bool closeHovered_ = false;
    int closeBtnX_ = 0;
    int closeBtnY_ = 0;
    int closeBtnW_ = 0;
    int closeBtnH_ = 0;
    bool closeBtnValid_ = false;

    std::function<void(Fl_Widget*)> closeCb_;
    std::function<void(Fl_Widget*)> selectionCb_;
};

} // namespace verdad

#endif // VERDAD_STYLED_TABS_H
