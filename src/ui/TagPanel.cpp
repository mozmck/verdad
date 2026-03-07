#include "ui/TagPanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "sword/SwordManager.h"
#include "tags/TagManager.h"

#include <FL/Fl.H>
#include <FL/Fl_Hold_Browser.H>
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

} // namespace

class TagVerseBrowser : public Fl_Hold_Browser {
public:
    TagVerseBrowser(TagPanel* owner, int X, int Y, int W, int H)
        : Fl_Hold_Browser(X, Y, W, H)
        , owner_(owner) {}

    int handle(int event) override {
        if (event == FL_PUSH) {
            int button = Fl::event_button();
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
    , tagBrowser_(nullptr)
    , verseBrowser_(nullptr)
    , newTagButton_(nullptr)
    , deleteTagButton_(nullptr)
    , renameTagButton_(nullptr) {

    begin();

    filterInput_ = new Fl_Input(X, Y, W, 28);
    filterInput_->when(FL_WHEN_CHANGED);
    filterInput_->callback(onFilterChange, this);
    filterInput_->tooltip("Filter tags by name or by verse reference");

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

    end();

    resizable(verseBrowser_);
    layoutChildren();
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
    auto tags = app_->tagManager().getAllTags();

    if (tags.empty()) {
        const char* name = fl_input("Create a new tag for %s:", "", verseKey.c_str());
        if (name && name[0]) {
            app_->tagManager().createTag(name);
            app_->tagManager().tagVerse(verseKey, name);
            app_->tagManager().save();
            selectedTagName_ = name;
            selectedVerseKey_ = verseKey;
            populateTags();
            refreshBiblePane();
        }
        return;
    }

    const char* choice = fl_input("Add tag to %s:\n(Enter tag name)",
                                   tags[0].name.c_str(), verseKey.c_str());
    if (choice && choice[0]) {
        std::string tagName = choice;
        if (app_->tagManager().getAllTags().empty() ||
            std::none_of(tags.begin(), tags.end(),
                         [&](const Tag& t) { return t.name == tagName; })) {
            app_->tagManager().createTag(tagName);
        }
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
    populateTags();
}

void TagPanel::layoutChildren() {
    if (!filterInput_ || !tagBrowser_ || !verseBrowser_ ||
        !newTagButton_ || !renameTagButton_ || !deleteTagButton_) {
        return;
    }

    const int padding = 2;
    const int filterH = 26;
    const int buttonH = 25;
    const int innerX = x() + padding;
    const int innerW = std::max(20, w() - 2 * padding);
    const int buttonY = y() + h() - buttonH - padding;

    int cy = y() + padding;
    filterInput_->resize(innerX, cy, innerW, filterH);
    cy += filterH + padding;

    int listAreaH = std::max(50, buttonY - cy - padding);
    int tagH = std::max(24, listAreaH / 2);
    int verseH = std::max(24, listAreaH - tagH - padding);

    tagBrowser_->resize(innerX, cy, innerW, tagH);
    cy += tagH + padding;
    verseBrowser_->resize(innerX, cy, innerW, verseH);

    const int buttonW = (innerW - 2 * padding) / 3;
    newTagButton_->resize(innerX, buttonY, buttonW, buttonH);
    renameTagButton_->resize(innerX + buttonW + padding, buttonY, buttonW, buttonH);
    deleteTagButton_->resize(innerX + 2 * (buttonW + padding), buttonY,
                             innerW - 2 * (buttonW + padding), buttonH);
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
    app_->mainWindow()->leftPane()->setPreviewText(html);
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

void TagPanel::onFilterChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self) return;
    self->populateTags();
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

    const char* name = fl_input("New tag name:");
    if (name && name[0]) {
        if (self->app_->tagManager().createTag(name)) {
            self->app_->tagManager().save();
            self->selectedTagName_ = name;
            self->populateTags();
            self->refreshBiblePane();
        } else {
            fl_alert("Tag '%s' already exists.", name);
        }
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
    const char* newName = fl_input("Rename tag '%s' to:", oldName.c_str(),
                                    oldName.c_str());
    if (newName && newName[0] && std::string(newName) != oldName) {
        if (self->app_->tagManager().renameTag(oldName, newName)) {
            self->app_->tagManager().save();
            self->selectedTagName_ = newName;
            self->populateTags();
            self->refreshBiblePane();
        } else {
            fl_alert("Cannot rename: tag '%s' already exists.", newName);
        }
    }
}

} // namespace verdad
