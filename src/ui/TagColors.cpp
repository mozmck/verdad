#include "ui/TagColors.h"

#include "ui/UiFontUtils.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>

namespace verdad {
namespace tag_colors {
namespace {

std::string colorName(const std::string& hex) {
    for (const auto& entry : palette()) {
        if (hex == entry.hex) return entry.name;
    }
    return "Custom";
}

Rgb clampRgb(double r, double g, double b) {
    auto clamp = [](double v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0, 255.0)));
    };
    return Rgb{clamp(r), clamp(g), clamp(b)};
}

/// A clickable color square used by the picker.
class SwatchButton : public Fl_Widget {
public:
    SwatchButton(int X, int Y, int W, int H, std::string hex)
        : Fl_Widget(X, Y, W, H)
        , hex_(std::move(hex)) {}

    const std::string& hex() const { return hex_; }
    void setHex(const std::string& hex) {
        hex_ = hex;
        redraw();
    }
    void setSelected(bool selected) {
        if (selected_ == selected) return;
        selected_ = selected;
        redraw();
    }

    void draw() override {
        Rgb rgb;
        const bool hasColor = parseHex(hex_, rgb);
        fl_color(FL_BACKGROUND_COLOR);
        fl_rectf(x(), y(), w(), h());

        const int inset = 3;
        const int sx = x() + inset;
        const int sy = y() + inset;
        const int sw = w() - 2 * inset;
        const int sh = h() - 2 * inset;
        if (hasColor) {
            fl_color(toFlColor(rgb));
            fl_rectf(sx, sy, sw, sh);
        }
        fl_color(fl_rgb_color(0x70, 0x70, 0x70));
        fl_rect(sx, sy, sw, sh);

        if (selected_) {
            fl_color(FL_FOREGROUND_COLOR);
            fl_line_style(FL_SOLID, 2);
            fl_rect(x() + 1, y() + 1, w() - 2, h() - 2);
            fl_line_style(0);
        }
        if (Fl::focus() == this) {
            draw_focus(FL_NO_BOX, x(), y(), w(), h());
        }
    }

    int handle(int event) override {
        switch (event) {
        case FL_PUSH:
            if (Fl::visible_focus()) take_focus();
            do_callback();
            return 1;
        case FL_FOCUS:
        case FL_UNFOCUS:
            redraw();
            return Fl::visible_focus() ? 1 : 0;
        case FL_KEYBOARD:
            if (Fl::event_key() == ' ') {
                do_callback();
                return 1;
            }
            break;
        default:
            break;
        }
        return Fl_Widget::handle(event);
    }

private:
    std::string hex_;
    bool selected_ = false;
};

class ColorPickerDialog {
public:
    ColorPickerDialog(const std::string& initial, const PickerOptions& options)
        : options_(options)
        , selected_(normalizeHex(initial))
        , dialog_(kDialogW, kDialogH) {
        dialog_.copy_label(options_.title.c_str());
        dialog_.set_modal();
        dialog_.begin();

        const auto& entries = palette();
        for (size_t i = 0; i < entries.size(); ++i) {
            const int col = static_cast<int>(i % kColumns);
            const int row = static_cast<int>(i / kColumns);
            auto* swatch = new SwatchButton(kMargin + col * (kSwatchW + kGap),
                                            kMargin + row * (kSwatchH + kGap),
                                            kSwatchW, kSwatchH, entries[i].hex);
            swatch->tooltip(entries[i].name);
            swatch->callback(onSwatch, this);
            swatches_.push_back(swatch);
        }

        const int rows = static_cast<int>((entries.size() + kColumns - 1) / kColumns);
        int y = kMargin + rows * (kSwatchH + kGap) + 4;
        int x = kMargin;

        if (!options_.emptyChoiceLabel.empty()) {
            emptySwatch_ = new SwatchButton(x, y, kSwatchH + 6, kSwatchH,
                                            normalizeHex(options_.emptyChoicePreview));
            emptySwatch_->tooltip(options_.emptyChoiceLabel.c_str());
            emptySwatch_->callback(onEmpty, this);
            x += emptySwatch_->w() + 2;
            auto* emptyButton = new Fl_Button(x, y + 2, 84, kSwatchH - 4);
            emptyButton->copy_label(options_.emptyChoiceLabel.c_str());
            emptyButton->callback(onEmpty, this);
            x += emptyButton->w() + 14;
        }

        customSwatch_ = new SwatchButton(x, y, kSwatchH + 6, kSwatchH, "");
        customSwatch_->tooltip("Custom color");
        customSwatch_->callback(onCustomSwatch, this);
        x += customSwatch_->w() + 2;
        auto* customButton = new Fl_Button(x, y + 2, 84, kSwatchH - 4, "Custom...");
        customButton->callback(onCustom, this);
        y += kSwatchH + 10;

        selectionLabel_ = new Fl_Box(kMargin, y, kDialogW - 2 * kMargin, 24);
        selectionLabel_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        auto* cancel = new Fl_Button(kDialogW - 180, kDialogH - 40, 80, 28, "Cancel");
        cancel->callback(onCancel, this);
        okButton_ = new Fl_Return_Button(kDialogW - 92, kDialogH - 40, 76, 28, "OK");
        okButton_->callback(onOk, this);

        dialog_.end();
        if (!selected_.empty() && colorName(selected_) == "Custom") {
            customSwatch_->setHex(selected_);
        }
        syncSelection();
        ui_font::applyCurrentAppUiFont(&dialog_);
    }

    bool open(std::string& color) {
        dialog_.show();
        okButton_->take_focus();
        while (dialog_.shown()) {
            Fl::wait();
        }
        if (!accepted_) return false;
        color = selected_;
        return true;
    }

private:
    static constexpr int kColumns = 5;
    static constexpr int kSwatchW = 52;
    static constexpr int kSwatchH = 34;
    static constexpr int kGap = 6;
    static constexpr int kMargin = 16;
    static constexpr int kDialogW = kMargin * 2 + kColumns * kSwatchW + (kColumns - 1) * kGap;
    static constexpr int kDialogH = 330;

    void select(const std::string& hex, bool acceptOnDoubleClick) {
        selected_ = hex;
        syncSelection();
        if (acceptOnDoubleClick && Fl::event_clicks() > 0) {
            accepted_ = true;
            dialog_.hide();
        }
    }

    void syncSelection() {
        bool matched = false;
        for (auto* swatch : swatches_) {
            const bool isSelected = !selected_.empty() && swatch->hex() == selected_;
            swatch->setSelected(isSelected);
            matched = matched || isSelected;
        }
        if (emptySwatch_) emptySwatch_->setSelected(selected_.empty());
        customSwatch_->setSelected(!selected_.empty() && !matched &&
                                   customSwatch_->hex() == selected_);

        std::string text;
        if (selected_.empty()) {
            text = "Selected: " + (options_.emptyChoiceLabel.empty()
                                       ? std::string("None")
                                       : options_.emptyChoiceLabel);
        } else {
            text = "Selected: " + colorName(selected_) + " (" + selected_ + ")";
        }
        selectionLabel_->copy_label(text.c_str());
    }

    static void onSwatch(Fl_Widget* w, void* data) {
        auto* self = static_cast<ColorPickerDialog*>(data);
        auto* swatch = static_cast<SwatchButton*>(w);
        if (self && swatch) self->select(swatch->hex(), true);
    }

    static void onEmpty(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<ColorPickerDialog*>(data);
        if (self) self->select("", false);
    }

    static void onCustomSwatch(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<ColorPickerDialog*>(data);
        if (!self) return;
        if (self->customSwatch_->hex().empty()) {
            onCustom(nullptr, data);
        } else {
            self->select(self->customSwatch_->hex(), true);
        }
    }

    static void onCustom(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<ColorPickerDialog*>(data);
        if (!self) return;

        Rgb start{0x4a, 0x86, 0xc8};
        parseHex(self->selected_, start);
        uchar r = static_cast<uchar>(start.r);
        uchar g = static_cast<uchar>(start.g);
        uchar b = static_cast<uchar>(start.b);
        if (!fl_color_chooser("Custom Color", r, g, b, 2)) return;

        const std::string hex = toHex(Rgb{r, g, b});
        self->customSwatch_->setHex(hex);
        self->select(hex, false);
    }

    static void onCancel(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<ColorPickerDialog*>(data);
        if (!self) return;
        self->accepted_ = false;
        self->dialog_.hide();
    }

    static void onOk(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<ColorPickerDialog*>(data);
        if (!self) return;
        self->accepted_ = true;
        self->dialog_.hide();
    }

    PickerOptions options_;
    std::string selected_;
    bool accepted_ = false;
    Fl_Double_Window dialog_;
    std::vector<SwatchButton*> swatches_;
    SwatchButton* emptySwatch_ = nullptr;
    SwatchButton* customSwatch_ = nullptr;
    Fl_Box* selectionLabel_ = nullptr;
    Fl_Return_Button* okButton_ = nullptr;
};

} // namespace

const std::vector<PaletteEntry>& palette() {
    static const std::vector<PaletteEntry> entries = {
        {"Red", "#e53935"},        {"Pink", "#d81b60"},
        {"Purple", "#8e24aa"},     {"Deep Purple", "#5e35b1"},
        {"Indigo", "#3949ab"},     {"Blue", "#1e88e5"},
        {"Light Blue", "#039be5"}, {"Cyan", "#00acc1"},
        {"Teal", "#00897b"},       {"Green", "#43a047"},
        {"Light Green", "#7cb342"},{"Lime", "#c0ca33"},
        {"Yellow", "#fdd835"},     {"Amber", "#ffb300"},
        {"Orange", "#fb8c00"},     {"Deep Orange", "#f4511e"},
        {"Brown", "#6d4c41"},      {"Gray", "#757575"},
        {"Blue Gray", "#546e7a"},  {"Black", "#212121"},
    };
    return entries;
}

bool parseHex(const std::string& text, Rgb& out) {
    if (text.size() != 7 || text[0] != '#') return false;
    for (size_t i = 1; i < text.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;
    if (std::sscanf(text.c_str() + 1, "%02x%02x%02x", &r, &g, &b) != 3) return false;
    out = Rgb{static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
    return true;
}

std::string toHex(const Rgb& color) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x",
                  std::clamp(color.r, 0, 255),
                  std::clamp(color.g, 0, 255),
                  std::clamp(color.b, 0, 255));
    return buffer;
}

Rgb fromFlColor(Fl_Color color) {
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
    Fl::get_color(color, r, g, b);
    return Rgb{r, g, b};
}

Fl_Color toFlColor(const Rgb& color) {
    return fl_rgb_color(static_cast<uchar>(std::clamp(color.r, 0, 255)),
                        static_cast<uchar>(std::clamp(color.g, 0, 255)),
                        static_cast<uchar>(std::clamp(color.b, 0, 255)));
}

Rgb blend(const Rgb& base, const Rgb& overlay, double amount) {
    const double t = std::clamp(amount, 0.0, 1.0);
    return clampRgb(base.r + (overlay.r - base.r) * t,
                    base.g + (overlay.g - base.g) * t,
                    base.b + (overlay.b - base.b) * t);
}

double luminance(const Rgb& color) {
    auto channel = [](int value) {
        const double c = value / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) +
           0.0722 * channel(color.b);
}

std::string normalizeHex(const std::string& text) {
    std::string value;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    Rgb rgb;
    return parseHex(value, rgb) ? value : std::string();
}

std::string markerStyle(const std::string& hex, const Rgb& contentBackground,
                        bool darkTheme, bool continued) {
    Rgb color;
    if (!parseHex(hex, color)) return "";

    const double tint = continued ? (darkTheme ? 0.14 : 0.07)
                                  : (darkTheme ? 0.30 : 0.16);
    const Rgb background = blend(contentBackground, color, tint);

    // Keep the label readable: darken light colors on light backgrounds and
    // lighten dark colors on dark backgrounds.
    Rgb text = color;
    const double lum = luminance(color);
    if (!darkTheme && lum > 0.45) {
        text = blend(color, Rgb{0, 0, 0}, 0.45);
    } else if (darkTheme && lum < 0.30) {
        text = blend(color, Rgb{255, 255, 255}, 0.50);
    }

    return "background-color:" + toHex(background) +
           ";border-color:" + toHex(color) +
           ";color:" + toHex(text) + ";";
}

std::string highlightStyle(const std::string& hex, const Rgb& contentBackground,
                           bool darkTheme) {
    Rgb color;
    if (!parseHex(hex, color)) return "";
    const Rgb background = blend(contentBackground, color, darkTheme ? 0.38 : 0.30);
    return "background-color:" + toHex(background) + ";";
}

bool chooseColor(std::string& color, const PickerOptions& options) {
    ColorPickerDialog dialog(color, options);
    return dialog.open(color);
}

} // namespace tag_colors
} // namespace verdad
