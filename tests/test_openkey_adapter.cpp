#include <iostream>
#include <string>

#include <fcitx-utils/utf8.h>

#include "Macro.h"
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

openkey::OpenKeyAdapter makeAdapter() {
    openkey::OpenKeyAdapter adapter;
    adapter.setInputType(0);    // Telex
    adapter.setCodeTable(0);    // Unicode precomposed
    adapter.setFreeMark(true);
    adapter.setCheckSpelling(true);
    adapter.setRestoreIfWrongSpelling(true);
    return adapter;
}

} // namespace

int main() {
    auto adapter = makeAdapter();

    expectEq("a", typeSequence(adapter, "a"), "a");
    expectEq("as", typeSequence(adapter, "as"), "á");
    expectEq("aw", typeSequence(adapter, "aw"), "ă");
    expectEq("aa", typeSequence(adapter, "aa"), "â");
    expectEq("dd", typeSequence(adapter, "dd"), "đ");
    expectEq("uw", typeSequence(adapter, "uw"), "ư");
    expectEq("suw", typeSequence(adapter, "suw"), "sư");
    expectEq("sww", typeSequence(adapter, "sww"), "sw");
    expectEq("ww", typeSequence(adapter, "ww"), "w");
    // OpenKey core follows standard Telex:
    // - đ = dd
    // - ươ = ow + w (e.g. uow)
    expectEq("dduowngf", typeSequence(adapter, "dduowngf"), "đường");
    expectEq("tieengs", typeSequence(adapter, "tieengs"), "tiếng");
    expectEq("ddieenj", typeSequence(adapter, "ddieenj"), "điện");
    expectEq("truowngf", typeSequence(adapter, "truowngf"), "trường");
    expectEq("nguoiwf", typeSequence(adapter, "nguoiwf"), "người");
    expectEq("ddww", typeSequence(adapter, "ddww"), "đw");
    {
        auto restoreAdapter = makeAdapter();
        std::string restored;
        if (!restoreAdapter.restoreFromRawAsciiOnWordBreak("đw", "ddww", ' ',
                                                           restored)) {
            failures++;
            std::cerr << "[FAIL] restoreFromRaw ddww: returned false\n";
        } else {
            expectEq("restoreFromRaw ddww", restored, "ddww");
        }
    }
    {
        auto restoreAdapter = makeAdapter();
        const std::string shown = typeSequence(restoreAdapter, "khoongg");
        expectEq("khoongg shown", shown, "khôngg");
        std::string restored;
        if (!restoreAdapter.restoreFromRawAsciiOnWordBreak(shown, "khoongg", ' ',
                                                           restored)) {
            failures++;
            std::cerr << "[FAIL] restoreFromRaw khoongg: returned false\n";
        } else {
            expectEq("restoreFromRaw khoongg", restored, "khoongg");
        }
    }
    {
        auto literalWAdapter = makeAdapter();
        literalWAdapter.setLiteralWAtWordStart(true);
        expectEq("literal w start", typeSequence(literalWAdapter, "w"), "w");
        expectEq("literal wa start", typeSequence(literalWAdapter, "wa"), "wa");
    }

    initMacroMap(nullptr, 0);
    addMacro("cjv", "cái gì vậy");
    addMacro("qtqd", "quá trời quá đất");
    {
        std::string expanded;
        if (!adapter.expandMacro("cjv", expanded)) {
            failures++;
            std::cerr << "[FAIL] expandMacro cjv: returned false\n";
        } else {
            expectEq("expandMacro cjv", expanded, "cái gì vậy");
        }
    }
    {
        std::string expanded;
        if (!adapter.expandMacro("qtqd", expanded)) {
            failures++;
            std::cerr << "[FAIL] expandMacro qtqd: returned false\n";
        } else {
            expectEq("expandMacro qtqd", expanded, "quá trời quá đất");
        }
    }

    // Surrounding-text extraction should be char-safe with emoji.
    {
        const std::string text = std::string("😀") + "abc";
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
