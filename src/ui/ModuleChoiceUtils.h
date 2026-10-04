#ifndef VERDAD_MODULE_CHOICE_UTILS_H
#define VERDAD_MODULE_CHOICE_UTILS_H

#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Item.H>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "sword/SwordManager.h"

namespace verdad {
namespace module_choice {

inline std::string trimCopy(const std::string& text) {
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

inline bool equalsIgnoreCaseAscii(const std::string& lhs,
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

inline std::string formatLabel(const ModuleInfo& module) {
    std::string description = trimCopy(module.description);
    if (description.empty() ||
        equalsIgnoreCaseAscii(description, module.name)) {
        return module.name;
    }

    return module.name + ": " + description;
}

inline std::string escapeMenuLabel(const std::string& label) {
    std::string escaped;
    escaped.reserve(label.size() + 8);
    for (char c : label) {
        if (c == '\\' || c == '/') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

/// Build a menu label that previews arbitrary user text (e.g. a selection).
/// Fl_Menu_::add() copies each '/'-separated label component into a fixed
/// 1024-byte stack buffer without bounds checks, so long text must be
/// truncated before it reaches FLTK. Whitespace runs collapse to one space,
/// and '/', '\\', and '&' are escaped so they display literally.
inline std::string previewMenuLabel(const std::string& prefix,
                                    const std::string& text,
                                    size_t maxChars = 60) {
    std::string label = prefix;
    size_t chars = 0;
    bool pendingSpace = false;
    bool truncated = false;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isspace(c)) {
            pendingSpace = chars > 0;
            continue;
        }
        bool startsChar = (c & 0xC0) != 0x80;
        if (startsChar) {
            if (chars + (pendingSpace ? 1 : 0) >= maxChars) {
                truncated = true;
                break;
            }
            if (pendingSpace) {
                label.push_back(' ');
                ++chars;
                pendingSpace = false;
            }
            ++chars;
        }
        if (c == '\\' || c == '/') label.push_back('\\');
        if (c == '&') label.push_back('&');
        label.push_back(static_cast<char>(c));
    }
    if (truncated) label += "\xE2\x80\xA6";
    return label;
}

inline void syncTooltipToSelection(Fl_Choice* choice,
                                   const std::vector<std::string>& displayLabels) {
    if (!choice) return;
    int index = choice->value();
    if (index < 0 || index >= static_cast<int>(displayLabels.size())) {
        choice->tooltip(nullptr);
        return;
    }
    choice->tooltip(displayLabels[static_cast<size_t>(index)].c_str());
}

inline void populateChoice(Fl_Choice* choice,
                           const std::vector<ModuleInfo>& modules,
                           std::vector<std::string>& moduleNames,
                           std::vector<std::string>& displayLabels) {
    if (!choice) return;

    choice->tooltip(nullptr);
    choice->clear();

    moduleNames.clear();
    displayLabels.clear();
    moduleNames.reserve(modules.size());
    displayLabels.reserve(modules.size());

    for (const auto& mod : modules) {
        std::string label = formatLabel(mod);
        choice->add(escapeMenuLabel(label).c_str());
        moduleNames.push_back(mod.name);
        displayLabels.push_back(std::move(label));
    }

    if (choice->size() > 0) {
        choice->value(0);
    }
    syncTooltipToSelection(choice, displayLabels);
}

inline int findChoiceIndexByModuleName(const std::vector<std::string>& moduleNames,
                                       const std::string& moduleName) {
    auto it = std::find(moduleNames.begin(), moduleNames.end(), moduleName);
    if (it == moduleNames.end()) return -1;
    return static_cast<int>(it - moduleNames.begin());
}

inline bool applyChoiceValue(Fl_Choice* choice,
                             const std::vector<std::string>& moduleNames,
                             const std::vector<std::string>& displayLabels,
                             const std::string& moduleName) {
    if (!choice || moduleName.empty()) return false;

    int index = findChoiceIndexByModuleName(moduleNames, moduleName);
    if (index < 0 || index >= choice->size()) return false;

    choice->value(index);
    syncTooltipToSelection(choice, displayLabels);
    return true;
}

inline std::string selectedModuleName(const Fl_Choice* choice,
                                      const std::vector<std::string>& moduleNames) {
    if (!choice) return "";

    int index = choice->value();
    if (index < 0 || index >= static_cast<int>(moduleNames.size())) return "";

    return moduleNames[static_cast<size_t>(index)];
}

inline std::string displayLabelForModuleName(
    const std::vector<std::string>& moduleNames,
    const std::vector<std::string>& displayLabels,
    const std::string& moduleName) {
    int index = findChoiceIndexByModuleName(moduleNames, moduleName);
    if (index < 0 || index >= static_cast<int>(displayLabels.size())) {
        return moduleName;
    }
    return displayLabels[static_cast<size_t>(index)];
}

} // namespace module_choice
} // namespace verdad

#endif // VERDAD_MODULE_CHOICE_UTILS_H
