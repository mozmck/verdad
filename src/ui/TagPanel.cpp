#include "ui/TagPanel.h"

#include "app/VerdadApp.h"
#include "ui/BiblePane.h"
#include "sword/SwordManager.h"
#include "tags/TagManager.h"
#include "ui/LeftPane.h"
#include "ui/MainWindow.h"
#include "ui/TagColors.h"
#include "ui/UiFontUtils.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <limits>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace verdad {
namespace {

constexpr const char* kTopLevelLabel = "(Top level)";
constexpr size_t kMaxRangePreviewVerses = 60;

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

bool containsNoCase(const std::string& haystack, const std::string& loweredNeedle) {
    return toLowerCopy(haystack).find(loweredNeedle) != std::string::npos;
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

void copyToClipboard(const std::string& text) {
    Fl::copy(text.c_str(), static_cast<int>(text.size()), 0);
    Fl::copy(text.c_str(), static_cast<int>(text.size()), 1);
}

struct TagFilterQuery {
    std::string raw;
    std::string lowered;
    bool hasVerseRef = false;
    VerseRange verseRange;

    bool empty() const { return lowered.empty(); }
};

TagFilterQuery buildTagFilterQuery(const std::string& text) {
    TagFilterQuery query;
    query.raw = trimCopy(text);
    query.lowered = toLowerCopy(query.raw);
    if (!query.raw.empty()) {
        query.hasVerseRef = VerseRange::parse(query.raw, query.verseRange);
    }
    return query;
}

bool targetMatchesFilter(const TagTarget& target,
                         const TagFilterQuery& query,
                         TagPanel::ResourceFilter resourceFilter) {
    if (!matchesResourceFilterKind(target, resourceFilter)) return false;
    if (query.empty()) return true;

    // A reference query matches any verse range (or commentary entry) that
    // overlaps it, so "John 3:16" finds a tag on "John 3:14-18".
    if (query.hasVerseRef && target.kind != TagTarget::Kind::GeneralBook) {
        VerseRange range;
        if (VerseRange::parse(target.sourceKey, range)) {
            if (range.overlaps(query.verseRange)) return true;
            if (target.kind == TagTarget::Kind::Verse) return false;
        }
    }

    if (containsNoCase(targetDisplayLabel(target), query.lowered)) return true;
    if (containsNoCase(target.moduleName, query.lowered)) return true;
    if (containsNoCase(target.sourceKey, query.lowered)) return true;
    if (containsNoCase(target.selectionText, query.lowered)) return true;
    return false;
}

std::string normalizeBookKey(const std::string& in) {
    return VerseRange::normalizeBookKey(in);
}

struct TargetEntry {
    TagTarget target;
    std::string sourceTag;
};

void sortEntriesCanonical(SwordManager& swordMgr,
                          const std::string& moduleName,
                          std::vector<TargetEntry>& entries) {
    if (entries.size() < 2) return;

    std::unordered_map<std::string, int> bookOrder;
    const auto books = swordMgr.getBookNames(moduleName);
    for (size_t i = 0; i < books.size(); ++i) {
        std::string key = normalizeBookKey(books[i]);
        if (!key.empty() && bookOrder.find(key) == bookOrder.end()) {
            bookOrder.emplace(key, static_cast<int>(i));
        }
    }

    struct SortKey {
        bool parsed = false;
        int bookRank = std::numeric_limits<int>::max();
        int startChapter = 0;
        int startVerse = 0;
        int endChapter = 0;
        int endVerse = 0;
        int kindRank = 0;
        std::string label;
    };

    auto keyFor = [&](const TargetEntry& entry) {
        SortKey key;
        key.kindRank = static_cast<int>(entry.target.kind);
        key.label = targetDisplayLabel(entry.target);
        VerseRange range;
        if (entry.target.verseRange(range)) {
            key.parsed = true;
            key.startChapter = range.startChapter;
            key.startVerse = range.startVerse;
            key.endChapter = range.endChapter;
            key.endVerse = range.endVerse;
            auto it = bookOrder.find(range.bookKey);
            if (it != bookOrder.end()) key.bookRank = it->second;
        }
        return key;
    };

    std::vector<std::pair<SortKey, TargetEntry>> keyed;
    keyed.reserve(entries.size());
    for (auto& entry : entries) {
        SortKey key = keyFor(entry);
        keyed.emplace_back(std::move(key), std::move(entry));
    }

    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& lhs, const auto& rhs) {
        const SortKey& a = lhs.first;
        const SortKey& b = rhs.first;
        if (a.parsed != b.parsed) return a.parsed > b.parsed;
        if (a.kindRank != b.kindRank) return a.kindRank < b.kindRank;
        if (a.parsed) {
            auto ta = std::make_tuple(a.bookRank, a.startChapter, a.startVerse,
                                      a.endChapter, a.endVerse);
            auto tb = std::make_tuple(b.bookRank, b.startChapter, b.startVerse,
                                      b.endChapter, b.endVerse);
            if (ta != tb) return ta < tb;
        }
        return a.label < b.label;
    });

    entries.clear();
    entries.reserve(keyed.size());
    for (auto& pair : keyed) {
        entries.push_back(std::move(pair.second));
    }
}

/// Resolve text typed into the Add Tag dialog to a tag name. Exact names and
/// full paths select existing tags; "Parent / Child" creates missing tags
/// along the path. Returns "" when nothing usable was entered.
std::string resolveTagInput(TagManager& tagMgr, const std::string& input) {
    const std::string text = trimCopy(input);
    if (text.empty()) return "";
    if (tagMgr.hasTag(text)) return text;

    for (const auto& tag : tagMgr.getAllTags()) {
        if (tagMgr.tagPath(tag.name) == text || tagMgr.tagPath(tag.name, "/") == text) {
            return tag.name;
        }
    }

    std::vector<std::string> segments;
    std::string segment;
    std::istringstream stream(text);
    while (std::getline(stream, segment, '/')) {
        segment = trimCopy(segment);
        if (!segment.empty()) segments.push_back(segment);
    }
    if (segments.size() <= 1) {
        tagMgr.createTag(text);
        return text;
    }

    std::string parent;
    for (const auto& name : segments) {
        if (!tagMgr.hasTag(name)) {
            tagMgr.createTag(name, "", parent);
        }
        parent = name;
    }
    return parent;
}

std::string pathListLabel(TagManager& tagMgr, const std::string& name) {
    return tagMgr.tagPath(name) + " (" + std::to_string(tagMgr.getTagCount(name)) + ")";
}

class AddTagDialog {
public:
    AddTagDialog(TagManager& tagMgr, const TagTarget& target)
        : tagMgr_(tagMgr)
        , target_(target)
        , dialog_(460, target.isVerse() ? 446 : 390, "Add Tag") {
        allTags_ = tagMgr_.getAllTags();

        dialog_.set_modal();
        dialog_.begin();

        int y = 16;
        prompt_ = new Fl_Box(16, y, dialog_.w() - 32, 44);
        prompt_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        std::string promptText =
            target_.isVerse()
                ? std::string("Select an existing tag or type a new one.\n"
                              "Type \"Parent / Child\" to create a subtag.")
                : "Add tag to " + target_.displayLabel() +
                  ":\nSelect an existing tag or type a new one (\"Parent / Child\" for a subtag).";
        prompt_->copy_label(promptText.c_str());
        y += 66;

        if (target_.isVerse()) {
            referenceInput_ = new Fl_Input(16, y, dialog_.w() - 32, 28,
                                           "Verses (e.g. Genesis 1:1-5):");
            referenceInput_->align(FL_ALIGN_TOP_LEFT);
            referenceInput_->value(target_.sourceKey.c_str());
            y += 56;
        }

        input_ = new Fl_Input(16, y, dialog_.w() - 32, 28, "Tag:");
        input_->align(FL_ALIGN_TOP_LEFT);
        input_->when(FL_WHEN_CHANGED);
        input_->callback(onInputChanged, this);
        y += 48;

        browser_ = new Fl_Hold_Browser(16, y, dialog_.w() - 32,
                                       dialog_.h() - y - 56, "Existing tags:");
        browser_->align(FL_ALIGN_TOP_LEFT);
        browser_->type(FL_HOLD_BROWSER);
        browser_->format_char(0);
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

    /// Returns the typed tag text (possibly a path) and the possibly edited target.
    bool open(std::string& tagText, TagTarget& target) {
        dialog_.show();
        input_->take_focus();
        while (dialog_.shown()) {
            Fl::wait();
        }

        if (!accepted_) return false;
        tagText = resultTagText_;
        target = target_;
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

        std::string text = self->tagMgr_.tagPath(selected);
        self->input_->value(text.c_str());
        self->input_->insert_position(static_cast<int>(text.size()));
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
        if (referenceInput_) {
            std::string ref = trimCopy(referenceInput_->value() ? referenceInput_->value() : "");
            VerseRange range;
            if (!VerseRange::parse(ref, range)) {
                fl_alert("\"%s\" is not a verse reference.\nUse a form like Genesis 1:1, "
                         "Genesis 1:1-5, or Genesis 1:30-2:3.", ref.c_str());
                referenceInput_->take_focus();
                return;
            }
            target_ = TagTarget::verse(ref);
        }

        std::string tagText = trimCopy(input_->value() ? input_->value() : "");
        if (tagText.empty()) {
            tagText = selectedTagName();
        }

        if (tagText.empty()) {
            fl_alert("Enter a tag name or select an existing tag.");
            input_->take_focus();
            return;
        }

        resultTagText_ = tagText;
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
        const std::string exactInput = trimCopy(input_->value() ? input_->value() : "");
        const std::string filter = toLowerCopy(exactInput);
        const std::string currentSelection = selectedTagName();

        browser_->clear();
        visibleTags_.clear();

        std::vector<std::pair<std::string, std::string>> rows;  // path, name
        rows.reserve(allTags_.size());
        for (const auto& tag : allTags_) {
            rows.emplace_back(tagMgr_.tagPath(tag.name), tag.name);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return toLowerCopy(a.first) < toLowerCopy(b.first);
        });

        int selectedLine = 0;
        for (const auto& row : rows) {
            const std::string& path = row.first;
            const std::string& name = row.second;
            if (!filter.empty() && !containsNoCase(path, filter)) {
                continue;
            }

            visibleTags_.push_back(name);
            browser_->add(pathListLabel(tagMgr_, name).c_str());

            if (selectedLine == 0 && !currentSelection.empty() && name == currentSelection) {
                selectedLine = browser_->size();
            }
            if (selectedLine == 0 && !exactInput.empty() &&
                (name == exactInput || path == exactInput)) {
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
    std::string resultTagText_;
    Fl_Double_Window dialog_;
    Fl_Box* prompt_ = nullptr;
    Fl_Input* referenceInput_ = nullptr;
    Fl_Input* input_ = nullptr;
    Fl_Hold_Browser* browser_ = nullptr;
    Fl_Button* cancelButton_ = nullptr;
    Fl_Return_Button* okButton_ = nullptr;
};

/// Dialog for choosing a parent tag, optionally together with a new tag name.
/// Used by "New Tag" and "Move To".
class TagParentDialog {
public:
    TagParentDialog(TagManager& tagMgr,
                    const char* title,
                    const std::string& promptText,
                    bool askName,
                    const std::string& initialParent,
                    const std::set<std::string>& excluded)
        : tagMgr_(tagMgr)
        , excluded_(excluded)
        , initialParent_(initialParent)
        , dialog_(440, askName ? 430 : 380, title) {
        dialog_.set_modal();
        dialog_.begin();

        int y = 16;
        prompt_ = new Fl_Box(16, y, dialog_.w() - 32, 36);
        prompt_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        prompt_->copy_label(promptText.c_str());
        y += 58;

        if (askName) {
            nameInput_ = new Fl_Input(16, y, dialog_.w() - 32, 28, "Name:");
            nameInput_->align(FL_ALIGN_TOP_LEFT);
            y += 50;
        }

        filterInput_ = new Fl_Input(16, y, dialog_.w() - 32, 28, "Parent (type to filter):");
        filterInput_->align(FL_ALIGN_TOP_LEFT);
        filterInput_->when(FL_WHEN_CHANGED);
        filterInput_->callback(onFilterChanged, this);
        y += 32;

        browser_ = new Fl_Hold_Browser(16, y, dialog_.w() - 32, dialog_.h() - y - 52);
        browser_->type(FL_HOLD_BROWSER);
        browser_->format_char(0);

        cancelButton_ = new Fl_Button(dialog_.w() - 180, dialog_.h() - 40, 80, 28, "Cancel");
        cancelButton_->callback(onCancel, this);

        okButton_ = new Fl_Return_Button(dialog_.w() - 92, dialog_.h() - 40, 76, 28, "OK");
        okButton_->callback(onOk, this);

        dialog_.end();
        populate();
        ui_font::applyCurrentAppUiFont(&dialog_);
    }

    bool open(std::string& name, std::string& parent) {
        dialog_.show();
        if (nameInput_) {
            nameInput_->take_focus();
        } else {
            filterInput_->take_focus();
        }
        while (dialog_.shown()) {
            Fl::wait();
        }
        if (!accepted_) return false;
        name = resultName_;
        parent = resultParent_;
        return true;
    }

private:
    static void onFilterChanged(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<TagParentDialog*>(data);
        if (self) self->populate();
    }

    static void onCancel(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<TagParentDialog*>(data);
        if (!self) return;
        self->accepted_ = false;
        self->dialog_.hide();
    }

    static void onOk(Fl_Widget* /*w*/, void* data) {
        auto* self = static_cast<TagParentDialog*>(data);
        if (!self) return;

        if (self->nameInput_) {
            self->resultName_ = trimCopy(self->nameInput_->value() ? self->nameInput_->value() : "");
            if (self->resultName_.empty()) {
                fl_alert("Blank tags are not valid.");
                self->nameInput_->take_focus();
                return;
            }
        }

        int line = self->browser_->value();
        if (line <= 0 || line > static_cast<int>(self->rows_.size())) {
            fl_alert("Select a parent tag, or \"%s\".", kTopLevelLabel);
            return;
        }
        self->resultParent_ = self->rows_[static_cast<size_t>(line - 1)];
        self->accepted_ = true;
        self->dialog_.hide();
    }

    void populate() {
        const std::string filter =
            toLowerCopy(trimCopy(filterInput_->value() ? filterInput_->value() : ""));
        std::string selected = initialParent_;
        int current = browser_->value();
        if (current > 0 && current <= static_cast<int>(rows_.size())) {
            selected = rows_[static_cast<size_t>(current - 1)];
        }

        browser_->clear();
        rows_.clear();

        int selectedLine = 0;
        if (filter.empty()) {
            rows_.push_back("");
            browser_->add(kTopLevelLabel);
            if (selected.empty()) selectedLine = 1;
        }

        std::vector<std::pair<std::string, std::string>> paths;
        for (const auto& tag : tagMgr_.getAllTags()) {
            if (excluded_.count(tag.name)) continue;
            paths.emplace_back(tagMgr_.tagPath(tag.name), tag.name);
        }
        std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
            return toLowerCopy(a.first) < toLowerCopy(b.first);
        });
        for (const auto& path : paths) {
            if (!filter.empty() && !containsNoCase(path.first, filter)) continue;
            rows_.push_back(path.second);
            browser_->add(path.first.c_str());
            if (selectedLine == 0 && path.second == selected) {
                selectedLine = browser_->size();
            }
        }
        if (selectedLine == 0 && !filter.empty() && browser_->size() > 0) {
            selectedLine = 1;
        }
        browser_->value(selectedLine);
        if (selectedLine > 0) browser_->middleline(selectedLine);
    }

    TagManager& tagMgr_;
    std::set<std::string> excluded_;
    std::string initialParent_;
    std::vector<std::string> rows_;  // parallel to browser lines; "" = top level
    bool accepted_ = false;
    std::string resultName_;
    std::string resultParent_;
    Fl_Double_Window dialog_;
    Fl_Box* prompt_ = nullptr;
    Fl_Input* nameInput_ = nullptr;
    Fl_Input* filterInput_ = nullptr;
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

class TagTree : public Fl_Tree {
public:
    TagTree(TagPanel* owner, int X, int Y, int W, int H)
        : Fl_Tree(X, Y, W, H)
        , owner_(owner) {}

    int handle(int event) override {
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
            Fl_Tree_Item* item = find_clicked();
            if (item && owner_ && owner_->itemTagNames_.count(item)) {
                if (!item->is_selected()) select_only(item, 1);
                owner_->showTagContextMenu(Fl::event_x(), Fl::event_y(), true);
            } else if (owner_) {
                owner_->showTagContextMenu(Fl::event_x(), Fl::event_y(), false);
            }
            return 1;
        }
        return Fl_Tree::handle(event);
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
    , tagTree_(nullptr)
    , itemBrowser_(nullptr)
    , newTagButton_(nullptr)
    , renameTagButton_(nullptr)
    , moveTagButton_(nullptr)
    , deleteTagButton_(nullptr)
    , includeSubtagsCheck_(nullptr)
    , removeTagButton_(nullptr) {
    begin();

    filterInput_ = new TagFilterInput(this, X, Y, W, 28);
    filterInput_->when(FL_WHEN_CHANGED);
    filterInput_->callback(onFilterChange, this);
    filterInput_->tooltip("Filter tags by name, item text, or verse reference");

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

    tagTree_ = new TagTree(this, X, Y, W, H);
    tagTree_->showroot(0);
    tagTree_->selectmode(FL_TREE_SELECT_SINGLE);
    tagTree_->callback(onTreeEvent, this);
    tagTree_->when(FL_WHEN_CHANGED);
    tagTree_->tooltip("Right-click for tag actions");

    newTagButton_ = new Fl_Button(X, Y, 10, 10, "New");
    newTagButton_->callback(onNewTag, this);
    newTagButton_->tooltip("Create a tag (optionally under another tag)");

    renameTagButton_ = new Fl_Button(X, Y, 10, 10, "Rename");
    renameTagButton_->callback(onRenameTag, this);

    moveTagButton_ = new Fl_Button(X, Y, 10, 10, "Move");
    moveTagButton_->callback(onMoveTag, this);
    moveTagButton_->tooltip("Move the selected tag under another tag");

    colorTagButton_ = new Fl_Button(X, Y, 10, 10, "Color");
    colorTagButton_->callback(onColorTag, this);
    colorTagButton_->tooltip("Choose the selected tag's marker and highlight color");

    deleteTagButton_ = new Fl_Button(X, Y, 10, 10, "Delete");
    deleteTagButton_->callback(onDeleteTag, this);

    itemBrowser_ = new TagItemBrowser(this, X, Y, W, H);
    itemBrowser_->type(FL_HOLD_BROWSER);
    itemBrowser_->callback(onItemSelect, this);

    includeSubtagsCheck_ = new Fl_Check_Button(X, Y, 10, 10, "Include subtags");
    includeSubtagsCheck_->callback(onIncludeSubtags, this);
    includeSubtagsCheck_->tooltip("Also list items tagged with the selected tag's subtags");

    removeTagButton_ = new Fl_Button(X, Y, 10, 10, "Remove");
    removeTagButton_->callback(onRemoveTag, this);
    removeTagButton_->tooltip("Remove the tag from the selected item");

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
    TagManager& tagMgr = app_->tagManager();
    AddTagDialog dialog(tagMgr, target);
    std::string tagText;
    TagTarget finalTarget = target;
    if (!dialog.open(tagText, finalTarget)) return;

    std::string tagName = resolveTagInput(tagMgr, tagText);
    if (tagName.empty()) return;

    tagMgr.tagTarget(finalTarget, tagName);
    selectedTagName_ = tagName;
    selectedTarget_ = finalTarget;
    hasSelectedTarget_ = true;
    expandAncestors(tagName);
    tagsChanged(finalTarget.isVerse());
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

    // Prefer a tag that directly covers this verse.
    std::vector<Tag> tags = app_ ? app_->tagManager().getTagsForVerse(verseKey)
                                 : std::vector<Tag>{};
    if (!tags.empty() &&
        std::none_of(tags.begin(), tags.end(),
                     [&](const Tag& tag) { return tag.name == selectedTagName_; })) {
        selectedTagName_ = tags.front().name;
    }

    updateFilterControls();
    populateTags();
}

void TagPanel::layoutChildren() {
    if (!filterInput_ || !clearFilterButton_ || !resourceFilterChoice_ ||
        !tagTree_ || !itemBrowser_ ||
        !newTagButton_ || !renameTagButton_ || !moveTagButton_ ||
        !colorTagButton_ || !deleteTagButton_ || !includeSubtagsCheck_ || !removeTagButton_) {
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

    tagTree_->resize(innerX, cy, innerW, tagH);
    cy += tagH + padding;

    const int buttonW = (innerW - 4 * padding) / 5;
    newTagButton_->resize(innerX, cy, buttonW, buttonH);
    renameTagButton_->resize(innerX + (buttonW + padding), cy, buttonW, buttonH);
    moveTagButton_->resize(innerX + 2 * (buttonW + padding), cy, buttonW, buttonH);
    colorTagButton_->resize(innerX + 3 * (buttonW + padding), cy, buttonW, buttonH);
    deleteTagButton_->resize(innerX + 4 * (buttonW + padding), cy,
                             innerW - 4 * (buttonW + padding), buttonH);
    cy += buttonH + padding;

    itemBrowser_->resize(innerX, cy, innerW, itemH);
    cy += itemH + padding;

    const int bottomRowY = std::min(cy, bottomY - buttonH);
    const int checkW = std::min(innerW / 2, 160);
    includeSubtagsCheck_->resize(innerX, bottomRowY, checkW, buttonH);
    removeTagButton_->resize(innerX + checkW + padding, bottomRowY,
                             std::max(20, innerW - checkW - padding), buttonH);
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

        std::string html;
        if (target.isVerseRange()) {
            const auto refs = app_->swordManager().expandVerseReferences(
                module, target.sourceKey, kMaxRangePreviewVerses + 1);
            const size_t shown = std::min(refs.size(), kMaxRangePreviewVerses);
            for (size_t i = 0; i < shown; ++i) {
                html += app_->swordManager().getVerseText(module, refs[i]);
            }
            if (refs.size() > shown) {
                html += "<p><i>Preview limited to the first " +
                        std::to_string(shown) + " verses.</i></p>";
            }
        }
        if (html.empty()) {
            html = app_->swordManager().getVerseText(module, target.sourceKey);
        }
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

void TagPanel::tagsChanged(bool refreshBible) {
    if (!app_) return;
    app_->tagManager().save();
    populateTags();
    refreshPreviewForSelection();

    if (refreshBible && app_->mainWindow()) {
        app_->mainWindow()->refreshTagDecorations(false);
    }
}

std::string TagPanel::defaultTagColor() const {
    std::string color = app_ ? tag_colors::normalizeHex(
                                   app_->optionDisplaySettings().defaultTagColor)
                             : std::string();
    return color.empty() ? tag_colors::kInitialDefaultMarkerColor : color;
}

Fl_Image* TagPanel::swatchIcon(const std::string& tagColor) {
    // Tags with their own color get a filled square; tags on the default
    // color get an outline in the default color.
    std::string hex = tag_colors::normalizeHex(tagColor);
    const bool filled = !hex.empty();
    if (!filled) hex = defaultTagColor();
    const std::string key = (filled ? "f" : "o") + hex;

    auto it = swatchIcons_.find(key);
    if (it != swatchIcons_.end()) return it->second.get();

    tag_colors::Rgb color;
    tag_colors::parseHex(hex, color);
    const tag_colors::Rgb border = filled
        ? tag_colors::blend(color, tag_colors::Rgb{0, 0, 0}, 0.35)
        : color;

    constexpr int kSize = 12;
    auto* data = new uchar[kSize * kSize * 4];
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            uchar* px = data + (y * kSize + x) * 4;
            const bool edge = x == 0 || y == 0 || x == kSize - 1 || y == kSize - 1;
            const bool innerEdge = !filled &&
                (x == 1 || y == 1 || x == kSize - 2 || y == kSize - 2);
            const tag_colors::Rgb& c = (edge || innerEdge) ? border : color;
            px[0] = static_cast<uchar>(c.r);
            px[1] = static_cast<uchar>(c.g);
            px[2] = static_cast<uchar>(c.b);
            px[3] = (edge || innerEdge || filled) ? 255 : 0;
        }
    }
    auto image = std::make_unique<Fl_RGB_Image>(data, kSize, kSize, 4);
    image->alloc_array = 1;
    Fl_Image* result = image.get();
    swatchIcons_.emplace(key, std::move(image));
    return result;
}

void TagPanel::expandAncestors(const std::string& tagName) {
    if (!app_) return;
    std::string current = app_->tagManager().parentOf(tagName);
    size_t guard = 0;
    while (!current.empty() && guard++ < 1000) {
        expandedTags_.insert(current);
        current = app_->tagManager().parentOf(current);
    }
}

std::string TagPanel::tagNameForItem(const Fl_Tree_Item* item) const {
    auto it = itemTagNames_.find(item);
    return it != itemTagNames_.end() ? it->second : std::string();
}

std::string TagPanel::treeLabelForTag(const std::string& tagName) const {
    const TagManager& tagMgr = app_->tagManager();
    const int own = tagMgr.getTagCount(tagName);
    std::string label = tagName + " (";
    if (tagMgr.hasChildren(tagName)) {
        const int total = tagMgr.getTagCount(tagName, true);
        if (total != own) {
            if (own > 0) label += std::to_string(own) + ", ";
            label += std::to_string(total) + " total)";
            return label;
        }
    }
    label += std::to_string(own) + ")";
    return label;
}

void TagPanel::populateTags() {
    if (!tagTree_ || !itemBrowser_ || !app_) return;

    applyResourceFilterFromChoice();
    TagManager& tagMgr = app_->tagManager();
    const TagFilterQuery filter = buildTagFilterQuery(
        filterInput_ ? filterInput_->value() : "");
    filterActive_ = !filter.empty() || selectedResourceFilter_ != ResourceFilter::All;

    // Decide which nodes are visible. A node is shown when it matches the
    // filter itself, when a descendant matches (shown dimmed so the match has
    // context), or when an ancestor's name matched.
    struct NodeInfo {
        bool visible = false;
        bool selfMatch = false;
        bool subtreeResourceOk = false;
    };
    std::unordered_map<std::string, NodeInfo> nodes;
    std::function<NodeInfo(const std::string&, bool)> evaluate =
        [&](const std::string& name, bool ancestorNameMatched) {
        NodeInfo node;
        const bool nameMatch = !filter.empty() && containsNoCase(name, filter.lowered);

        bool anyChildVisible = false;
        bool childResourceOk = false;
        for (const auto& child : tagMgr.childrenOf(name)) {
            NodeInfo childInfo = evaluate(child, ancestorNameMatched || nameMatch);
            anyChildVisible = anyChildVisible || childInfo.visible;
            childResourceOk = childResourceOk || childInfo.subtreeResourceOk;
        }

        const auto targets = tagMgr.getTargetsWithTag(name);
        const bool ownResourceOk =
            selectedResourceFilter_ == ResourceFilter::All ||
            std::any_of(targets.begin(), targets.end(), [&](const TagTarget& target) {
                return matchesResourceFilterKind(target, selectedResourceFilter_);
            });
        node.subtreeResourceOk = ownResourceOk || childResourceOk;

        if (filter.empty()) {
            node.selfMatch = ownResourceOk;
        } else if (nameMatch) {
            node.selfMatch = node.subtreeResourceOk;
        } else {
            node.selfMatch = std::any_of(targets.begin(), targets.end(),
                                         [&](const TagTarget& target) {
                return targetMatchesFilter(target, filter, selectedResourceFilter_);
            });
        }
        node.visible = node.selfMatch || anyChildVisible ||
                       (ancestorNameMatched && node.subtreeResourceOk);
        nodes[name] = node;
        return node;
    };
    const std::vector<std::string> roots = tagMgr.childrenOf("");
    for (const auto& root : roots) {
        evaluate(root, false);
    }

    populatingTree_ = true;
    const int scrollPos = tagTree_->vposition();
    if (tagTree_->root()) tagTree_->clear_children(tagTree_->root());
    itemTagNames_.clear();

    std::unordered_map<std::string, Fl_Tree_Item*> itemsByName;
    std::vector<Fl_Tree_Item*> orderedItems;
    const Fl_Color dimColor = fl_inactive(tagTree_->item_labelfgcolor());
    std::function<void(Fl_Tree_Item*, const std::string&)> addNode =
        [&](Fl_Tree_Item* parentItem, const std::string& name) {
        const NodeInfo& node = nodes[name];
        if (!node.visible) return;

        const std::string label = treeLabelForTag(name);
        Fl_Tree_Item* item = tagTree_->add(parentItem, label.c_str());
        if (!item) return;
        if (filterActive_ && !node.selfMatch) item->labelfgcolor(dimColor);
        Tag tag;
        if (tagMgr.getTag(name, tag)) item->usericon(swatchIcon(tag.color));
        itemTagNames_[item] = name;
        itemsByName[name] = item;
        orderedItems.push_back(item);

        for (const auto& child : tagMgr.childrenOf(name)) {
            addNode(item, child);
        }
        if (item->has_children()) {
            if (filterActive_ || expandedTags_.count(name)) {
                item->open();
            } else {
                item->close();
            }
        }
    };
    for (const auto& root : roots) {
        addNode(tagTree_->root(), root);
    }

    // Keep the current tag selected when it is still a match; otherwise pick
    // the first matching tag.
    Fl_Tree_Item* selectedItem = nullptr;
    auto isSelectable = [&](const std::string& name) {
        auto it = nodes.find(name);
        return it != nodes.end() && it->second.visible &&
               (!filterActive_ || it->second.selfMatch);
    };
    if (!selectedTagName_.empty() && itemsByName.count(selectedTagName_) &&
        isSelectable(selectedTagName_)) {
        selectedItem = itemsByName[selectedTagName_];
    }
    if (!selectedItem) {
        for (Fl_Tree_Item* item : orderedItems) {
            if (isSelectable(itemTagNames_[item])) {
                selectedItem = item;
                break;
            }
        }
    }
    if (!selectedItem && !orderedItems.empty() && !filterActive_) {
        selectedItem = orderedItems.front();
    }

    if (selectedItem) {
        for (Fl_Tree_Item* parent = selectedItem->parent();
             parent && parent != tagTree_->root();
             parent = parent->parent()) {
            if (!parent->is_open()) {
                parent->open();
                if (!filterActive_) expandedTags_.insert(tagNameForItem(parent));
            }
        }
    }

    // Item positions are only computed while drawing; compute them now so
    // vposition()/show_item() work against the rebuilt tree.
    tagTree_->calc_dimensions();
    tagTree_->calc_tree();
    tagTree_->vposition(scrollPos);
    if (selectedItem) {
        tagTree_->select_only(selectedItem, 0);
        tagTree_->set_item_focus(selectedItem);
        tagTree_->show_item(selectedItem);
        selectedTagName_ = itemTagNames_[selectedItem];
    } else {
        tagTree_->deselect_all(nullptr, 0);
    }
    populatingTree_ = false;
    tagTree_->redraw();

    if (selectedItem) {
        populateTargets(selectedTagName_);
    } else {
        selectedTagName_.clear();
        itemBrowser_->clear();
        visibleTargets_.clear();
        visibleTargetTags_.clear();
        hasSelectedTarget_ = false;
    }
}

void TagPanel::populateTargets(const std::string& tagName) {
    if (!itemBrowser_ || !app_) return;

    itemBrowser_->clear();
    itemBrowser_->value(0);
    visibleTargets_.clear();
    visibleTargetTags_.clear();
    if (tagName.empty()) {
        hasSelectedTarget_ = false;
        return;
    }

    TagManager& tagMgr = app_->tagManager();
    std::vector<std::string> sourceTags{tagName};
    if (includeSubtags_) {
        const auto descendants = tagMgr.descendantsOf(tagName);
        sourceTags.insert(sourceTags.end(), descendants.begin(), descendants.end());
    }

    std::vector<TargetEntry> entries;
    std::set<std::string> seen;
    for (const auto& source : sourceTags) {
        for (auto& target : tagMgr.getTargetsWithTag(source)) {
            if (!seen.insert(target.identityKey()).second) continue;
            entries.push_back(TargetEntry{std::move(target), source});
        }
    }
    sortEntriesCanonical(app_->swordManager(), activeBibleModule(), entries);

    const TagFilterQuery filter = filterTargetsByText_
        ? buildTagFilterQuery(filterInput_ ? filterInput_->value() : "")
        : TagFilterQuery{};
    const TagFilterQuery noTextFilter;

    // When the filter text matched a tag's name (or an ancestor's), show all of
    // that tag's items instead of filtering them by the same text.
    auto pathMatchesFilter = [&](const std::string& name) {
        if (filter.empty()) return false;
        std::string current = name;
        size_t guard = 0;
        while (!current.empty() && guard++ < 1000) {
            if (containsNoCase(current, filter.lowered)) return true;
            current = tagMgr.parentOf(current);
        }
        return false;
    };

    int selectedLine = 0;
    for (const auto& entry : entries) {
        const TagFilterQuery& query =
            pathMatchesFilter(entry.sourceTag) ? noTextFilter : filter;
        if (!targetMatchesFilter(entry.target, query, selectedResourceFilter_)) continue;

        visibleTargets_.push_back(entry.target);
        visibleTargetTags_.push_back(entry.sourceTag);
        std::string line = targetDisplayLabel(entry.target);
        if (entry.sourceTag != tagName) {
            line += "   [" + entry.sourceTag + "]";
        }
        itemBrowser_->add(line.c_str());

        if (hasSelectedTarget_ && targetEquals(entry.target, selectedTarget_)) {
            selectedLine = itemBrowser_->size();
        }
    }

    if (selectedLine == 0 && !visibleTargets_.empty()) {
        selectedLine = 1;
    }

    if (selectedLine > 0) {
        itemBrowser_->value(selectedLine);
        itemBrowser_->middleline(selectedLine);
        selectedTarget_ = visibleTargets_[selectedLine - 1];
        hasSelectedTarget_ = true;
        refreshPreviewForSelection();
    } else {
        hasSelectedTarget_ = false;
    }
}

void TagPanel::showItemContextMenu(int screenX, int screenY) {
    if (!app_ || !itemBrowser_ || itemBrowser_->size() <= 0) return;

    int line = itemBrowser_->value();
    if (line <= 0 || line > static_cast<int>(visibleTargets_.size())) return;

    const TagTarget& target = visibleTargets_[static_cast<size_t>(line - 1)];
    Fl_Menu_Button menu(screenX, screenY, 0, 0);
    ui_font::applyCurrentAppMenuFont(&menu);

    menu.add("Copy Item Label", 0, [](Fl_Widget*, void* data) {
        auto* self = static_cast<TagPanel*>(data);
        if (!self || !self->hasSelectedTarget_) return;
        copyToClipboard(targetDisplayLabel(self->selectedTarget_));
    }, this);

    if (target.kind == TagTarget::Kind::Verse) {
        menu.add("Copy Verse Reference", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (!self || !self->hasSelectedTarget_) return;
            copyToClipboard(self->selectedTarget_.sourceKey);
        }, this);

        menu.add("Edit Verse Range...", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (self) self->editSelectedVerseRange();
        }, this);
    }

    menu.add("Remove from Tag", 0, [](Fl_Widget*, void* data) {
        onRemoveTag(nullptr, data);
    }, this);

    menu.popup();
}

void TagPanel::showTagContextMenu(int screenX, int screenY, bool onItem) {
    if (!app_) return;

    Fl_Menu_Button menu(screenX, screenY, 0, 0);
    ui_font::applyCurrentAppMenuFont(&menu);

    const bool hasTag = onItem && !selectedTagName_.empty();
    if (hasTag) {
        menu.add("New Subtag...", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (self) self->createTagUnder(self->selectedTagName_);
        }, this);
    }
    menu.add("New Top-Level Tag...", 0, [](Fl_Widget*, void* data) {
        auto* self = static_cast<TagPanel*>(data);
        if (self) self->createTagUnder("");
    }, this, hasTag ? FL_MENU_DIVIDER : 0);

    if (hasTag) {
        menu.add("Rename...", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (self) self->renameSelectedTag();
        }, this);
        menu.add("Move To...", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (self) self->moveSelectedTag();
        }, this);
        menu.add("Set Color...", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (self) self->setSelectedTagColor();
        }, this);
        menu.add("Copy Tag Path", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (!self || !self->app_ || self->selectedTagName_.empty()) return;
            copyToClipboard(self->app_->tagManager().tagPath(self->selectedTagName_));
        }, this, FL_MENU_DIVIDER);
    }

    menu.add("Expand All", 0, [](Fl_Widget*, void* data) {
        auto* self = static_cast<TagPanel*>(data);
        if (self) self->setAllExpanded(true);
    }, this);
    menu.add("Collapse All", 0, [](Fl_Widget*, void* data) {
        auto* self = static_cast<TagPanel*>(data);
        if (self) self->setAllExpanded(false);
    }, this, hasTag ? FL_MENU_DIVIDER : 0);

    if (hasTag) {
        menu.add("Delete...", 0, [](Fl_Widget*, void* data) {
            auto* self = static_cast<TagPanel*>(data);
            if (self) self->deleteSelectedTag();
        }, this);
    }

    menu.popup();
}

void TagPanel::setAllExpanded(bool expanded) {
    if (!app_) return;
    if (expanded) {
        for (const auto& tag : app_->tagManager().getAllTags()) {
            if (app_->tagManager().hasChildren(tag.name)) expandedTags_.insert(tag.name);
        }
    } else {
        expandedTags_.clear();
    }
    populateTags();
}

void TagPanel::createTagUnder(const std::string& parentName) {
    if (!app_) return;
    TagManager& tagMgr = app_->tagManager();

    std::string prompt = parentName.empty()
        ? std::string("Create a new tag. Choose a parent to make it a subtag.")
        : "Create a new subtag of \"" + tagMgr.tagPath(parentName) + "\".";
    TagParentDialog dialog(tagMgr, "New Tag", prompt, true, parentName, {});
    std::string name;
    std::string parent;
    if (!dialog.open(name, parent)) return;

    if (tagMgr.hasTag(name)) {
        fl_alert("Tag '%s' already exists.", name.c_str());
        return;
    }

    if (!tagMgr.createTag(name, "", parent)) {
        fl_alert("Could not create tag '%s'.", name.c_str());
        return;
    }

    // A new tag has no items yet, so any active filter would hide it.
    if (filterInput_) filterInput_->value("");
    if (resourceFilterChoice_) resourceFilterChoice_->value(0);
    filterTargetsByText_ = true;
    updateFilterControls();

    selectedTagName_ = name;
    hasSelectedTarget_ = false;
    expandAncestors(name);
    tagsChanged(false);
}

void TagPanel::renameSelectedTag() {
    if (!app_ || selectedTagName_.empty()) return;

    const std::string oldName = selectedTagName_;
    const char* rawNewName = fl_input("Rename tag '%s' to:", oldName.c_str(),
                                      oldName.c_str());
    if (!rawNewName) return;

    std::string newName = trimCopy(rawNewName);
    if (newName.empty()) {
        fl_alert("Blank tags are not valid.");
        return;
    }
    if (newName == oldName) return;

    if (app_->tagManager().renameTag(oldName, newName)) {
        if (expandedTags_.erase(oldName)) expandedTags_.insert(newName);
        selectedTagName_ = newName;
        tagsChanged(true);
    } else {
        fl_alert("Cannot rename: tag '%s' already exists.", newName.c_str());
    }
}

void TagPanel::moveSelectedTag() {
    if (!app_ || selectedTagName_.empty()) return;
    TagManager& tagMgr = app_->tagManager();

    const std::string name = selectedTagName_;
    std::set<std::string> excluded{name};
    for (const auto& descendant : tagMgr.descendantsOf(name)) {
        excluded.insert(descendant);
    }

    TagParentDialog dialog(tagMgr, "Move Tag",
                           "Move \"" + tagMgr.tagPath(name) + "\" under:",
                           false, tagMgr.parentOf(name), excluded);
    std::string unusedName;
    std::string parent;
    if (!dialog.open(unusedName, parent)) return;

    if (!tagMgr.setParent(name, parent)) {
        fl_alert("Cannot move '%s' there.", name.c_str());
        return;
    }
    expandAncestors(name);
    tagsChanged(true);
}

void TagPanel::deleteSelectedTag() {
    if (!app_ || selectedTagName_.empty()) return;
    TagManager& tagMgr = app_->tagManager();

    const std::string tagName = selectedTagName_;
    const std::string parent = tagMgr.parentOf(tagName);
    bool deleteDescendants = false;

    const auto descendants = tagMgr.descendantsOf(tagName);
    if (!descendants.empty()) {
        std::string message =
            "Delete tag '" + tagName + "' and remove it from all items?\n\n"
            "It has " + std::to_string(descendants.size()) +
            (descendants.size() == 1 ? " subtag." : " subtags.") +
            " Keep them (moved up one level) or delete them too?";
        int choice = fl_choice("%s", "Cancel", "Keep Subtags", "Delete All",
                               message.c_str());
        if (choice == 0) return;
        deleteDescendants = choice == 2;
    } else {
        int confirm = fl_choice("Delete tag '%s' and remove it from all items?",
                                "Cancel", "Delete", nullptr, tagName.c_str());
        if (confirm != 1) return;
    }

    tagMgr.deleteTag(tagName, deleteDescendants);
    expandedTags_.erase(tagName);
    selectedTagName_ = parent;
    visibleTargets_.clear();
    visibleTargetTags_.clear();
    hasSelectedTarget_ = false;
    tagsChanged(true);
}

void TagPanel::setSelectedTagColor() {
    if (!app_ || selectedTagName_.empty()) return;
    TagManager& tagMgr = app_->tagManager();

    Tag tag;
    if (!tagMgr.getTag(selectedTagName_, tag)) return;

    tag_colors::PickerOptions options;
    options.title = "Color for " + tag.name;
    options.emptyChoiceLabel = "Default";
    options.emptyChoicePreview = defaultTagColor();

    std::string color = tag_colors::normalizeHex(tag.color);
    if (!tag_colors::chooseColor(color, options)) return;
    if (color == tag_colors::normalizeHex(tag.color)) return;

    tagMgr.setTagColor(tag.name, color);
    tagsChanged(true);
}

void TagPanel::editSelectedVerseRange() {
    if (!app_ || !itemBrowser_) return;
    int line = itemBrowser_->value();
    if (line <= 0 || line > static_cast<int>(visibleTargets_.size())) return;

    const TagTarget oldTarget = visibleTargets_[static_cast<size_t>(line - 1)];
    const std::string sourceTag = visibleTargetTags_[static_cast<size_t>(line - 1)];
    if (!oldTarget.isVerse()) return;

    const char* raw = fl_input("Verses tagged '%s' (e.g. Genesis 1:1-5):",
                               oldTarget.sourceKey.c_str(), sourceTag.c_str());
    if (!raw) return;

    std::string ref = trimCopy(raw);
    VerseRange range;
    if (!VerseRange::parse(ref, range)) {
        fl_alert("\"%s\" is not a verse reference.", ref.c_str());
        return;
    }

    TagTarget newTarget = TagTarget::verse(ref);
    if (targetEquals(newTarget, oldTarget)) return;

    TagManager& tagMgr = app_->tagManager();
    tagMgr.untagTarget(oldTarget, sourceTag);
    tagMgr.tagTarget(newTarget, sourceTag);
    selectedTarget_ = newTarget;
    hasSelectedTarget_ = true;
    tagsChanged(true);
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

void TagPanel::onTreeEvent(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->tagTree_ || self->populatingTree_) return;

    Fl_Tree_Item* item = self->tagTree_->callback_item();
    const std::string name = self->tagNameForItem(item);
    if (name.empty()) return;

    switch (self->tagTree_->callback_reason()) {
    case FL_TREE_REASON_OPENED:
        if (!self->filterActive_) self->expandedTags_.insert(name);
        break;
    case FL_TREE_REASON_CLOSED:
        if (!self->filterActive_) self->expandedTags_.erase(name);
        break;
    case FL_TREE_REASON_SELECTED:
    case FL_TREE_REASON_RESELECTED:
        if (name != self->selectedTagName_ || self->visibleTargets_.empty()) {
            self->selectedTagName_ = name;
            self->populateTargets(name);
        }
        break;
    default:
        break;
    }
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
    if (!self) return;
    self->createTagUnder("");
}

void TagPanel::onDeleteTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (self) self->deleteSelectedTag();
}

void TagPanel::onRenameTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (self) self->renameSelectedTag();
}

void TagPanel::onMoveTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (self) self->moveSelectedTag();
}

void TagPanel::onColorTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (self) self->setSelectedTagColor();
}

void TagPanel::onIncludeSubtags(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->includeSubtagsCheck_) return;
    self->includeSubtags_ = self->includeSubtagsCheck_->value() != 0;
    self->populateTargets(self->selectedTagName_);
}

void TagPanel::onRemoveTag(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<TagPanel*>(data);
    if (!self || !self->app_ || !self->hasSelectedTarget_ || !self->itemBrowser_) return;

    int idx = self->itemBrowser_->value();
    if (idx <= 0 || idx > static_cast<int>(self->visibleTargets_.size())) return;

    const TagTarget target = self->visibleTargets_[static_cast<size_t>(idx - 1)];
    const std::string sourceTag = self->visibleTargetTags_[static_cast<size_t>(idx - 1)];
    if (sourceTag.empty()) return;

    self->app_->tagManager().untagTarget(target, sourceTag);
    self->hasSelectedTarget_ = false;
    self->tagsChanged(target.isVerse());
}

} // namespace verdad
