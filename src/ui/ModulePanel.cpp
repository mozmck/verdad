#include "ui/ModulePanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/ModuleChoiceUtils.h"
#include "ui/RightPane.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace verdad {
namespace {

constexpr const char* kBibleGroup = "Biblical Texts";
constexpr const char* kCommentaryGroup = "Commentaries";
constexpr const char* kDictionaryGroup = "Dictionaries";
constexpr const char* kGeneralBookGroup = "General Books";

enum class ModuleGroup {
    Bible,
    Commentary,
    Dictionary,
    GeneralBook,
    Unknown,
};

struct ModuleTreeSelection {
    ModuleGroup group = ModuleGroup::Unknown;
    std::string groupLabel;
    std::string moduleName;

    bool valid() const {
        return group != ModuleGroup::Unknown && !moduleName.empty();
    }
};

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

bool equalsIgnoreCaseAscii(const std::string& lhs,
                           const std::string& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

ModuleGroup moduleGroupFromLabel(const std::string& label) {
    if (label == kBibleGroup) return ModuleGroup::Bible;
    if (label == kCommentaryGroup) return ModuleGroup::Commentary;
    if (label == kDictionaryGroup) return ModuleGroup::Dictionary;
    if (label == kGeneralBookGroup) return ModuleGroup::GeneralBook;
    return ModuleGroup::Unknown;
}

std::string normalizeLanguageCode(const std::string& languageCode) {
    std::string code = trimCopy(languageCode);
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });

    size_t sep = code.find_first_of("-_");
    if (sep != std::string::npos) code = code.substr(0, sep);

    if (code == "eng") return "en";
    if (code == "spa") return "es";
    if (code == "fra" || code == "fre") return "fr";
    if (code == "deu" || code == "ger") return "de";
    if (code == "por") return "pt";
    if (code == "ita") return "it";
    if (code == "rus") return "ru";
    if (code == "nld" || code == "dut") return "nl";
    if (code == "ell" || code == "gre") return "el";
    if (code == "heb" || code == "hbo") return "he";
    if (code == "ara") return "ar";
    if (code == "zho" || code == "chi") return "zh";
    if (code == "lat") return "la";

    return code;
}

std::string languageDisplayName(const std::string& languageCode) {
    std::string code = normalizeLanguageCode(languageCode);
    if (code == "en") return "English";
    if (code == "es") return "Spanish";
    if (code == "fr") return "French";
    if (code == "de") return "German";
    if (code == "pt") return "Portuguese";
    if (code == "it") return "Italian";
    if (code == "ru") return "Russian";
    if (code == "nl") return "Dutch";
    if (code == "el" || code == "grc") return "Greek";
    if (code == "he") return "Hebrew";
    if (code == "la") return "Latin";
    if (code == "ar") return "Arabic";
    if (code == "zh") return "Chinese";

    if (code.empty()) return "";

    std::string label = code;
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return label;
}

std::string formatLanguageLabel(const ModuleInfo& info) {
    const std::string raw = trimCopy(info.language);
    if (raw.empty()) return "";

    const std::string display = languageDisplayName(raw);
    const std::string code = normalizeLanguageCode(raw);
    if (display.empty()) return raw;
    if (code.empty() || equalsIgnoreCaseAscii(display, raw) ||
        equalsIgnoreCaseAscii(display, code)) {
        return display;
    }
    return display + " (" + code + ")";
}

void appendMetaRow(std::ostringstream& html,
                   const char* label,
                   const std::string& value) {
    const std::string trimmed = trimCopy(value);
    if (trimmed.empty()) return;

    html << "<tr><td class=\"label\">" << htmlEscape(label) << "</td><td>"
         << htmlEscape(trimmed) << "</td></tr>";
}

ModuleTreeSelection selectionForItem(
    Fl_Tree_Item* item,
    const std::unordered_map<const Fl_Tree_Item*, std::string>& itemModuleNames) {
    ModuleTreeSelection selection;
    if (!item || item->children() > 0) return selection;

    Fl_Tree_Item* parent = item->parent();
    if (!parent) return selection;

    selection.groupLabel = parent->label() ? parent->label() : "";
    selection.group = moduleGroupFromLabel(selection.groupLabel);
    auto it = itemModuleNames.find(item);
    if (it != itemModuleNames.end()) {
        selection.moduleName = it->second;
    } else {
        selection.moduleName = item->label() ? item->label() : "";
    }
    return selection;
}

std::vector<ModuleInfo> modulesForGroup(VerdadApp* app, ModuleGroup group) {
    if (!app) return {};

    switch (group) {
    case ModuleGroup::Bible:
        return app->swordManager().getBibleModules();
    case ModuleGroup::Commentary:
        return app->swordManager().getCommentaryModules();
    case ModuleGroup::Dictionary:
        return app->swordManager().getDictionaryModules();
    case ModuleGroup::GeneralBook:
        return app->swordManager().getGeneralBookModules();
    case ModuleGroup::Unknown:
        break;
    }

    return {};
}

const ModuleInfo* lookupModuleInfo(VerdadApp* app,
                                   ModuleGroup group,
                                   const std::string& moduleName,
                                   std::vector<ModuleInfo>& modulesOut) {
    modulesOut = modulesForGroup(app, group);
    for (const auto& module : modulesOut) {
        if (module.name == moduleName) {
            return &module;
        }
    }
    return nullptr;
}

std::string buildModulePreviewHtml(const ModuleTreeSelection& selection,
                                   const ModuleInfo* info) {
    std::ostringstream html;
    html << "<div class=\"module-preview\">";
    html << "<h3>" << htmlEscape(selection.moduleName) << "</h3>";

    const std::string description =
        (info && !info->description.empty())
            ? info->description
            : "No description available for this module.";
    html << "<p class=\"module-preview-summary\">"
         << htmlEscape(description) << "</p>";

    html << "<table class=\"module-preview-meta\">";
    appendMetaRow(html, "Type", selection.groupLabel);
    if (info) {
        if (!info->abbreviation.empty() &&
            !equalsIgnoreCaseAscii(trimCopy(info->abbreviation),
                                   trimCopy(selection.moduleName))) {
            appendMetaRow(html, "Abbreviation", info->abbreviation);
        }
        appendMetaRow(html, "Version", info->version);
        appendMetaRow(html, "Markup", info->markup);
        if (!info->category.empty() &&
            !equalsIgnoreCaseAscii(trimCopy(info->category),
                                   trimCopy(selection.groupLabel))) {
            appendMetaRow(html, "Category", info->category);
        }
        appendMetaRow(html, "Language", formatLanguageLabel(*info));
        appendMetaRow(html, "License", info->distributionLicense);
        appendMetaRow(html, "Text source", info->textSource);
    }
    html << "</table>";

    if (info && !info->featureLabels.empty()) {
        html << "<div class=\"module-preview-section\">"
             << "<div class=\"module-preview-section-title\">Features</div>"
             << "<ul>";
        for (const auto& feature : info->featureLabels) {
            html << "<li>" << htmlEscape(feature) << "</li>";
        }
        html << "</ul></div>";
    }

    if (info && !trimCopy(info->aboutHtml).empty()) {
        html << "<div class=\"module-preview-section\">"
             << "<div class=\"module-preview-section-title\">About</div>"
             << "<div class=\"module-preview-about\">"
             << info->aboutHtml
             << "</div></div>";
    }

    html << "</div>";
    return html.str();
}

void previewSelectedModule(VerdadApp* app,
                           MainWindow* mainWindow,
                           const ModuleTreeSelection& selection) {
    if (!app || !mainWindow || !selection.valid()) return;

    std::vector<ModuleInfo> modules;
    const ModuleInfo* infoPtr =
        lookupModuleInfo(app, selection.group, selection.moduleName, modules);

    if (auto* leftPane = mainWindow->leftPane()) {
        leftPane->setPreviewText(buildModulePreviewHtml(selection, infoPtr));
    }
}

void activateSelectedModule(MainWindow* mainWindow,
                            const ModuleTreeSelection& selection) {
    if (!mainWindow || !selection.valid()) return;

    BiblePane* biblePane = mainWindow->biblePane();
    RightPane* rightPane = mainWindow->rightPane();
    if (!biblePane || !rightPane) return;

    switch (selection.group) {
    case ModuleGroup::Bible:
        biblePane->setModule(selection.moduleName);
        break;
    case ModuleGroup::Commentary:
        rightPane->setDocumentsTabActive(false);
        rightPane->setDictionaryTabActive(false);
        rightPane->setCommentaryModule(selection.moduleName, true);
        break;
    case ModuleGroup::Dictionary:
        rightPane->setDictionaryModule(selection.moduleName);
        if (!rightPane->currentDictionaryKey().empty()) {
            rightPane->showDictionaryEntry(selection.moduleName,
                                           rightPane->currentDictionaryKey());
        }
        break;
    case ModuleGroup::GeneralBook:
        rightPane->setGeneralBookModule(selection.moduleName);
        rightPane->setDocumentsTabActive(false);
        rightPane->setDictionaryTabActive(true);
        rightPane->showGeneralBookEntry(selection.moduleName,
                                        rightPane->currentGeneralBookKey());
        break;
    case ModuleGroup::Unknown:
        break;
    }
}

} // namespace

ModulePanel::ModulePanel(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app) {

    begin();

    tree_ = new Fl_Tree(X + 2, Y + 2, W - 4, H - 4);
    tree_->showroot(0);
    tree_->selectmode(FL_TREE_SELECT_SINGLE);
    tree_->item_reselect_mode(FL_TREE_SELECTABLE_ALWAYS);
    tree_->callback(onTreeSelect, this);
    tree_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);

    end();
    resizable(tree_);

    populateTree();
}

ModulePanel::~ModulePanel() = default;

void ModulePanel::refresh() {
    populateTree();
}

void ModulePanel::populateTree() {
    itemModuleNames_.clear();
    tree_->clear();
    tree_->deselect_all();

    // Add modules organized by type
    auto addModuleGroup = [this](const std::string& groupName,
                                  const std::vector<ModuleInfo>& modules) {
        if (modules.empty()) return;

        Fl_Tree_Item* groupItem = tree_->add(groupName.c_str());
        if (!groupItem) return;

        for (const auto& mod : modules) {
            Fl_Tree_Item* item = tree_->add(groupItem,
                                            module_choice::formatLabel(mod).c_str());
            if (item) {
                itemModuleNames_[item] = mod.name;
            }
        }
    };

    addModuleGroup(kBibleGroup, app_->swordManager().getBibleModules());
    addModuleGroup(kCommentaryGroup, app_->swordManager().getCommentaryModules());
    addModuleGroup(kDictionaryGroup, app_->swordManager().getDictionaryModules());
    addModuleGroup(kGeneralBookGroup, app_->swordManager().getGeneralBookModules());

    // Open all top-level groups
    for (Fl_Tree_Item* item = tree_->first(); item; item = tree_->next(item)) {
        if (item->depth() == 0 && item->children() > 0) {
            item->open();
        }
    }

    tree_->redraw();
}

void ModulePanel::onTreeSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<ModulePanel*>(data);
    if (!self || !self->tree_) return;

    ModuleTreeSelection selection =
        selectionForItem(self->tree_->callback_item(), self->itemModuleNames_);
    if (!selection.valid()) return;

    MainWindow* mainWindow = self->app_ ? self->app_->mainWindow() : nullptr;
    Fl_Tree_Reason reason = self->tree_->callback_reason();
    if (reason == FL_TREE_REASON_SELECTED ||
        reason == FL_TREE_REASON_RESELECTED) {
        previewSelectedModule(self->app_, mainWindow, selection);
    }

    if (reason == FL_TREE_REASON_RESELECTED) {
        activateSelectedModule(mainWindow, selection);
    }
}

void ModulePanel::onTreeDoubleClick(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<ModulePanel*>(data);
    if (!self || !self->tree_) return;

    ModuleTreeSelection selection =
        selectionForItem(self->tree_->callback_item(), self->itemModuleNames_);
    if (!selection.valid()) return;

    MainWindow* mainWindow = self->app_ ? self->app_->mainWindow() : nullptr;
    previewSelectedModule(self->app_, mainWindow, selection);
    activateSelectedModule(mainWindow, selection);
}

} // namespace verdad
