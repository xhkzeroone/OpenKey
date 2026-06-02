#include <iostream>
#include <string>

#include <fcitx-utils/utf8.h>

#include "openkey_adapter.h"
#include "surrounding_text_utils.h"

namespace {

int failures = 0;

void expectEq(const std::string &name, const std::string &got,
              const std::string &want) {
    if (got == want) {
        return;
    }
    failures++;
    std::cerr << "[FAIL] " << name << "\n  got:  " << got << "\n  want: " << want
              << "\n";
}

std::string typeSequence(openkey::OpenKeyAdapter &adapter,
                         const std::string &ascii) {
    std::string word;
    for (char c : ascii) {
        auto r = adapter.processAsciiKey(word, c);
        if (r.handled) {
            word = std::move(r.newWord);
        } else {
            word.push_back(c);
        }
    }
    return word;
}

} // namespace

int main() {
    openkey::OpenKeyAdapter adapter;
    adapter.setInputType(0);    // Telex
    adapter.setCodeTable(0);    // Unicode precomposed
    adapter.setFreeMark(true);
    adapter.setCheckSpelling(true);

    expectEq("a", typeSequence(adapter, "a"), "a");
    expectEq("as", typeSequence(adapter, "as"), u8"á");
    expectEq("aw", typeSequence(adapter, "aw"), u8"ă");
    expectEq("aa", typeSequence(adapter, "aa"), u8"â");
    expectEq("dd", typeSequence(adapter, "dd"), u8"đ");
    expectEq("uw", typeSequence(adapter, "uw"), u8"ư");
    expectEq("suw", typeSequence(adapter, "suw"), u8"sư");
    expectEq("sww", typeSequence(adapter, "sww"), "sw");
    expectEq("ww", typeSequence(adapter, "ww"), "w");
    // OpenKey core follows standard Telex:
    // - đ = dd
    // - ươ = ow + w (e.g. uow)
    expectEq("dduowngf", typeSequence(adapter, "dduowngf"), u8"đường");
    expectEq("tieengs", typeSequence(adapter, "tieengs"), u8"tiếng");
    expectEq("ddieenj", typeSequence(adapter, "ddieenj"), u8"điện");
    expectEq("truowngf", typeSequence(adapter, "truowngf"), u8"trường");
    expectEq("nguoiwf", typeSequence(adapter, "nguoiwf"), u8"người");

    // Surrounding-text extraction should be char-safe with emoji.
    {
        const std::string text = std::string(u8"😀") + "abc";
        openkey::WordSegment seg;
        const unsigned int cursorChar = fcitx::utf8::length(text);
        const bool ok = openkey::extractWordBeforeCursor(text, cursorChar, seg);
        if (!ok) {
            failures++;
            std::cerr << "[FAIL] extractWordBeforeCursor emoji: returned false\n";
        } else {
            expectEq("extract word", seg.word, "abc");
            if (seg.startChar != 1 || seg.endChar != 4) {
                failures++;
                std::cerr << "[FAIL] extract offsets\n  got:  " << seg.startChar
                          << ".." << seg.endChar << "\n  want: 1..4\n";
            }
        }
    }

    if (failures == 0) {
        std::cerr << "[OK] all tests passed\n";
        return 0;
    }
    return 1;
}
