#pragma once

#include <swbuf.h>

#include <string>

namespace verdad {

// stripText() returns const char* in released SWORD versions and an owning
// SWBuf in newer versions. Copy during the call, before any temporary expires.
inline std::string copySwordText(const char* text) {
    return text ? std::string(text) : std::string();
}

inline std::string copySwordText(const sword::SWBuf& text) {
    return copySwordText(text.c_str());
}

} // namespace verdad
