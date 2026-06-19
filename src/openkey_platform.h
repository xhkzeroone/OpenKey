#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace openkey {

struct KeyInfo {
    uint32_t sym = 0;
    uint32_t rawSym = 0;
    uint32_t code = 0;
    uint32_t modifiers = 0;
    bool isRelease = false;
};

constexpr uint32_t kKeyBackSpace = 0xff08;
constexpr uint32_t kKeyTab = 0xff09;
constexpr uint32_t kKeyReturn = 0xff0d;
constexpr uint32_t kKeyEscape = 0xff1b;
constexpr uint32_t kKeyDelete = 0xffff;
constexpr uint32_t kKeyLeft = 0xff51;
constexpr uint32_t kKeyUp = 0xff52;
constexpr uint32_t kKeyRight = 0xff53;
constexpr uint32_t kKeyDown = 0xff54;
constexpr uint32_t kKeyHome = 0xff50;
constexpr uint32_t kKeyEnd = 0xff57;
constexpr uint32_t kKeyPageUp = 0xff55;
constexpr uint32_t kKeyPageDown = 0xff56;
constexpr uint32_t kKeyInsert = 0xff63;
constexpr uint32_t kKeyKpEnter = 0xff8d;
constexpr uint32_t kKeyIsoEnter = 0xfe34;
constexpr uint32_t kKeyShiftL = 0xffe1;
constexpr uint32_t kKeyShiftR = 0xffe2;

inline bool keyIs(const KeyInfo &key, uint32_t sym) {
    return key.sym == sym || key.rawSym == sym;
}

inline bool keyIsCursorMove(const KeyInfo &key) {
    return keyIs(key, kKeyLeft) || keyIs(key, kKeyRight) ||
           keyIs(key, kKeyUp) || keyIs(key, kKeyDown) ||
           keyIs(key, kKeyHome) || keyIs(key, kKeyEnd) ||
           keyIs(key, kKeyPageUp) || keyIs(key, kKeyPageDown);
}

inline uint32_t keyUnicode(const KeyInfo &key) {
    return (key.sym >= 0x20 && key.sym <= 0x7e) ? key.sym : 0;
}

inline std::string keyUtf8(const KeyInfo &key) {
    const uint32_t unicode = keyUnicode(key);
    if (unicode == 0) {
        return {};
    }
    return std::string(1, static_cast<char>(unicode));
}

struct SurroundingTextInfo {
    std::string text;
    unsigned int cursor = 0;
    unsigned int anchor = 0;
    bool valid = false;
};

using TimerHandle = uint64_t;

struct RewriteTiming {
    uint64_t interKeyUsec = 2000;
    uint64_t commitDelayUsec = 60000;
};

enum class BackspaceMethod {
    Uinput,
    None,
};

class IMContext {
public:
    virtual ~IMContext() = default;

    virtual void commitString(std::string_view text) = 0;
    virtual void deleteSurroundingText(int offsetChars,
                                       unsigned int nChars) = 0;
    virtual void forwardKey(const KeyInfo &key, bool isRelease) = 0;
    virtual void forwardKeyPressAndRelease(const KeyInfo &key) = 0;

    virtual void setPreedit(std::string_view text, int cursorBytePos) = 0;
    virtual void clearPreedit() = 0;
    virtual bool supportsPreedit() const = 0;

    virtual SurroundingTextInfo getSurroundingText() const = 0;
    virtual bool supportsSurroundingText() const = 0;

    virtual TimerHandle scheduleOnce(uint64_t delayUsec,
                                     std::function<void()> cb) = 0;
    virtual void cancelTimer(TimerHandle &handle) = 0;

    virtual std::string program() const = 0;
    virtual bool isX11() const = 0;
    virtual bool isPasswordField() const = 0;
    virtual bool isLegacyFrontend() const = 0;
};

class RewriteContext {
public:
    virtual ~RewriteContext() = default;
    virtual IMContext &imContext() = 0;

    virtual BackspaceMethod sendBackspaces(int count,
                                           uint64_t interKeyUsec) = 0;
    virtual bool scheduleRemoteRewrite(unsigned int deleteCount,
                                       uint64_t interBackspaceUsec,
                                       uint64_t commitDelayUsec) = 0;
    virtual bool trackedWordStillBeforeCursor(std::string_view shownText,
                                              bool requireSurroundingText) = 0;
    virtual bool looksLikeAutocomplete(std::string_view shownText) = 0;
};

} // namespace openkey
