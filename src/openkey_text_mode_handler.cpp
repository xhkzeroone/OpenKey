#include "openkey_text_mode_handler.h"

#include <algorithm>

#include <fcitx-utils/utf8.h>

#include "openkey_adapter.h"
#include "surrounding_text_utils.h"

namespace openkey {
namespace {

static bool isComposingASCII(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    if (c >= '0' && c <= '9') {
        return true;
    }
    return c == '[' || c == ']';
}

static bool isBoundaryASCII(char c) {
    switch (c) {
    case ' ':
    case '.':
    case ',':
    case ';':
    case ':':
    case '?':
    case '!':
    case ')':
    case ']':
    case '}':
    case '"':
    case '\'':
        return true;
    default:
        return false;
    }
}

static bool isMacroTriggerKey(char c) {
    if (c == ' ') {
        return true;
    }
    switch (c) {
    case ',':
    case '.':
    case '/':
    case ';':
    case '\'':
    case '\\':
    case '-':
    case '=':
        return true;
    default:
        return false;
    }
}

static std::size_t commonPrefixBytesUTF8Boundary(const std::string &s1,
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

static unsigned int utf8CharCount(const std::string &s) {
    if (!fcitx::utf8::validate(s)) {
        return 0;
    }
    return static_cast<unsigned int>(fcitx::utf8::length(s));
}

} // namespace

PreeditModeHandler::PreeditModeHandler(TextModeHandlerDeps deps)
    : deps_(std::move(deps)) {}

bool PreeditModeHandler::handleKey(IMContext &context, const KeyInfo &key,
                                   OpenKeyTextState &state) {
    auto commitPreeditAndMaybeAppend = [&](const std::string &suffixUtf8) {
        if (state.composing.empty()) {
            return false;
        }
        std::string out = state.composing;
        out += suffixUtf8;
        context.commitString(out);
        state.lastCommitted = state.composing;
        state.composing.clear();
        state.preeditKeyBuffer.clear();
        updatePreeditUI(context, state);
        return true;
    };

    if (keyIsCursorMove(key) || keyIs(key, kKeyDelete) || keyIs(key, kKeyTab)) {
        commitAndClearPreedit(context, state);
        return false;
    }

    if (keyIs(key, kKeyBackSpace)) {
        if (state.composing.empty()) {
            return false;
        }
        const auto len = fcitx::utf8::length(state.composing);
        if (len > 0) {
            auto it = fcitx::utf8::nextNChar(state.composing.begin(), len - 1);
            state.composing.erase(it, state.composing.end());
        } else {
            state.composing.clear();
        }
        if (!state.preeditKeyBuffer.empty()) {
            state.preeditKeyBuffer.pop_back();
        }
        updatePreeditUI(context, state);
        return true;
    }

    if (keyIs(key, kKeyEscape)) {
        if (state.composing.empty()) {
            return false;
        }
        state.composing.clear();
        state.preeditKeyBuffer.clear();
        updatePreeditUI(context, state);
        return true;
    }

    if (keyIs(key, kKeyReturn) || keyIs(key, kKeyKpEnter) ||
        keyIs(key, kKeyIsoEnter)) {
        commitAndClearPreedit(context, state);
        return false;
    }

    const uint32_t uni = keyUnicode(key);
    const std::string utf8 = keyUtf8(key);

    if (!state.composing.empty() && (utf8.empty() || uni > 0x7F)) {
        commitAndClearPreedit(context, state);
        return false;
    }

    auto expandMacroBeforeBoundary = [&](char trigger) {
        if (!deps_.enableMacro || !deps_.enableMacro() ||
            !isMacroTriggerKey(trigger) || state.composing.empty()) {
            return false;
        }
        std::string replacement;
        if (!deps_.adapter->expandMacro(state.composing, replacement)) {
            return false;
        }
        state.composing = std::move(replacement);
        return true;
    };

    auto restoreBeforeBoundary = [&](char trigger) {
        if (!deps_.restoreIfWrongSpelling || !deps_.restoreIfWrongSpelling() ||
            state.composing.empty()) {
            return false;
        }
        std::string restoredWord;
        const bool restored =
            !state.preeditKeyBuffer.empty()
                ? deps_.adapter->restoreFromRawAsciiOnWordBreak(
                      state.composing, state.preeditKeyBuffer, trigger,
                      restoredWord)
                : deps_.adapter->restoreOnWordBreak(state.composing, trigger,
                                                    restoredWord);
        if (!restored) {
            return false;
        }
        state.composing = std::move(restoredWord);
        return true;
    };

    if (uni < 0x20 || uni > 0x7E) {
        return false;
    }

    const char c = static_cast<char>(uni);
    if (c == ' ') {
        if (!expandMacroBeforeBoundary(c)) {
            restoreBeforeBoundary(c);
        }
        return commitPreeditAndMaybeAppend(" ");
    }

    if (!isComposingASCII(c)) {
        const std::string boundaryUtf8 = keyUtf8(key);
        if (!expandMacroBeforeBoundary(c)) {
            restoreBeforeBoundary(c);
        }
        if (!boundaryUtf8.empty()) {
            return commitPreeditAndMaybeAppend(boundaryUtf8);
        }
        commitAndClearPreedit(context, state);
        return false;
    }

    auto r = deps_.adapter->processAsciiKey(state.composing, c);
    if (!r.handled) {
        return false;
    }
    state.composing = std::move(r.newWord);
    state.preeditKeyBuffer.push_back(c);
    updatePreeditUI(context, state);
    return true;
}

void PreeditModeHandler::reset(OpenKeyTextState &state) {
    state.composing.clear();
    state.preeditKeyBuffer.clear();
}

void PreeditModeHandler::updatePreeditUI(IMContext &context,
                                         const OpenKeyTextState &state) {
    if (state.composing.empty()) {
        context.clearPreedit();
        return;
    }
    context.setPreedit(state.composing, static_cast<int>(state.composing.size()));
}

void PreeditModeHandler::commitAndClearPreedit(IMContext &context,
                                               OpenKeyTextState &state) {
    if (state.composing.empty()) {
        return;
    }
    context.commitString(state.composing);
    state.lastCommitted = state.composing;
    state.composing.clear();
    state.preeditKeyBuffer.clear();
    updatePreeditUI(context, state);
}

SurroundingTextModeHandler::SurroundingTextModeHandler(
    TextModeHandlerDeps deps)
    : deps_(std::move(deps)) {}

bool SurroundingTextModeHandler::handleKey(IMContext &context,
                                           const KeyInfo &key,
                                           OpenKeyTextState &state) {
    state.composing.clear();

    if (keyIs(key, kKeyDelete) || keyIsCursorMove(key)) {
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        clearRollbackSnapshot(state);
        state.noSeedNextWord = false;
        return false;
    }

    if (keyIs(key, kKeyBackSpace)) {
        if (!state.rollbackWord.empty()) {
            if (!fcitx::utf8::validate(state.rollbackWord) ||
                !fcitx::utf8::validate(state.rollbackDisplay)) {
                state.rollbackWord.clear();
                state.rollbackDisplay.clear();
                state.rollbackRawBuffer.clear();
                clearRollbackSnapshot(state);
                return false;
            }
            const auto len = fcitx::utf8::length(state.rollbackWord);
            if (len > 0) {
                auto it = fcitx::utf8::nextNChar(state.rollbackWord.begin(), len - 1);
                state.rollbackWord.erase(it, state.rollbackWord.end());
            } else {
                state.rollbackWord.clear();
            }

            std::string newDisplay = state.rollbackWord;
            const std::size_t prefixLen =
                commonPrefixBytesUTF8Boundary(state.rollbackDisplay, newDisplay);
            const unsigned int deleteChars =
                utf8CharCount(state.rollbackDisplay.substr(prefixLen));
            if (deleteChars > 128) {
                state.rollbackWord.clear();
                state.rollbackDisplay.clear();
                state.rollbackRawBuffer.clear();
                clearRollbackSnapshot(state);
                return false;
            }
            if (deleteChars > 0) {
                context.deleteSurroundingText(-static_cast<int>(deleteChars),
                                              deleteChars);
            }
            if (newDisplay.size() > prefixLen) {
                context.commitString(newDisplay.substr(prefixLen));
            }
            state.rollbackDisplay = std::move(newDisplay);
            if (!state.rollbackRawBuffer.empty()) {
                state.rollbackRawBuffer.pop_back();
            }
            if (state.rollbackDisplay.empty()) {
                state.rollbackRawBuffer.clear();
            }
            return true;
        }
        if (restoreRollbackSnapshotAfterBoundary(state)) {
            return false;
        }
        if (!state.macroBuffer.empty()) {
            state.macroBuffer.pop_back();
        }
        return false;
    }

    if (keyIs(key, kKeyReturn) || keyIs(key, kKeyKpEnter) ||
        keyIs(key, kKeyIsoEnter)) {
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        clearRollbackSnapshot(state);
        state.noSeedNextWord = false;
        return false;
    }

    const uint32_t uni = keyUnicode(key);
    if (!(uni >= 0x20 && uni <= 0x7E)) {
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        clearRollbackSnapshot(state);
        state.noSeedNextWord = false;
        return false;
    }
    const char c = static_cast<char>(uni);

    if (isComposingASCII(c) && !state.rollbackDisplay.empty()) {
        const auto st = context.getSurroundingText();
        if (!st.valid || st.cursor != st.anchor ||
            !fcitx::utf8::validate(st.text) ||
            st.cursor > fcitx::utf8::length(st.text)) {
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            clearRollbackSnapshot(state);
            return false;
        }
        WordSegment seg;
        if (!extractWordBeforeCursor(st.text, st.cursor, seg) ||
            seg.word != state.rollbackDisplay) {
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            clearRollbackSnapshot(state);
            return false;
        }
    }

    auto replaceRollbackDisplay = [&](const std::string &replacement) {
        const unsigned int deleteChars = utf8CharCount(state.rollbackDisplay);
        if (deleteChars == 0 || deleteChars > 128) {
            return false;
        }
        context.deleteSurroundingText(-static_cast<int>(deleteChars), deleteChars);
        context.commitString(replacement);
        state.rollbackWord = replacement;
        state.rollbackDisplay = replacement;
        state.rollbackRawBuffer.clear();
        state.lastCommitted = replacement;
        return true;
    };

    auto expandMacroBeforeBoundary = [&](char trigger) {
        if (!deps_.enableMacro || !deps_.enableMacro() ||
            !isMacroTriggerKey(trigger) || state.rollbackDisplay.empty()) {
            return false;
        }
        std::string replacement;
        if (!deps_.adapter->expandMacro(state.rollbackDisplay, replacement)) {
            return false;
        }
        return replaceRollbackDisplay(replacement);
    };

    auto restoreBeforeBoundary = [&](char trigger) {
        if (!deps_.restoreIfWrongSpelling || !deps_.restoreIfWrongSpelling() ||
            state.rollbackDisplay.empty()) {
            return false;
        }
        std::string restoredWord;
        const bool restored =
            !state.rollbackRawBuffer.empty()
                ? deps_.adapter->restoreFromRawAsciiOnWordBreak(
                      state.rollbackDisplay, state.rollbackRawBuffer, trigger,
                      restoredWord)
                : deps_.adapter->restoreOnWordBreak(state.rollbackDisplay,
                                                    trigger, restoredWord);
        if (!restored) {
            return false;
        }
        return replaceRollbackDisplay(restoredWord);
    };

    if (isBoundaryASCII(c)) {
        if (!expandMacroBeforeBoundary(c)) {
            restoreBeforeBoundary(c);
        }
        rememberRollbackSnapshot(state);
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        state.noSeedNextWord = true;
        return false;
    }

    if (!isComposingASCII(c)) {
        if (!expandMacroBeforeBoundary(c)) {
            restoreBeforeBoundary(c);
        }
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        clearRollbackSnapshot(state);
        state.noSeedNextWord = true;
        return false;
    }

    if (state.noSeedNextWord) {
        state.noSeedNextWord = false;
        clearRollbackSnapshot(state);
    } else if (state.rollbackWord.empty()) {
        const auto st = context.getSurroundingText();
        if (st.valid && st.cursor == st.anchor) {
            WordSegment seg;
            if (extractWordBeforeCursor(st.text, st.cursor, seg)) {
                state.rollbackWord = seg.word;
                state.rollbackDisplay = seg.word;
                state.rollbackRawBuffer.clear();
            }
        }
    }

    auto r = deps_.adapter->processAsciiKey(state.rollbackWord, c);
    if (!r.handled || !fcitx::utf8::validate(r.newWord)) {
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        clearRollbackSnapshot(state);
        return false;
    }

    const std::size_t prefixLen =
        commonPrefixBytesUTF8Boundary(state.rollbackDisplay, r.newWord);
    const unsigned int deleteChars =
        utf8CharCount(state.rollbackDisplay.substr(prefixLen));
    if (deleteChars > 128) {
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        clearRollbackSnapshot(state);
        return false;
    }
    if (deleteChars > 0) {
        context.deleteSurroundingText(-static_cast<int>(deleteChars), deleteChars);
    }
    if (r.newWord.size() > prefixLen) {
        context.commitString(r.newWord.substr(prefixLen));
    }

    state.rollbackWord = r.newWord;
    state.rollbackDisplay = r.newWord;
    state.rollbackRawBuffer.push_back(c);
    state.lastCommitted = state.rollbackDisplay;
    return true;
}

void SurroundingTextModeHandler::reset(OpenKeyTextState &state) {
    state.macroBuffer.clear();
    state.rollbackWord.clear();
    state.rollbackDisplay.clear();
    state.rollbackRawBuffer.clear();
    clearRollbackSnapshot(state);
    state.noSeedNextWord = false;
}

void SurroundingTextModeHandler::clearRollbackSnapshot(
    OpenKeyTextState &state) const {
    state.rollbackSnapshotWord.clear();
    state.rollbackSnapshotDisplay.clear();
    state.rollbackSnapshotRawBuffer.clear();
    state.canReseedRollbackSnapshot = false;
}

void SurroundingTextModeHandler::rememberRollbackSnapshot(
    OpenKeyTextState &state) const {
    if (deps_.enableBackspaceSnapshot && !deps_.enableBackspaceSnapshot()) {
        clearRollbackSnapshot(state);
        return;
    }
    if (state.rollbackDisplay.empty()) {
        clearRollbackSnapshot(state);
        return;
    }
    state.rollbackSnapshotWord = state.rollbackWord;
    state.rollbackSnapshotDisplay = state.rollbackDisplay;
    state.rollbackSnapshotRawBuffer = state.rollbackRawBuffer;
    state.canReseedRollbackSnapshot = true;
}

bool SurroundingTextModeHandler::restoreRollbackSnapshotAfterBoundary(
    OpenKeyTextState &state) const {
    if (deps_.enableBackspaceSnapshot && !deps_.enableBackspaceSnapshot()) {
        clearRollbackSnapshot(state);
        return false;
    }
    if (!state.canReseedRollbackSnapshot ||
        state.rollbackSnapshotDisplay.empty()) {
        return false;
    }
    state.rollbackWord = state.rollbackSnapshotWord;
    state.rollbackDisplay = state.rollbackSnapshotDisplay;
    state.rollbackRawBuffer = state.rollbackSnapshotRawBuffer;
    state.noSeedNextWord = false;
    clearRollbackSnapshot(state);
    return true;
}

} // namespace openkey
