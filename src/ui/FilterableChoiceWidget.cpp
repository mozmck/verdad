#include "ui/FilterableChoiceWidget.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Button.H>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace verdad {
namespace {

char lowerAsciiChar(char c) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
}

std::string_view trimView(std::string_view text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

bool equalsNoCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;

    for (size_t i = 0; i < left.size(); ++i) {
        if (lowerAsciiChar(left[i]) != lowerAsciiChar(right[i])) {
            return false;
        }
    }
    return true;
}

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;

    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char left, char right) {
                              return lowerAsciiChar(left) == lowerAsciiChar(right);
                          });
    return it != haystack.end();
}

std::string escapeChoiceLabel(const std::string& label) {
    std::string escaped;
    escaped.reserve(label.size());
    for (char c : label) {
        if (c == '\\' || c == '/') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

std::string unescapeChoiceLabel(const char* label) {
    if (!label) return "";

    std::string unescaped;
    for (size_t i = 0; label[i] != '\0'; ++i) {
        if (label[i] == '\\' && label[i + 1] != '\0') {
            unescaped.push_back(label[i + 1]);
            ++i;
            continue;
        }
        unescaped.push_back(label[i]);
    }
    return unescaped;
}

int findMenuIndexByLabel(Fl_Menu_Button* menuButton, const std::string& label) {
    if (!menuButton || label.empty()) return -1;

    for (int i = 0; i < menuButton->size(); ++i) {
        const Fl_Menu_Item* item = menuButton->menu()
            ? &menuButton->menu()[i]
            : nullptr;
        if (!item || !item->label()) continue;
        if (unescapeChoiceLabel(item->label()) == label) return i;
    }

    return -1;
}

} // namespace

FilterableChoiceWidget::FilterableChoiceWidget(int X, int Y, int W, int H,
                                               const char* label)
    : Fl_Input_Choice(X, Y, W, H, label) {
    input()->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    input()->callback(onInputChanged, this);
    menubutton()->callback(onMenuSelected, this);
}

FilterableChoiceWidget::~FilterableChoiceWidget() = default;

void FilterableChoiceWidget::setItems(const std::vector<std::string>& items) {
    ownedItems_ = items;
    items_ = &ownedItems_;
    if (!selectedItem_.empty() && exactItemMatch(selectedItem_).empty()) {
        selectedItem_.clear();
    }
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setItemsView(const std::vector<std::string>* items) {
    items_ = items ? items : &ownedItems_;
    if (!selectedItem_.empty() && exactItemMatch(selectedItem_).empty()) {
        selectedItem_.clear();
    }
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setSelectedValue(const std::string& value) {
    selectedItem_ = exactItemMatch(value);
    Fl_Input_Choice::value(value.c_str());
    refreshMenu(value);
}

void FilterableChoiceWidget::setDisplayedValue(const std::string& value) {
    selectedItem_ = exactItemMatch(value);
    Fl_Input_Choice::value(value.c_str());
    clearMenu();
}

std::string FilterableChoiceWidget::selectedValue() const {
    std::string typed = Fl_Input_Choice::value() ? Fl_Input_Choice::value() : "";
    std::string exact = exactItemMatch(typed);
    if (!exact.empty()) return exact;
    return selectedItem_;
}

void FilterableChoiceWidget::setNoMatchesLabel(const std::string& label) {
    noMatchesLabel_ = label;
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setShowAllWhenFilterEmpty(bool showAll) {
    showAllWhenFilterEmpty_ = showAll;
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setMaxVisibleItems(size_t maxItems) {
    if (maxItems == 0) {
        maxVisibleItems_ = std::numeric_limits<size_t>::max();
    } else {
        maxVisibleItems_ = maxItems;
    }
    refreshMenu(input() && input()->value() ? input()->value() : "");
}

void FilterableChoiceWidget::setEnsureItemsCallback(std::function<void()> callback) {
    ensureItemsCallback_ = std::move(callback);
}

const std::vector<std::string>& FilterableChoiceWidget::items() const {
    return items_ ? *items_ : ownedItems_;
}

void FilterableChoiceWidget::clearMenu() {
    Fl_Menu_Button* menuButton = menubutton();
    if (!menuButton) return;

    menuButton->clear();
    menuButton->deactivate();
}

void FilterableChoiceWidget::refreshMenu(const std::string& filter) {
    Fl_Menu_Button* menuButton = menubutton();
    if (!menuButton) return;

    std::string_view normalizedFilter = trimView(filter);
    if (normalizedFilter.empty() && !showAllWhenFilterEmpty_) {
        clearMenu();
        return;
    }

    clearMenu();

    size_t matchCount = 0;
    for (const auto& item : items()) {
        if (normalizedFilter.empty() ||
            containsNoCase(item, normalizedFilter)) {
            if (matchCount < maxVisibleItems_) {
                menuButton->add(escapeChoiceLabel(item).c_str());
            }
            ++matchCount;
        }
    }

    if (matchCount == 0) {
        if (!noMatchesLabel_.empty()) {
            menuButton->add(noMatchesLabel_.c_str());
            menuButton->value(0);
        }
        menuButton->deactivate();
        return;
    }

    menuButton->activate();
    std::string exact = exactItemMatch(
        input() && input()->value() ? input()->value() : "");
    if (!exact.empty()) {
        selectedItem_ = exact;
    }

    int selectedIndex = -1;
    if (!selectedItem_.empty()) {
        selectedIndex = findMenuIndexByLabel(menuButton, selectedItem_);
    }
    if (selectedIndex < 0 && menuButton->size() > 0) {
        selectedIndex = 0;
    }
    if (selectedIndex >= 0) {
        menuButton->value(selectedIndex);
    }
}

std::string FilterableChoiceWidget::exactItemMatch(const std::string& value) const {
    std::string_view wanted = trimView(value);
    if (wanted.empty()) return "";

    for (const auto& item : items()) {
        if (equalsNoCase(item, wanted)) {
            return item;
        }
    }
    return "";
}

void FilterableChoiceWidget::onInputChanged(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<FilterableChoiceWidget*>(data);
    if (!self) return;

    if (self->items().empty() && self->ensureItemsCallback_) {
        self->ensureItemsCallback_();
    }

    std::string filter = self->input() && self->input()->value()
        ? self->input()->value()
        : "";
    self->refreshMenu(filter);

    const int key = Fl::event_key();
    if (Fl::event() == FL_KEYBOARD &&
        (key == FL_Enter || key == FL_KP_Enter)) {
        self->do_callback();
    }
}

void FilterableChoiceWidget::onMenuSelected(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<FilterableChoiceWidget*>(data);
    if (!self) return;

    Fl_Menu_Button* menuButton = self->menubutton();
    if (!menuButton) return;

    const Fl_Menu_Item* item = menuButton->mvalue();
    if (!item || !item->label()) return;

    std::string selected = unescapeChoiceLabel(item->label());
    if (!self->noMatchesLabel_.empty() && selected == self->noMatchesLabel_) {
        return;
    }

    self->selectedItem_ = selected;
    self->Fl_Input_Choice::value(selected.c_str());
    self->refreshMenu(selected);
    self->do_callback();
}

} // namespace verdad
