#include <litehtml.h>
#include <litehtml/master_css.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct TextRun {
    std::string text;
    litehtml::position pos;
};

// Deterministic font metrics let line wrapping run without a display server.
class LayoutContainer : public litehtml::document_container {
public:
    std::vector<TextRun> runs;
    litehtml::uint_ptr create_font(const litehtml::font_description& descr,
            const litehtml::document*, litehtml::font_metrics* fm) override {
        const auto size = descr.size;
        fm->height = size;
        fm->ascent = size * 0.8f;
        fm->descent = size * 0.2f;
        fm->x_height = size * 0.5f;
        fm->draw_spaces = true;
        return 1;
    }
    void delete_font(litehtml::uint_ptr) override {}
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr) override {
        int count = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
            if ((*p & 0xc0) != 0x80) ++count;
        }
        return count * 10;
    }
    void draw_text(litehtml::uint_ptr, const char* text, litehtml::uint_ptr,
            litehtml::web_color, const litehtml::position& pos) override {
        runs.push_back({text, pos});
    }
    litehtml::pixel_t pt_to_px(float pt) const override { return pt; }
    litehtml::pixel_t get_default_font_size() const override { return 20; }
    const char* get_default_font_name() const override { return "test"; }
    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker&) override {}
    void load_image(const char*, const char*, bool) override {}
    void get_image_size(const char*, const char*, litehtml::size& size) override { size = {0, 0}; }
    void draw_image(litehtml::uint_ptr, const litehtml::background_layer&, const std::string&, const std::string&) override {}
    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer&, const litehtml::web_color&) override {}
    void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer&, const litehtml::background_layer::linear_gradient&) override {}
    void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer&, const litehtml::background_layer::radial_gradient&) override {}
    void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer&, const litehtml::background_layer::conic_gradient&) override {}
    void draw_borders(litehtml::uint_ptr, const litehtml::borders&, const litehtml::position&, bool) override {}
    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<litehtml::document>&, const litehtml::element::ptr&) override {}
    void on_anchor_click(const char*, const litehtml::element::ptr&) override {}
    void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(litehtml::string&, litehtml::text_transform) override {}
    void import_css(litehtml::string&, const litehtml::string&, litehtml::string&) override {}
    void set_clip(const litehtml::position&, const litehtml::border_radiuses&) override {}
    void del_clip() override {}
    void get_viewport(litehtml::position& viewport) const override { viewport = {0, 0, 800, 600}; }
    litehtml::element::ptr create_element(const char*, const litehtml::string_map&, const std::shared_ptr<litehtml::document>&) override { return nullptr; }
    void get_media_features(litehtml::media_features& media) const override { media.type = litehtml::media_type_screen; media.width = 800; media.height = 600; }
    void get_language(litehtml::string& language, litehtml::string& culture) const override { language = "en"; culture = ""; }

    void layout(const std::string& html, int width) {
        const std::string css = std::string(litehtml::master_css) +
            "body { margin: 0; font-size: 20px; line-height: 1.5; } "
            "a.noteMarker { font-size: 0.7em; vertical-align: super; }";
        auto doc = litehtml::document::createFromString(html.c_str(), this, css.c_str());
        doc->render(width);
        runs.clear();
        doc->draw(0, 0, 0, nullptr);
    }
};

int failures = 0;

void attached(const std::string& html, const std::string& word,
              const std::string& suffix, int width) {
    LayoutContainer container;
    container.layout(html, width);
    const TextRun* a = nullptr;
    const TextRun* b = nullptr;
    for (const auto& run : container.runs) {
        auto text = run.text;
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (text == word) a = &run;
        if (text == suffix) b = &run;
    }
    if (!a || !b || b->pos.x < a->pos.x + a->pos.width - 0.1f ||
        b->pos.y >= a->pos.y + a->pos.height ||
        (width >= 70 && b->pos.x + b->pos.width > width + 0.1f)) {
        std::cerr << "Split attached text at width " << width << ": " << html << '\n';
        for (const auto& run : container.runs) {
            std::cerr << "  [" << run.text << "] " << run.pos.x << ',' << run.pos.y
                      << " " << run.pos.width << 'x' << run.pos.height << '\n';
        }
        ++failures;
    }
}

void breaksBetween(const std::string& html, const std::string& first,
                   const std::string& second, int width) {
    LayoutContainer container;
    container.layout(html, width);
    const TextRun* a = nullptr;
    const TextRun* b = nullptr;
    for (const auto& run : container.runs) {
        auto text = run.text;
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (text == first) a = &run;
        if (text == second) b = &run;
    }
    if (!a || !b || b->pos.y <= a->pos.y) {
        std::cerr << "Lost line break at width " << width << ": " << html << '\n';
        ++failures;
    }
}

}

int main() {
    for (int width = 70; width <= 140; ++width) {
        attached("Before <b>word</b>.", "word", ".", width);
        attached("Before <span>word</span><a class='noteMarker' href='#note'>a</a>", "word", "a", width);
        attached("Before <b>word</b><span>,</span>", "word", ",", width);
        attached("Before <b>word</b><span>;</span>", "word", ";", width);
        attached("Before <b>word</b><span>:</span>", "word", ":", width);
        attached("Before <b>(</b><span>word</span><i>)</i>", "word", ")", width);
        attached("Before <b>(</b><span>word</span><i>)</i>", "(", "word", width);
        attached("Before <b>word</b><span>!</span>", "word", "!", width);
        attached("Before <b>word</b><span>?</span>", "word", "?", width);
        attached("Before <b>word</b><span>”</span>", "word", "”", width);
        attached("Before <b>word</b><span>…</span>", "word", "…", width);
        attached("Before <b>word</b><a class='noteMarker' href='#note'><small><sup>*n1</sup></small></a>", "word", "*n1", width);
        attached("Before <b>word</b><a class='noteMarker' href='#note'>a</a><span>.</span>", "a", ".", width);
        attached("Before <b>word</b><span>.</span><a class='noteMarker' href='#note'>a</a>", ".", "a", width);
        attached("Before <span>word</span><a class='noteMarker' href='#n1'>a</a><a class='noteMarker' href='#n2'>b</a>.", "word", "a", width);
        attached("Before <span>word</span><a class='noteMarker' href='#n1'>a</a><a class='noteMarker' href='#n2'>b</a>.", "a", "b", width);
        attached("Before <span>word</span><a class='noteMarker' href='#n1'>a</a><a class='noteMarker' href='#n2'>b</a>.", "b", ".", width);
        attached("Before <b>wo<i>rd</i></b>.", "rd", ".", width);
        attached("Before <span style='display: inline-table'><span style='display: table-row'>word</span></span>.", "word", ".", width);
        attached("Before <b>word</b><span></span>.", "word", ".", width);
        attached("<b>Before word</b><span>.</span>", "word", ".", width);
        attached("Before <span style='display: inline-block'>word</span>.", "word", ".", width);
    }
    attached("Before <span>word</span><a class='noteMarker' href='#note'>a</a>", "word", "a", 110);
    attached("<span>word</span>.", "word", ".", 45);
    // A very narrow pane may overflow an unbroken word, but cannot detach its suffix.
    attached("<span>word</span><a class='noteMarker'>a</a>.", "word", "a", 35);
    attached("<span>word</span><a class='noteMarker'>a</a>.", "a", ".", 35);
    attached("Before <b>left</b><span>—</span><i>right</i>", "left", "—", 140);
    attached("Before <b>left</b><span>—</span><i>right</i>", "—", "right", 140);
    attached("Before <b>left</b><span>-</span><i>right</i>", "left", "-", 140);
    attached("Before <b>left</b><span>-</span><i>right</i>", "-", "right", 140);
    attached("Before <b>création</b>.", "création", ".", 150);
    for (const std::string dash : {"-", "–", "—"}) {
        breaksBetween("Before <span>" + dash + "</span> after", "Before", dash, 70);
    }
    breaksBetween("<span>word</span> after", "word", "after", 80);
    breaksBetween("<span>word</span><br>after", "word", "after", 100);
    breaksBetween("<div style='white-space: pre-wrap'>word after</div>", "word", "after", 80);
    breaksBetween("中文", "中", "文", 10);
    breaksBetween("中word", "中", "word", 40);
    attached("中。", "中", "。", 10);
    if (failures) return EXIT_FAILURE;
    std::cout << "HTML wrapping regressions passed.\n";
    return EXIT_SUCCESS;
}
