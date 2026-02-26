#ifndef VERDAD_HTML_WIDGET_H
#define VERDAD_HTML_WIDGET_H

#include <FL/Fl_Widget.H>
#include <FL/Fl_Scrollbar.H>
// Save and undefine X11 True/False macros that conflict with litehtml
#ifdef True
#define _SAVED_X11_TRUE True
#undef True
#endif
#ifdef False
#define _SAVED_X11_FALSE False
#undef False
#endif

#include <litehtml.h>
#include <litehtml/render_item.h>

// Restore X11 True/False macros
#ifdef _SAVED_X11_TRUE
#define True _SAVED_X11_TRUE
#undef _SAVED_X11_TRUE
#endif
#ifdef _SAVED_X11_FALSE
#define False _SAVED_X11_FALSE
#undef _SAVED_X11_FALSE
#endif
#include <string>
#include <functional>
#include <map>
#include <memory>

namespace verdad {

/// FLTK widget that renders XHTML content using litehtml.
/// Implements litehtml::document_container for drawing with FLTK.
class HtmlWidget : public Fl_Widget, public litehtml::document_container {
public:
    HtmlWidget(int X, int Y, int W, int H, const char* label = nullptr);
    ~HtmlWidget() override;

    /// Set HTML content to display
    void setHtml(const std::string& html, const std::string& baseUrl = "");

    /// Set the master CSS stylesheet
    void setMasterCSS(const std::string& css);

    /// Scroll to a named anchor
    void scrollToAnchor(const std::string& anchor);

    /// Scroll to top
    void scrollToTop();

    /// Get current scroll position
    int scrollY() const { return scrollY_; }

    /// Set vertical scroll position (clamped to content range).
    void setScrollY(int y);

    /// Get currently loaded HTML fragment.
    const std::string& currentHtml() const { return currentHtml_; }

    /// Handle widget geometry updates and rerender on resize.
    void resize(int X, int Y, int W, int H) override;

    // Callbacks
    using LinkCallback = std::function<void(const std::string& url)>;
    using HoverCallback = std::function<void(const std::string& word,
                                              const std::string& href,
                                              const std::string& strong,
                                              const std::string& morph,
                                              int x, int y)>;
    using ContextCallback = std::function<void(const std::string& word,
                                                const std::string& href,
                                                int x, int y)>;

    void setLinkCallback(LinkCallback cb) { linkCallback_ = std::move(cb); }
    void setHoverCallback(HoverCallback cb) { hoverCallback_ = std::move(cb); }
    void setContextCallback(ContextCallback cb) { contextCallback_ = std::move(cb); }

    // --- litehtml::document_container interface ---
    litehtml::uint_ptr create_font(const litehtml::font_description& descr,
                                    const litehtml::document* doc,
                                    litehtml::font_metrics* fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char* text,
                   litehtml::uint_ptr hFont, litehtml::web_color color,
                   const litehtml::position& pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override;
    litehtml::pixel_t get_default_font_size() const override;
    const char* get_default_font_name() const override;
    void draw_list_marker(litehtml::uint_ptr hdc,
                          const litehtml::list_marker& marker) override;
    void load_image(const char* src, const char* baseurl,
                    bool redraw_on_ready) override;
    void get_image_size(const char* src, const char* baseurl,
                        litehtml::size& sz) override;
    void draw_solid_fill(litehtml::uint_ptr hdc,
                         const litehtml::background_layer& layer,
                         const litehtml::web_color& color) override;
    void draw_image(litehtml::uint_ptr hdc,
                    const litehtml::background_layer& layer,
                    const std::string& url,
                    const std::string& base_url) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc,
                              const litehtml::background_layer& layer,
                              const litehtml::background_layer::linear_gradient& gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc,
                              const litehtml::background_layer& layer,
                              const litehtml::background_layer::radial_gradient& gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc,
                             const litehtml::background_layer& layer,
                             const litehtml::background_layer::conic_gradient& gradient) override;
    void draw_borders(litehtml::uint_ptr hdc,
                      const litehtml::borders& borders,
                      const litehtml::position& draw_pos,
                      bool root) override;
    void set_caption(const char* caption) override;
    void set_base_url(const char* base_url) override;
    void link(const std::shared_ptr<litehtml::document>& doc,
              const litehtml::element::ptr& el) override;
    void on_anchor_click(const char* url,
                         const litehtml::element::ptr& el) override;
    void on_mouse_event(const litehtml::element::ptr& el,
                        litehtml::mouse_event event) override;
    void set_cursor(const char* cursor) override;
    void transform_text(litehtml::string& text,
                        litehtml::text_transform tt) override;
    void import_css(litehtml::string& text, const litehtml::string& url,
                    litehtml::string& baseurl) override;
    void set_clip(const litehtml::position& pos,
                  const litehtml::border_radiuses& bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position& viewport) const override;
    std::shared_ptr<litehtml::element> create_element(
        const char* tag_name,
        const litehtml::string_map& attributes,
        const std::shared_ptr<litehtml::document>& doc) override;
    void get_media_features(litehtml::media_features& media) const override;
    void get_language(litehtml::string& language,
                      litehtml::string& culture) const override;

protected:
    void draw(void) override;
    int handle(int event) override;

private:
    std::shared_ptr<litehtml::document> doc_;
    std::string masterCSS_;
    std::string currentHtml_;
    std::string baseUrl_;
    int scrollY_ = 0;
    int contentHeight_ = 0;

    // Font cache
    struct FontInfo {
        Fl_Font flFont;
        int size;
        int weight;
        bool italic;
        int decorationLine;
    };
    std::map<litehtml::uint_ptr, FontInfo> fonts_;
    litehtml::uint_ptr nextFontId_ = 1;

    // Scrollbar
    Fl_Scrollbar* scrollbar_;

    // Callbacks
    LinkCallback linkCallback_;
    HoverCallback hoverCallback_;
    ContextCallback contextCallback_;
    std::string pendingLinkUrl_;
    bool linkCallbackScheduled_ = false;
    bool reflowScheduled_ = false;

    // Mouse tracking
    std::string lastHoverWord_;
    std::string lastHoverHref_;
    std::string lastHoverStrong_;
    std::string lastHoverMorph_;

    // Clip stack
    struct ClipRect {
        int x, y, w, h;
    };
    std::vector<ClipRect> clipStack_;

    /// Render the document at the current widget size
    void renderDocument();

    /// Update scrollbar range
    void updateScrollbar();

    /// Static scrollbar callback
    static void scrollbarCallback(Fl_Widget* w, void* data);

    /// Deferred link dispatch callback to avoid mutating the document inside litehtml click handling.
    static void dispatchDeferredLink(void* data);

    /// Deferred document reflow callback used to coalesce rapid resize events.
    static void dispatchDeferredReflow(void* data);

    /// Map an FLTK font from a face name
    Fl_Font mapFont(const char* faceName, int weight, bool italic);
};

} // namespace verdad

#endif // VERDAD_HTML_WIDGET_H
