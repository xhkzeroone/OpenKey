#include <algorithm>
#include <iostream>
#include <sstream>
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

std::string utf8DropLastN(const std::string &s, size_t n) {
    if (n == 0) {
        return s;
    }
    if (!fcitx::utf8::validate(s)) {
        return {};
    }
    const auto len = fcitx::utf8::length(s);
    if (n >= len) {
        return {};
    }
    const auto keep = len - n;
    auto it = fcitx::utf8::nextNChar(s.begin(), keep);
    return std::string(s.begin(), it);
}

std::size_t commonPrefixBytesUTF8Boundary(const std::string &s1,
                                          const std::string &s2) {
    std::size_t n = std::min(s1.size(), s2.size());
    std::size_t i = 0;
    while (i < n && s1[i] == s2[i]) {
        ++i;
    }
    while (i > 0 && i < s1.size() &&
           (static_cast<unsigned char>(s1[i]) & 0xC0u) == 0x80u) {
        --i;
    }
    return i;
}

unsigned int utf8CharCount(const std::string &s) {
    if (!fcitx::utf8::validate(s)) {
        return 0;
    }
    return static_cast<unsigned int>(fcitx::utf8::length(s));
}

std::string quote(const std::string &s) {
    return "\"" + s + "\"";
}

std::string simulateBackspaceSnapshot(openkey::OpenKeyAdapter &adapter,
                                      const std::string &ascii) {
    std::string shown;
    std::string rawAscii;
    std::string appText;
    std::string snapshotShown;
    std::string snapshotRawAscii;
    bool canReseedFromSnapshot = false;
    std::ostringstream out;

    out << "mode=nonPreedit\n";
    for (char c : ascii) {
        if (c == ' ') {
            if (!shown.empty()) {
                snapshotShown = shown;
                snapshotRawAscii = rawAscii;
                canReseedFromSnapshot = true;
            } else if (!canReseedFromSnapshot || snapshotShown.empty()) {
                snapshotShown.clear();
                snapshotRawAscii.clear();
                canReseedFromSnapshot = false;
            }
            appText.push_back(' ');
            shown.clear();
            rawAscii.clear();
            out << "key space: forward -> app=" << quote(appText)
                << " shown=\"\" snapshot=" << quote(snapshotShown) << "\n";
            continue;
        }
        if (c == '<') {
            if (canReseedFromSnapshot && shown.empty() &&
                !snapshotShown.empty()) {
                shown = snapshotShown;
                rawAscii = snapshotRawAscii;
                snapshotShown.clear();
                snapshotRawAscii.clear();
                canReseedFromSnapshot = false;
            }
            appText = utf8DropLastN(appText, 1);
            out << "key backspace: forward -> app=" << quote(appText)
                << " shown=" << quote(shown) << "\n";
            continue;
        }

        const auto r = adapter.processAsciiKey(shown, c);
        if (!r.handled) {
            appText.push_back(c);
            shown.clear();
            rawAscii.clear();
            out << "key " << c << ": forward -> app=" << quote(appText)
                << " shown=\"\"\n";
            continue;
        }

        const std::size_t prefixLen =
            commonPrefixBytesUTF8Boundary(shown, r.newWord);
        const unsigned int deleteCount =
            utf8CharCount(shown.substr(prefixLen));
        const std::string commitText = r.newWord.substr(prefixLen);

        if (deleteCount == 0) {
            appText += commitText;
            out << "key " << c << ": commit " << quote(commitText)
                << " -> app=" << quote(appText) << " shown="
                << quote(r.newWord) << "\n";
        } else {
            appText = utf8DropLastN(appText, deleteCount);
            appText += commitText;
            out << "key " << c << ": helper PLAN backspaces=" << deleteCount
                << "; helper DONE; commit " << quote(commitText)
                << " -> app=" << quote(appText) << " shown="
                << quote(r.newWord) << "\n";
        }

        shown = r.newWord;
        rawAscii.push_back(c);
    }

    out << "final app=" << quote(appText) << " shown=" << quote(shown);
    return out.str();
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
    expectEq("pussh", typeSequence(adapter, "pussh"), "push");
    expectEq("dduwocj", typeSequence(adapter, "dduwocj"), "được");
    {
        auto restoreAdapter = makeAdapter();
        const std::string shown = typeSequence(restoreAdapter, "pussh");
        std::string restored;
        if (restoreAdapter.restoreFromRawAsciiOnWordBreak(shown, "pussh", ' ',
                                                           restored)) {
            failures++;
            std::cerr << "[FAIL] restoreFromRaw pussh: expected false, got true (restored to: " << restored << ")\n";
        }
    }
    {
        auto restoreAdapter = makeAdapter();
        const std::string shown = typeSequence(restoreAdapter, "wwass");
        expectEq("wwass shown", shown, "was");
        std::string restored;
        if (restoreAdapter.restoreFromRawAsciiOnWordBreak(shown, "wwass", ' ',
                                                           restored)) {
            failures++;
            std::cerr << "[FAIL] restoreFromRaw wwass: expected false, got true (restored to: " << restored << ")\n";
        }
    }
    {
        auto restoreAdapter = makeAdapter();
        const std::string shown = typeSequence(restoreAdapter, "thorr");
        expectEq("thorr shown", shown, "thor");
        std::string restored;
        if (restoreAdapter.restoreFromRawAsciiOnWordBreak(shown, "thorr", ' ',
                                                           restored)) {
            failures++;
            std::cerr << "[FAIL] restoreFromRaw thorr: expected false, got true (restored to: " << restored << ")\n";
        }
    }
    {
        auto restoreAdapter = makeAdapter();
        const std::string shown = typeSequence(restoreAdapter, "mass");
        expectEq("mass shown", shown, "mas");
        std::string restored;
        if (restoreAdapter.restoreFromRawAsciiOnWordBreak(shown, "mass", ' ',
                                                           restored)) {
            failures++;
            std::cerr << "[FAIL] restoreFromRaw mass: expected false, got true (restored to: " << restored << ")\n";
        }
    }
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
    {
        auto snapshotAdapter = makeAdapter();
        expectEq("nonPreedit snapshot",
                 simulateBackspaceSnapshot(snapshotAdapter, "as"),
                 "mode=nonPreedit\n"
                 "key a: commit \"a\" -> app=\"a\" shown=\"a\"\n"
                 "key s: helper PLAN backspaces=1; helper DONE; commit \"á\" -> app=\"á\" shown=\"á\"\n"
                 "final app=\"á\" shown=\"á\"");
    }
    {
        auto snapshotAdapter = makeAdapter();
        expectEq("nonPreedit reseed snapshot",
                 simulateBackspaceSnapshot(snapshotAdapter, "as <f"),
                 "mode=nonPreedit\n"
                 "key a: commit \"a\" -> app=\"a\" shown=\"a\"\n"
                 "key s: helper PLAN backspaces=1; helper DONE; commit \"á\" -> app=\"á\" shown=\"á\"\n"
                 "key space: forward -> app=\"á \" shown=\"\" snapshot=\"á\"\n"
                 "key backspace: forward -> app=\"á\" shown=\"á\"\n"
                 "key f: helper PLAN backspaces=1; helper DONE; commit \"à\" -> app=\"à\" shown=\"à\"\n"
                 "final app=\"à\" shown=\"à\"");
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
