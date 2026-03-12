#include "ui/TagPanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/VerseReferenceSort.h"
#include "ui/VerseListCopyMenu.h"
#include "sword/SwordManager.h"
#include "tags/TagManager.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <cctype>

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

bool verseMatchesFilter(const std::string& verseKey, const TagFilterQuery& query) {
    if (query.lowered.empty()) return true;

    if (toLowerCopy(verseKey).find(query.lowered) != std::string::npos) {
        return true;
    }

    if (!query.hasVerseRef) return false;

    try {
        SwordManager::VerseRef verseRef = SwordManager::parseVerseRef(verseKey);
        return verseRef.book == query.verseRef.book &&
               verseRef.chapter == query.verseRef.chapter &&
               verseRef.verse == query.verseRef.verse;
    } catch (...) {
        return false;
    }
}

bool tagMatchesFilter(TagManager& tagMgr,
                      const std::string& tagName,
                      const TagFilterQuery& query) {
    if (query.lowered.empty()) return true;

    if (toLowerCopy(tagName).find(query.lowered) != std::string::npos) {
        return true;
    }

    const auto verses = tagMgr.getVersesWithTag(tagName);
    return std::any_of(verses.begin(), verses.end(),
                       [&](const std::string& verseKey) {
                           return verseMatchesFilter(verseKey, query);
                       });
}

class AddTagDialog {
public:
    AddTagDialog(TagManager& tagMgr, const std::string& verseKey)
        : tagMgr_(tagMgr)
        , verseKey_(verseKey)
        , dialog_(440, 380, "Add Tag") {
        allTags_ = tagMgr_.getAllTags();

        dialog_.set_modal();
        dialog_.begin();

        prompt_ = new Fl_Box(16, 16, dialog_.w() - 32, 44);
        prompt_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        std::string promptText = "Add tag to " + verseKey_ +
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
    std::string verseKey_;
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

class TagVerseBrowser : public Fl_Hold_Browser {
public:
    TagVerseBrowser(TagPanel* owner, int X, int Y, int W, int H)
        : Fl_Hold_Browser(X, Y, W, H)
        , owner_(owner) {}

    int handle(int event) override {
        if (event == FL_PUSH) {
            int button = Fl::event_button();
            if (button == FL_RIGHT_MOUSE) {
                pressedLine_ = 0;
                if (owner_) {
                    owner_->showVerseListContextMenu(Fl::event_x(), Fl::event_y());
                }
                return 1;
            }
            if (button == FL_LEFT_MOUSE || button == FL_MIDDLE_MOUSE) {
                pressedLine_ = lineAtEvent();
            } else {
                pressedLine_ = 0;
            }
        }

        const int button = (event == FL_RELEASE) ? Fl::event_button() : 0;
        const int line = (event == FL_RELEASE) ? lineAtEvent() : 0;
        const bool isDoubleClick = (event == FL_RELEASE) ? (Fl::event_clicks() > 0) : false;
        const int handled = Fl_Hold_Browser::handle(event);

        if (event == FL_RELEASE && owner_ &&
            (button == FL_LEFT_MOUSE || button == FL_MIDDLE_MOUSE)) {
            int targetLine = line;
            if (pressedLine_ > 0 && (targetLine <= 0 || targetLine == pressedLine_)) {
                targetLine = pressedLine_;
            }
            owner_->activateVerseLine(targetLine > 0 ? targetLine : value(),
                                      button, isDoubleClick);
            pressedLine_ = 0;
            return 1;
        }

        return handled;
    }

private:
    int lineAtEvent() {
        void* item = find_item(Fl::event_y());
        return item ? lineno(item) : 0;
    }

    TagPanel* owner_ = nullptr;
    int pressedLine_ = 0;
};

TagPanel::TagPanel(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , filterInput_(nullptr)
    , clearFilterButton_(nullptr)
    , tagBrowser_(nullptr)
    , verseBrowser_(nullptr)
    , newTagButton_(nullptr)
    , deleteTagButton_(nullptr)
    , renameTagButton_(nullptr)
    , removeVerseButton_(nullptr) {
    begin();

    filterInput_ = new TagFilterInput(this, X, Y, W, 28);
    filterInput_->when(FL_WHEN_CHANGED);
    filterInput_->callback(onFilterChange, this);
    filterInput_->tooltip("Filter tags by name or by verse reference");

    clearFilterButton_ = new Fl_Button(X, Y, 10, 10, "X");
    clearFilterButton_->callback(onClearFilter, this);
    clearFilterButton_->tooltip("Clear the tag filter");

    tagBrowser_ = new Fl_Hold_Browser(X, Y, W, H);
    tagBrowser_->type(FL_HOLD_BROWSER);
    tagBrowser_->callback(onTagSelect, this);

    verseBrowser_ = new TagVerseBrowser(this, X, Y, W, H);
    verseBrowser_->callback(onVerseSelect, this);
    verseBrowser_->when(FL_WHEN_CHANGED);

    newTagButton_ = new Fl_Button(X, Y, 10, 10, "New");
    newTagButton_->callback(onNewTag, this);

    renameTagButton_ = new Fl_Button(X, Y, 10, 10, "Rename");
    renameTagButton_->callback(onRenameTag, this);

    deleteTagButton_ = new Fl_Button(X, Y, 10, 10, "Delete");
    deleteTagButton_->callback(onDeleteTag, this);

    removeVerseButton_ = new Fl_Button(X, Y, 10, 10, "Remove Verse");
    removeVerseButton_->callback(onRemoveVerse, this);

    end();

    resizable(verseBrowser_);
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

void TagPanel::showAddTagDialog(const std::string& verseKey) {
    AddTagDialog dialog(app_->tagManager(), verseKey);
    std::string tagName;
    if (dialog.open(tagName)) {
        app_->tagManager().tagVerse(verseKey, tagName);
        app_->tagManager().save();
        selectedTagName_ = tagName;
        selectedVerseKey_ = verseKey;
        populateTags();
        refreshBiblePane();
    }
}

void TagPanel::showTagsForVerse(const std::string& verseKey) {
    if (!filterInput_) return;
    filterInput_->value(verseKey.c_str());
    selectedVerseKey_ = verseKey;
    updateFilterControls();
    populateTags();
}

void TagPanel::layoutChildren() {
    if (!filterInput_ || !clearFilterButton_ || !tagBrowser_ || !verseBrowser_ ||
        !newTagButton_ || !renameTagButton_ || !deleteTagButton_ ||
        !removeVerseButton_) {
        return;
    }

    const int padding = 2;
    const int topInset = 12;
    const int filterH = 26;
    const int clearButtonW = 52;
    const int buttonH = 25;
    const int innerX = x() + padding;
    const int innerW = std::max(20, w() - 2 * padding);
    const int bottomY = y() + h() - padding;

    int cy = y() + topInset;
    int actualClearButtonW = std::min(clearButtonW, std::max(0, innerW - 20 - padding));
    int filterInputW = std::max(20, innerW - actualClearButtonW - padding);
    filterInput_->resize(innerX, cy, filterInputW, filterH);
    clearFilterButton_->resize(innerX + filterInputW + padding, cy,
                               std::max(0, innerW - filterInputW - padding), filterH);
    cy += filterH + padding;

    int listAreaH = std::max(50, bottomY - cy - (2 * buttonH) - (3 * padding));
    int tagH = std::max(24, listAreaH / 2);
    int verseH = std::max(24, listAreaH - tagH);

    tagBrowser_->resize(innerX, cy, innerW, tagH);
    cy += tagH + padding;

    const int buttonW = (innerW - 2 * padding) / 3;
    newTagButton_->resize(innerX, cy, buttonW, buttonH);
    renameTagButton_->resize(innerX + buttonW + padding, cy, buttonW, buttonH);
    deleteTagButton_->resize(innerX + 2 * (buttonW + padding), cy,
                             innerW - 2 * (buttonW + padding), buttonH);
    cy += buttonH + padding;

    verseBrowser_->resize(innerX, cy, innerW, verseH);
    cy += verseH + padding;

    removeVerseButton_->resize(innerX, std::min(cy, bottomY - buttonH), innerW, buttonH);
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

    filterInput_->value("");
    selectedVerseKey_.clear();
    updateFilterControls();
    populateTags();
    if (focusInput) {
        filterInput_->take_focus();
    }
}

void TagPanel::refreshBiblePane() {
    if (app_ && app_->mainWindow() && app_->mainWindow()->biblePane()) {
        app_->mainWindow()->biblePane()->refresh();
    }
}

std::string TagPanel::activeBibleModule() const {
    if (app_ && app_->mainWindow() && app_->mainWindow()->biblePane()) {
        std::string module = trimCopy(app_->mainWindow()->biblePane()->currentModule());
        if (!module.empty()) return module;
    }

    auto bibles = app_->swordManager().getBibleModules();
    if (!bibles.empty()) return bibles.front().name;
    return "";
}

void TagPanel::updateVersePreview(const std::string& verseKey) {
    if (verseKey.empty() || !app_ || !app_->mainWindow() || !app_->mainWindow()->leftPane()) {
        return;
    }

    std::string module = activeBibleModule();
    if (module.empty()) return;

    std::string html = app_->swordManager().getVerseText(module, verseKey);
    app_->mainWindow()->leftPane()->setVersePreviewText(html, module, verseKey);
}

void TagPanel::populateTags() {
    tagBrowser_->clear();
    tagBrowser_->value(0);
    visibleTags_.clear();

    const TagFilterQuery filter = buildTagFilterQuery(
        filterInput_ ? filterInput_->value() : "");

    auto tags = app_->tagManager().getAllTags();
    for (const auto& tag : tags) {
        if (!tagMatchesFilter(app_->tagManager(), tag.name, filter)) {
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
        populateVerses(selectedTagName_);
    } else {
        selectedTagName_.clear();
        selectedVerseKey_.clear();
        verseBrowser_->clear();
        verseBrowser_->value(0);
    }
}

void TagPanel::populateVerses(const std::string& tagName) {
    verseBrowser_->clear();
    verseBrowser_->value(0);

    auto verses = app_->tagManager().getVersesWithTag(tagName);
    verse_reference_sort::sortVerseKeysCanonical(app_->swordManager(),
                                                 activeBibleModule(),
                                                 verses);
    const TagFilterQuery filter = buildTagFilterQuery(
        filterInput_ ? filterInput_->value() : "");
    int selectedLine = 0;
    int firstFilterMatch = 0;

    for (const auto& verse : verses) {
        verseBrowser_->add(verse.c_str());
        int line = verseBrowser_->size();
        if (selectedLine == 0 && !selectedVerseKey_.empty() &&
            verse == selectedVerseKey_) {
            selectedLine = line;
        }
        if (firstFilterMatch == 0 && verseMatchesFilter(verse, filter)) {
            firstFilterMatch = line;
        }
    }

    if (selectedLine == 0 && firstFilterMatch > 0) {
        selectedLine = firstFilterMatch;
    }
    if (selectedLine == 0 && !verses.empty()) {
        selectedLine = 1;
    }

    if (selectedLine > 0) {
        verseBrowser_->value(selectedLine);
        const char* text = verseBrowser_->text(selectedLine);
        selectedVerseKey_ = text ? text : "";
        updateVersePreview(selectedVerseKey_);
    } else {
        selectedVerseKey_.clear();
    }
}

void TagPanel::activateVerseLine(int line, int mouseButton, bool isDoubleClick) {
    if (!app_ || !app_->mainWindow()) return;
    if (line <= 0 || line > verseBrowser_->size()) return;

    const char* text = verseBrowser_->text(line);
    if (!text || !text[0]) return;

    std::string verseKey = text;
    std::string module = activeBibleModule();

    if (mouseButton == FL_MIDDLE_MOUSE) {
        app_->mainWindow()->openInNewStudyTab(module, verseKey);
        return;
    }

    if (mouseButton == FL_LEFT_MOUSE && isDoubleClick) {
        if (!module.empty()) {
            app_->mainWindow()->navigateTo(module, verseKey);
        } else {
            app_->mainWindow()->navigateTo(verseKey);
        }
    }
}

void TagPanel::showVerseListContextMenu(int screenX, int screenY) {
    if (!app_ || !verseBrowser_ || verseBrowser_->size() <= 0) return;

    std::vector<verse_list_copy::Entry> entries;
    entries.reserve(static_cast<size_t>(verseBrowser_->size()));
    const std::string module = activeBibleModule();
    for (int line = 1; line <= verseBrowser_->size(); ++line) {
        const char* text = verseBrowser_->text(line);
        if (!text || !text[0]) continue;
        entries.push_back({module, text});
    }

    verse_list_copy::showVerseListCopyMenu(
        app_->swordManager(), entries, screenX, screenY);
}

void TagPanel::onFilterChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;
    self->updateFilterControls();
    self->populateTags();
}

void TagPanel::onClearFilter(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;
    self->clearFilter(true);
}

void TagPanel::onTagSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;

    int idx = self->tagBrowser_->value();
    if (idx <= 0 || idx > static_cast<int>(self->visibleTags_.size())) {
        self->selectedTagName_.clear();
        self->verseBrowser_->clear();
        self->selectedVerseKey_.clear();
        return;
    }

    self->selectedTagName_ = self->visibleTags_[idx - 1];
    self->populateVerses(self->selectedTagName_);
}

void TagPanel::onVerseSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;

    int idx = self->verseBrowser_->value();
    if (idx <= 0 || idx > self->verseBrowser_->size()) return;

    const char* text = self->verseBrowser_->text(idx);
    if (text && text[0]) {
        self->selectedVerseKey_ = text;
        self->updateVersePreview(self->selectedVerseKey_);
    }
}

void TagPanel::onNewTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

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
        self->refreshBiblePane();
    } else {
        fl_alert("Tag '%s' already exists.", name.c_str());
    }
}

void TagPanel::onDeleteTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    int idx = self->tagBrowser_->value();
    if (idx <= 0) return;

    if (idx - 1 >= static_cast<int>(self->visibleTags_.size())) return;

    std::string tagName = self->visibleTags_[idx - 1];
    int confirm = fl_choice("Delete tag '%s' and remove from all verses?",
                            "Cancel", "Delete", nullptr, tagName.c_str());
    if (confirm == 1) {
        self->app_->tagManager().deleteTag(tagName);
        self->app_->tagManager().save();
        if (self->selectedTagName_ == tagName) {
            self->selectedTagName_.clear();
            self->selectedVerseKey_.clear();
        }
        self->populateTags();
        self->refreshBiblePane();
    }
}

void TagPanel::onRenameTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    int idx = self->tagBrowser_->value();
    if (idx <= 0) return;

    if (idx - 1 >= static_cast<int>(self->visibleTags_.size())) return;

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
        self->refreshBiblePane();
    } else {
        fl_alert("Cannot rename: tag '%s' already exists.", newName.c_str());
    }
}

void TagPanel::onRemoveVerse(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;
    if (self->selectedTagName_.empty() || self->selectedVerseKey_.empty()) return;

    self->app_->tagManager().untagVerse(self->selectedVerseKey_, self->selectedTagName_);
    self->app_->tagManager().save();
    self->populateTags();
    self->refreshBiblePane();
}

} // namespace verdad
