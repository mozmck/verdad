#include "ui/RightPane.h"
#include "app/VerdadApp.h"
#include "ui/HtmlWidget.h"
#include "ui/StyledTabs.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>

#include <algorithm>
#include <fstream>

namespace verdad {
namespace {

void layoutTopTabs(int tabsX,
                   int tabsY,
                   int tabsW,
                   int tabsH,
                   Fl_Group* commentaryGroup,
                   Fl_Choice* commentaryChoice,
                   HtmlWidget* commentaryHtml,
                   Fl_Group* generalBooksGroup,
                   Fl_Choice* generalBookChoice,
                   Fl_Input* generalBookKeyInput,
                   Fl_Button* generalBookGoButton,
                   HtmlWidget* generalBookHtml) {
    if (!commentaryGroup || !commentaryChoice || !commentaryHtml ||
        !generalBooksGroup || !generalBookChoice || !generalBookKeyInput ||
        !generalBookGoButton || !generalBookHtml) {
        return;
    }

    const int tabsHeaderH = 25;
    const int choiceH = 25;
    const int goW = 42;

    int clampedTabsW = std::max(20, tabsW);
    int clampedTabsH = std::max(20, tabsH);
    int panelY = tabsY + tabsHeaderH;
    int panelH = std::max(20, clampedTabsH - tabsHeaderH);

    commentaryGroup->resize(tabsX, panelY, clampedTabsW, panelH);
    generalBooksGroup->resize(tabsX, panelY, clampedTabsW, panelH);

    int choiceW = std::max(20, clampedTabsW - 4);
    commentaryChoice->resize(tabsX + 2, panelY + 2, choiceW, choiceH);
    commentaryHtml->resize(tabsX + 2,
                           panelY + choiceH + 4,
                           choiceW,
                           std::max(10, panelH - choiceH - 6));

    generalBookChoice->resize(tabsX + 2, panelY + 2, choiceW, choiceH);

    int keyW = std::max(20, clampedTabsW - 4 - goW - 2);
    generalBookKeyInput->resize(tabsX + 2,
                                panelY + choiceH + 4,
                                keyW,
                                choiceH);
    generalBookGoButton->resize(tabsX + clampedTabsW - goW - 2,
                                panelY + choiceH + 4,
                                goW,
                                choiceH);

    int gbHtmlY = panelY + (choiceH * 2) + 6;
    int gbHtmlH = std::max(10, panelH - (choiceH * 2) - 8);
    generalBookHtml->resize(tabsX + 2,
                            gbHtmlY,
                            choiceW,
                            gbHtmlH);
}

void layoutDictionaryPane(int paneX,
                          int paneY,
                          int paneW,
                          int paneH,
                          Fl_Group* dictionaryPaneGroup,
                          Fl_Choice* dictionaryChoice,
                          HtmlWidget* dictionaryHtml) {
    if (!dictionaryPaneGroup || !dictionaryChoice || !dictionaryHtml) return;

    const int choiceH = 25;

    int clampedPaneW = std::max(20, paneW);
    int clampedPaneH = std::max(20, paneH);

    dictionaryPaneGroup->resize(paneX, paneY, clampedPaneW, clampedPaneH);

    int choiceW = std::max(20, clampedPaneW - 4);
    dictionaryChoice->resize(paneX + 2, paneY + 2, choiceW, choiceH);
    dictionaryHtml->resize(paneX + 2,
                           paneY + choiceH + 4,
                           choiceW,
                           std::max(10, clampedPaneH - choiceH - 6));
}

} // namespace

RightPane::RightPane(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , contentTile_(nullptr)
    , contentResizeBox_(nullptr)
    , tabs_(nullptr)
    , commentaryGroup_(nullptr)
    , commentaryChoice_(nullptr)
    , commentaryHtml_(nullptr)
    , currentCommentary_()
    , currentCommentaryRef_()
    , dictionaryPaneGroup_(nullptr)
    , dictionaryChoice_(nullptr)
    , dictionaryHtml_(nullptr)
    , currentDictionary_()
    , currentDictKey_()
    , generalBooksGroup_(nullptr)
    , generalBookChoice_(nullptr)
    , generalBookKeyInput_(nullptr)
    , generalBookGoButton_(nullptr)
    , generalBookHtml_(nullptr)
    , currentGeneralBook_()
    , currentGeneralBookKey_() {
    box(FL_FLAT_BOX);

    begin();

    const int padding = 2;
    const int minTopH = 100;
    const int minBottomH = 90;
    const int tabsHeaderH = 25;
    const int choiceH = 25;
    const int goW = 42;

    int tileX = X + padding;
    int tileY = Y + padding;
    int tileW = std::max(20, W - 2 * padding);
    int tileH = std::max(minTopH + minBottomH, H - 2 * padding);

    contentTile_ = new Fl_Tile(tileX, tileY, tileW, tileH);
    contentTile_->begin();

    contentResizeBox_ = new Fl_Box(tileX, tileY, tileW, tileH);
    contentResizeBox_->box(FL_NO_BOX);

    int dictInitH = std::clamp(170, minBottomH,
                               std::max(minBottomH, tileH - minTopH));
    int tabsInitH = std::max(minTopH, tileH - dictInitH);

    tabs_ = new StyledTabs(tileX, tileY, tileW, tabsInitH);
    tabs_->begin();

    int panelY = tileY + tabsHeaderH;
    int panelH = std::max(20, tabsInitH - tabsHeaderH);

    commentaryGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "Commentary");
    commentaryGroup_->begin();
    commentaryChoice_ = new Fl_Choice(tileX + 2, panelY + 2, tileW - 4, choiceH);
    commentaryChoice_->callback(onCommentaryModuleChange, this);
    commentaryHtml_ = new HtmlWidget(tileX + 2,
                                     panelY + choiceH + 4,
                                     tileW - 4,
                                     panelH - choiceH - 6);
    commentaryGroup_->end();
    commentaryGroup_->resizable(commentaryHtml_);

    generalBooksGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "General Books");
    generalBooksGroup_->begin();
    generalBookChoice_ = new Fl_Choice(tileX + 2, panelY + 2, tileW - 4, choiceH);
    generalBookChoice_->callback(onGeneralBookModuleChange, this);

    generalBookKeyInput_ = new Fl_Input(tileX + 2,
                                        panelY + choiceH + 4,
                                        tileW - 4 - goW - 2,
                                        choiceH);
    generalBookKeyInput_->when(FL_WHEN_ENTER_KEY);
    generalBookKeyInput_->callback(onGeneralBookKeyInput, this);

    generalBookGoButton_ = new Fl_Button(tileX + tileW - goW - 2,
                                         panelY + choiceH + 4,
                                         goW,
                                         choiceH,
                                         "Go");
    generalBookGoButton_->callback(onGeneralBookGo, this);

    generalBookHtml_ = new HtmlWidget(tileX + 2,
                                      panelY + (choiceH * 2) + 6,
                                      tileW - 4,
                                      panelH - (choiceH * 2) - 8);
    generalBooksGroup_->end();
    generalBooksGroup_->resizable(generalBookHtml_);

    tabs_->end();
    tabs_->value(commentaryGroup_);

    dictionaryPaneGroup_ = new Fl_Group(tileX,
                                        tileY + tabsInitH,
                                        tileW,
                                        tileH - tabsInitH);
    dictionaryPaneGroup_->begin();
    dictionaryChoice_ = new Fl_Choice(tileX + 2,
                                      tileY + tabsInitH + 2,
                                      tileW - 4,
                                      choiceH);
    dictionaryChoice_->callback(onDictionaryModuleChange, this);
    dictionaryHtml_ = new HtmlWidget(tileX + 2,
                                     tileY + tabsInitH + choiceH + 4,
                                     tileW - 4,
                                     (tileH - tabsInitH) - choiceH - 6);
    dictionaryPaneGroup_->end();
    dictionaryPaneGroup_->resizable(dictionaryHtml_);

    contentTile_->end();
    contentTile_->resizable(contentResizeBox_);
    contentTile_->init_sizes();

    end();
    resizable(contentTile_);

    // Load CSS once and apply to all HTML widgets.
    std::string cssFile = app_->getDataDir() + "/master.css";
    std::ifstream cssStream(cssFile);
    if (cssStream.is_open()) {
        std::string css((std::istreambuf_iterator<char>(cssStream)),
                        std::istreambuf_iterator<char>());
        if (!css.empty()) {
            if (commentaryHtml_) commentaryHtml_->setMasterCSS(css);
            if (dictionaryHtml_) dictionaryHtml_->setMasterCSS(css);
            if (generalBookHtml_) generalBookHtml_->setMasterCSS(css);
        }
    }

    populateCommentaryModules();
    populateDictionaryModules();
    populateGeneralBookModules();
}

RightPane::~RightPane() = default;

void RightPane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!contentTile_ || !contentResizeBox_ || !tabs_ || !commentaryGroup_ ||
        !commentaryChoice_ || !commentaryHtml_ || !dictionaryPaneGroup_ ||
        !dictionaryChoice_ || !dictionaryHtml_ || !generalBooksGroup_ ||
        !generalBookChoice_ || !generalBookKeyInput_ ||
        !generalBookGoButton_ || !generalBookHtml_) {
        return;
    }

    const int padding = 2;
    const int minTopH = 100;
    const int minBottomH = 90;

    int tileX = X + padding;
    int tileY = Y + padding;
    int tileW = std::max(20, W - 2 * padding);
    int tileH = std::max(minTopH + minBottomH, H - 2 * padding);

    contentTile_->resize(tileX, tileY, tileW, tileH);
    contentResizeBox_->resize(tileX, tileY, tileW, tileH);

    int oldTabsH = tabs_->h();
    if (oldTabsH <= 0) oldTabsH = tileH - minBottomH;
    int tabsH = std::clamp(oldTabsH,
                           minTopH,
                           std::max(minTopH, tileH - minBottomH));

    tabs_->resize(tileX, tileY, tileW, tabsH);
    layoutTopTabs(tileX,
                  tileY,
                  tileW,
                  tabsH,
                  commentaryGroup_,
                  commentaryChoice_,
                  commentaryHtml_,
                  generalBooksGroup_,
                  generalBookChoice_,
                  generalBookKeyInput_,
                  generalBookGoButton_,
                  generalBookHtml_);

    int dictY = tileY + tabsH;
    int dictH = tileH - tabsH;
    layoutDictionaryPane(tileX,
                         dictY,
                         tileW,
                         dictH,
                         dictionaryPaneGroup_,
                         dictionaryChoice_,
                         dictionaryHtml_);

    contentTile_->init_sizes();

    if (contentTile_) {
        contentTile_->damage(FL_DAMAGE_ALL);
        contentTile_->redraw();
    }
    if (tabs_) {
        tabs_->damage(FL_DAMAGE_ALL);
        tabs_->redraw();
    }
    damage(FL_DAMAGE_ALL);
    redraw();
}

void RightPane::showCommentary(const std::string& reference) {
    if (currentCommentary_.empty()) return;
    showCommentary(currentCommentary_, reference);
}

void RightPane::showCommentary(const std::string& moduleName,
                               const std::string& reference) {
    currentCommentary_ = moduleName;
    currentCommentaryRef_ = reference;

    int verse = 0;
    std::string chapterKey = commentaryChapterKeyForReference(reference, &verse);
    bool haveChapterKey = !chapterKey.empty();

    bool needReload = true;
    if (commentaryHtml_ && haveChapterKey) {
        needReload = (loadedCommentaryModule_ != moduleName ||
                      loadedCommentaryChapterKey_ != chapterKey ||
                      commentaryHtml_->currentHtml().empty());
    }

    if (commentaryHtml_ && needReload) {
        std::string html;
        if (haveChapterKey) {
            std::string cacheKey = moduleName + "|" + chapterKey;
            if (!lookupCommentaryCache(cacheKey, html)) {
                html = app_->swordManager().getCommentaryText(moduleName, reference);
                storeCommentaryCache(cacheKey, html);
            }
            loadedCommentaryModule_ = moduleName;
            loadedCommentaryChapterKey_ = chapterKey;
        } else {
            html = app_->swordManager().getCommentaryText(moduleName, reference);
            loadedCommentaryModule_.clear();
            loadedCommentaryChapterKey_.clear();
        }
        commentaryHtml_->setHtml(html);
    }

    if (commentaryHtml_) {
        if (verse > 0) {
            commentaryHtml_->scrollToAnchor("v" + std::to_string(verse));
        } else {
            commentaryHtml_->scrollToTop();
        }
    }

    if (tabs_) {
        tabs_->value(commentaryGroup_);
    }
}

void RightPane::showDictionaryEntry(const std::string& key) {
    if (currentDictionary_.empty()) {
        if (!key.empty()) {
            char prefix = key[0];
            if (prefix == 'H') {
                auto dicts = app_->swordManager().getDictionaryModules();
                for (const auto& d : dicts) {
                    if (d.name.find("Hebrew") != std::string::npos ||
                        d.name == "StrongsHebrew") {
                        setDictionaryModule(d.name);
                        break;
                    }
                }
            } else if (prefix == 'G') {
                auto dicts = app_->swordManager().getDictionaryModules();
                for (const auto& d : dicts) {
                    if (d.name.find("Greek") != std::string::npos ||
                        d.name == "StrongsGreek") {
                        setDictionaryModule(d.name);
                        break;
                    }
                }
            }
        }
    }

    if (!currentDictionary_.empty()) {
        showDictionaryEntry(currentDictionary_, key);
    }
}

void RightPane::showDictionaryEntry(const std::string& moduleName,
                                    const std::string& key) {
    currentDictionary_ = moduleName;
    currentDictKey_ = key;

    std::string html = app_->swordManager().getDictionaryEntry(moduleName, key);
    if (dictionaryHtml_) {
        dictionaryHtml_->setHtml(html);
    }
}

void RightPane::showGeneralBookEntry(const std::string& key) {
    if (currentGeneralBook_.empty()) {
        auto books = app_->swordManager().getGeneralBookModules();
        if (!books.empty()) {
            setGeneralBookModule(books.front().name);
        }
    }

    if (!currentGeneralBook_.empty()) {
        showGeneralBookEntry(currentGeneralBook_, key);
    }
}

void RightPane::showGeneralBookEntry(const std::string& moduleName,
                                     const std::string& key) {
    currentGeneralBook_ = moduleName;
    currentGeneralBookKey_ = key;

    if (generalBookKeyInput_) {
        generalBookKeyInput_->value(currentGeneralBookKey_.c_str());
    }

    std::string html = app_->swordManager().getGeneralBookEntry(moduleName, key);
    if (generalBookHtml_) {
        generalBookHtml_->setHtml(html);
    }
}

void RightPane::setCommentaryModule(const std::string& moduleName) {
    currentCommentary_ = moduleName;
    loadedCommentaryModule_.clear();
    loadedCommentaryChapterKey_.clear();

    if (!commentaryChoice_) return;
    for (int i = 0; i < commentaryChoice_->size(); i++) {
        const Fl_Menu_Item& item = commentaryChoice_->menu()[i];
        if (item.label() && moduleName == item.label()) {
            commentaryChoice_->value(i);
            break;
        }
    }
}

void RightPane::setDictionaryModule(const std::string& moduleName) {
    currentDictionary_ = moduleName;

    if (!dictionaryChoice_) return;
    for (int i = 0; i < dictionaryChoice_->size(); i++) {
        const Fl_Menu_Item& item = dictionaryChoice_->menu()[i];
        if (item.label() && moduleName == item.label()) {
            dictionaryChoice_->value(i);
            break;
        }
    }
}

void RightPane::setGeneralBookModule(const std::string& moduleName) {
    currentGeneralBook_ = moduleName;

    if (!generalBookChoice_) return;
    for (int i = 0; i < generalBookChoice_->size(); i++) {
        const Fl_Menu_Item& item = generalBookChoice_->menu()[i];
        if (item.label() && moduleName == item.label()) {
            generalBookChoice_->value(i);
            break;
        }
    }
}

bool RightPane::isDictionaryTabActive() const {
    return tabs_ && tabs_->value() == generalBooksGroup_;
}

void RightPane::setDictionaryTabActive(bool dictionaryActive) {
    if (!tabs_) return;
    tabs_->value(dictionaryActive ? generalBooksGroup_ : commentaryGroup_);
    tabs_->redraw();
}

int RightPane::dictionaryPaneHeight() const {
    return dictionaryPaneGroup_ ? dictionaryPaneGroup_->h() : 0;
}

int RightPane::commentaryScrollY() const {
    return commentaryHtml_ ? commentaryHtml_->scrollY() : 0;
}

void RightPane::setCommentaryScrollY(int y) {
    if (commentaryHtml_) {
        commentaryHtml_->setScrollY(y);
    }
}

void RightPane::setDictionaryPaneHeight(int height) {
    if (!contentTile_ || !tabs_ || !commentaryGroup_ || !commentaryChoice_ ||
        !commentaryHtml_ || !generalBooksGroup_ || !generalBookChoice_ ||
        !generalBookKeyInput_ || !generalBookGoButton_ || !generalBookHtml_ ||
        !dictionaryPaneGroup_ || !dictionaryChoice_ || !dictionaryHtml_) {
        return;
    }

    const int minTopH = 100;
    const int minBottomH = 90;

    int tileX = contentTile_->x();
    int tileY = contentTile_->y();
    int tileW = contentTile_->w();
    int tileH = contentTile_->h();
    if (tileH < (minTopH + minBottomH)) return;

    int bottomH = std::clamp(height,
                             minBottomH,
                             std::max(minBottomH, tileH - minTopH));
    int tabsH = tileH - bottomH;

    tabs_->resize(tileX, tileY, tileW, tabsH);
    layoutTopTabs(tileX,
                  tileY,
                  tileW,
                  tabsH,
                  commentaryGroup_,
                  commentaryChoice_,
                  commentaryHtml_,
                  generalBooksGroup_,
                  generalBookChoice_,
                  generalBookKeyInput_,
                  generalBookGoButton_,
                  generalBookHtml_);

    layoutDictionaryPane(tileX,
                         tileY + tabsH,
                         tileW,
                         bottomH,
                         dictionaryPaneGroup_,
                         dictionaryChoice_,
                         dictionaryHtml_);

    contentTile_->init_sizes();
    contentTile_->damage(FL_DAMAGE_ALL);
    contentTile_->redraw();
}

void RightPane::setStudyState(const std::string& commentaryModule,
                              const std::string& commentaryReference,
                              const std::string& dictionaryModule,
                              const std::string& dictionaryKey,
                              const std::string& generalBookModule,
                              const std::string& generalBookKey,
                              bool dictionaryActive) {
    if (!commentaryModule.empty()) {
        setCommentaryModule(commentaryModule);
    }
    if (!dictionaryModule.empty()) {
        setDictionaryModule(dictionaryModule);
    }
    if (!generalBookModule.empty()) {
        setGeneralBookModule(generalBookModule);
    }

    currentCommentaryRef_ = commentaryReference;
    currentDictKey_ = dictionaryKey;
    currentGeneralBookKey_ = generalBookKey;
    if (generalBookKeyInput_) {
        generalBookKeyInput_->value(currentGeneralBookKey_.c_str());
    }

    setDictionaryTabActive(dictionaryActive);
}

RightPane::DisplayBuffer RightPane::captureDisplayBuffer() const {
    DisplayBuffer buf;
    if (commentaryHtml_) {
        buf.commentaryHtml = commentaryHtml_->currentHtml();
        buf.commentaryScrollY = commentaryHtml_->scrollY();
        buf.hasCommentary = !buf.commentaryHtml.empty();
    }
    if (dictionaryHtml_) {
        buf.dictionaryHtml = dictionaryHtml_->currentHtml();
        buf.dictionaryScrollY = dictionaryHtml_->scrollY();
        buf.hasDictionary = !buf.dictionaryHtml.empty();
    }
    if (generalBookHtml_) {
        buf.generalBookHtml = generalBookHtml_->currentHtml();
        buf.generalBookScrollY = generalBookHtml_->scrollY();
        buf.hasGeneralBook = !buf.generalBookHtml.empty();
    }
    return buf;
}

void RightPane::restoreDisplayBuffer(const DisplayBuffer& buffer, bool dictionaryActive) {
    if (commentaryHtml_ && buffer.hasCommentary) {
        commentaryHtml_->setHtml(buffer.commentaryHtml);
        commentaryHtml_->setScrollY(buffer.commentaryScrollY);
    }
    if (dictionaryHtml_ && buffer.hasDictionary) {
        dictionaryHtml_->setHtml(buffer.dictionaryHtml);
        dictionaryHtml_->setScrollY(buffer.dictionaryScrollY);
    }
    if (generalBookHtml_ && buffer.hasGeneralBook) {
        generalBookHtml_->setHtml(buffer.generalBookHtml);
        generalBookHtml_->setScrollY(buffer.generalBookScrollY);
    }
    setDictionaryTabActive(dictionaryActive);
}

void RightPane::redrawChrome() {
    damage(FL_DAMAGE_ALL);
    if (tabs_) {
        tabs_->damage(FL_DAMAGE_ALL);
        tabs_->redraw();
    }
    if (commentaryChoice_) commentaryChoice_->redraw();
    if (dictionaryChoice_) dictionaryChoice_->redraw();
    if (generalBookChoice_) generalBookChoice_->redraw();
    if (generalBookKeyInput_) generalBookKeyInput_->redraw();
    if (generalBookGoButton_) generalBookGoButton_->redraw();
    redraw();
}

void RightPane::refresh() {
    bool keepGeneralBooksTab = isDictionaryTabActive();

    if (!currentCommentary_.empty() && !currentCommentaryRef_.empty()) {
        showCommentary(currentCommentary_, currentCommentaryRef_);
    }
    if (!currentDictionary_.empty() && !currentDictKey_.empty()) {
        showDictionaryEntry(currentDictionary_, currentDictKey_);
    }
    if (!currentGeneralBook_.empty()) {
        showGeneralBookEntry(currentGeneralBook_, currentGeneralBookKey_);
    }

    setDictionaryTabActive(keepGeneralBooksTab);
}

std::string RightPane::commentaryChapterKeyForReference(const std::string& reference,
                                                        int* verseOut) const {
    if (verseOut) *verseOut = 0;

    SwordManager::VerseRef ref;
    try {
        ref = SwordManager::parseVerseRef(reference);
    } catch (...) {
        return "";
    }

    if (ref.book.empty() || ref.chapter <= 0) return "";
    if (verseOut) {
        *verseOut = ref.verse > 0 ? ref.verse : 0;
    }
    return ref.book + " " + std::to_string(ref.chapter);
}

bool RightPane::lookupCommentaryCache(const std::string& cacheKey,
                                      std::string& htmlOut) {
    auto it = commentaryChapterCache_.find(cacheKey);
    if (it == commentaryChapterCache_.end()) return false;
    htmlOut = it->second;

    auto ordIt = std::find(commentaryChapterCacheOrder_.begin(),
                           commentaryChapterCacheOrder_.end(),
                           cacheKey);
    if (ordIt != commentaryChapterCacheOrder_.end()) {
        commentaryChapterCacheOrder_.erase(ordIt);
    }
    commentaryChapterCacheOrder_.push_back(cacheKey);
    return true;
}

void RightPane::storeCommentaryCache(const std::string& cacheKey,
                                     const std::string& html) {
    commentaryChapterCache_[cacheKey] = html;

    auto ordIt = std::find(commentaryChapterCacheOrder_.begin(),
                           commentaryChapterCacheOrder_.end(),
                           cacheKey);
    if (ordIt != commentaryChapterCacheOrder_.end()) {
        commentaryChapterCacheOrder_.erase(ordIt);
    }
    commentaryChapterCacheOrder_.push_back(cacheKey);

    while (commentaryChapterCacheOrder_.size() > kCommentaryChapterCacheLimit) {
        const std::string evict = commentaryChapterCacheOrder_.front();
        commentaryChapterCacheOrder_.pop_front();
        commentaryChapterCache_.erase(evict);
    }
}

void RightPane::setHtmlStyleOverride(const std::string& css) {
    if (commentaryHtml_) commentaryHtml_->setStyleOverrideCss(css);
    if (dictionaryHtml_) dictionaryHtml_->setStyleOverrideCss(css);
    if (generalBookHtml_) generalBookHtml_->setStyleOverrideCss(css);
}

void RightPane::populateCommentaryModules() {
    if (!commentaryChoice_) return;
    commentaryChoice_->clear();

    auto mods = app_->swordManager().getCommentaryModules();
    for (const auto& mod : mods) {
        commentaryChoice_->add(mod.name.c_str());
    }

    if (commentaryChoice_->size() > 0) {
        commentaryChoice_->value(0);
        const Fl_Menu_Item& item = commentaryChoice_->menu()[0];
        if (item.label()) {
            currentCommentary_ = item.label();
        }
    }
}

void RightPane::populateDictionaryModules() {
    if (!dictionaryChoice_) return;
    dictionaryChoice_->clear();

    auto mods = app_->swordManager().getDictionaryModules();
    for (const auto& mod : mods) {
        dictionaryChoice_->add(mod.name.c_str());
    }

    if (dictionaryChoice_->size() > 0) {
        dictionaryChoice_->value(0);
        const Fl_Menu_Item& item = dictionaryChoice_->menu()[0];
        if (item.label()) {
            currentDictionary_ = item.label();
        }
    }
}

void RightPane::populateGeneralBookModules() {
    if (!generalBookChoice_) return;
    generalBookChoice_->clear();

    auto mods = app_->swordManager().getGeneralBookModules();
    for (const auto& mod : mods) {
        generalBookChoice_->add(mod.name.c_str());
    }

    if (generalBookChoice_->size() > 0) {
        generalBookChoice_->value(0);
        const Fl_Menu_Item& item = generalBookChoice_->menu()[0];
        if (item.label()) {
            currentGeneralBook_ = item.label();
        }
        showGeneralBookEntry(currentGeneralBook_, currentGeneralBookKey_);
    } else {
        currentGeneralBook_.clear();
        currentGeneralBookKey_.clear();
        if (generalBookKeyInput_) {
            generalBookKeyInput_->value("");
        }
        if (generalBookHtml_) {
            generalBookHtml_->setHtml(
                "<p><i>No general book modules installed.</i></p>");
        }
    }
}

void RightPane::onCommentaryModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->commentaryChoice_) return;
    const Fl_Menu_Item* item = self->commentaryChoice_->mvalue();
    if (item && item->label()) {
        self->currentCommentary_ = item->label();
        if (!self->currentCommentaryRef_.empty()) {
            self->showCommentary(self->currentCommentary_,
                                 self->currentCommentaryRef_);
        }
    }
}

void RightPane::onDictionaryModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dictionaryChoice_) return;
    const Fl_Menu_Item* item = self->dictionaryChoice_->mvalue();
    if (item && item->label()) {
        self->currentDictionary_ = item->label();
        if (!self->currentDictKey_.empty()) {
            self->showDictionaryEntry(self->currentDictionary_,
                                      self->currentDictKey_);
        }
    }
}

void RightPane::onGeneralBookModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->generalBookChoice_) return;
    const Fl_Menu_Item* item = self->generalBookChoice_->mvalue();
    if (item && item->label()) {
        self->currentGeneralBook_ = item->label();
        self->showGeneralBookEntry(self->currentGeneralBook_,
                                   self->currentGeneralBookKey_);
    }
}

void RightPane::onGeneralBookGo(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || self->currentGeneralBook_.empty() || !self->generalBookKeyInput_) {
        return;
    }

    self->currentGeneralBookKey_ = self->generalBookKeyInput_->value()
                                   ? self->generalBookKeyInput_->value()
                                   : "";
    self->showGeneralBookEntry(self->currentGeneralBook_,
                               self->currentGeneralBookKey_);
}

void RightPane::onGeneralBookKeyInput(Fl_Widget* /*w*/, void* data) {
    onGeneralBookGo(nullptr, data);
}

} // namespace verdad
