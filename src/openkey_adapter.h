#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct vKeyHookState;

namespace openkey {

struct OpenKeyProcessResult {
    bool handled = false;
    std::string newWord;
};

// Small adapter around OpenKey core (Sources/OpenKey/engine) to work with
// text-based frameworks (commit/delete surrounding) instead of fake backspace.
class OpenKeyAdapter {
public:
    OpenKeyAdapter();

    // Configure globals in OpenKey core.
    void setInputType(int inputType);
    void setFreeMark(bool freeMark);
    void setCodeTable(int codeTable);
    void setCheckSpelling(bool checkSpelling);
    void setUseModernOrthography(bool enabled);
    void setQuickTelex(bool enabled);
    void setRestoreIfWrongSpelling(bool enabled);
    void setUpperCaseFirstChar(bool enabled);
    void setAllowConsonantZFWJ(bool enabled);
    void setQuickStartConsonant(bool enabled);
    void setQuickEndConsonant(bool enabled);
    void setUseMacro(bool enabled);
    void setUseMacroInEnglishMode(bool enabled);
    void setAutoCapsMacro(bool enabled);
    void setUseSmartSwitchKey(bool enabled);
    void setRememberCode(bool enabled);
    void setOtherLanguage(bool enabled);
    void setTempOffSpelling(bool enabled);
    void setTempOffOpenKey(bool enabled);

    // Try to expand a macro from a plain ASCII word (UTF-8).
    // Returns true and sets outReplacement if a macro matches.
    bool expandMacro(const std::string &asciiWord,
                     std::string &outReplacement) const;

    // Try the core "restore wrong spelling on word break" path.
    // Returns only the restored word; the caller still owns forwarding the
    // actual word-break key.
    bool restoreOnWordBreak(const std::string &currentWord, char breakChar,
                            std::string &outRestoredWord) const;

    // Convert a full raw ASCII Telex/VNI buffer into Vietnamese UTF-8.
    // This feeds characters into OpenKey core sequentially.
    std::string convertRawBuffer(const std::string &rawAscii) const;

    // Process a printable ASCII key (already layout-resolved).
    // currentWord is UTF-8, in Unicode precomposed form.
    OpenKeyProcessResult processAsciiKey(const std::string &currentWord,
                                         char asciiChar) const;

private:
    const vKeyHookState *hookState_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> unicodeToInternal_;
    // Map from legacy (TCVN3/VNI) 16-bit codes to Unicode codepoint.
    // Only used when vCodeTable != 0.
    mutable int legacyToUnicodeTable_ = -1;
    mutable std::unordered_map<uint16_t, uint16_t> legacyToUnicode_;

    void ensureReverseMap();
    void ensureLegacyToUnicodeMap() const;
    std::vector<uint32_t> encodeWordToInternal(const std::string &word) const;
    std::string engineCodeToUTF8(uint32_t code) const;
    static std::string utf8DropLastN(const std::string &s, size_t n);
};

} // namespace openkey
