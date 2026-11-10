#include "sword/SwordText.h"

#include <swmodule.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

namespace {
int failures = 0;

void expect(const std::string& text, const std::string& expected,
            const char* description) {
    if (text != expected) {
        std::cerr << description << ": got [" << text
                  << "], expected [" << expected << "]\n";
        ++failures;
    }
}

// Compile the actual installed stripText() signature as well as exercising
// both return types below; either API must produce an owning std::string.
static_assert(std::is_same_v<
    decltype(verdad::copySwordText(std::declval<sword::SWModule&>().stripText())),
    std::string>);
}

int main() {
    expect(verdad::copySwordText(nullptr), "", "Null legacy text");
    expect(verdad::copySwordText(""), "", "Empty legacy text");
    expect(verdad::copySwordText(sword::SWBuf()), "", "Empty SWORD buffer");

    char legacyText[] = "Original legacy text";
    const std::string legacyCopy = verdad::copySwordText(legacyText);
    legacyText[0] = 'X';
    expect(legacyCopy, "Original legacy text", "Legacy buffer must be copied");

    sword::SWBuf buffer("Original SWORD text");
    const std::string bufferCopy = verdad::copySwordText(buffer);
    buffer = "Replacement text";
    expect(bufferCopy, "Original SWORD text", "SWORD buffer must be copied");

    // The temporary SWBuf is destroyed at the end of this statement.
    const std::string expected(2048, 'a');
    const std::string temporaryCopy =
        verdad::copySwordText(sword::SWBuf(expected.c_str()));
    sword::SWBuf replacement(std::string(2048, 'b').c_str());
    expect(temporaryCopy, expected, "Temporary buffer must remain owned");

    if (failures) return EXIT_FAILURE;
    std::cout << "SWORD text compatibility regressions passed.\n";
    return EXIT_SUCCESS;
}
