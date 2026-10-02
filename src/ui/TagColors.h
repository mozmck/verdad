#ifndef VERDAD_TAG_COLORS_H
#define VERDAD_TAG_COLORS_H

#include <FL/Enumerations.H>

#include <string>
#include <vector>

namespace verdad {
namespace tag_colors {

/// The marker color used for tags left on the default color, until the user
/// changes it in Settings.
constexpr const char* kInitialDefaultMarkerColor = "#4a86c8";

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

struct PaletteEntry {
    const char* name;
    const char* hex;
};

/// The preset swatches offered by the color picker.
const std::vector<PaletteEntry>& palette();

/// Parse "#rrggbb". Returns false for anything else (including "").
bool parseHex(const std::string& text, Rgb& out);
std::string toHex(const Rgb& color);
Rgb fromFlColor(Fl_Color color);
Fl_Color toFlColor(const Rgb& color);

/// Mix two colors: amount 0 returns base, 1 returns overlay.
Rgb blend(const Rgb& base, const Rgb& overlay, double amount);

/// Relative luminance in [0, 1].
double luminance(const Rgb& color);

/// Normalize user or stored text to "#rrggbb", or "" when it is not a color.
std::string normalizeHex(const std::string& text);

/// Inline CSS for a verse "Tag" marker drawn in the given color.
std::string markerStyle(const std::string& hex, const Rgb& contentBackground,
                        bool darkTheme, bool continued);

/// Inline CSS that highlights a verse with the given color.
std::string highlightStyle(const std::string& hex, const Rgb& contentBackground,
                           bool darkTheme);

/// Options for the color picker's extra (non-palette) choice.
struct PickerOptions {
    std::string title = "Choose Color";
    /// Label for the empty choice ("Default", "None"); empty hides it.
    std::string emptyChoiceLabel;
    /// Color shown for the empty choice's sample, if any.
    std::string emptyChoicePreview;
};

/// Show the color picker. color is in/out: "#rrggbb" or "" for the empty
/// choice. Returns false when cancelled.
bool chooseColor(std::string& color, const PickerOptions& options);

} // namespace tag_colors
} // namespace verdad

#endif // VERDAD_TAG_COLORS_H
