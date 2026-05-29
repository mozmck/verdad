#include "ui/TagPanel.h"

#include "app/VerdadApp.h"
#include "ui/BiblePane.h"
#include "sword/SwordManager.h"
#include "tags/TagManager.h"
#include "ui/LeftPane.h"
#include "ui/MainWindow.h"
#include "ui/UiFontUtils.h"
#include "ui/VerseReferenceSort.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace verdad {
namespace {

std::string trimCopy(const std::string& text) {
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

std::string toLowerCopy(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return out;
}

bool targetEquals(const TagTarget& a, const TagTarget& b) {
    return a.kind == b.kind &&
           a.moduleName == b.moduleName &&
           a.sourceKey == b.sourceKey &&
           a.selectionText == b.selectionText;
}

bool matchesResourceFilterKind(const TagTarget& target,
                               TagPanel::ResourceFilter filter) {
    switch (filter) {
    case TagPanel::ResourceFilter::All:
        return true;
    case TagPanel::ResourceFilter::Verse:
        return target.kind == TagTarget::Kind::Verse;
    case TagPanel::ResourceFilter::Commentary:
        return target.kind == TagTarget::Kind::Commentary;
    case TagPanel::ResourceFilter::GeneralBook:
        return target.kind == TagTarget::Kind::GeneralBook;
    }
    return true;
}

std::string targetDisplayLabel(const TagTarget& target) {
    return target.displayLabel();
}

struct TagFilterQuery {
    std::string raw;
    std::string lowered;
    bool hasVerseRef = false;
    SwordManager::VerseRef verseRef;
};

TagFilterQuery buildTagFilterQuery(const std::string& text) {
    TagFilterQuery query;
    query.raw = trimCopy(text);
    query.lowered = toLowerCopy(query.raw);

    if (query.raw.empty()) return query;

    try {
        query.verseRef = SwordManager::parseVerseRef(query.raw);
        query.hasVerseRef = !query.verseRef.book.empty() &&
                            query.verseRef.chapter > 0 &&
                            query.verseRef.verse > 0;
    } catch (...) {
        query.verseRef = SwordManager::VerseRef{};
        query.hasVerseRef = false;
    }

    return query;
}

bool targetMatchesFilter(const TagTarget& target,
                         const TagFilterQuery& query,
                         TagPanel::ResourceFilter resourceFilter) {
    if (!matchesResourceFilterKind(target, resourceFilter)) return false;
    if (query.lowered.empty()) return true;

    const std::string label = toLowerCopy(targetDisplayLabel(target));
    if (label.find(query.lowered) != std::string::npos) return true;

    if (toLowerCopy(target.moduleName).find(query.lowered) != std::string::npos) return true;
    if (toLowerCopy(target.sourceKey).find(query.lowered) != std::string::npos) return true;
    if (toLowerCopy(target.selectionText).find(query.lowered) != std::string::npos) return true;

    if (!query.hasVerseRef || target.kind != TagTarget::Kind::Verse) return false;

    try {
        SwordManager::VerseRef verseRef = SwordManager::parseVerseRef(target.sourceKey);
        return verseRef.book == query.verseRef.book &&
               verseRef.chapter == query.verseRef.chapter &&
               verseRef.verse == query.verseRef.verse;
    } catch (...) {
        return false;
    }
}

bool tagMatchesFilter(TagManager& tagMgr,
                      const std::string& tagName,
                      const TagFilterQuery& query,
                      TagPanel::ResourceFilter resourceFilter) {
    if (query.lowered.empty()) return true;

    if (toLowerCopy(tagName).find(query.lowered) != std::string::npos) {
        return true;
    }

    const auto targets = tagMgr.getTargetsWithTag(tagName);
    return std::any_of(targets.begin(), targets.end(),
                       [&](const TagTarget& target) {
                           return targetMatchesFilter(target, query, resourceFilter);
                       });
}

struct TargetSortKey {
    bool parsed = false;
    int bookRank = std::numeric_limits<int>::max();
    int chapter = std::numeric_limits<int>::max();
    int verse = std::numeric_limits<int>::max();
    std::string kindToken;
    std::string moduleName;
    std::string sourceKey;
    std::string selectionText;
    std::string label;
};

std::string normalizeBookKey(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
}

void sortTargetsCanonical(SwordManager& swordMgr,
                          const std::string& moduleName,
                          std::vector<TagTarget>& targets) {
    if (targets.size() < 2) return;

    std::unordered_map<std::string, int> bookOrder;
    const auto books = swordMgr.getBookNames(moduleName);
    for (size_t i = 0; i < books.size(); ++i) {
        std::string key = normalizeBookKey(books[i]);
        if (!key.empty() && bookOrder.find(key) == bookOrder.end()) {
            bookOrder.emplace(key, static_cast<int>(i));
        }
    }

    struct Entry {
        TagTarget target;
        TargetSortKey key;
    };

    std::vector<Entry> entries;
    entries.reserve(targets.size());
    for (const auto& target : targets) {
        Entry entry;
        entry.target = target;
        entry.key.kindToken = target.kind == TagTarget::Kind::Verse
            ? "verse"
            : (target.kind == TagTarget::Kind::Commentary ? "commentary" : "general_book");
        entry.key.moduleName = target.moduleName;
        entry.key.sourceKey = target.sourceKey;
        entry.key.selectionText = target.selectionText;
        entry.key.label = targetDisplayLabel(target);

        if (target.kind == TagTarget::Kind::Verse) {
            SwordManager::VerseRef parsed;
            try {
                parsed = SwordManager::parseVerseRef(target.sourceKey);
            } catch (...) {
                parsed = SwordManager::VerseRef{};
            }
            if (!parsed.book.empty() && parsed.chapter > 0 && parsed.verse > 0) {
                entry.key.parsed = true;
                entry.key.chapter = parsed.chapter;
                entry.key.verse = parsed.verse;
                std::string normBook = normalizeBookKey(parsed.book);
                auto it = bookOrder.find(normBook);
                if (it != bookOrder.end()) {
                    entry.key.bookRank = it->second;
                }
            }
        }

        entries.push_back(std::move(entry));
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) {
        if (a.key.parsed != b.key.parsed) return a.key.parsed > b.key.parsed;
        if (a.key.kindToken != b.key.kindToken) return a.key.kindToken < b.key.kindToken;
        if (a.key.parsed) {
            if (a.key.bookRank != b.key.bookRank) return a.key.bookRank < b.key.bookRank;
            if (a.key.chapter != b.key.chapter) return a.key.chapter < b.key.chapter;
            if (a.key.verse != b.key.verse) return a.key.verse < b.key.verse;
        }
        if (a.key.moduleName != b.key.moduleName) return a.key.moduleName < b.key.moduleName;
        if (a.key.sourceKey != b.key.sourceKey) return a.key.sourceKey < b.key.sourceKey;
        if (a.key.selectionText != b.key.selectionText) return a.key.selectionText < b.key.selectionText;
        return a.key.label < b.key.label;
    });

    targets.clear();
    targets.reserve(entries.size());
    for (auto& entry : entries) {
        targets.push_back(std::move(entry.target));
    }
}

class AddTagDialog {
public:
    AddTagDialog(TagManager& tagMgr, const TagTarget& target)
        : tagMgr_(tagMgr)
        , target_(target)
        , dialog_(440, 380, "Add Tag") {
        allTags_ = tagMgr_.getAllTags();

        dialog_.set_modal();
        dialog_.begin();

        prompt_ = new Fl_Box(16, 16, dialog_.w() - 32, 44);
        prompt_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        std::string promptText = "Add tag to " + target_.displayLabel() +
                                 ":\nSelect an existing tag or type a new one.";
        prompt_->copy_label(promptText.c_str());

        input_ = new Fl_Input(16, 72, dialog_.w() - 32, 28, "Tag:");
        input_->align(FL_ALIGN_TOP_LEFT);
        input_->when(FL_WHEN_CHANGED);
        input_->callback(onInputChanged, this);

        browser_ = new Fl_Hold_Browser(16, 120, dialog_.w() - 32, 200, "Existing tags:");
        browser_->align(FL_ALIGN_TOP_LEFT);
        browser_->type(FL_HOLD_BROWSER);
        browser_->when(FL_WHEN_CHANGED);
        browser_->callback(onBrowserSelect, this);

        cancelButton_ = new Fl_Button(dialog_.w() - 180, dialog_.h() - 40, 80, 28, "Cancel");
        cancelButton_->callback(onCancel, this);

        okButton_ = new Fl_Return_Button(dialog_.w() - 92, dialog_.h() - 40, 76, 28, "OK");
        okButton_->callback(onOk, this);

        dialog_.end();
        updateVisibleTags();
        ui_font::applyCurrentAppUiFont(&dialog_);
    }

    bool open(std::string& tagName) {
        dialog_.show();
        input_->take_focus();
        while (dialog_.shown()) {
            Fl::wait();
        }

        if (!accepted_) return false;
        tagName = resultTagName_;
        return true;
    }

private:
    static void onInputChanged(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<AddTagDialog*>(data);
        if (!self) return;
        self->updateVisibleTags();
    }

    static void onBrowserSelect(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<AddTagDialog*>(data);
        if (!self) return;

        std::string selected = self->selectedTagName();
        if (selected.empty()) return;

        self->input_->value(selected.c_str());
        self->input_->insert_position(static_cast<int>(selected.size()));
    }

    static void onCancel(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<AddTagDialog*>(data);
        if (!self) return;
        self->accepted_ = false;
        self->dialog_.hide();
    }

    static void onOk(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<AddTagDialog*>(data);
        if (!self) return;
        self->accept();
    }

    void accept() {
        std::string tagName = trimCopy(input_->value() ? input_->value() : "");
        if (tagName.empty()) {
            tagName = selectedTagName();
        }

        if (tagName.empty()) {
            fl_alert("Enter a tag name or select an existing tag.");
            input_->take_focus();
            return;
        }

        resultTagName_ = tagName;
        accepted_ = true;
        dialog_.hide();
    }

    std::string selectedTagName() const {
        int index = browser_->value();
        if (index <= 0 || index > static_cast<int>(visibleTags_.size())) {
            return "";
        }
        return visibleTags_[index - 1];
    }

    void updateVisibleTags() {
        const std::string filter = toLowerCopy(trimCopy(input_->value() ? input_->value() : ""));
        const std::string currentSelection = selectedTagName();
        const std::string exactInput = trimCopy(input_->value() ? input_->value() : "");

        browser_->clear();
        visibleTags_.clear();

        int selectedLine = 0;
        for (const auto& tag : allTags_) {
            if (!filter.empty() && toLowerCopy(tag.name).find(filter) == std::string::npos) {
                continue;
            }

            visibleTags_.push_back(tag.name);
            std::string line = tag.name + " (" +
                               std::to_string(tagMgr_.getTagCount(tag.name)) + ")";
            browser_->add(line.c_str());

            if (selectedLine == 0 && !currentSelection.empty() && tag.name == currentSelection) {
                selectedLine = browser_->size();
            }
            if (selectedLine == 0 && !exactInput.empty() && tag.name == exactInput) {
                selectedLine = browser_->size();
            }
        }

        browser_->value(selectedLine);
    }

    TagManager& tagMgr_;
    TagTarget target_;
    std::vector<Tag> allTags_;
    std::vector<std::string> visibleTags_;
    bool accepted_ = false;
    std::string resultTagName_;
    Fl_Double_Window dialog_;
    Fl_Box* prompt_ = nullptr;
    Fl_Input* input_ = nullptr;
    Fl_Hold_Browser* browser_ = nullptr;
    Fl_Button* cancelButton_ = nullptr;
    Fl_Return_Button* okButton_ = nullptr;
};

} // namespace

class TagFilterInput : public Fl_Input {
public:
    TagFilterInput(TagPanel* owner, int X, int Y, int W, int H)
        : Fl_Input(X, Y, W, H)
        , owner_(owner) {}

    int handle(int event) override {
        if ((event == FL_KEYBOARD || event == FL_SHORTCUT) &&
            Fl::focus() == this &&
            Fl::event_key() == FL_Escape && owner_) {
            owner_->clearFilter(true);
            return 1;
        }
        return Fl_Input::handle(event);
    }

private:
    TagPanel* owner_ = nullptr;
};

class TagItemBrowser : public Fl_Hold_Browser {
public:
    TagItemBrowser(TagPanel* owner, int X, int Y, int W, int H)
        : Fl_Hold_Browser(X, Y, W, H)
        , owner_(owner) {}

    int handle(int event) override {
        if (event == FL_PUSH) {
            int button = Fl::event_button();
            if (button == FL_RIGHT_MOUSE) {
                if (owner_) owner_->showItemContextMenu(Fl::event_x(), Fl::event_y());
                return 1;
            }
        }
        return Fl_Hold_Browser::handle(event);
    }

private:
    TagPanel* owner_ = nullptr;
};

TagPanel::TagPanel(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , filterInput_(nullptr)
    , clearFilterButton_(nullptr)
    , resourceFilterChoice_(nullptr)
    , tagBrowser_(nullptr)
    , itemBrowser_(nullptr)
    , newTagButton_(nullptr)
    , deleteTagButton_(nullptr)
    , renameTagButton_(nullptr)
    , removeTagButton_(nullptr) {
    begin();

    filterInput_ = new TagFilterInput(this, X, Y, W, 28);
    filterInput_->when(FL_WHEN_CHANGED);
    filterInput_->callback(onFilterChange, this);
    filterInput_->tooltip("Filter tags by name or by item text/reference");

    clearFilterButton_ = new Fl_Button(X, Y, 10, 10, "X");
    clearFilterButton_->callback(onClearFilter, this);
    clearFilterButton_->tooltip("Clear the tag filter");

    resourceFilterChoice_ = new Fl_Choice(X, Y, 10, 10, "Types:");
    resourceFilterChoice_->add("All");
    resourceFilterChoice_->add("Verses");
    resourceFilterChoice_->add("Commentaries");
    resourceFilterChoice_->add("General Books");
    resourceFilterChoice_->value(0);
    resourceFilterChoice_->callback(onResourceFilterChange, this);
    resourceFilterChoice_->tooltip("Choose which resource types to browse and search");

    tagBrowser_ = new Fl_Hold_Browser(X, Y, W, H);
    tagBrowser_->type(FL_HOLD_BROWSER);
    tagBrowser_->callback(onTagSelect, this);

    itemBrowser_ = new TagItemBrowser(this, X, Y, W, H);
    itemBrowser_->type(FL_HOLD_BROWSER);
    itemBrowser_->callback(onItemSelect, this);

    newTagButton_ = new Fl_Button(X, Y, 10, 10, "New");
    newTagButton_->callback(onNewTag, this);

    renameTagButton_ = new Fl_Button(X, Y, 10, 10, "Rename");
    renameTagButton_->callback(onRenameTag, this);

    deleteTagButton_ = new Fl_Button(X, Y, 10, 10, "Delete");
    deleteTagButton_->callback(onDeleteTag, this);

    removeTagButton_ = new Fl_Button(X, Y, 10, 10, "Remove");
    removeTagButton_->callback(onRemoveTag, this);

    end();

    resizable(itemBrowser_);
    layoutChildren();
    updateFilterControls();
    populateTags();
}

TagPanel::~TagPanel() = default;

void TagPanel::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);
    layoutChildren();
}

void TagPanel::refresh() {
    populateTags();
}

void TagPanel::setVerseListLineSpacing(int pixels) {
    if (!itemBrowser_) return;
    const int spacing = std::clamp(pixels, 0, 16);
    if (itemBrowser_->linespacing() == spacing) return;
    itemBrowser_->linespacing(spacing);
    if (!selectedTagName_.empty()) {
        populateTargets(selectedTagName_);
    } else {
        itemBrowser_->redraw();
    }
}

void TagPanel::showAddTagDialog(const std::string& verseKey) {
    showAddTagDialog(TagTarget::verse(verseKey));
}

void TagPanel::showAddTagDialog(const TagTarget& target) {
    if (!app_) return;
    AddTagDialog dialog(app_->tagManager(), target);
    std::string tagName;
    if (dialog.open(tagName)) {
        app_->tagManager().tagTarget(target, tagName);
        app_->tagManager().save();
        selectedTagName_ = tagName;
        selectedTarget_ = target;
        hasSelectedTarget_ = true;
        populateTags();
        refreshPreviewForSelection();
    }
}

void TagPanel::showTagsForVerse(const std::string& verseKey) {
    if (!filterInput_) return;
    filterInput_->value(verseKey.c_str());
    filterTargetsByText_ = false;
    if (resourceFilterChoice_) {
        resourceFilterChoice_->value(1);
    }
    selectedTarget_ = TagTarget::verse(verseKey);
    hasSelectedTarget_ = true;
    updateFilterControls();
    populateTags();
}

void TagPanel::layoutChildren() {
    if (!filterInput_ || !clearFilterButton_ || !resourceFilterChoice_ ||
        !tagBrowser_ || !itemBrowser_ ||
        !newTagButton_ || !renameTagButton_ || !deleteTagButton_ ||
        !removeTagButton_) {
        return;
    }

    const int padding = 2;
    const int topInset = 12;
    const int filterH = 26;
    const int choiceW = 140;
    const int clearButtonW = 52;
    const int buttonH = 25;
    const int innerX = x() + padding;
    const int innerW = std::max(20, w() - 2 * padding);
    const int bottomY = y() + h() - padding;

    int cy = y() + topInset;
    int actualChoiceW = std::min(choiceW, std::max(0, innerW - 20 - padding));
    int actualClearButtonW = std::min(clearButtonW, std::max(0, innerW - actualChoiceW - 20 - (2 * padding)));
    int filterInputW = std::max(20, innerW - actualChoiceW - actualClearButtonW - (2 * padding));
    filterInput_->resize(innerX, cy, filterInputW, filterH);
    resourceFilterChoice_->resize(innerX + filterInputW + padding, cy,
                                  actualChoiceW, filterH);
    clearFilterButton_->resize(innerX + filterInputW + padding + actualChoiceW + padding, cy,
                               std::max(0, innerW - filterInputW - actualChoiceW - (2 * padding)), filterH);
    cy += filterH + padding;

    int listAreaH = std::max(50, bottomY - cy - (2 * buttonH) - (3 * padding));
    int tagH = std::max(24, listAreaH / 2);
    int itemH = std::max(24, listAreaH - tagH);

    tagBrowser_->resize(innerX, cy, innerW, tagH);
    cy += tagH + padding;

    const int buttonW = (innerW - 2 * padding) / 3;
    newTagButton_->resize(innerX, cy, buttonW, buttonH);
    renameTagButton_->resize(innerX + buttonW + padding, cy, buttonW, buttonH);
    deleteTagButton_->resize(innerX + 2 * (buttonW + padding), cy,
                             innerW - 2 * (buttonW + padding), buttonH);
    cy += buttonH + padding;

    itemBrowser_->resize(innerX, cy, innerW, itemH);
    cy += itemH + padding;

    removeTagButton_->resize(innerX, std::min(cy, bottomY - buttonH), innerW, buttonH);
}

void TagPanel::updateFilterControls() {
    if (!filterInput_ || !clearFilterButton_) return;

    const char* value = filterInput_->value();
    if (value && value[0]) {
        clearFilterButton_->activate();
    } else {
        clearFilterButton_->deactivate();
    }
}

void TagPanel::clearFilter(bool focusInput) {
    if (!filterInput_) return;

    filterTargetsByText_ = true;
    filterInput_->value("");
    updateFilterControls();
    populateTags();
    if (focusInput) {
        filterInput_->take_focus();
    }
}

void TagPanel::applyResourceFilterFromChoice() {
    if (!resourceFilterChoice_) return;

    switch (resourceFilterChoice_->value()) {
    case 1:
        selectedResourceFilter_ = ResourceFilter::Verse;
        break;
    case 2:
        selectedResourceFilter_ = ResourceFilter::Commentary;
        break;
    case 3:
        selectedResourceFilter_ = ResourceFilter::GeneralBook;
        break;
    case 0:
    default:
        selectedResourceFilter_ = ResourceFilter::All;
        break;
    }
}

bool TagPanel::targetMatchesResourceFilter(const TagTarget& target) const {
    return matchesResourceFilterKind(target, selectedResourceFilter_);
}

bool TagPanel::tagMatchesResourceFilter(const std::string& tagName) const {
    if (!app_) return false;
    const auto targets = app_->tagManager().getTargetsWithTag(tagName);
    return std::any_of(targets.begin(), targets.end(),
                       [&](const TagTarget& target) {
                           return targetMatchesResourceFilter(target);
                       });
}

std::string TagPanel::activeBibleModule() const {
    if (app_ && app_->mainWindow() && app_->mainWindow()->biblePane()) {
        std::string module = trimCopy(app_->mainWindow()->biblePane()->currentModule());
        if (!module.empty()) return module;
    }

    if (!app_) return "";
    auto bibles = app_->swordManager().getBibleModules();
    if (!bibles.empty()) return bibles.front().name;
    return "";
}

void TagPanel::refreshPreviewForSelection() {
    if (!hasSelectedTarget_ || !app_ || !app_->mainWindow() || !app_->mainWindow()->leftPane()) {
        return;
    }
    updateTargetPreview(selectedTarget_);
}

void TagPanel::updateTargetPreview(const TagTarget& target) {
    if (!app_ || !app_->mainWindow() || !app_->mainWindow()->leftPane()) return;

    if (target.kind == TagTarget::Kind::Verse) {
        std::string module = activeBibleModule();
        if (module.empty()) return;
        std::string html = app_->swordManager().getVerseText(module, target.sourceKey);
        app_->mainWindow()->leftPane()->setVersePreviewText(html, module, target.sourceKey);
        return;
    }

    std::ostringstream html;
    html << "<div class=\"preview-verse-block\">";
    html << "<div class=\"preview-verse-ref\">";
    html << "<a class=\"preview-verse-link\" href=\"open-preview-resource\">";
    html << (target.kind == TagTarget::Kind::Commentary ? "Open commentary" : "Open general book");
    html << "</a></div>";
    html << "<div class=\"preview-tag-resource\">";
    html << "<div><b>" << (target.kind == TagTarget::Kind::Commentary ? "Commentary" : "General Book")
         << "</b></div>";
    if (!target.moduleName.empty()) {
        html << "<div>" << target.moduleName << "</div>";
    }
    if (!target.sourceKey.empty()) {
        html << "<div>" << target.sourceKey << "</div>";
    }
    if (!target.selectionText.empty()) {
        html << "<div>" << target.selectionText << "</div>";
    }
    html << "</div></div>";

    LeftPane::PreviewKind kind =
        (target.kind == TagTarget::Kind::Commentary)
            ? LeftPane::PreviewKind::Commentary
            : LeftPane::PreviewKind::GeneralBook;
    app_->mainWindow()->leftPane()->setResourcePreviewText(
        html.str(), target.moduleName, target.sourceKey, kind);
}

void TagPanel::populateTags() {
    if (!tagBrowser_ || !itemBrowser_) return;

    applyResourceFilterFromChoice();
    tagBrowser_->clear();
    tagBrowser_->value(0);
    itemBrowser_->clear();
    itemBrowser_->value(0);
    visibleTags_.clear();
    visibleTargets_.clear();

    const TagFilterQuery filter = buildTagFilterQuery(
        filterInput_ ? filterInput_->value() : "");

    auto tags = app_->tagManager().getAllTags();
    for (const auto& tag : tags) {
        if (!tagMatchesResourceFilter(tag.name)) {
            continue;
        }
        if (!tagMatchesFilter(app_->tagManager(), tag.name, filter, selectedResourceFilter_)) {
            continue;
        }
        visibleTags_.push_back(tag.name);
        int count = app_->tagManager().getTagCount(tag.name);
        std::string line = tag.name + " (" + std::to_string(count) + ")";
        tagBrowser_->add(line.c_str());
    }

    int selectedLine = 0;
    if (!selectedTagName_.empty()) {
        auto it = std::find(visibleTags_.begin(), visibleTags_.end(), selectedTagName_);
        if (it != visibleTags_.end()) {
            selectedLine = static_cast<int>(std::distance(visibleTags_.begin(), it)) + 1;
        }
    }
    if (selectedLine == 0 && !visibleTags_.empty()) {
        selectedLine = 1;
    }

    if (selectedLine > 0) {
        tagBrowser_->value(selectedLine);
        selectedTagName_ = visibleTags_[selectedLine - 1];
        populateTargets(selectedTagName_);
    } else {
        selectedTagName_.clear();
        hasSelectedTarget_ = false;
    }
}

void TagPanel::populateTargets(const std::string& tagName) {
    if (!itemBrowser_) return;

    itemBrowser_->clear();
    itemBrowser_->value(0);
    visibleTargets_.clear();

    auto targets = app_->tagManager().getTargetsWithTag(tagName);
    sortTargetsCanonical(app_->swordManager(), activeBibleModule(), targets);

    const TagFilterQuery filter = filterTargetsByText_
        ? buildTagFilterQuery(filterInput_ ? filterInput_->value() : "")
        : TagFilterQuery{};

    int selectedLine = 0;
    for (const auto& target : targets) {
        if (!targetMatchesFilter(target, filter, selectedResourceFilter_)) continue;

        visibleTargets_.push_back(target);
        std::string line = targetDisplayLabel(target);
        itemBrowser_->add(line.c_str());

        if (hasSelectedTarget_ && targetEquals(target, selectedTarget_)) {
            selectedLine = itemBrowser_->size();
        }
    }

    if (selectedLine == 0 && !visibleTargets_.empty()) {
        selectedLine = 1;
    }

    if (selectedLine > 0) {
        itemBrowser_->value(selectedLine);
        selectedTarget_ = visibleTargets_[selectedLine - 1];
        hasSelectedTarget_ = true;
        refreshPreviewForSelection();
    } else {
        hasSelectedTarget_ = false;
    }
}

void TagPanel::activateTargetLine(int line, int mouseButton, bool isDoubleClick) {
    if (!app_ || !app_->mainWindow()) return;
    if (line <= 0 || line > static_cast<int>(visibleTargets_.size())) return;

    const TagTarget& target = visibleTargets_[static_cast<size_t>(line - 1)];
    selectedTarget_ = target;
    hasSelectedTarget_ = true;

    if (mouseButton == FL_MIDDLE_MOUSE && target.kind == TagTarget::Kind::Verse) {
        std::string module = activeBibleModule();
        app_->mainWindow()->openInNewStudyTab(module, target.sourceKey);
        return;
    }

    if (mouseButton == FL_LEFT_MOUSE && isDoubleClick) {
        if (target.kind == TagTarget::Kind::Verse) {
            std::string module = activeBibleModule();
            if (!module.empty()) {
                app_->mainWindow()->navigateTo(module, target.sourceKey);
            } else {
                app_->mainWindow()->navigateTo(target.sourceKey);
            }
        } else if (target.kind == TagTarget::Kind::Commentary) {
            app_->mainWindow()->showCommentary(target.moduleName,
                                               target.sourceKey,
                                               target.selectionText);
        } else {
            app_->mainWindow()->showGeneralBookEntry(target.moduleName,
                                                     target.sourceKey,
                                                     target.selectionText);
        }
    }
}

void TagPanel::showItemContextMenu(int screenX, int screenY) {
    if (!app_ || !itemBrowser_ || itemBrowser_->size() <= 0) return;

    int line = itemBrowser_->value();
    if (line <= 0 || line > static_cast<int>(visibleTargets_.size())) return;

    const TagTarget& target = visibleTargets_[static_cast<size_t>(line - 1)];
    Fl_Menu_Button menu(screenX, screenY, 0, 0);
    ui_font::applyCurrentAppMenuFont(&menu);

    std::string copyLabel = "Copy Item Label";
    menu.add(copyLabel.c_str(), 0, [](Fl_Widget*, void* data) {
        auto* self = static_cast<TagPanel*>(data);
        if (!self || !self->hasSelectedTarget_) return;
        std::string text = targetDisplayLabel(self->selectedTarget_);
        Fl::copy(text.c_str(), static_cast<int>(text.size()), 0);
        Fl::copy(text.c_str(), static_cast<int>(text.size()), 1);
    }, this);

    if (target.kind == TagTarget::Kind::Verse) {
        std::string verseLabel = "Copy Verse Reference";
        menu.add(verseLabel.c_str(), 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (!self || !self->hasSelectedTarget_) return;
            std::string text = self->selectedTarget_.sourceKey;
            Fl::copy(text.c_str(), static_cast<int>(text.size()), 0);
            Fl::copy(text.c_str(), static_cast<int>(text.size()), 1);
        }, this);
    }

    menu.popup();
}

void TagPanel::onFilterChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;
    self->filterTargetsByText_ = true;
    self->updateFilterControls();
    self->populateTags();
}

void TagPanel::onClearFilter(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;
    self->clearFilter(true);
}

void TagPanel::onResourceFilterChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;
    self->populateTags();
}

void TagPanel::onTagSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->tagBrowser_) return;

    int idx = self->tagBrowser_->value();
    if (idx <= 0 || idx > static_cast<int>(self->visibleTags_.size())) {
        self->selectedTagName_.clear();
        self->itemBrowser_->clear();
        self->visibleTargets_.clear();
        self->hasSelectedTarget_ = false;
        return;
    }

    self->selectedTagName_ = self->visibleTags_[idx - 1];
    self->populateTargets(self->selectedTagName_);
}

void TagPanel::onItemSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->itemBrowser_) return;

    int idx = self->itemBrowser_->value();
    if (idx <= 0 || idx > static_cast<int>(self->visibleTargets_.size())) return;

    self->selectedTarget_ = self->visibleTargets_[idx - 1];
    self->hasSelectedTarget_ = true;
    self->updateTargetPreview(self->selectedTarget_);
}

void TagPanel::onNewTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->app_) return;

    const char* rawName = fl_input("New tag name:");
    if (!rawName) return;

    std::string name = trimCopy(rawName);
    if (name.empty()) {
        fl_alert("Blank tags are not valid.");
        return;
    }

    if (self->app_->tagManager().createTag(name)) {
        self->app_->tagManager().save();
        self->selectedTagName_ = name;
        self->populateTags();
        self->refreshPreviewForSelection();
    } else {
        fl_alert("Tag '%s' already exists.", name.c_str());
    }
}

void TagPanel::onDeleteTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->app_ || !self->tagBrowser_) return;

    int idx = self->tagBrowser_->value();
    if (idx <= 0 || idx - 1 >= static_cast<int>(self->visibleTags_.size())) return;

    std::string tagName = self->visibleTags_[idx - 1];
    int confirm = fl_choice("Delete tag '%s' and remove it from all items?",
                            "Cancel", "Delete", nullptr, tagName.c_str());
    if (confirm == 1) {
        self->app_->tagManager().deleteTag(tagName);
        self->app_->tagManager().save();
        if (self->selectedTagName_ == tagName) {
            self->selectedTagName_.clear();
            self->visibleTargets_.clear();
            self->hasSelectedTarget_ = false;
        }
        self->populateTags();
        self->refreshPreviewForSelection();
    }
}

void TagPanel::onRenameTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->app_ || !self->tagBrowser_) return;

    int idx = self->tagBrowser_->value();
    if (idx <= 0 || idx - 1 >= static_cast<int>(self->visibleTags_.size())) return;

    std::string oldName = self->visibleTags_[idx - 1];
    const char* rawNewName = fl_input("Rename tag '%s' to:", oldName.c_str(),
                                      oldName.c_str());
    if (!rawNewName) return;

    std::string newName = trimCopy(rawNewName);
    if (newName.empty()) {
        fl_alert("Blank tags are not valid.");
        return;
    }
    if (newName == oldName) return;

    if (self->app_->tagManager().renameTag(oldName, newName)) {
        self->app_->tagManager().save();
        self->selectedTagName_ = newName;
        self->populateTags();
        self->refreshPreviewForSelection();
    } else {
        fl_alert("Cannot rename: tag '%s' already exists.", newName.c_str());
    }
}

void TagPanel::onRemoveTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->app_ || !self->hasSelectedTarget_) return;

    if (self->selectedTagName_.empty()) return;
    self->app_->tagManager().untagTarget(self->selectedTarget_, self->selectedTagName_);
    self->app_->tagManager().save();
    self->populateTags();
    self->refreshPreviewForSelection();
}

} // namespace verdad
