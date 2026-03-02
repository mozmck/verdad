#include "ui/ModulePanel.h"
#include "app/VerdadApp.h"
#include "ui/MainWindow.h"
#include "ui/BiblePane.h"
#include "ui/RightPane.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>

namespace verdad {

ModulePanel::ModulePanel(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app) {

    begin();

    tree_ = new Fl_Tree(X + 2, Y + 2, W - 4, H - 4);
    tree_->showroot(0);
    tree_->selectmode(FL_TREE_SELECT_SINGLE);
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
    tree_->clear();
    tree_->deselect_all();

    // Add modules organized by type
    auto addModuleGroup = [this](const std::string& groupName,
                                  const std::vector<ModuleInfo>& modules) {
        if (modules.empty()) return;

        for (const auto& mod : modules) {
            std::string path = groupName + "/" + mod.name;
            Fl_Tree_Item* item = tree_->add(path.c_str());
            if (item) {
                // Store module info as tooltip
                std::string tip = mod.description;
                if (mod.hasStrongs) tip += " [Strong's]";
                if (mod.hasMorph) tip += " [Morph]";
                item->label(mod.name.c_str());
            }
        }
    };

    addModuleGroup("Biblical Texts", app_->swordManager().getBibleModules());
    addModuleGroup("Commentaries", app_->swordManager().getCommentaryModules());
    addModuleGroup("Dictionaries", app_->swordManager().getDictionaryModules());

    // Open all top-level groups
    for (Fl_Tree_Item* item = tree_->first(); item; item = tree_->next(item)) {
        if (item->depth() == 1) {
            item->open();
        }
    }

    tree_->redraw();
}

void ModulePanel::onTreeSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<ModulePanel*>(data);

    Fl_Tree_Item* item = self->tree_->callback_item();
    if (!item || item->children() > 0) return; // Skip group nodes

    // Get the parent group to determine type
    Fl_Tree_Item* parent = item->parent();
    if (!parent) return;

    std::string type = parent->label() ? parent->label() : "";
    std::string modName = item->label() ? item->label() : "";

    if (modName.empty()) return;

    // Handle double-click
    if (Fl::event_clicks() > 0) {
        onTreeDoubleClick(nullptr, data);
        return;
    }
}

void ModulePanel::onTreeDoubleClick(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<ModulePanel*>(data);

    Fl_Tree_Item* item = self->tree_->callback_item();
    if (!item || item->children() > 0) return;

    Fl_Tree_Item* parent = item->parent();
    if (!parent) return;

    std::string type = parent->label() ? parent->label() : "";
    std::string modName = item->label() ? item->label() : "";

    if (self->app_->mainWindow()) {
        if (type == "Biblical Texts") {
            self->app_->mainWindow()->biblePane()->setModule(modName);
        } else if (type == "Commentaries") {
            self->app_->mainWindow()->rightPane()->setCommentaryModule(modName);
        } else if (type == "Dictionaries") {
            self->app_->mainWindow()->rightPane()->setDictionaryModule(modName);
        }
    }
}

} // namespace verdad
