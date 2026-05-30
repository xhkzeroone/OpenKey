#include "openkey_adapter.h"

#include <algorithm>

#include <fcitx-utils/utf8.h>

// OpenKey core.
#include "DataType.h"
#include "Engine.h"
#include "Macro.h"
#include "Vietnamese.h"

namespace openkey {
namespace {

static uint32_t markMaskFromIndex(int idx) {
    switch (idx) {
    case 0:
        return MARK1_MASK;
    case 1:
        return MARK2_MASK;
    case 2:
        return MARK3_MASK;
    case 3:
        return MARK4_MASK;
    case 4:
        return MARK5_MASK;
    default:
        return 0;
    }
}

static bool isAsciiHandled(char c) {
    // Vietnamese.cpp defines _characterMap as map<Uint32, Uint32>.
    return _characterMap.find(static_cast<uint32_t>(static_cast<unsigned char>(c))) !=
           _characterMap.end();
}

} // namespace

OpenKeyAdapter::OpenKeyAdapter() {
    hookState_ = static_cast<const vKeyHookState *>(vKeyInit());
    ensureReverseMap();
}

void OpenKeyAdapter::setInputType(int inputType) { vInputType = inputType; }
void OpenKeyAdapter::setFreeMark(bool freeMark) { vFreeMark = freeMark ? 1 : 0; }
void OpenKeyAdapter::setCodeTable(int codeTable) {
    if (vCodeTable == codeTable) {
        return;
    }
    vCodeTable = codeTable;
    legacyToUnicodeTable_ = -1;
    legacyToUnicode_.clear();
    // Macro content codes depend on current table.
    onTableCodeChange();
}
void OpenKeyAdapter::setCheckSpelling(bool checkSpelling) {
    vCheckSpelling = checkSpelling ? 1 : 0;
}
void OpenKeyAdapter::setUseModernOrthography(bool enabled) {
    vUseModernOrthography = enabled ? 1 : 0;
}
void OpenKeyAdapter::setQuickTelex(bool enabled) { vQuickTelex = enabled ? 1 : 0; }
void OpenKeyAdapter::setRestoreIfWrongSpelling(bool enabled) {
    vRestoreIfWrongSpelling = enabled ? 1 : 0;
}
void OpenKeyAdapter::setFixRecommendBrowser(bool enabled) {
    vFixRecommendBrowser = enabled ? 1 : 0;
}
void OpenKeyAdapter::setUpperCaseFirstChar(bool enabled) {
    vUpperCaseFirstChar = enabled ? 1 : 0;
}
void OpenKeyAdapter::setAllowConsonantZFWJ(bool enabled) {
    vAllowConsonantZFWJ = enabled ? 1 : 0;
}
void OpenKeyAdapter::setQuickStartConsonant(bool enabled) {
    vQuickStartConsonant = enabled ? 1 : 0;
}
void OpenKeyAdapter::setQuickEndConsonant(bool enabled) {
    vQuickEndConsonant = enabled ? 1 : 0;
}
void OpenKeyAdapter::setUseMacro(bool enabled) { vUseMacro = enabled ? 1 : 0; }
void OpenKeyAdapter::setUseMacroInEnglishMode(bool enabled) {
    vUseMacroInEnglishMode = enabled ? 1 : 0;
}
void OpenKeyAdapter::setAutoCapsMacro(bool enabled) {
    vAutoCapsMacro = enabled ? 1 : 0;
}
void OpenKeyAdapter::setUseSmartSwitchKey(bool enabled) {
    vUseSmartSwitchKey = enabled ? 1 : 0;
}
void OpenKeyAdapter::setRememberCode(bool enabled) { vRememberCode = enabled ? 1 : 0; }
void OpenKeyAdapter::setOtherLanguage(bool enabled) { vOtherLanguage = enabled ? 1 : 0; }
void OpenKeyAdapter::setTempOffSpelling(bool enabled) {
    vTempOffSpelling = enabled ? 1 : 0;
}
void OpenKeyAdapter::setTempOffOpenKey(bool enabled) {
    vTempOffOpenKey = enabled ? 1 : 0;
}

void OpenKeyAdapter::ensureReverseMap() {
    if (!unicodeToInternal_.empty()) {
        return;
    }

    // Build reverse map from Unicode precomposed (codeTable=0) to OpenKey
    // internal TypingWord encoding.
    for (const auto &kv : _codeTable[0]) {
        const uint32_t internalKey = kv.first;
        const auto &vec = kv.second;
        for (size_t idx = 0; idx < vec.size(); idx++) {
            const uint16_t uni16 = vec[idx];
            if (!uni16) {
                continue;
            }

            const bool isCaps = (idx % 2 == 0);
            const uint32_t capsMask = isCaps ? CAPS_MASK : 0;

            uint32_t internal = 0;
            if ((internalKey & CHAR_MASK) == KEY_D && vec.size() == 2) {
                // OpenKey represents Đ/đ as KEY_D with TONE_MASK.
                internal = KEY_D | TONE_MASK | capsMask;
            } else if (vec.size() == 14 &&
                       ((internalKey & CHAR_MASK) == KEY_A ||
                        (internalKey & CHAR_MASK) == KEY_O ||
                        (internalKey & CHAR_MASK) == KEY_U ||
                        (internalKey & CHAR_MASK) == KEY_E)) {
                const uint32_t baseKey = (internalKey & CHAR_MASK);
                if (idx < 4) {
                    // 0..3: ^ and w variants (CAPS/NORMAL pairs).
                    const bool isTone = (idx < 2);
                    internal = baseKey | (isTone ? TONE_MASK : TONEW_MASK) |
                               capsMask;
                } else {
                    // 4..13: mark variants without ^/w.
                    const int markIdx = static_cast<int>((idx - 4) / 2);
                    const uint32_t markMask = markMaskFromIndex(markIdx);
                    internal = baseKey | markMask | capsMask;
                }
            } else if (vec.size() == 10) {
                // mark variants for: A/O/U/E with tone flags (Â/Ă/Ơ/Ư/Ê...)
                // or I/Y marks.
                const int markIdx = static_cast<int>(idx / 2);
                const uint32_t markMask = markMaskFromIndex(markIdx);
                internal = internalKey | markMask | capsMask;
            } else {
                // Fallback: keep as pure unicode (should be word break anyway).
                continue;
            }

            unicodeToInternal_[static_cast<uint32_t>(uni16)] = internal;
        }
    }
}

void OpenKeyAdapter::ensureLegacyToUnicodeMap() const {
    if (vCodeTable == 0) {
        legacyToUnicode_.clear();
        legacyToUnicodeTable_ = 0;
        return;
    }
    if (legacyToUnicodeTable_ == vCodeTable && !legacyToUnicode_.empty()) {
        return;
    }
    legacyToUnicodeTable_ = vCodeTable;
    legacyToUnicode_.clear();

    const auto &legacy = _codeTable[vCodeTable];
    const auto &unicode = _codeTable[0];
    for (const auto &kv : legacy) {
        auto itU = unicode.find(kv.first);
        if (itU == unicode.end()) {
            continue;
        }
        const auto &legacyVec = kv.second;
        const auto &uniVec = itU->second;
        const size_t n = std::min(legacyVec.size(), uniVec.size());
        for (size_t idx = 0; idx < n; idx++) {
            const uint16_t legacyCode = legacyVec[idx];
            const uint16_t unicodeCode = uniVec[idx];
            if (!legacyCode || !unicodeCode) {
                continue;
            }
            legacyToUnicode_.emplace(legacyCode, unicodeCode);
        }
    }
}

std::vector<uint32_t>
OpenKeyAdapter::encodeWordToInternal(const std::string &word) const {
    std::vector<uint32_t> result;
    if (!fcitx::utf8::validate(word)) {
        return result;
    }
    for (auto it = fcitx::utf8::UTF8CharIterator(word.begin(), word.end()),
              end = fcitx::utf8::UTF8CharIterator(word.end(), word.end());
         it != end; ++it) {
        const uint32_t cp = *it;
        if (cp <= 0x7F) {
            const char c = static_cast<char>(cp);
            auto iter = _characterMap.find(static_cast<uint32_t>(
                static_cast<unsigned char>(c)));
            if (iter != _characterMap.end()) {
                result.push_back(iter->second);
                continue;
            }
        }
        auto iter = unicodeToInternal_.find(cp);
        if (iter != unicodeToInternal_.end()) {
            result.push_back(iter->second);
            continue;
        }
        // Stop at unsupported character; caller should treat word boundary.
        result.clear();
        return result;
    }
    return result;
}

std::string OpenKeyAdapter::engineCodeToUTF8(uint32_t code) const {
    if (code & PURE_CHARACTER_MASK) {
        const uint32_t cp = (code & ~PURE_CHARACTER_MASK);
        return fcitx::utf8::UCS4ToUTF8(cp);
    }
    if (code & CHAR_CODE_MASK) {
        uint32_t cp = (code & CHAR_MASK);
        if (vCodeTable != 0) {
            ensureLegacyToUnicodeMap();
            auto it = legacyToUnicode_.find(static_cast<uint16_t>(cp));
            if (it != legacyToUnicode_.end()) {
                cp = it->second;
            }
        }
        return fcitx::utf8::UCS4ToUTF8(cp);
    }

    // Key code path (unconverted character).
    const uint32_t keyCode = code & (CHAR_MASK | CAPS_MASK);
    const uint16_t ch = keyCodeToCharacter(keyCode);
    if (!ch) {
        return {};
    }
    return fcitx::utf8::UCS4ToUTF8(ch);
}

std::string OpenKeyAdapter::utf8DropLastN(const std::string &s, size_t n) {
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

bool OpenKeyAdapter::expandMacro(const std::string &asciiWord,
                                std::string &outReplacement) const {
    if (asciiWord.empty() || !fcitx::utf8::validate(asciiWord)) {
        return false;
    }

    std::vector<uint32_t> key;
    key.reserve(asciiWord.size());
    for (unsigned char ch : asciiWord) {
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
        auto it = _characterMap.find(static_cast<uint32_t>(ch));
        if (it == _characterMap.end()) {
            return false;
        }
        key.push_back(it->second);
    }

    std::vector<uint32_t> macroContentCode;
    if (!findMacro(key, macroContentCode)) {
        return false;
    }

    std::string out;
    for (const auto code : macroContentCode) {
        out += engineCodeToUTF8(code);
    }
    if (!fcitx::utf8::validate(out)) {
        return false;
    }
    outReplacement = std::move(out);
    return true;
}

OpenKeyProcessResult OpenKeyAdapter::processAsciiKey(const std::string &currentWord,
                                                    char asciiChar) const {
    OpenKeyProcessResult result;
    if (!isAsciiHandled(asciiChar)) {
        return result;
    }

    // UniKey-like usability quirk:
    // OpenKey core treats standalone 'w' as ư, but the second 'w' restores to
    // "uw". For typical Telex expectations, allow "ww" to yield "w".
    if (vInputType == vTelex &&
        (asciiChar == 'w' || asciiChar == 'W') &&
        (currentWord == u8"ư" || currentWord == u8"Ư")) {
        result.handled = true;
        result.newWord = std::string(1, asciiChar);
        return result;
    }

    const auto internalWord = encodeWordToInternal(currentWord);
    if (!currentWord.empty() && internalWord.empty()) {
        // Unsupported / non-Vietnamese word segment.
        return result;
    }

    std::vector<uint32_t> word = internalWord;
    if (word.size() > MAX_BUFF) {
        word.resize(MAX_BUFF);
    }

    vSetCurrentWord(word.empty() ? nullptr : word.data(),
                    static_cast<Uint8>(word.size()));

    // Map ascii char to OpenKey keycode + caps status.
    const auto it = _characterMap.find(static_cast<uint32_t>(
        static_cast<unsigned char>(asciiChar)));
    if (it == _characterMap.end()) {
        return result;
    }
    const uint32_t internalKey = it->second;
    const Uint16 data = static_cast<Uint16>(internalKey & CHAR_MASK);
    const Uint8 capsStatus = (internalKey & CAPS_MASK) ? 1 : 0;

    vKeyHandleEvent(vKeyEvent::Keyboard, vKeyEventState::KeyDown, data,
                    capsStatus, false);

    // Derive new word by applying hook state output.
    // For vDoNothing, OpenKey expects caller to just emit the raw key.
    std::string newWord = currentWord;
    if (hookState_->code == vDoNothing) {
        newWord += asciiChar;
    } else if (hookState_->code == vWillProcess ||
               hookState_->code == vRestore ||
               hookState_->code == vRestoreAndStartNewSession) {
        newWord = utf8DropLastN(currentWord, hookState_->backspaceCount);
        for (int i = hookState_->newCharCount - 1; i >= 0; i--) {
            newWord += engineCodeToUTF8(hookState_->charData[i]);
        }
        if (hookState_->code == vRestore ||
            hookState_->code == vRestoreAndStartNewSession) {
            newWord += asciiChar;
        }
    } else {
        // Other codes (macro) are handled at higher level (word break).
        return result;
    }

    result.handled = true;
    result.newWord = std::move(newWord);
    return result;
}

std::string OpenKeyAdapter::convertRawBuffer(const std::string &rawAscii) const {
    std::string word;
    if (rawAscii.empty()) {
        return word;
    }
    // rawAscii must be plain ASCII (Telex/VNI typing sequence).
    // We intentionally do not treat UTF-8 multibyte here.
    for (unsigned char ch : rawAscii) {
        if (ch < 0x20 || ch > 0x7E) {
            // Stop at unsupported char; return best-effort.
            break;
        }
        const char c = static_cast<char>(ch);
        auto r = processAsciiKey(word, c);
        if (r.handled) {
            word = std::move(r.newWord);
        } else {
            word.push_back(c);
        }
    }
    if (!fcitx::utf8::validate(word)) {
        return {};
    }
    return word;
}

} // namespace openkey
