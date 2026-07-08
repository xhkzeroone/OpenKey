#include "surrounding_text_utils.h"

#include <vector>

#include <fcitx-utils/utf8.h>

namespace openkey {
namespace {

static bool isWordChar(uint32_t cp) {
    if (cp == '_') {
        return true;
    }
    if (cp <= 0x7F) {
        const unsigned char c = static_cast<unsigned char>(cp);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z')) {
            return true;
        }
        return false;
    }

    // Combining marks.
    if (cp >= 0x0300 && cp <= 0x036F) {
        return true;
    }

    // Vietnamese letters are mostly in these blocks.
    if ((cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF)) {
        return true;
    }
    return false;
}

} // namespace

bool extractWordBeforeCursor(const std::string &text, unsigned int cursorChar,
                             WordSegment &out) {
    out = {};
    if (!fcitx::utf8::validate(text)) {
        return false;
    }

    const auto totalLen = fcitx::utf8::length(text);
    if (cursorChar > totalLen) {
        return false;
    }

    std::vector<uint32_t> cps;
    cps.reserve(totalLen);
    for (auto it = fcitx::utf8::UTF8CharIterator(text.begin(), text.end()),
              end = fcitx::utf8::UTF8CharIterator(text.end(), text.end());
         it != end; ++it) {
        cps.push_back(*it);
    }

    unsigned int start = cursorChar;
    while (start > 0 && isWordChar(cps[start - 1])) {
        start--;
    }

    const unsigned int endChar = cursorChar;
    const int startByte = fcitx::utf8::ncharByteLength(text.begin(), start);
    const int endByte = fcitx::utf8::ncharByteLength(text.begin(), endChar);
    if (startByte < 0 || endByte < startByte) {
        return false;
    }

    out.word = text.substr(static_cast<size_t>(startByte),
                           static_cast<size_t>(endByte - startByte));
    out.startChar = start;
    out.endChar = endChar;
    return true;
}

} // namespace openkey
