#include "ui/ReadingPlanEditorDialog.h"

#include "app/VerdadApp.h"
#include "reading/ReadingPlanGenerator.h"
#include "reading/ReadingPlanUtils.h"
#include "sword/SwordManager.h"
#include "ui/BiblePane.h"
#include "ui/MainWindow.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>

#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace verdad {
namespace {

bool parseNonNegativeInt(const std::string& text, int& out) {
    std::string trimmed = reading::trimCopy(text);
    if (trimmed.empty()) {
        out = 0;
        return true;
    }

    try {
        out = std::stoi(trimmed);
    } catch (...) {
        return false;
    }
    return out >= 0;
}

bool parseCustomBookRuleLine(const std::string& line,
                             std::string& bookOut,
                             int& repeatCountOut) {
    static const std::regex kPattern(
        R"(^\s*(.+?)(?:\s*(?:x|\*)\s*(\d+))?\s*$)",
        std::regex::icase);

    std::smatch match;
    if (!std::regex_match(line, match, kPattern)) {
        return false;
    }

    bookOut = reading::trimCopy(match[1].str());
    repeatCountOut = 1;
    if (match.size() > 2 && match[2].matched) {
        try {
            repeatCountOut = std::stoi(match[2].str());
        } catch (...) {
            return false;
        }
    }

    return !bookOut.empty() && repeatCountOut > 0;
}

std::string resolvePreferredBibleModule(VerdadApp* app) {
    if (!app) return "";
    if (app->mainWindow() && app->mainWindow()->biblePane()) {
        std::string current = reading::trimCopy(app->mainWindow()->biblePane()->currentModule());
        if (!current.empty()) return current;
    }

    auto modules = app->swordManager().getBibleModules();
    return modules.empty() ? std::string() : modules.front().name;
}

std::string timeframeLabel(ReadingPlanTimeframeKind kind, int value) {
    switch (kind) {
    case ReadingPlanTimeframeKind::Days:
        return std::to_string(value) + " day" + (value == 1 ? "" : "s");
    case ReadingPlanTimeframeKind::Weeks:
        return std::to_string(value) + " week" + (value == 1 ? "" : "s");
    case ReadingPlanTimeframeKind::Months:
        return std::to_string(value) + " month" + (value == 1 ? "" : "s");
    case ReadingPlanTimeframeKind::OneYear:
        return "1 year";
    case ReadingPlanTimeframeKind::TwoYears:
        return "2 years";
    }
    return "";
}

bool timeframeKindNeedsAmount(ReadingPlanTimeframeKind kind) {
    return kind == ReadingPlanTimeframeKind::Days ||
           kind == ReadingPlanTimeframeKind::Weeks ||
           kind == ReadingPlanTimeframeKind::Months;
}

class ReadingPlanCreationController {
public:
    ReadingPlanCreationController(VerdadApp* app, ReadingPlan& plan)
        : app_(app)
        , dialog_(780, 640, "New Reading Plan")
        , workingPlan_(plan) {
        dialog_.set_modal();
        moduleName_ = resolvePreferredBibleModule(app_);
        buildUi();
        loadDefaults();
        updateControls();
        updateSummary();
    }

    bool open() {
        dialog_.show();
        while (dialog_.shown()) {
            Fl::wait();
        }
        return accepted_;
    }

    const ReadingPlan& plan() const { return workingPlan_; }

private:
    VerdadApp* app_ = nullptr;
    Fl_Double_Window dialog_;
    ReadingPlan workingPlan_;
    std::string moduleName_;
    bool accepted_ = false;

    Fl_Input* nameInput_ = nullptr;
    Fl_Multiline_Input* descriptionInput_ = nullptr;
    Fl_Choice* timeframeChoice_ = nullptr;
    Fl_Input* timeframeValueInput_ = nullptr;
    Fl_Choice* splitChoice_ = nullptr;
    Fl_Input* wholeBibleCountInput_ = nullptr;
    Fl_Input* oldTestamentCountInput_ = nullptr;
    Fl_Input* newTestamentCountInput_ = nullptr;
    Fl_Multiline_Input* customBooksInput_ = nullptr;
    Fl_Box* sourceModuleBox_ = nullptr;
    Fl_Box* summaryBox_ = nullptr;

    void buildUi() {
        const int margin = 14;
        const int labelW = 90;
        const int rowH = 26;

        auto* nameLabel = new Fl_Box(margin, 16, labelW, 24, "Name");
        nameLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        nameInput_ = new Fl_Input(margin + labelW, 14, 662, rowH);

        auto* notesLabel = new Fl_Box(margin, 52, labelW, 24, "Notes");
        notesLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        descriptionInput_ = new Fl_Multiline_Input(margin + labelW, 50, 662, 68);

        auto* timeframeLabelBox = new Fl_Box(margin, 132, 110, 24, "Time frame");
        timeframeLabelBox->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        timeframeChoice_ = new Fl_Choice(126, 130, 168, rowH);
        timeframeChoice_->add("Days");
        timeframeChoice_->add("Weeks");
        timeframeChoice_->add("Months");
        timeframeChoice_->add("One year");
        timeframeChoice_->add("Two years");
        timeframeChoice_->callback(onControlChanged, this);

        auto* amountLabel = new Fl_Box(306, 132, 42, 24, "Days");
        amountLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        timeframeValueInput_ = new Fl_Input(352, 130, 80, rowH);
        timeframeValueInput_->callback(onControlChanged, this);
        timeframeValueInput_->when(FL_WHEN_CHANGED);

        auto* splitLabel = new Fl_Box(458, 132, 40, 24, "Split");
        splitLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        splitChoice_ = new Fl_Choice(502, 130, 110, rowH);
        splitChoice_->add("Chapter");
        splitChoice_->add("Verse");
        splitChoice_->callback(onControlChanged, this);

        auto* sourceLabel = new Fl_Box(margin, 166, 100, 24, "Bible source");
        sourceLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        sourceModuleBox_ = new Fl_Box(126, 166, 640, 24, "");
        sourceModuleBox_->box(FL_DOWN_FRAME);
        sourceModuleBox_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        auto* rangeHelp = new Fl_Box(margin, 200, 752, 32,
                                     "Use repeat counts to combine ranges. "
                                     "For example, OT=1 and NT=2 will schedule the Old Testament once and the New Testament twice.");
        rangeHelp->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

        auto* wholeLabel = new Fl_Box(margin, 240, 110, 24, "Whole Bible");
        wholeLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        wholeBibleCountInput_ = new Fl_Input(126, 238, 54, rowH);
        wholeBibleCountInput_->callback(onControlChanged, this);
        wholeBibleCountInput_->when(FL_WHEN_CHANGED);

        auto* otLabel = new Fl_Box(202, 240, 100, 24, "Old Testament");
        otLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        oldTestamentCountInput_ = new Fl_Input(304, 238, 54, rowH);
        oldTestamentCountInput_->callback(onControlChanged, this);
        oldTestamentCountInput_->when(FL_WHEN_CHANGED);

        auto* ntLabel = new Fl_Box(382, 240, 104, 24, "New Testament");
        ntLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        newTestamentCountInput_ = new Fl_Input(488, 238, 54, rowH);
        newTestamentCountInput_->callback(onControlChanged, this);
        newTestamentCountInput_->when(FL_WHEN_CHANGED);

        auto* customLabel = new Fl_Box(margin, 278, 160, 24, "Custom books");
        customLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        customBooksInput_ = new Fl_Multiline_Input(margin, 304, 466, 272);
        customBooksInput_->tooltip("One book per line. Optional repeat suffix: John x5");
        customBooksInput_->callback(onControlChanged, this);
        customBooksInput_->when(FL_WHEN_CHANGED);

        auto* customHelp = new Fl_Box(margin, 580, 466, 38,
                                      "Custom book lines can use plain book names or a repeat suffix.\nExamples: John   or   Romans x3");
        customHelp->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

        summaryBox_ = new Fl_Box(500, 304, 266, 282, "");
        summaryBox_->box(FL_DOWN_FRAME);
        summaryBox_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

        auto* cancelButton = new Fl_Button(dialog_.w() - 270, dialog_.h() - 42, 76, 28, "Cancel");
        auto* blankButton = new Fl_Button(dialog_.w() - 182, dialog_.h() - 42, 80, 28, "Blank");
        auto* okButton = new Fl_Return_Button(dialog_.w() - 96, dialog_.h() - 42, 76, 28, "Create");
        cancelButton->callback(onCancel, this);
        blankButton->callback(onAcceptBlank, this);
        okButton->callback(onAccept, this);

        dialog_.end();
        dialog_.hotspot(okButton);
    }

    void loadDefaults() {
        nameInput_->value(workingPlan_.summary.name.c_str());
        descriptionInput_->value(workingPlan_.summary.description.c_str());

        timeframeChoice_->value(3);
        timeframeValueInput_->value("30");
        splitChoice_->value(0);
        wholeBibleCountInput_->value("1");
        oldTestamentCountInput_->value("0");
        newTestamentCountInput_->value("0");
        customBooksInput_->value("");

        if (moduleName_.empty()) {
            sourceModuleBox_->copy_label("No Bible module available.");
        } else {
            std::string sourceLabel = moduleName_;
            const std::string description =
                reading::trimCopy(app_->swordManager().getModuleDescription(moduleName_));
            if (!description.empty() && description != moduleName_) {
                sourceLabel += " - " + description;
            }
            sourceModuleBox_->copy_label(sourceLabel.c_str());
        }
    }

    ReadingPlanTimeframeKind timeframeKindFromChoice() const {
        switch (timeframeChoice_ ? timeframeChoice_->value() : 3) {
        case 0:
            return ReadingPlanTimeframeKind::Days;
        case 1:
            return ReadingPlanTimeframeKind::Weeks;
        case 2:
            return ReadingPlanTimeframeKind::Months;
        case 3:
            return ReadingPlanTimeframeKind::OneYear;
        case 4:
            return ReadingPlanTimeframeKind::TwoYears;
        default:
            return ReadingPlanTimeframeKind::OneYear;
        }
    }

    void updateControls() {
        const ReadingPlanTimeframeKind kind = timeframeKindFromChoice();
        const bool needsAmount = timeframeKindNeedsAmount(kind);

        if (needsAmount) timeframeValueInput_->activate();
        else timeframeValueInput_->deactivate();
    }

    void updateSummary() {
        int wholeBibleCount = 0;
        int oldTestamentCount = 0;
        int newTestamentCount = 0;
        parseNonNegativeInt(wholeBibleCountInput_->value() ? wholeBibleCountInput_->value() : "",
                            wholeBibleCount);
        parseNonNegativeInt(oldTestamentCountInput_->value() ? oldTestamentCountInput_->value() : "",
                            oldTestamentCount);
        parseNonNegativeInt(newTestamentCountInput_->value() ? newTestamentCountInput_->value() : "",
                            newTestamentCount);

        int customRuleCount = 0;
        for (const auto& line : reading::splitPlanLines(
                 customBooksInput_->value() ? customBooksInput_->value() : "")) {
            std::string book;
            int repeat = 0;
            if (parseCustomBookRuleLine(line, book, repeat)) {
                ++customRuleCount;
            }
        }

        std::ostringstream summary;
        summary << "Generator summary\n\n";
        summary << "Time frame: " << timeframeLabel(timeframeKindFromChoice(),
                                                    parsePreviewAmount())
                << "\n";
        summary << "Split by: " << ((splitChoice_ && splitChoice_->value() == 1) ? "Verse" : "Chapter") << "\n";
        summary << "Whole Bible: " << wholeBibleCount << "\n";
        summary << "Old Testament: " << oldTestamentCount << "\n";
        summary << "New Testament: " << newTestamentCount << "\n";
        summary << "Custom book lines: " << customRuleCount << "\n\n";
        summary << "Custom book examples:\n";
        summary << "John\n";
        summary << "Romans x2\n";
        summary << "1 Samuel x3";
        summaryBox_->copy_label(summary.str().c_str());
    }

    int parsePreviewAmount() const {
        int value = 0;
        parseNonNegativeInt(timeframeValueInput_->value() ? timeframeValueInput_->value() : "",
                            value);
        return value;
    }

    bool buildPlanSummary(ReadingPlan& plan, std::string& errorMessage) const {
        plan.summary.id = workingPlan_.summary.id;
        plan.summary.name = reading::trimCopy(nameInput_->value() ? nameInput_->value() : "");
        plan.summary.description =
            reading::trimCopy(descriptionInput_->value() ? descriptionInput_->value() : "");
        if (plan.summary.name.empty()) {
            errorMessage = "Enter a plan name.";
            return false;
        }
        return true;
    }

    bool buildRequest(ReadingPlanGenerationRequest& request, std::string& errorMessage) const {
        request.moduleName = moduleName_;
        request.name = reading::trimCopy(nameInput_->value() ? nameInput_->value() : "");
        request.description =
            reading::trimCopy(descriptionInput_->value() ? descriptionInput_->value() : "");
        request.timeframeKind = timeframeKindFromChoice();
        request.splitMode =
            (splitChoice_ && splitChoice_->value() == 1)
                ? ReadingPlanSplitMode::Verse
                : ReadingPlanSplitMode::Chapter;

        if (timeframeKindNeedsAmount(request.timeframeKind)) {
            if (!parseNonNegativeInt(
                    timeframeValueInput_->value() ? timeframeValueInput_->value() : "",
                    request.timeframeValue)) {
                errorMessage = "Enter a whole-number value for the selected time frame.";
                return false;
            }
        } else {
            request.timeframeValue = 0;
        }

        int wholeBibleCount = 0;
        int oldTestamentCount = 0;
        int newTestamentCount = 0;
        if (!parseNonNegativeInt(wholeBibleCountInput_->value() ? wholeBibleCountInput_->value() : "",
                                 wholeBibleCount) ||
            !parseNonNegativeInt(oldTestamentCountInput_->value() ? oldTestamentCountInput_->value() : "",
                                 oldTestamentCount) ||
            !parseNonNegativeInt(newTestamentCountInput_->value() ? newTestamentCountInput_->value() : "",
                                 newTestamentCount)) {
            errorMessage = "Range counts must be whole numbers.";
            return false;
        }

        if (wholeBibleCount > 0) {
            request.scopeRules.push_back(
                ReadingPlanScopeRule{ReadingPlanScopeKind::WholeBible, wholeBibleCount, {}});
        }
        if (oldTestamentCount > 0) {
            request.scopeRules.push_back(
                ReadingPlanScopeRule{ReadingPlanScopeKind::OldTestament, oldTestamentCount, {}});
        }
        if (newTestamentCount > 0) {
            request.scopeRules.push_back(
                ReadingPlanScopeRule{ReadingPlanScopeKind::NewTestament, newTestamentCount, {}});
        }

        for (const auto& line : reading::splitPlanLines(
                 customBooksInput_->value() ? customBooksInput_->value() : "")) {
            std::string book;
            int repeatCount = 0;
            if (!parseCustomBookRuleLine(line, book, repeatCount)) {
                errorMessage =
                    "Custom book lines must be a book name with an optional xN suffix.";
                return false;
            }
            request.scopeRules.push_back(
                ReadingPlanScopeRule{ReadingPlanScopeKind::Books, repeatCount, {book}});
        }

        if (request.scopeRules.empty()) {
            errorMessage = "Select at least one range to generate.";
            return false;
        }

        return true;
    }

    bool validateAndCreateBlankPlan() {
        ReadingPlan blankPlan;
        std::string errorMessage;
        if (!buildPlanSummary(blankPlan, errorMessage)) {
            fl_alert("%s", errorMessage.c_str());
            return false;
        }

        ReadingPlanGenerationRequest templateRequest;
        templateRequest.timeframeKind = timeframeKindFromChoice();
        templateRequest.timeframeValue = parsePreviewAmount();

        std::vector<std::string> templateDates;
        if (!buildReadingPlanTemplateDates(templateRequest, templateDates, &errorMessage)) {
            fl_alert("%s", errorMessage.c_str());
            return false;
        }

        blankPlan.summary.startDateIso = defaultReadingPlanDisplayStartDateIso();
        blankPlan.days.reserve(templateDates.size());
        for (size_t i = 0; i < templateDates.size(); ++i) {
            ReadingPlanDay day;
            day.sequenceNumber = static_cast<int>(i) + 1;
            day.dateIso = templateDates[i];
            blankPlan.days.push_back(std::move(day));
        }
        blankPlan.summary.totalDays = static_cast<int>(templateDates.size());
        blankPlan.summary.completedDays = 0;
        workingPlan_ = std::move(blankPlan);
        return true;
    }

    bool validateAndGenerate() {
        if (moduleName_.empty()) {
            fl_alert("No Bible module is available for reading-plan generation.");
            return false;
        }

        ReadingPlanGenerationRequest request;
        std::string errorMessage;
        if (!buildRequest(request, errorMessage)) {
            fl_alert("%s", errorMessage.c_str());
            return false;
        }

        ReadingPlan generated;
        if (!generateReadingPlan(app_->swordManager(), request, generated, &errorMessage)) {
            fl_alert("%s", errorMessage.c_str());
            return false;
        }

        workingPlan_ = std::move(generated);
        return true;
    }

    static void onControlChanged(Fl_Widget*, void* data) {
        auto* self = static_cast<ReadingPlanCreationController*>(data);
        if (!self) return;
        self->updateControls();
        self->updateSummary();
    }

    static void onCancel(Fl_Widget*, void* data) {
        auto* self = static_cast<ReadingPlanCreationController*>(data);
        if (!self) return;
        self->dialog_.hide();
    }

    static void onAccept(Fl_Widget*, void* data) {
        auto* self = static_cast<ReadingPlanCreationController*>(data);
        if (!self) return;
        if (!self->validateAndGenerate()) return;
        self->accepted_ = true;
        self->dialog_.hide();
    }

    static void onAcceptBlank(Fl_Widget*, void* data) {
        auto* self = static_cast<ReadingPlanCreationController*>(data);
        if (!self) return;
        if (!self->validateAndCreateBlankPlan()) return;
        self->accepted_ = true;
        self->dialog_.hide();
    }
};

} // namespace

bool ReadingPlanEditorDialog::createPlan(VerdadApp* app, ReadingPlan& plan) {
    ReadingPlanCreationController controller(app, plan);
    if (!controller.open()) return false;
    plan = controller.plan();
    return true;
}

} // namespace verdad
