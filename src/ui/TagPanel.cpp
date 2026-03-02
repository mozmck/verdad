#include "ui/TagPanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "tags/TagManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <algorithm>

namespace verdad {

TagPanel::TagPanel(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app) {

    begin();

    int padding = 2;
    int buttonH = 25;
    int halfH = (H - buttonH - 4 * padding) / 2;

    // Tag list at top
    tagBrowser_ = new Fl_Browser(X + padding, Y + padding,
                                  W - 2 * padding, halfH);
    tagBrowser_->type(FL_HOLD_BROWSER);
    tagBrowser_->callback(onTagSelect, this);

    // Verses for selected tag
    int vy = Y + padding + halfH + padding;
    verseBrowser_ = new Fl_Browser(X + padding, vy,
                                    W - 2 * padding, halfH);
    verseBrowser_->type(FL_HOLD_BROWSER);
    verseBrowser_->callback(onVerseSelect, this);
    verseBrowser_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);

    // Buttons at bottom
    int by = Y + H - buttonH - padding;
    int buttonW = (W - 4 * padding) / 3;

    newTagButton_ = new Fl_Button(X + padding, by, buttonW, buttonH, "New");
    newTagButton_->callback(onNewTag, this);

    renameTagButton_ = new Fl_Button(X + 2 * padding + buttonW, by,
                                      buttonW, buttonH, "Rename");
    renameTagButton_->callback(onRenameTag, this);

    deleteTagButton_ = new Fl_Button(X + 3 * padding + 2 * buttonW, by,
                                      buttonW, buttonH, "Delete");
    deleteTagButton_->callback(onDeleteTag, this);

    end();

    // Make the verse browser resizable
    resizable(verseBrowser_);

    populateTags();
}

TagPanel::~TagPanel() = default;

void TagPanel::refresh() {
    populateTags();
}

void TagPanel::showAddTagDialog(const std::string& verseKey) {
    // Show dialog with existing tags to choose from, or create new
    auto tags = app_->tagManager().getAllTags();

    if (tags.empty()) {
        const char* name = fl_input("Create a new tag for %s:", "", verseKey.c_str());
        if (name && name[0]) {
            app_->tagManager().createTag(name);
            app_->tagManager().tagVerse(verseKey, name);
            app_->tagManager().save();
            populateTags();
        }
        return;
    }

    // Build choice list
    std::string choices;
    for (size_t i = 0; i < tags.size(); i++) {
        if (i > 0) choices += "|";
        choices += tags[i].name;
    }
    choices += "|+ New Tag";

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
        populateTags();
    }
}

void TagPanel::populateTags() {
    tagBrowser_->clear();
    tagBrowser_->value(0);

    auto tags = app_->tagManager().getAllTags();
    for (const auto& tag : tags) {
        int count = app_->tagManager().getTagCount(tag.name);
        std::string line = tag.name + " (" + std::to_string(count) + ")";
        tagBrowser_->add(line.c_str());
    }
}

void TagPanel::populateVerses(const std::string& tagName) {
    verseBrowser_->clear();
    verseBrowser_->value(0);

    auto verses = app_->tagManager().getVersesWithTag(tagName);
    for (const auto& verse : verses) {
        verseBrowser_->add(verse.c_str());
    }
}

void TagPanel::onTagSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    int idx = self->tagBrowser_->value();
    if (idx <= 0) return;

    auto tags = self->app_->tagManager().getAllTags();
    if (idx - 1 < static_cast<int>(tags.size())) {
        self->populateVerses(tags[idx - 1].name);
    }
}

void TagPanel::onVerseSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    // Handle double-click
    if (Fl::event_clicks() > 0) {
        onVerseDoubleClick(nullptr, data);
    }

    (void)self;
}

void TagPanel::onVerseDoubleClick(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    int idx = self->verseBrowser_->value();
    if (idx <= 0) return;

    const char* text = self->verseBrowser_->text(idx);
    if (text && self->app_->mainWindow()) {
        self->app_->mainWindow()->navigateTo(text);
    }
}

void TagPanel::onNewTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    const char* name = fl_input("New tag name:");
    if (name && name[0]) {
        if (self->app_->tagManager().createTag(name)) {
            self->app_->tagManager().save();
            self->populateTags();
        } else {
            fl_alert("Tag '%s' already exists.", name);
        }
    }
}

void TagPanel::onDeleteTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    int idx = self->tagBrowser_->value();
    if (idx <= 0) return;

    auto tags = self->app_->tagManager().getAllTags();
    if (idx - 1 >= static_cast<int>(tags.size())) return;

    std::string tagName = tags[idx - 1].name;
    int confirm = fl_choice("Delete tag '%s' and remove from all verses?",
                            "Cancel", "Delete", nullptr, tagName.c_str());
    if (confirm == 1) {
        self->app_->tagManager().deleteTag(tagName);
        self->app_->tagManager().save();
        self->populateTags();
        self->verseBrowser_->clear();
    }
}

void TagPanel::onRenameTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);

    int idx = self->tagBrowser_->value();
    if (idx <= 0) return;

    auto tags = self->app_->tagManager().getAllTags();
    if (idx - 1 >= static_cast<int>(tags.size())) return;

    std::string oldName = tags[idx - 1].name;
    const char* newName = fl_input("Rename tag '%s' to:", oldName.c_str(),
                                    oldName.c_str());
    if (newName && newName[0] && std::string(newName) != oldName) {
        if (self->app_->tagManager().renameTag(oldName, newName)) {
            self->app_->tagManager().save();
            self->populateTags();
        } else {
            fl_alert("Cannot rename: tag '%s' already exists.", newName);
        }
    }
}

} // namespace verdad
