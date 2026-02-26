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
    if (linkCallbackScheduled_) {
        Fl::remove_timeout(dispatchDeferredLink, this);
        linkCallbackScheduled_ = false;
    }
    if (reflowScheduled_) {
        Fl::remove_timeout(dispatchDeferredReflow, this);
        reflowScheduled_ = false;
    }

    // Destroy the document while callback targets (fonts_, etc.) are still live.
    doc_.reset();
    fonts_.clear();

    // The scrollbar is created while a parent Fl_Group is current, so FLTK owns
    // and destroys it with the group children list.
    scrollbar_ = nullptr;
}

void HtmlWidget::setHtml(const std::string& html, const std::string& baseUrl) {
    if (reflowScheduled_) {
        Fl::remove_timeout(dispatchDeferredReflow, this);
        reflowScheduled_ = false;
    }

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

void HtmlWidget::setScrollY(int y) {
    int maxScroll = std::max(0, contentHeight_ - h());
    scrollY_ = std::clamp(y, 0, maxScroll);
    if (scrollbar_->visible()) {
        scrollbar_->value(scrollY_, h(), 0, contentHeight_);
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
                auto el = doc_->root_render()->get_element_by_point(
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    [](const std::shared_ptr<litehtml::render_item>&) { return true; });

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
                auto el = doc_->root_render()->get_element_by_point(
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    Fl::event_x() - x(), Fl::event_y() - y() + scrollY_,
                    [](const std::shared_ptr<litehtml::render_item>&) { return true; });

                std::string word, href, strong, morph;
                if (el) {
                    // Get text content of the element under the cursor
                    litehtml::string elText;
                    el->get_text(elText);
                    word = elText;

                    // Walk up parent chain (max 5 levels) to find data-strong,
                    // data-morph, and href attributes
                    auto cur = el;
                    for (int depth = 0; cur && depth < 5; ++depth) {
                        if (href.empty()) {
                            auto h = cur->get_attr("href");
                            if (h && *h) href = h;
                        }
                        if (strong.empty()) {
                            auto s = cur->get_attr("data-strong");
                            if (s && *s) strong = s;
                        }
                        if (morph.empty()) {
                            auto m = cur->get_attr("data-morph");
                            if (m && *m) morph = m;
                        }
                        if (!strong.empty() && !morph.empty() && !href.empty())
                            break;
                        cur = cur->parent();
                    }
                }

                if (href != lastHoverHref_ || word != lastHoverWord_ ||
                    strong != lastHoverStrong_ || morph != lastHoverMorph_) {
                    lastHoverWord_   = word;
                    lastHoverHref_   = href;
                    lastHoverStrong_ = strong;
                    lastHoverMorph_  = morph;
                    hoverCallback_(word, href, strong, morph,
                                   Fl::event_x(), Fl::event_y());
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

    case FL_ENTER:
        return 1;

    case FL_LEAVE:
        if (hoverCallback_ && (!lastHoverWord_.empty() || !lastHoverHref_.empty() ||
                                !lastHoverStrong_.empty() || !lastHoverMorph_.empty())) {
            lastHoverWord_.clear();
            lastHoverHref_.clear();
            lastHoverStrong_.clear();
            lastHoverMorph_.clear();
            hoverCallback_("", "", "", "", 0, 0);
        }
        return 1;

    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1;

    default:
        break;
    }

    return Fl_Widget::handle(event);
}

void HtmlWidget::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);

    if (scrollbar_) {
        const int sbW = 16;
        scrollbar_->resize(X + W - sbW, Y, sbW, H);
    }

    if (doc_) {
        // Coalesce many resize events while dragging splitters.
        if (reflowScheduled_) {
            Fl::remove_timeout(dispatchDeferredReflow, this);
        }
        Fl::add_timeout(0.04, dispatchDeferredReflow, this);
        reflowScheduled_ = true;
    }
    redraw();
}

void HtmlWidget::dispatchDeferredReflow(void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    if (!self) return;

    self->reflowScheduled_ = false;
    if (!self->doc_) return;

    self->renderDocument();
    self->updateScrollbar();
    self->redraw();
}

// --- litehtml::document_container implementation ---

litehtml::uint_ptr HtmlWidget::create_font(const litehtml::font_description& descr,
                                             const litehtml::document* /*doc*/,
                                             litehtml::font_metrics* fm) {
    FontInfo fi;
    fi.flFont = mapFont(descr.family.c_str(), descr.weight,
                         descr.style == litehtml::font_style_italic);
    fi.size = static_cast<int>(descr.size);
    fi.weight = descr.weight;
    fi.italic = (descr.style == litehtml::font_style_italic);
    fi.decorationLine = descr.decoration_line;

    // Measure font metrics
    fl_font(fi.flFont, fi.size);
    if (fm) {
        fm->height = static_cast<litehtml::pixel_t>(fl_height());
        fm->ascent = static_cast<litehtml::pixel_t>(fl_height() - fl_descent());
        fm->descent = static_cast<litehtml::pixel_t>(fl_descent());
        fm->x_height = fm->ascent * 0.5f;
        fm->draw_spaces = (descr.decoration_line != 0);
    }

    litehtml::uint_ptr id = nextFontId_++;
    fonts_[id] = fi;
    return id;
}

void HtmlWidget::delete_font(litehtml::uint_ptr hFont) {
    fonts_.erase(hFont);
}

litehtml::pixel_t HtmlWidget::text_width(const char* text, litehtml::uint_ptr hFont) {
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return 0;

    fl_font(it->second.flFont, it->second.size);
    return static_cast<litehtml::pixel_t>(fl_width(text));
}

void HtmlWidget::draw_text(litehtml::uint_ptr hdc, const char* text,
                            litehtml::uint_ptr hFont, litehtml::web_color color,
                            const litehtml::position& pos) {
    (void)hdc;
    auto it = fonts_.find(hFont);
    if (it == fonts_.end()) return;

    fl_font(it->second.flFont, it->second.size);
    fl_color(color.red, color.green, color.blue);
    fl_draw(text, static_cast<int>(pos.x),
            static_cast<int>(pos.y + pos.height) - fl_descent());

    // Draw decorations
    if (it->second.decorationLine & litehtml::text_decoration_line_underline) {
        int lineY = static_cast<int>(pos.y + pos.height) - fl_descent() + 2;
        fl_line(static_cast<int>(pos.x), lineY,
                static_cast<int>(pos.x + pos.width), lineY);
    }
    if (it->second.decorationLine & litehtml::text_decoration_line_line_through) {
        int lineY = static_cast<int>(pos.y + pos.height / 2);
        fl_line(static_cast<int>(pos.x), lineY,
                static_cast<int>(pos.x + pos.width), lineY);
    }
}

litehtml::pixel_t HtmlWidget::pt_to_px(float pt) const {
    // Approximate: 96 DPI standard
    return static_cast<litehtml::pixel_t>(pt * 96.0f / 72.0f);
}

litehtml::pixel_t HtmlWidget::get_default_font_size() const {
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
        fl_pie(static_cast<int>(marker.pos.x), static_cast<int>(marker.pos.y),
               static_cast<int>(marker.pos.width), static_cast<int>(marker.pos.height), 0, 360);
    } else if (marker.marker_type == litehtml::list_style_type_circle) {
        fl_arc(static_cast<int>(marker.pos.x), static_cast<int>(marker.pos.y),
               static_cast<int>(marker.pos.width), static_cast<int>(marker.pos.height), 0, 360);
    } else if (marker.marker_type == litehtml::list_style_type_square) {
        fl_rectf(static_cast<int>(marker.pos.x), static_cast<int>(marker.pos.y),
                 static_cast<int>(marker.pos.width), static_cast<int>(marker.pos.height));
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

void HtmlWidget::draw_solid_fill(litehtml::uint_ptr hdc,
                                  const litehtml::background_layer& layer,
                                  const litehtml::web_color& color) {
    (void)hdc;
    if (color.alpha > 0) {
        fl_color(color.red, color.green, color.blue);
        fl_rectf(static_cast<int>(layer.clip_box.x),
                 static_cast<int>(layer.clip_box.y),
                 static_cast<int>(layer.clip_box.width),
                 static_cast<int>(layer.clip_box.height));
    }
}

void HtmlWidget::draw_image(litehtml::uint_ptr /*hdc*/,
                             const litehtml::background_layer& /*layer*/,
                             const std::string& /*url*/,
                             const std::string& /*base_url*/) {
    // Image drawing not implemented for Bible text
}

void HtmlWidget::draw_linear_gradient(litehtml::uint_ptr /*hdc*/,
                                       const litehtml::background_layer& /*layer*/,
                                       const litehtml::background_layer::linear_gradient& /*gradient*/) {
    // Gradient drawing not implemented
}

void HtmlWidget::draw_radial_gradient(litehtml::uint_ptr /*hdc*/,
                                       const litehtml::background_layer& /*layer*/,
                                       const litehtml::background_layer::radial_gradient& /*gradient*/) {
    // Gradient drawing not implemented
}

void HtmlWidget::draw_conic_gradient(litehtml::uint_ptr /*hdc*/,
                                      const litehtml::background_layer& /*layer*/,
                                      const litehtml::background_layer::conic_gradient& /*gradient*/) {
    // Gradient drawing not implemented
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

    int x = static_cast<int>(draw_pos.x);
    int y = static_cast<int>(draw_pos.y);
    int w = static_cast<int>(draw_pos.width);
    int h = static_cast<int>(draw_pos.height);

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
    if (!url || !linkCallback_) return;

    // Defer callback execution until after litehtml finishes processing the
    // current mouse event. This avoids invalidating doc_ during on_lbutton_up.
    pendingLinkUrl_ = url;
    if (linkCallbackScheduled_) {
        Fl::remove_timeout(dispatchDeferredLink, this);
    }
    Fl::add_timeout(0.0, dispatchDeferredLink, this);
    linkCallbackScheduled_ = true;
}

void HtmlWidget::dispatchDeferredLink(void* data) {
    auto* self = static_cast<HtmlWidget*>(data);
    if (!self) return;

    self->linkCallbackScheduled_ = false;
    if (!self->linkCallback_ || self->pendingLinkUrl_.empty()) return;

    std::string url = std::move(self->pendingLinkUrl_);
    self->pendingLinkUrl_.clear();
    self->linkCallback_(url);
}

void HtmlWidget::on_mouse_event(const litehtml::element::ptr& /*el*/,
                                 litehtml::mouse_event /*event*/) {
    // Mouse event handling not needed beyond what handle() provides
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
    ClipRect cr{static_cast<int>(pos.x), static_cast<int>(pos.y),
                static_cast<int>(pos.width), static_cast<int>(pos.height)};
    clipStack_.push_back(cr);
    fl_push_clip(static_cast<int>(pos.x), static_cast<int>(pos.y),
                 static_cast<int>(pos.width), static_cast<int>(pos.height));
}

void HtmlWidget::del_clip() {
    if (!clipStack_.empty()) {
        clipStack_.pop_back();
        fl_pop_clip();
    }
}

void HtmlWidget::get_viewport(litehtml::position& client) const {
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
