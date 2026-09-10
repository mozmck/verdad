#include "sword/SwordManager.h"
#include "ui/HtmlWidget.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace verdad {
namespace {

struct BenchOptions {
    int warmIterations = 3;
    int layoutWidth = 900;
    int layoutHeight = 700;
    bool skipLayout = false;
    std::vector<std::string> modules;
};

struct BenchSample {
    std::string module;
    std::string language;
    std::string book;
    int chapter = 1;
    int selectedVerse = 1;
};

struct PhaseStats {
    double totalMs = 0.0;
    double minMs = std::numeric_limits<double>::max();
    double maxMs = 0.0;
    size_t runs = 0;
    size_t totalBytes = 0;

    void add(double ms, size_t bytes = 0) {
        totalMs += ms;
        minMs = std::min(minMs, ms);
        maxMs = std::max(maxMs, ms);
        ++runs;
        totalBytes += bytes;
    }

    double averageMs() const {
        return runs ? (totalMs / static_cast<double>(runs)) : 0.0;
    }

    double averageBytes() const {
        return runs ? static_cast<double>(totalBytes) / static_cast<double>(runs) : 0.0;
    }
};

class Stopwatch {
public:
    Stopwatch()
        : start_(std::chrono::steady_clock::now()) {}

    double elapsedMs() const {
        using namespace std::chrono;
        return duration_cast<microseconds>(
                   steady_clock::now() - start_)
                   .count() /
               1000.0;
    }

private:
    std::chrono::steady_clock::time_point start_;
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

std::string lowerAsciiCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return text;
}

std::string normalizeLanguageCode(const std::string& language) {
    std::string code = lowerAsciiCopy(trimCopy(language));
    size_t sep = code.find_first_of("-_");
    if (sep != std::string::npos) code.erase(sep);
    if (code == "eng") return "en";
    if (code == "spa") return "es";
    if (code == "fra" || code == "fre") return "fr";
    if (code == "deu" || code == "ger") return "de";
    if (code == "ell" || code == "gre") return "el";
    if (code == "heb" || code == "hbo") return "he";
    if (code == "ara") return "ar";
    if (code == "rus") return "ru";
    if (code == "zho" || code == "chi") return "zh";
    if (code.empty()) return "und";
    return code;
}

std::string loadMasterCss() {
    const std::filesystem::path cssPath =
        std::filesystem::current_path() / "data" / "master.css";
    std::ifstream input(cssPath);
    if (!input.is_open()) return "";

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string defaultTextOverrideCss() {
    return
        "body,\n"
        "div.chapter,\n"
        "div.verse,\n"
        "div.parallel-col,\n"
        "div.parallel-col-last,\n"
        "div.parallel-cell,\n"
        "div.parallel-cell-last,\n"
        "div.interlinear,\n"
        "div.interlinear-line,\n"
        "div.commentary-heading,\n"
        "div.commentary,\n"
        "div.commentary-text,\n"
        "div.dictionary,\n"
        "div.general-book,\n"
        "div.mag-lite {\n"
        "  font-family: \"DejaVu Serif\" !important;\n"
        "  font-size: 14px !important;\n"
        "  line-height: 1.2 !important;\n"
        "}\n";
}

bool tryAppendSample(SwordManager& sword,
                     const ModuleInfo& module,
                     int preferredBookIndex,
                     int preferredChapter,
                     std::vector<BenchSample>& out) {
    const auto books = sword.getBookNames(module.name);
    if (books.empty()) return false;

    int bookIndex = std::clamp(preferredBookIndex, 0, static_cast<int>(books.size()) - 1);
    const std::string& book = books[bookIndex];
    const int chapterCount = sword.getChapterCount(module.name, book);
    if (chapterCount <= 0) return false;

    const int chapter = std::clamp(preferredChapter, 1, chapterCount);
    int selectedVerse = sword.getVerseCount(module.name, book, chapter);
    if (selectedVerse <= 0) selectedVerse = 1;
    selectedVerse = std::max(1, selectedVerse / 2);

    out.push_back(BenchSample{
        module.name,
        normalizeLanguageCode(module.language),
        book,
        chapter,
        selectedVerse,
    });
    return true;
}

std::vector<BenchSample> buildAutoCorpus(SwordManager& sword,
                                         const std::vector<ModuleInfo>& modules) {
    static const char* kPreferredLanguages[] = {
        "en", "es", "de", "fr", "ru", "el", "he", "ar", "zh",
    };

    std::vector<const ModuleInfo*> selectedModules;
    std::set<std::string> selectedNames;
    std::set<std::string> selectedLanguages;

    auto trySelectModule = [&](const ModuleInfo& module) {
        if (selectedModules.size() >= 5) return;
        if (!selectedNames.insert(module.name).second) return;
        selectedModules.push_back(&module);
        selectedLanguages.insert(normalizeLanguageCode(module.language));
    };

    for (const char* preferred : kPreferredLanguages) {
        for (const auto& module : modules) {
            if (normalizeLanguageCode(module.language) == preferred &&
                !selectedLanguages.count(preferred)) {
                trySelectModule(module);
                break;
            }
        }
        if (selectedModules.size() >= 5) break;
    }

    for (const auto& module : modules) {
        if (selectedModules.size() >= 5) break;
        const std::string language = normalizeLanguageCode(module.language);
        if (selectedLanguages.insert(language).second) {
            trySelectModule(module);
        }
    }

    std::vector<BenchSample> samples;
    for (const ModuleInfo* module : selectedModules) {
        if (!module) continue;
        size_t before = samples.size();
        tryAppendSample(sword, *module, 0, 1, samples);
        tryAppendSample(sword, *module, 18, 23, samples);
        tryAppendSample(sword, *module, 42, 1, samples);
        if (samples.size() == before) {
            tryAppendSample(sword, *module, 0, 1, samples);
        }
    }

    return samples;
}

std::vector<BenchSample> buildCorpus(SwordManager& sword,
                                     const BenchOptions& options) {
    const std::vector<ModuleInfo> modules = sword.getBibleModules();
    if (modules.empty()) return {};

    if (options.modules.empty()) {
        return buildAutoCorpus(sword, modules);
    }

    std::vector<BenchSample> samples;
    for (const std::string& requested : options.modules) {
        auto it = std::find_if(modules.begin(), modules.end(),
                               [&](const ModuleInfo& module) {
                                   return module.name == requested;
                               });
        if (it == modules.end()) continue;
        size_t before = samples.size();
        tryAppendSample(sword, *it, 0, 1, samples);
        tryAppendSample(sword, *it, 18, 23, samples);
        tryAppendSample(sword, *it, 42, 1, samples);
        if (samples.size() == before) {
            tryAppendSample(sword, *it, 0, 1, samples);
        }
    }

    return samples;
}

bool shouldSkipLayoutBenchmark() {
#if defined(_WIN32) || defined(__APPLE__)
    return false;
#else
    const char* display = std::getenv("DISPLAY");
    return !display || !*display;
#endif
}

bool parseArgs(int argc, char* argv[], BenchOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto nextValue = [&](int& index) -> const char* {
            if (index + 1 >= argc) return nullptr;
            ++index;
            return argv[index];
        };

        if (arg == "--iterations") {
            const char* value = nextValue(i);
            if (!value) return false;
            options.warmIterations = std::max(1, std::atoi(value));
        } else if (arg == "--layout-width") {
            const char* value = nextValue(i);
            if (!value) return false;
            options.layoutWidth = std::max(320, std::atoi(value));
        } else if (arg == "--layout-height") {
            const char* value = nextValue(i);
            if (!value) return false;
            options.layoutHeight = std::max(240, std::atoi(value));
        } else if (arg == "--skip-layout") {
            options.skipLayout = true;
        } else if (arg == "--module") {
            const char* value = nextValue(i);
            if (!value) return false;
            options.modules.push_back(value);
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return false;
        }
    }

    return true;
}

void printUsage(const char* argv0) {
    std::cerr
        << "Usage: " << (argv0 ? argv0 : "verdad-bench")
        << " [--iterations N] [--layout-width PX] [--layout-height PX]\n"
        << "       [--skip-layout] [--module NAME ...]\n";
}

void printPhase(const char* label, const PhaseStats& stats) {
    const double minMs = stats.runs ? stats.minMs : 0.0;
    const double maxMs = stats.runs ? stats.maxMs : 0.0;
    std::cout << std::left << std::setw(18) << label
              << " runs=" << stats.runs
              << " avg_ms=" << std::fixed << std::setprecision(3) << stats.averageMs()
              << " min_ms=" << minMs
              << " max_ms=" << maxMs;
    if (stats.totalBytes > 0) {
        std::cout << " avg_bytes=" << static_cast<size_t>(stats.averageBytes());
    }
    std::cout << '\n';
}

} // namespace
} // namespace verdad

int main(int argc, char* argv[]) {
    using namespace verdad;

    BenchOptions options;
    if (!parseArgs(argc, argv, options)) {
        printUsage(argv[0]);
        return (argc > 1) ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    SwordManager sword;
    if (!sword.initialize()) {
        std::cerr << "Failed to initialize SWORD.\n";
        return EXIT_FAILURE;
    }

    std::vector<BenchSample> samples = buildCorpus(sword, options);
    if (samples.empty()) {
        std::cerr << "No benchmark samples available.\n";
        return EXIT_FAILURE;
    }

    bool runLayout = !options.skipLayout && !shouldSkipLayoutBenchmark();

    std::unique_ptr<Fl_Window> window;
    HtmlWidget* widget = nullptr;
    if (runLayout) {
        Fl::scheme("gtk+");
        Fl::set_fonts("-*");
        window = std::make_unique<Fl_Window>(options.layoutWidth, options.layoutHeight);
        widget = new HtmlWidget(0, 0, options.layoutWidth, options.layoutHeight);
        window->resizable(widget);
        const std::string masterCss = loadMasterCss();
        if (!masterCss.empty()) widget->setMasterCSS(masterCss);
        widget->setStyleOverrideCss(defaultTextOverrideCss());
        window->end();
    }

    PhaseStats prepareCold;
    PhaseStats prepareWarm;
    PhaseStats renderVerse;
    PhaseStats renderParagraph;
    PhaseStats layoutVerse;
    PhaseStats layoutParagraph;
    PhaseStats fullCold;

    std::cout << "Corpus (" << samples.size() << " samples)\n";
    for (const auto& sample : samples) {
        std::cout << "  " << sample.module
                  << " [" << sample.language << "] "
                  << sample.book << ' ' << sample.chapter
                  << " verse=" << sample.selectedVerse << '\n';
    }
    std::cout << '\n';

    for (const auto& sample : samples) {
        sword.clearRenderCaches();
        {
            Stopwatch timer;
            auto prepared = sword.prepareChapterText(sample.module, sample.book, sample.chapter);
            prepareCold.add(timer.elapsedMs());

            if (!prepared) continue;

            for (int i = 0; i < options.warmIterations; ++i) {
                Stopwatch warmTimer;
                auto warmPrepared = sword.prepareChapterText(sample.module,
                                                             sample.book,
                                                             sample.chapter);
                prepareWarm.add(warmTimer.elapsedMs());
            }

            Stopwatch renderTimer;
            std::string verseHtml = sword.renderPreparedChapterText(
                *prepared, false, sample.selectedVerse);
            renderVerse.add(renderTimer.elapsedMs(), verseHtml.size());

            renderTimer = Stopwatch();
            std::string paragraphHtml = sword.renderPreparedChapterText(
                *prepared, true, sample.selectedVerse);
            renderParagraph.add(renderTimer.elapsedMs(), paragraphHtml.size());

            if (widget) {
                Stopwatch layoutTimer;
                widget->setHtml(verseHtml);
                layoutVerse.add(layoutTimer.elapsedMs(), verseHtml.size());

                layoutTimer = Stopwatch();
                widget->setHtml(paragraphHtml);
                layoutParagraph.add(layoutTimer.elapsedMs(), paragraphHtml.size());
            }
        }

        sword.clearRenderCaches();
        Stopwatch fullTimer;
        std::string html = sword.getChapterText(sample.module,
                                                sample.book,
                                                sample.chapter,
                                                false,
                                                sample.selectedVerse);
        fullCold.add(fullTimer.elapsedMs(), html.size());
    }

    std::cout << "Summary\n";
    printPhase("prepare_cold", prepareCold);
    printPhase("prepare_warm", prepareWarm);
    printPhase("render_verse", renderVerse);
    printPhase("render_paragraph", renderParagraph);
    if (widget) {
        printPhase("layout_verse", layoutVerse);
        printPhase("layout_paragraph", layoutParagraph);
    } else {
        std::cout << "layout            skipped (no display or --skip-layout)\n";
    }
    printPhase("full_cold", fullCold);

    return EXIT_SUCCESS;
}
