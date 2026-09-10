#include "sword/ScriptureReference.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void check(const std::string& text,
           const std::vector<std::pair<std::string, std::string>>& expected) {
    const auto refs = verdad::scripture::verseReferences(text);
    const auto ranges = verdad::scripture::verseReferenceRanges(text);
    if (refs.size() != expected.size() || ranges.size() != expected.size()) {
        std::cerr << "Wrong reference count for: " << text << " (got "
                  << refs.size() << ", expected " << expected.size() << ")\n";
        ++failures;
        return;
    }
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto& ref = refs[i];
        int start = -1, end = -1;
        const std::string target = verdad::scripture::verseReferenceAtPosition(
            text, ref.start + 1, &start, &end);
        if (text.substr(ref.start, ref.end - ref.start) != expected[i].first ||
            ref.reference != expected[i].second || target != ref.reference ||
            start != ref.start || end != ref.end ||
            ranges[i] != std::make_pair(ref.start, ref.end)) {
            std::cerr << "Incorrect link for: " << text << " -> " << ref.reference << '\n';
            ++failures;
        }
    }
}
}

int main() {
    // The two reference lists from the Millenium studypad.
    check("(Mt 12:22-29; Luke 8:30-31; 10:18-20; 13:16; John 12:31-32; "
          "16:8-11; 17:15; Acts 26:18; Rom 16:20; Col 2:14-15; Eph 4:8; "
          "2 Thess 2:7; Heb 2:14-15; 1 Pet 3:19; 2 Pet 2:4; 1 John 2:13; "
          "3:8; 4:3-5; 5:18; Jude 6; Rev 12:10)", {
        {"Mt 12:22-29", "Matthew 12:22-29"},
        {"Luke 8:30-31", "Luke 8:30-31"}, {"10:18-20", "Luke 10:18-20"},
        {"13:16", "Luke 13:16"}, {"John 12:31-32", "John 12:31-32"},
        {"16:8-11", "John 16:8-11"}, {"17:15", "John 17:15"},
        {"Acts 26:18", "Acts 26:18"}, {"Rom 16:20", "Romans 16:20"},
        {"Col 2:14-15", "Colossians 2:14-15"}, {"Eph 4:8", "Ephesians 4:8"},
        {"2 Thess 2:7", "II Thessalonians 2:7"}, {"Heb 2:14-15", "Hebrews 2:14-15"},
        {"1 Pet 3:19", "I Peter 3:19"}, {"2 Pet 2:4", "II Peter 2:4"},
        {"1 John 2:13", "I John 2:13"}, {"3:8", "I John 3:8"},
        {"4:3-5", "I John 4:3-5"}, {"5:18", "I John 5:18"},
        {"Jude 6", "Jude 1:6"}, {"Rev 12:10", "Revelation of John 12:10"}
    });
    check("Ro 2:29; Ro 9:6-8; Gal 3:15-29; Gal 6:16; Eph 2:11-22; "
          "Phil 3:3; 1 Peter 2:9-10; Heb 8:6-13", {
        {"Ro 2:29", "Romans 2:29"}, {"Ro 9:6-8", "Romans 9:6-8"},
        {"Gal 3:15-29", "Galatians 3:15-29"}, {"Gal 6:16", "Galatians 6:16"},
        {"Eph 2:11-22", "Ephesians 2:11-22"}, {"Phil 3:3", "Philippians 3:3"},
        {"1 Peter 2:9-10", "I Peter 2:9-10"}, {"Heb 8:6-13", "Hebrews 8:6-13"}
    });
    check("See 1 John 2:13; 3:8 and read 2 Thess 2:7", {
        {"1 John 2:13", "I John 2:13"}, {"3:8", "I John 3:8"},
        {"2 Thess 2:7", "II Thessalonians 2:7"}
    });
    check("See mt.12:22–29,30; Mk 1:1; Lk 2:1; Jn 3:16, 18; 4:1-3", {
        {"mt.12:22–29", "Matthew 12:22-29"}, {"30", "Matthew 12:30"},
        {"Mk 1:1", "Mark 1:1"}, {"Lk 2:1", "Luke 2:1"},
        {"Jn 3:16", "John 3:16"}, {"18", "John 3:18"}, {"4:1-3", "John 4:1-3"}
    });
    check("II Tim 2:3; 1Jn 2:13; 3:8; Phlm 4, 6; 2 John 5; Obad 3", {
        {"II Tim 2:3", "II Timothy 2:3"}, {"1Jn 2:13", "I John 2:13"},
        {"3:8", "I John 3:8"}, {"Phlm 4", "Philemon 1:4"},
        {"6", "Philemon 1:6"}, {"2 John 5", "II John 1:5"}, {"Obad 3", "Obadiah 1:3"}
    });
    check("John 3:35—4:3, 5; 5 : 1 - 3", {
        {"John 3:35—4:3", "John 3:35-4:3"}, {"5", "John 4:5"},
        {"5 : 1 - 3", "John 5:1-3"}
    });
    check("John 3:16; 99:1; 4:5", {{"John 3:16", "John 3:16"}});
    check("John 3:16 and 18; 19", {{"John 3:16", "John 3:16"}});
    check("John 3:16. 18, 20", {{"John 3:16", "John 3:16"}});
    check("John 3:16;\n4:5", {{"John 3:16", "John 3:16"}});
    check("John 3:16;; 18", {{"John 3:16", "John 3:16"}});
    check("John 3; 18; 3:16; 4:5", {});
    check("Unknown 3:16; John 0:1; John 3:0; John 3:99; John 3:20-16; "
          "Jude 26; John 3:36-4:99; John 9999999999999999:1", {});
    check("xJohn 3:16; _John 3:16; John 3:16xyz; John 3:16:7", {});
    if (failures) return EXIT_FAILURE;
    std::cout << "Scripture reference regressions passed.\n";
    return EXIT_SUCCESS;
}
