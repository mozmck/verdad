#include "ui/HtmlWidget.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Scrollbar.H>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <sstream>

namespace verdad {

HtmlWidget::HtmlWidget(int X, int Y, int W, int H, const char* label)
    : Fl_Widget(X, Y, W, H, label) {
    // Create vertical scrollbar
    int sbW = 16;
    scrollbar_ = new Fl_Scrollbar(X + W - sbW, Y, sbW, H);
    scrollbar_->type(FL_VERTICAL);
    scrollbar_->callback(scrollbarCallback, this);
    scrollbar_->hide();

    // Default CSS
    masterCSS_ =
        "body { font-family: serif; font-size: 14px; }\n"
        "sup { font-size: 0.7em; vertical-align: super; }\n";
}

HtmlWidget::~HtmlWidget() {
    delete scrollbar_;
}

void HtmlWidget::setHtml(const std::string& html, const std::string& baseUrl) {
    currentHtml_ = html;
    baseUrl_ = baseUrl;
    scrollY_ = 0;

    // Wrap in basic HTML if not already
    std::string fullHtml = html;
    if (html.find("<html") == std::string::npos &&
        html.find("<HTML") == std::string::npos) {
        fullHtml = "<html><body>" + html + "</body></html>";
    }

    // Create litehtml document
    doc_ = litehtml::document::createFromString(fullHtml, this, masterCSS_);

    renderDocument();
    updateScrollbar();
    redraw();
}

void HtmlWidget::setMasterCSS(const std::string& css) {
    masterCSS_ = css;
}

void HtmlWidget::scrollToAnchor(const std::string& anchor) {
    // litehtml doesn't have built-in anchor scrolling,
    // so we would need to find the element position
    (void)anchor;
    redraw();
}

void HtmlWidget::scrollToTop() {
    scrollY_ = 0;
    if (scrollbar_->visible()) {
        scrollbar_->value(0);
    }
    redraw();
}

void HtmlWidget::renderDocument() {
    if (!doc_) return;

    int sbW = scrollbar_->visible() ? 16 : 0;
    int renderWidth = w() - sbW;
    if (renderWidth < 10) renderWidth = 10;

    doc_->render(renderWidth);
    contentHeight_ = doc_->height();
}

void HtmlWidget::updateScrollbar() {
    if (!doc_) {
        scrollbar_->hide();
        return;
    }

    if (contentHeight_ > h()) {
        scrollbar_->show();
        scrollbar_->resize(x() + w() - 16, y(), 16, h());
        scrollbar_->value(scrollY_, h(), 0, contentHeight_);
        scrollbar_->linesize(20);

        // Re-render with scrollbar width accounted for
        renderDocument();
    } else {
        scrollbar_->hide();
        scrollY_ = 0;
    }
}

void HtmlWidget::scrollbarCallback(Fl_Widget* w, void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    auto* sb = static_cast<Fl_Scrollbar*>(w);
    self->scrollY_ = sb->value();
    self->redraw();
}

void HtmlWidget::draw() {
    // Draw background
    fl_push_clip(x(), y(), w(), h());
    fl_color(FL_WHITE);
    fl_rectf(x(), y(), w(), h());

    if (doc_) {
        // Set up clip for content area (exclude scrollbar)
        int sbW = scrollbar_->visible() ? 16 : 0;
        fl_push_clip(x(), y(), w() - sbW, h());

        // Draw the litehtml document with scroll offset
        litehtml::position clip(x(), y(), w() - sbW, h());
        doc_->draw(0, x(), y() - scrollY_, &clip);

        fl_pop_clip();
    } else {
        // No content
        fl_color(FL_DARK3);
        fl_font(FL_HELVETICA, 12);
        fl_draw("No content", x() + 10, y() + 20);
    }

    fl_pop_clip();

    // Draw scrollbar
    if (scrollbar_->visible()) {
        scrollbar_->redraw();
    }
}

int HtmlWidget::handle(int event) {
    switch (event) {
    case FL_PUSH: {
        if (Fl::event_button() == FL_RIGHT_MOUSE && contextCallback_) {
            // Right-click context menu
            if (doc_) {
                litehtml::position::vector redraw;
                std::vector<litehtml::position> clientRects;
                auto el = doc_->root()->get_element_by_point(
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_);

                std::string word, href;
                if (el) {
                    // Try to get text content and href
                    auto parent = el->parent();
                    if (parent) {
                        auto attrHref = parent->get_attr("href");
                        if (attrHref) href = attrHref;
                    }
                }

                contextCallback_(word, href, Fl::event_x(), Fl::event_y());
            }
            return 1;
        }

        if (doc_) {
            litehtml::position::vector redraw;
            if (doc_->on_lbutton_down(Fl::event_x() - x(),
                                       Fl::event_y() - y() + scrollY_,
                                       Fl::event_x() - x(),
                                       Fl::event_y() - y() + scrollY_,
                                       redraw)) {
                this->redraw();
            }
        }
        return 1;
    }

    case FL_RELEASE: {
        if (doc_) {
            litehtml::position::vector redraw;
            if (doc_->on_lbutton_up(Fl::event_x() - x(),
                                     Fl::event_y() - y() + scrollY_,
                                     Fl::event_x() - x(),
                                     Fl::event_y() - y() + scrollY_,
                                     redraw)) {
                this->redraw();
            }
        }
        return 1;
    }

    case FL_MOVE: {
        if (doc_) {
            litehtml::position::vector redraw;
            if (doc_->on_mouse_over(Fl::event_x() - x(),
                                     Fl::event_y() - y() + scrollY_,
                                     Fl::event_x() - x(),
                                     Fl::event_y() - y() + scrollY_,
                                     redraw)) {
                this->redraw();
            }

            // Check for hover over links/words
            if (hoverCallback_) {
                auto el = doc_->root()->get_element_by_point(
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_);

                std::string word, href;
                if (el) {
                    auto parent = el->parent();
                    if (parent) {
                        auto attrHref = parent->get_attr("href");
                        if (attrHref) href = attrHref;

                        auto attrClass = parent->get_attr("class");
                        if (attrClass) {
                            std::string cls(attrClass);
                            if (cls.find("strongs") != std::string::npos ||
                                cls.find("lemma") != std::string::npos) {
                                // This is a Strong's annotated word
                            }
                        }
                    }
                }

                if (href != lastHoverHref_ || word != lastHoverWord_) {
                    lastHoverWord_ = word;
                    lastHoverHref_ = href;
                    if (!href.empty() || !word.empty()) {
                        hoverCallback_(word, href,
                                       Fl::event_x(), Fl::event_y());
                    }
                }
            }
        }
        return 1;
    }

    case FL_MOUSEWHEEL: {
        int dy = Fl::event_dy() * 30;
        scrollY_ += dy;
        if (scrollY_ < 0) scrollY_ = 0;
        if (scrollY_ > contentHeight_ - h())
            scrollY_ = std::max(0, contentHeight_ - h());

        if (scrollbar_->visible()) {
            scrollbar_->value(scrollY_, h(), 0, contentHeight_);
        }
        this->redraw();
        return 1;
    }

    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1;

    default:
        break;
    }

    return Fl_Widget::handle(event);
}

// --- litehtml::document_container implementation ---

litehtml::uint_ptr HtmlWidget::create_font(const char* faceName, int size,
                                             int weight,
                                             litehtml::font_style italic,
                                             unsigned int decoration,
                                             litehtml::font_metrics* fm) {
    FontInfo fi;
    fi.flFont = mapFont(faceName, weight, italic == litehtml::font_style_italic);
    fi.size = size;
    fi.weight = weight;
    fi.italic = (italic == litehtml::font_style_italic);
    fi.decoration = decoration;

    // Measure font metrics
    fl_font(fi.flFont, fi.size);
    if (fm) {
        fm->height = fl_height();
        fm->ascent = fl_height() - fl_descent();
        fm->descent = fl_descent();
        fm->x_height = static_cast<int>(fm->ascent * 0.5);
        fm->draw_spaces = (decoration != 0);
    }

    litehtml::uint_ptr id = nextFontId_++;
    fonts_[id] = fi;
    return id;
}

void HtmlWidget::delete_font(litehtml::uint_ptr hFont) {
    fonts_.erase(hFont);
}

int HtmlWidget::text_width(const char* text, litehtml::uint_ptr hFont) {
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return 0;

    fl_font(it->second.flFont, it->second.size);
    return static_cast<int>(fl_width(text));
}

void HtmlWidget::draw_text(litehtml::uint_ptr hdc, const char* text,
                            litehtml::uint_ptr hFont, litehtml::web_color color,
                            const litehtml::position& pos) {
    (void)hdc;
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return;

    fl_font(it->second.flFont, it->second.size);
    fl_color(color.red, color.green, color.blue);
    fl_draw(text, pos.x, pos.y + pos.height - fl_descent());

    // Draw decorations
    if (it->second.decoration & litehtml::font_decoration_underline) {
        int lineY = pos.y + pos.height - fl_descent() + 2;
        fl_line(pos.x, lineY, pos.x + pos.width, lineY);
    }
    if (it->second.decoration & litehtml::font_decoration_linethrough) {
        int lineY = pos.y + pos.height / 2;
        fl_line(pos.x, lineY, pos.x + pos.width, lineY);
    }
}

int HtmlWidget::pt_to_px(int pt) const {
    // Approximate: 96 DPI standard
    return static_cast<int>(pt * 96.0 / 72.0);
}

int HtmlWidget::get_default_font_size() const {
    return 14;
}

const char* HtmlWidget::get_default_font_name() const {
    return "serif";
}

void HtmlWidget::draw_list_marker(litehtml::uint_ptr hdc,
                                   const litehtml::list_marker& marker) {
    (void)hdc;
    fl_color(marker.color.red, marker.color.green, marker.color.blue);

    if (marker.marker_type == litehtml::list_style_type_disc) {
        fl_pie(marker.pos.x, marker.pos.y,
               marker.pos.width, marker.pos.height, 0, 360);
    } else if (marker.marker_type == litehtml::list_style_type_circle) {
        fl_arc(marker.pos.x, marker.pos.y,
               marker.pos.width, marker.pos.height, 0, 360);
    } else if (marker.marker_type == litehtml::list_style_type_square) {
        fl_rectf(marker.pos.x, marker.pos.y,
                 marker.pos.width, marker.pos.height);
    }
}

void HtmlWidget::load_image(const char* /*src*/, const char* /*baseurl*/,
                             bool /*redraw_on_ready*/) {
    // Image loading not implemented for Bible text
}

void HtmlWidget::get_image_size(const char* /*src*/, const char* /*baseurl*/,
                                 litehtml::size& sz) {
    sz.width = 0;
    sz.height = 0;
}

void HtmlWidget::draw_background(litehtml::uint_ptr hdc,
                                  const std::vector<litehtml::background_paint>& bg) {
    (void)hdc;
    for (const auto& b : bg) {
        if (b.color.alpha > 0) {
            fl_color(b.color.red, b.color.green, b.color.blue);
            fl_rectf(b.clip_box.x, b.clip_box.y,
                     b.clip_box.width, b.clip_box.height);
        }
    }
}

void HtmlWidget::draw_borders(litehtml::uint_ptr hdc,
                               const litehtml::borders& borders,
                               const litehtml::position& draw_pos,
                               bool /*root*/) {
    (void)hdc;

    // Draw each border side
    auto drawSide = [&](const litehtml::border& b, int x1, int y1, int x2, int y2) {
        if (b.width > 0 && b.style != litehtml::border_style_none &&
            b.style != litehtml::border_style_hidden) {
            fl_color(b.color.red, b.color.green, b.color.blue);
            fl_line_style(FL_SOLID, b.width);
            fl_line(x1, y1, x2, y2);
            fl_line_style(0);
        }
    };

    int x = draw_pos.x;
    int y = draw_pos.y;
    int w = draw_pos.width;
    int h = draw_pos.height;

    drawSide(borders.top, x, y, x + w, y);
    drawSide(borders.bottom, x, y + h, x + w, y + h);
    drawSide(borders.left, x, y, x, y + h);
    drawSide(borders.right, x + w, y, x + w, y + h);
}

void HtmlWidget::set_caption(const char* /*caption*/) {
    // Not used for our widget
}

void HtmlWidget::set_base_url(const char* base_url) {
    if (base_url) baseUrl_ = base_url;
}

void HtmlWidget::link(const std::shared_ptr<litehtml::document>& /*doc*/,
                       const litehtml::element::ptr& /*el*/) {
    // Handle <link> elements (e.g., stylesheets) - not needed for our use
}

void HtmlWidget::on_anchor_click(const char* url,
                                  const litehtml::element::ptr& /*el*/) {
    if (url && linkCallback_) {
        linkCallback_(url);
    }
}

void HtmlWidget::set_cursor(const char* cursor) {
    if (!cursor) return;
    if (strcmp(cursor, "pointer") == 0) {
        fl_cursor(FL_CURSOR_HAND);
    } else {
        fl_cursor(FL_CURSOR_DEFAULT);
    }
}

void HtmlWidget::transform_text(litehtml::string& text,
                                 litehtml::text_transform tt) {
    switch (tt) {
    case litehtml::text_transform_capitalize: {
        bool newWord = true;
        for (auto& ch : text) {
            unsigned char uc = static_cast<unsigned char>(ch);
            if (newWord && std::isalpha(uc)) {
                ch = static_cast<char>(std::toupper(uc));
                newWord = false;
            } else if (std::isspace(uc)) {
                newWord = true;
            }
        }
        break;
    }
    case litehtml::text_transform_uppercase:
        for (auto& ch : text) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        break;
    case litehtml::text_transform_lowercase:
        for (auto& ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        break;
    default:
        break;
    }
}

void HtmlWidget::import_css(litehtml::string& /*text*/,
                             const litehtml::string& /*url*/,
                             litehtml::string& /*baseurl*/) {
    // CSS import not needed for our use
}

void HtmlWidget::set_clip(const litehtml::position& pos,
                           const litehtml::border_radiuses& /*bdr_radius*/) {
    ClipRect cr{pos.x, pos.y, pos.width, pos.height};
    clipStack_.push_back(cr);
    fl_push_clip(pos.x, pos.y, pos.width, pos.height);
}

void HtmlWidget::del_clip() {
    if (!clipStack_.empty()) {
        clipStack_.pop_back();
        fl_pop_clip();
    }
}

void HtmlWidget::get_client_rect(litehtml::position& client) const {
    int sbW = scrollbar_->visible() ? 16 : 0;
    client.x = x();
    client.y = y();
    client.width = w() - sbW;
    client.height = h();
}

std::shared_ptr<litehtml::element> HtmlWidget::create_element(
    const char* /*tag_name*/,
    const litehtml::string_map& /*attributes*/,
    const std::shared_ptr<litehtml::document>& /*doc*/) {
    return nullptr; // Use default element creation
}

void HtmlWidget::get_media_features(litehtml::media_features& media) const {
    int sbW = scrollbar_->visible() ? 16 : 0;
    media.type = litehtml::media_type_screen;
    media.width = w() - sbW;
    media.height = h();
    media.device_width = Fl::w();
    media.device_height = Fl::h();
    media.color = 8;
    media.monochrome = 0;
    media.color_index = 256;
    media.resolution = 96;
}

void HtmlWidget::get_language(litehtml::string& language,
                               litehtml::string& culture) const {
    language = "en";
    culture = "";
}

Fl_Font HtmlWidget::mapFont(const char* faceName, int weight, bool italic) {
    if (!faceName) return FL_HELVETICA;

    std::string face(faceName);

    // Map common font family names to FLTK fonts
    Fl_Font base = FL_HELVETICA;

    if (face.find("serif") != std::string::npos &&
        face.find("sans") == std::string::npos) {
        base = FL_TIMES;
    } else if (face.find("sans") != std::string::npos) {
        base = FL_HELVETICA;
    } else if (face.find("mono") != std::string::npos ||
               face.find("courier") != std::string::npos ||
               face.find("Courier") != std::string::npos) {
        base = FL_COURIER;
    } else if (face.find("Times") != std::string::npos ||
               face.find("Georgia") != std::string::npos) {
        base = FL_TIMES;
    } else if (face.find("Arial") != std::string::npos ||
               face.find("Helvetica") != std::string::npos ||
               face.find("DejaVu Sans") != std::string::npos) {
        base = FL_HELVETICA;
    }

    // Apply bold/italic modifiers
    if (weight >= 700 && italic) {
        return base + FL_BOLD_ITALIC;
    } else if (weight >= 700) {
        return base + FL_BOLD;
    } else if (italic) {
        return base + FL_ITALIC;
    }

    return base;
}

} // namespace verdad
