#pragma once

#include <string>

namespace openkey {

struct WordSegment {
    std::string word;
    unsigned int startChar = 0;
    unsigned int endChar = 0;
};

bool extractWordBeforeCursor(const std::string &text, unsigned int cursorChar,
                            WordSegment &out);

} // namespace openkey
