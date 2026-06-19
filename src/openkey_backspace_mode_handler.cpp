#include "openkey_backspace_mode_handler.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>

#include "openkey.h"
#include "openkey_adapter.h"

namespace openkey {
namespace {

static std::string utf8DropLastN(const std::string &s, size_t n) {
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

static bool isComposingASCII(char c) {
    // Restrict to keys relevant to OpenKey Telex/VNI processing.
    // (Avoid swallowing punctuation that should be handled by application.)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    if (c >= '0' && c <= '9') {
        return true;
    }
    if (c == '[' || c == ']') {
        return true;
    }
    return false;
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

static char toLowerASCII(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

static bool endsWithASCIIInsensitive(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char a = toLowerASCII(s[s.size() - n + i]);
        const char b = toLowerASCII(suffix[i]);
        if (a != b) {
            return false;
        }
    }
    return true;
}

static std::string asciiLower(std::string s) {
    for (char &c : s) {
        c = toLowerASCII(c);
    }
    return s;
}

static std::string programBaseName(std::string program) {
    const auto pos = program.find_last_of('/');
    if (pos != std::string::npos) {
        program.erase(0, pos + 1);
    }
    if (endsWithASCIIInsensitive(program, ".desktop")) {
        program.resize(program.size() - std::strlen(".desktop"));
    }
    return program;
}

static std::string normalizedProgramName(const std::string &program) {
    return asciiLower(programBaseName(program));
}

static bool isFirefoxLikeProgram(const std::string &program) {
    if (program.empty()) {
        return false;
    }

    const std::string base = normalizedProgramName(program);
    static const std::vector<std::string> kFirefoxPatterns = {
        "firefox",
        "librewolf",
        "waterfox",
        "floorp",
        "zen",
        "tor-browser",
        "mullvad",
        "icecat",
    };

    for (const auto &pattern : kFirefoxPatterns) {
        if (base.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool needsTransientResetPreserve(const std::string &program) {
    return isFirefoxLikeProgram(program);
}

static bool isBrowserLikeProgram(const std::string &program) {
    if (program.empty()) {
        return false;
    }

    const std::string base = normalizedProgramName(program);
    static const std::vector<std::string> kBrowserPatterns = {
        "chrome",   "chromium", "edge",     "msedge",   "brave",
        "vivaldi",  "opera",    "coccoc",   "yandex",   "firefox",
        "librewolf","waterfox", "floorp",   "zen",      "epiphany",
        "falkon",   "midori",   "qutebrowser", "palemoon", "basilisk",
        "nyxt",     "otter",    "dooble",   "messenger", "helium",
        "arc",       "mullvad",  "tor-browser", "torbrowser",
        "code",      "code-oss", "vscode",   "codium",  "vscodium",
        "vs-code",   "vs_code",  "vscoe",    "cursor",  "windsurf",
        "antigravity", "kiro",   "trae",     "opencode", "open-code",
        "claude-code", "codex",  "codex-cli", "gemini-cli",
        "qwen-code", "aider",    "devin",    "devin-desktop",
        "zed",       "zed-editor", "void",   "void-editor", "pearai",
        "nimbalyst", "tabby",    "tabnine",  "continue", "cline",
        "roo-code",  "github-copilot", "copilot", "replit",
        "idea",      "intellij", "fleet",
        "pycharm",   "webstorm", "phpstorm", "clion",   "goland",
        "rubymine",  "rider",    "datagrip", "android-studio",
        "dataspell", "rustrover", "aqua",    "appcode", "studio",
        "eclipse",   "netbeans", "qtcreator", "qt-creator",
        "kdevelop",  "gnome-builder", "builder", "geany", "codeblocks",
        "code::blocks", "sublime_text", "sublime-text", "sublime",
        "atom",      "lapce",    "lite-xl",  "lite_xl", "nova",
        "kate",      "gedit",    "gnome-text-editor", "text-editor",
        "texteditor", "emacs",   "vim",      "nvim",    "neovim",
        "helix",     "hx",
        "window:",
    };

    for (const auto &pattern : kBrowserPatterns) {
        if (base.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool isElectronLikeProgram(const std::string &program) {
    if (program.empty()) {
        return false;
    }

    const std::string base = normalizedProgramName(program);
    static const std::vector<std::string> kElectronPatterns = {
        "electron", "code",     "vscode",   "codium",  "cursor",
        "windsurf", "discord",  "slack",    "teams",   "obsidian",
        "notion",   "signal",   "element",  "postman", "insomnia",
        "figma",    "caprine",  "mattermost",
    };

    for (const auto &pattern : kElectronPatterns) {
        if (base.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Delta timing tuned based on NonPreedit values
// Format: {interKeyUsec, commitDelayUsec}
static constexpr RewriteTiming kDeltaWaylandTiming{10000 , 50000};
static constexpr RewriteTiming kDeltaWaylandBrowserTiming{10000, 50000};
static constexpr RewriteTiming kDeltaWaylandElectronTiming{10000, 50000};
static constexpr RewriteTiming kDeltaX11Timing{10000, 80000};
static constexpr RewriteTiming kDeltaWaylandFcitx4Timing{10000, 50000};
static constexpr RewriteTiming kDeltaX11Fcitx4Timing{10000, 80000};
static constexpr RewriteTiming kDeltaX11BrowserTiming{10000, 80000};
static constexpr RewriteTiming kDeltaX11FirefoxFarmilyTiming{30000, 80000};
static constexpr RewriteTiming kDeltaWaylandFirefoxFarmilyTiming{20000, 50000};
static constexpr uint64_t kDeltaPostCommitPumpDelayUsec = 10000;

static constexpr RewriteTiming kNonPreeditWaylandTiming{10000, 30000};
static constexpr RewriteTiming kNonPreeditWaylandFirefoxFarmilyTiming{20000, 50000};
static constexpr RewriteTiming kNonPreeditX11Timing{10000, 80000};
static constexpr RewriteTiming kNonPreeditX11BrowserTiming{10000, 80000};
static constexpr RewriteTiming kNonPreeditX11FirefoxFarmilyTiming{30000, 80000};
static constexpr uint64_t kNonPreeditPostCommitPumpDelayUsec = 10000;

static RewriteTiming deltaTimingFor(const IMContext &context,
                                    const std::string &program) {
    const bool x11 = context.isX11();
    const bool fcitx4 = context.isLegacyFrontend();

    if (x11 && fcitx4) {
        return kDeltaX11Fcitx4Timing;
    }
    if (!x11 && fcitx4) {
        return kDeltaWaylandFcitx4Timing;
    }
    if(x11 && isFirefoxLikeProgram(program)) {
        return kDeltaX11FirefoxFarmilyTiming;
    }

    if(!x11 && isFirefoxLikeProgram(program)) {
        return kDeltaWaylandFirefoxFarmilyTiming;
    }
    if (!x11 && isBrowserLikeProgram(program)) {
        return kDeltaWaylandBrowserTiming;
    }
    if (!x11 && isElectronLikeProgram(program)) {
        return kDeltaWaylandElectronTiming;
    }
    if (x11 && isBrowserLikeProgram(program)) {
        return kDeltaX11BrowserTiming;
    }
    if (x11) {
        return kDeltaX11Timing;
    }
    return kDeltaWaylandTiming;
}

static RewriteTiming nonPreeditTimingFor(const IMContext &context,
                                         const std::string &program) {
    const bool x11 = context.isX11();

    if(x11 && isFirefoxLikeProgram(program)) {
        return kNonPreeditX11FirefoxFarmilyTiming;
    }

    if(!x11 && isFirefoxLikeProgram(program)) {
        return kNonPreeditWaylandFirefoxFarmilyTiming;
    }

    if (x11 && isBrowserLikeProgram(program)) {
        return kNonPreeditX11BrowserTiming;
    }

    if (x11) {
        return kNonPreeditX11Timing;
    }
    return kNonPreeditWaylandTiming;
}

static std::size_t commonPrefixBytesUTF8Boundary(const std::string &s1,
                                                 const std::string &s2) {
    std::size_t n = std::min(s1.size(), s2.size());
    std::size_t i = 0;
    while (i < n && s1[i] == s2[i]) {
        ++i;
    }
    // Ensure i is a valid UTF-8 boundary in s1.
    // If i points into a continuation byte (10xxxxxx), backtrack.
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

class BackspaceRewriteModeHandler final : public BackspaceModeHandler {
public:
    explicit BackspaceRewriteModeHandler(BackspaceModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(std::shared_ptr<RewriteContext> context, const KeyInfo &key,
               FcitxOpenKeyState &state) override {
        auto isBackspace = [&]() {
            return keyIs(key, kKeyBackSpace);
        };

        if (key.isRelease) {
            return false;
        }

        const auto adapterShared = deps_.adapter;
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        auto &deltaState = state.delta;

        if (deltaState.waitingBackspaceAck) {
            if (isBackspace()) {
                deltaState.seenBackspaces++;
                if (deltaState.seenBackspaces < deltaState.expectedBackspaces) {
                    return false;
                }

                context->imContext().cancelTimer(deltaState.ackTimeoutTimer);

                const RewriteTiming timing =
                    deltaTimingFor(context->imContext(), state.program);
                scheduleFinishPendingBackspaceCommit(context, state,
                                                     timing.commitDelayUsec);
                return true;
            }

            deltaState.queuedKeys.push_back(key);
            return true;
        }

        if (deltaState.rewriteLock) {
            deltaState.queuedKeys.push_back(key);
            return true;
        }

        if (isBackspace()) {
            if (needsTransientResetPreserve(state.program) &&
                !deltaState.shownText.empty() &&
                !context->trackedWordStillBeforeCursor(deltaState.shownText,
                                                      false)) {
                clearWordState(deltaState);
                return false;
            }
            if (restoreBackspaceSnapshot(deltaState)) {
                return true;
            }
            if (!deltaState.shownText.empty()) {
                deltaState.shownText = utf8DropLastN(deltaState.shownText, 1);
                if (!deltaState.rawAsciiBuffer.empty()) {
                    deltaState.rawAsciiBuffer.pop_back();
                }
                if (deltaState.shownText.empty()) {
                    deltaState.rawAsciiBuffer.clear();
                    deltaState.hasRewrittenCurrentWord = false;
                }
                deltaState.allowTransientResetPreserve =
                    !deltaState.shownText.empty();
            }
            return false;
        }

        if (keyIs(key, kKeyShiftL) || keyIs(key, kKeyShiftR)) {
            return false;
        }

        const uint32_t uni = keyUnicode(key);

        if (!(uni >= 0x20 && uni <= 0x7E)) {
            clearWordState(deltaState);
            return false;
        }

        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);
            if (!isComposingASCII(c)) {
                const bool handled =
                    processQueuedKey(context, state, key, adapterShared, debug);
                return handled;
            }

            deltaState.queuedKeys.push_back(key);
            pumpQueue(context, state, adapterShared, debug);
            return true;
        }

        if (keyIsCursorMove(key) || keyIs(key, kKeyDelete) ||
            keyIs(key, kKeyEscape)) {
            clearWordState(deltaState);
        }

        return false;
    }

private:
    bool backspaceSnapshotEnabled() const {
        return !deps_.enableBackspaceSnapshot ||
               deps_.enableBackspaceSnapshot();
    }

    void clearBackspaceSnapshot(DeltaRewriteState &deltaState) const {
        const bool hadSnapshot =
            deltaState.canReseedFromBackspaceSnapshot ||
            !deltaState.backspaceSnapshotShownText.empty() ||
            !deltaState.backspaceSnapshotRawAsciiBuffer.empty() ||
            deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace;
        if (hadSnapshot && deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot clear"
                         << " mode=backspace"
                         << " shown=" << deltaState.shownText
                         << " snapshotShown="
                         << deltaState.backspaceSnapshotShownText
                         << " snapshotRaw="
                         << deltaState.backspaceSnapshotRawAsciiBuffer
                         << " canReseed="
                         << deltaState.canReseedFromBackspaceSnapshot
                         << " preserveAfterBoundaryBackspace="
                         << deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace;
        }
        deltaState.backspaceSnapshotShownText.clear();
        deltaState.backspaceSnapshotRawAsciiBuffer.clear();
        deltaState.backspaceSnapshotHasRewrittenCurrentWord = false;
        deltaState.canReseedFromBackspaceSnapshot = false;
        deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace = false;
        deltaState.allowBackspaceSnapshotResetPreserve = false;
    }

    void rememberBackspaceSnapshot(DeltaRewriteState &deltaState) const {
        if (!backspaceSnapshotEnabled()) {
            clearBackspaceSnapshot(deltaState);
            return;
        }
        if (deltaState.shownText.empty()) {
            if (deltaState.canReseedFromBackspaceSnapshot &&
                !deltaState.backspaceSnapshotShownText.empty()) {
                deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
                if (deps_.debugEnabled && deps_.debugEnabled()) {
                    FCITX_INFO() << "openkey: bs-snapshot remember-boundary"
                                 << " mode=backspace"
                                 << " snapshotShown="
                                 << deltaState.backspaceSnapshotShownText
                                 << " snapshotRaw="
                                 << deltaState.backspaceSnapshotRawAsciiBuffer;
                }
                return;
            }
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot remember-skip"
                             << " mode=backspace"
                             << " reason=empty-shown"
                             << " snapshotShown="
                             << deltaState.backspaceSnapshotShownText
                             << " canReseed="
                             << deltaState.canReseedFromBackspaceSnapshot;
            }
            clearBackspaceSnapshot(deltaState);
            return;
        }
        deltaState.backspaceSnapshotShownText = deltaState.shownText;
        deltaState.backspaceSnapshotRawAsciiBuffer = deltaState.rawAsciiBuffer;
        deltaState.backspaceSnapshotHasRewrittenCurrentWord =
            deltaState.hasRewrittenCurrentWord;
        deltaState.canReseedFromBackspaceSnapshot = true;
        deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
        if (deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot remember"
                         << " mode=backspace"
                         << " shown=" << deltaState.shownText
                         << " raw=" << deltaState.rawAsciiBuffer
                         << " rewritten="
                         << deltaState.hasRewrittenCurrentWord;
        }
    }

    bool restoreBackspaceSnapshot(DeltaRewriteState &deltaState) const {
        if (!backspaceSnapshotEnabled()) {
            clearBackspaceSnapshot(deltaState);
            return false;
        }
        if (!deltaState.canReseedFromBackspaceSnapshot ||
            !deltaState.shownText.empty() ||
            deltaState.backspaceSnapshotShownText.empty()) {
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot restore-miss"
                             << " mode=backspace"
                             << " shown=" << deltaState.shownText
                             << " snapshotShown="
                             << deltaState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << deltaState.backspaceSnapshotRawAsciiBuffer
                             << " canReseed="
                             << deltaState.canReseedFromBackspaceSnapshot;
            }
            return false;
        }
        deltaState.shownText = deltaState.backspaceSnapshotShownText;
        deltaState.rawAsciiBuffer = deltaState.backspaceSnapshotRawAsciiBuffer;
        deltaState.hasRewrittenCurrentWord =
            deltaState.backspaceSnapshotHasRewrittenCurrentWord;
        deltaState.restoredFromBackspaceSnapshot = true;
        deltaState.allowBackspaceSnapshotResetPreserve = true;
        if (deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot restore"
                         << " mode=backspace"
                         << " shown=" << deltaState.shownText
                         << " raw=" << deltaState.rawAsciiBuffer
                         << " rewritten="
                         << deltaState.hasRewrittenCurrentWord;
        }
        clearBackspaceSnapshot(deltaState);
        deltaState.allowBackspaceSnapshotResetPreserve = true;
        return true;
    }

    void clearWordState(DeltaRewriteState &deltaState,
                        bool clearSnapshot = true) const {
        deltaState.shownText.clear();
        deltaState.rawAsciiBuffer.clear();
        deltaState.hasRewrittenCurrentWord = false;
        deltaState.restoredFromBackspaceSnapshot = false;
        deltaState.allowBackspaceSnapshotResetPreserve = false;
        deltaState.allowTransientResetPreserve = false;
        deltaState.rewriteLock = false;
        deltaState.waitingBackspaceAck = false;
        deltaState.processingQueue = false;
        deltaState.expectedBackspaces = 0;
        deltaState.seenBackspaces = 0;
        deltaState.queuedKeys.clear();
        deltaState.commitTimer = 0;
        deltaState.ackTimeoutTimer = 0;
        deltaState.pendingConvertedText.clear();
        deltaState.pendingShownTextAfterCommit.clear();
        if (clearSnapshot) {
            clearBackspaceSnapshot(deltaState);
        }
    }

    void cancelTimers(IMContext &imContext, DeltaRewriteState &deltaState) const {
        imContext.cancelTimer(deltaState.commitTimer);
        imContext.cancelTimer(deltaState.ackTimeoutTimer);
    }

    void scheduleFinishPendingBackspaceCommit(std::shared_ptr<RewriteContext> context,
                                          FcitxOpenKeyState &state,
                                          uint64_t commitDelayUsec) {
        auto &deltaState = state.delta;

        if (commitDelayUsec == 0) {
            finishPendingBackspaceCommit(context, state);
            return;
        }

        context->imContext().cancelTimer(deltaState.ackTimeoutTimer);
        deltaState.ackTimeoutTimer = context->imContext().scheduleOnce(
            commitDelayUsec, [this, context, &state]() {
                state.delta.ackTimeoutTimer = 0;
                finishPendingBackspaceCommit(context, state);
            });
        if (deltaState.ackTimeoutTimer == 0) {
            finishPendingBackspaceCommit(context, state);
        }
    }

    void finishPendingBackspaceCommit(std::shared_ptr<RewriteContext> context,
                                      FcitxOpenKeyState &state) {
        auto &deltaState = state.delta;
        const std::string commitText = std::move(deltaState.pendingConvertedText);
        const std::string shownAfter =
            std::move(deltaState.pendingShownTextAfterCommit);
        context->imContext().cancelTimer(deltaState.commitTimer);
        deltaState.pendingConvertedText.clear();
        deltaState.pendingShownTextAfterCommit.clear();

        if (!commitText.empty()) {
            const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
            if (debug) {
                FCITX_INFO() << "openkey: bs-delta commit"
                             << " program=" << state.program;
            }
            context->imContext().commitString(commitText);
        }
        deltaState.shownText = shownAfter;
        deltaState.allowTransientResetPreserve = !deltaState.shownText.empty();
        deltaState.waitingBackspaceAck = false;
        deltaState.expectedBackspaces = 0;
        deltaState.seenBackspaces = 0;
        context->imContext().cancelTimer(deltaState.ackTimeoutTimer);
        if (deltaState.shownText.empty()) {
            deltaState.rawAsciiBuffer.clear();
            deltaState.hasRewrittenCurrentWord = false;
        }

        if (!commitText.empty() && schedulePostCommitPump(context, state)) {
            return;
        }

        finishPostCommitPump(context, state);
    }

    bool schedulePostCommitPump(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state) {
        auto &deltaState = state.delta;
        if (kDeltaPostCommitPumpDelayUsec == 0) {
            return false;
        }

        deltaState.commitTimer = context->imContext().scheduleOnce(
            kDeltaPostCommitPumpDelayUsec, [this, context, &state]() {
                state.delta.commitTimer = 0;
                finishPostCommitPump(context, state);
            });
        return deltaState.commitTimer != 0;
    }

    void finishPostCommitPump(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state) {
        auto &deltaState = state.delta;
        context->imContext().cancelTimer(deltaState.commitTimer);
        deltaState.rewriteLock = false;
        pumpQueue(context, state, deps_.adapter,
                  deps_.debugEnabled ? deps_.debugEnabled() : false);
    }

    void scheduleAckTimeout(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state,
                            const RewriteTiming &timing, int deleteCount) {
        auto &deltaState = state.delta;
        context->imContext().cancelTimer(deltaState.ackTimeoutTimer);

        // Calculate dynamic ACK timeout: (deleteCount - 1) × interKeyUsec + buffer
        // Note: commitDelayUsec is applied separately AFTER receiving ACKs
        const uint64_t injectTime = deleteCount > 1 ? (deleteCount - 1) * timing.interKeyUsec : 0;
        const uint64_t buffer = 50000; // 50ms buffer for app to process backspaces
        const uint64_t dynamicTimeout = injectTime + buffer;
        
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        if (debug) {
            FCITX_INFO() << "openkey: bs-delta ack-timeout scheduled"
                         << " program=" << state.program
                         << " deletes=" << deleteCount
                         << " injectTime=" << injectTime
                         << " buffer=" << buffer
                         << " timeout=" << dynamicTimeout;
        }
        
        deltaState.ackTimeoutTimer = context->imContext().scheduleOnce(
            dynamicTimeout, [this, context, &state]() {
                if (!state.delta.waitingBackspaceAck) {
                    return;
                }
                state.delta.ackTimeoutTimer = 0;
                FCITX_INFO() << "openkey: bs-delta ack timeout"
                             << " program=" << state.program
                             << " seen=" << state.delta.seenBackspaces
                             << " expected=" << state.delta.expectedBackspaces;
                const RewriteTiming timing =
                    deltaTimingFor(context->imContext(), state.program);
                scheduleFinishPendingBackspaceCommit(context, state,
                                                     timing.commitDelayUsec);
            });
    }

    bool applyWordDelta(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state,
                        bool debug, const std::string &newWord, char asciiChar,
                        const char *reason,
                        bool compareWithRawAppend = true) {
        auto &deltaState = state.delta;
        if (!fcitx::utf8::validate(deltaState.shownText) ||
            !fcitx::utf8::validate(newWord)) {
            clearWordState(deltaState);
            return false;
        }

        const std::string oldShown = deltaState.shownText;
        const std::string rawAppend =
            compareWithRawAppend ? oldShown + asciiChar : oldShown;
        const RewriteTiming timing =
            deltaTimingFor(context->imContext(), state.program);
        const std::size_t prefixLen =
            commonPrefixBytesUTF8Boundary(deltaState.shownText, newWord);
        unsigned int deleteCount =
            utf8CharCount(deltaState.shownText.substr(prefixLen));
        std::string commitText = newWord.substr(prefixLen);
        const bool browserAutocomplete =
            deleteCount > 0 &&
            isBrowserLikeProgram(state.program) &&
            !deltaState.hasRewrittenCurrentWord &&
            context->looksLikeAutocomplete(deltaState.shownText);
        if (browserAutocomplete) {
            deleteCount += 1;
        }
        if (deleteCount > 128) {
            deleteCount = utf8CharCount(deltaState.shownText);
            commitText = newWord;
        }

        if (debug) {
            FCITX_INFO() << "openkey: bs-delta program=" << state.program
                         << " reason=" << reason
                         << " from=" << deltaState.shownText
                         << " to=" << newWord
                         << " delete=" << deleteCount
                         << " commit=" << commitText
                         << " inter=" << timing.interKeyUsec
                         << " commitDelay=" << timing.commitDelayUsec
                         << " autocomplete=" << browserAutocomplete
                         << " queued=" << deltaState.queuedKeys.size();
        }

        if (deleteCount == 0) {
            if (!commitText.empty()) {
                context->imContext().commitString(commitText);
            }
            deltaState.shownText = newWord;
            deltaState.hasRewrittenCurrentWord =
                deltaState.hasRewrittenCurrentWord || (newWord != rawAppend);
            deltaState.restoredFromBackspaceSnapshot = false;
            deltaState.allowBackspaceSnapshotResetPreserve = false;
            deltaState.allowTransientResetPreserve = true;
            return true;
        }

        const auto method = context->sendBackspaces(
            static_cast<int>(deleteCount), timing.interKeyUsec);

        if (method == BackspaceMethod::Uinput) {
            // One extra backspace acts as a sentinel: app receives the first N,
            // OpenKey consumes the last one and commits when it arrives.
            context->sendBackspaces(1, timing.interKeyUsec);
            deltaState.rewriteLock = true;
            deltaState.waitingBackspaceAck = true;
            deltaState.expectedBackspaces = static_cast<int>(deleteCount) + 1;
            deltaState.seenBackspaces = 0;
            deltaState.pendingConvertedText = std::move(commitText);
            deltaState.pendingShownTextAfterCommit = newWord;
            deltaState.hasRewrittenCurrentWord =
                deltaState.hasRewrittenCurrentWord || (newWord != rawAppend);
            deltaState.restoredFromBackspaceSnapshot = false;
            deltaState.allowBackspaceSnapshotResetPreserve = false;
            deltaState.allowTransientResetPreserve = true;
            scheduleAckTimeout(context, state, timing,
                               static_cast<int>(deleteCount));
            return true;
        }

        clearWordState(deltaState);
        return false;
    }

    bool maybeExpandMacroBeforeBoundary(std::shared_ptr<RewriteContext> context,
                                        FcitxOpenKeyState &state,
                                        const KeyInfo &boundaryKey,
                                        std::shared_ptr<OpenKeyAdapter> adapterShared,
                                        bool debug, char trigger) {
        auto &deltaState = state.delta;
        if (!deps_.enableMacro || !deps_.enableMacro() || !adapterShared ||
            !isMacroTriggerKey(trigger) || deltaState.shownText.empty()) {
            return false;
        }

        adapterShared->setCodeTable(state.codeTable);
        std::string replacement;
        if (!adapterShared->expandMacro(deltaState.shownText, replacement)) {
            return false;
        }

        if (!applyWordDelta(context, state, debug, replacement, trigger, "macro",
                            false)) {
            return false;
        }

        if (deltaState.hasPendingRewrite()) {
            deltaState.queuedKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(deltaState);
            context->imContext().forwardKeyPressAndRelease(boundaryKey);
            clearWordState(deltaState, false);
            deltaState.allowBackspaceSnapshotResetPreserve =
                needsTransientResetPreserve(state.program) && trigger == ' ';
        }
        return true;
    }

    bool maybeRestoreBeforeBoundary(std::shared_ptr<RewriteContext> context,
                                    FcitxOpenKeyState &state,
                                    const KeyInfo &boundaryKey,
                                    std::shared_ptr<OpenKeyAdapter> adapterShared,
                                    bool debug, char trigger) {
        auto &deltaState = state.delta;
        if (!deps_.restoreIfWrongSpelling ||
            !deps_.restoreIfWrongSpelling() || !adapterShared ||
            deltaState.shownText.empty()) {
            return false;
        }

        adapterShared->setCodeTable(state.codeTable);
        std::string restoredWord;
        const bool restored =
            !deltaState.rawAsciiBuffer.empty()
                ? adapterShared->restoreFromRawAsciiOnWordBreak(
                      deltaState.shownText, deltaState.rawAsciiBuffer, trigger,
                      restoredWord)
                : adapterShared->restoreOnWordBreak(deltaState.shownText, trigger,
                                                   restoredWord);
        if (!restored) {
            return false;
        }

        if (!applyWordDelta(context, state, debug, restoredWord, trigger,
                            "restore-boundary", false)) {
            return false;
        }

        if (deltaState.hasPendingRewrite()) {
            deltaState.queuedKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(deltaState);
            context->imContext().forwardKeyPressAndRelease(boundaryKey);
            clearWordState(deltaState, false);
            deltaState.allowBackspaceSnapshotResetPreserve =
                needsTransientResetPreserve(state.program) && trigger == ' ';
        }
        return true;
    }

    bool processQueuedKey(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state,
                          const KeyInfo &key,
                          std::shared_ptr<OpenKeyAdapter> adapterShared,
                          bool debug) {
        auto &deltaState = state.delta;

        if (keyIsCursorMove(key) || keyIs(key, kKeyDelete)) {
            clearWordState(deltaState);
            return false;
        }

        if (keyIs(key, kKeyEscape)) {
            clearWordState(deltaState);
            return false;
        }

        if (keyIs(key, kKeyBackSpace)) {
            if (needsTransientResetPreserve(state.program) &&
                !deltaState.shownText.empty() &&
                !context->trackedWordStillBeforeCursor(deltaState.shownText,
                                                      false)) {
                clearWordState(deltaState);
                return false;
            }
            if (!restoreBackspaceSnapshot(deltaState)) {
                clearWordState(deltaState);
            }
            return false; // let app handle physical Backspace
        }

        const uint32_t uni = keyUnicode(key);
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            // Word boundaries: clear composition state, forward to app
            if (isBoundaryASCII(c) || keyIs(key, kKeyReturn) ||
                keyIs(key, kKeyKpEnter) || keyIs(key, kKeyIsoEnter) ||
                keyIs(key, kKeyTab)) {
                if (maybeExpandMacroBeforeBoundary(context, state, key, adapterShared,
                                                   debug, c)) {
                    return true;
                }
                if (maybeRestoreBeforeBoundary(context, state, key, adapterShared,
                                               debug, c)) {
                    return true;
                }
                rememberBackspaceSnapshot(deltaState);
                clearWordState(deltaState, false);
                deltaState.allowBackspaceSnapshotResetPreserve =
                    needsTransientResetPreserve(state.program) && c == ' ';
                return false;
            }

            // Only composing chars go to the engine.
            // Non-composing non-boundary chars (e.g. !@#$) clear state and forward.
            if (!isComposingASCII(c)) {
                clearWordState(deltaState);
                return false;
            }

            if (deltaState.shownText.empty() &&
                deltaState.canReseedFromBackspaceSnapshot) {
                clearBackspaceSnapshot(deltaState);
            }

            if (needsTransientResetPreserve(state.program) &&
                !deltaState.shownText.empty() &&
                !context->trackedWordStillBeforeCursor(deltaState.shownText,
                                                      false)) {
                clearWordState(deltaState);
                return false;
            }

            if (!adapterShared) {
                clearWordState(deltaState);
                return false;
            }
            adapterShared->setCodeTable(state.codeTable);
            const auto r =
                adapterShared->processAsciiKey(deltaState.shownText, c);
            if (!r.handled) {
                clearWordState(deltaState);
                return false;
            }
            deltaState.rawAsciiBuffer.push_back(c);
            return applyWordDelta(context, state, debug, r.newWord, c, "ascii");
        }

        clearWordState(deltaState);
        return false;
    }

    void pumpQueue(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state,
                   std::shared_ptr<OpenKeyAdapter> adapterShared, bool debug) {
        auto &deltaState = state.delta;
        if (deltaState.processingQueue || deltaState.rewriteLock) {
            return;
        }
        deltaState.processingQueue = true;
        while (!deltaState.rewriteLock && !deltaState.queuedKeys.empty()) {
            const KeyInfo key = deltaState.queuedKeys.front();
            deltaState.queuedKeys.pop_front();
            const bool handled =
                processQueuedKey(context, state, key, adapterShared, debug);
            if (!handled) {
                context->imContext().forwardKeyPressAndRelease(key);
            }
        }
        deltaState.processingQueue = false;
    }

    BackspaceModeDeps deps_;
};

class NonPreeditBackspaceRewriteModeHandler final : public BackspaceModeHandler {
public:
    explicit NonPreeditBackspaceRewriteModeHandler(BackspaceModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(std::shared_ptr<RewriteContext> context, const KeyInfo &key,
               FcitxOpenKeyState &state) override {
        auto isBackspace = [&]() {
            return keyIs(key, kKeyBackSpace);
        };

        if (key.isRelease) {
            return false;
        }

        const auto adapterShared = deps_.adapter;
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        auto &nonPreeditState = state.nonPreeditDelta;

        context->imContext().cancelTimer(state.delta.commitTimer);
        context->imContext().cancelTimer(state.delta.ackTimeoutTimer);
        state.delta.clear();

        if (nonPreeditState.rewriteLock) {
            if (isBackspace()) {
                return false;
            }

            nonPreeditState.nonPreeditKeys.push_back(key);
            return true;
        }

        if (isBackspace()) {
            if (needsTransientResetPreserve(state.program) &&
                !nonPreeditState.shownText.empty() &&
                !context->trackedWordStillBeforeCursor(
                    nonPreeditState.shownText, false)) {
                clearComposeState(nonPreeditState, "backspace-cursor-mismatch");
                return false;
            }
            if (restoreBackspaceSnapshot(nonPreeditState)) {
                return true;
            }
            // Không clear hết — chỉ drop ký tự cuối để giữ context
            if (!nonPreeditState.shownText.empty()) {
                nonPreeditState.shownText = utf8DropLastN(nonPreeditState.shownText, 1);
                if (!nonPreeditState.rawAsciiBuffer.empty()) {
                    nonPreeditState.rawAsciiBuffer.pop_back();
                }
                if (nonPreeditState.shownText.empty()) {
                    nonPreeditState.rawAsciiBuffer.clear();
                    nonPreeditState.hasRewrittenCurrentWord = false;
                }
                nonPreeditState.allowTransientResetPreserve =
                    !nonPreeditState.shownText.empty();
            }
            return false; // để app tự xóa ký tự trên màn hình
        }

        if (keyIs(key, kKeyShiftL) || keyIs(key, kKeyShiftR)) {
            return false;
        }

        const uint32_t uni = keyUnicode(key);

        if (!(uni >= 0x20 && uni <= 0x7E)) {
            clearComposeState(nonPreeditState, "non-printable-boundary");
            return false;
        }

        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);
            if (!isComposingASCII(c)) {
                const bool handled =
                    processNonPreeditKey(context, state, key, adapterShared,
                                         debug);
                return handled;
            }

            nonPreeditState.nonPreeditKeys.push_back(key);
            pumpNonPreedit(context, state, adapterShared, debug);
            return true;
        }

        if (keyIsCursorMove(key) || keyIs(key, kKeyDelete) ||
            keyIs(key, kKeyEscape)) {
            clearComposeState(nonPreeditState, "cursor-delete");
        }

        return false;
    }

    void handleRemoteCommitAction(std::shared_ptr<RewriteContext> context,
                                  FcitxOpenKeyState &state,
                                  uint64_t txId) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (nonPreeditState.remotePendingTxId != txId) {
            return;
        }
        finishPendingBackspaceCommit(context, state);
    }

private:
    bool backspaceSnapshotEnabled() const {
        return !deps_.enableBackspaceSnapshot ||
               deps_.enableBackspaceSnapshot();
    }

    void clearBackspaceSnapshot(
        NonPreeditDeltaRewriteState &nonPreeditState) const {
        const bool hadSnapshot =
            nonPreeditState.canReseedFromBackspaceSnapshot ||
            !nonPreeditState.backspaceSnapshotShownText.empty() ||
            !nonPreeditState.backspaceSnapshotRawAsciiBuffer.empty() ||
            nonPreeditState.preserveBackspaceSnapshotAfterBoundaryBackspace;
        if (hadSnapshot && deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot clear"
                         << " mode=nonPreedit"
                         << " shown=" << nonPreeditState.shownText
                         << " snapshotShown="
                         << nonPreeditState.backspaceSnapshotShownText
                         << " snapshotRaw="
                         << nonPreeditState.backspaceSnapshotRawAsciiBuffer
                         << " canReseed="
                         << nonPreeditState.canReseedFromBackspaceSnapshot
                         << " preserveAfterBoundaryBackspace="
                         << nonPreeditState.preserveBackspaceSnapshotAfterBoundaryBackspace;
        }
        nonPreeditState.backspaceSnapshotShownText.clear();
        nonPreeditState.backspaceSnapshotRawAsciiBuffer.clear();
        nonPreeditState.backspaceSnapshotHasRewrittenCurrentWord = false;
        nonPreeditState.canReseedFromBackspaceSnapshot = false;
        nonPreeditState.preserveBackspaceSnapshotAfterBoundaryBackspace = false;
        nonPreeditState.allowBackspaceSnapshotResetPreserve = false;
    }

    void rememberBackspaceSnapshot(
        NonPreeditDeltaRewriteState &nonPreeditState) const {
        if (!backspaceSnapshotEnabled()) {
            clearBackspaceSnapshot(nonPreeditState);
            return;
        }
        if (nonPreeditState.shownText.empty()) {
            if (nonPreeditState.canReseedFromBackspaceSnapshot &&
                !nonPreeditState.backspaceSnapshotShownText.empty()) {
                nonPreeditState.preserveBackspaceSnapshotAfterBoundaryBackspace =
                    true;
                if (deps_.debugEnabled && deps_.debugEnabled()) {
                    FCITX_INFO() << "openkey: bs-snapshot remember-boundary"
                                 << " mode=nonPreedit"
                                 << " snapshotShown="
                                 << nonPreeditState.backspaceSnapshotShownText
                                 << " snapshotRaw="
                                 << nonPreeditState.backspaceSnapshotRawAsciiBuffer;
                }
                return;
            }
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot remember-skip"
                             << " mode=nonPreedit"
                             << " reason=empty-shown"
                             << " snapshotShown="
                             << nonPreeditState.backspaceSnapshotShownText
                             << " canReseed="
                             << nonPreeditState.canReseedFromBackspaceSnapshot;
            }
            clearBackspaceSnapshot(nonPreeditState);
            return;
        }
        nonPreeditState.backspaceSnapshotShownText = nonPreeditState.shownText;
        nonPreeditState.backspaceSnapshotRawAsciiBuffer =
            nonPreeditState.rawAsciiBuffer;
        nonPreeditState.backspaceSnapshotHasRewrittenCurrentWord =
        nonPreeditState.hasRewrittenCurrentWord;
        nonPreeditState.canReseedFromBackspaceSnapshot = true;
        nonPreeditState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
        if (deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot remember"
                         << " mode=nonPreedit"
                         << " shown=" << nonPreeditState.shownText
                         << " raw=" << nonPreeditState.rawAsciiBuffer
                         << " rewritten="
                         << nonPreeditState.hasRewrittenCurrentWord;
        }
    }

    bool restoreBackspaceSnapshot(
        NonPreeditDeltaRewriteState &nonPreeditState) const {
        if (!backspaceSnapshotEnabled()) {
            clearBackspaceSnapshot(nonPreeditState);
            return false;
        }
        if (!nonPreeditState.canReseedFromBackspaceSnapshot ||
            !nonPreeditState.shownText.empty() ||
            nonPreeditState.backspaceSnapshotShownText.empty()) {
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot restore-miss"
                             << " mode=nonPreedit"
                             << " shown=" << nonPreeditState.shownText
                             << " snapshotShown="
                             << nonPreeditState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << nonPreeditState.backspaceSnapshotRawAsciiBuffer
                             << " canReseed="
                             << nonPreeditState.canReseedFromBackspaceSnapshot;
            }
            return false;
        }
        nonPreeditState.shownText =
            nonPreeditState.backspaceSnapshotShownText;
        nonPreeditState.rawAsciiBuffer =
            nonPreeditState.backspaceSnapshotRawAsciiBuffer;
        nonPreeditState.hasRewrittenCurrentWord =
            nonPreeditState.backspaceSnapshotHasRewrittenCurrentWord;
        nonPreeditState.restoredFromBackspaceSnapshot = true;
        nonPreeditState.allowBackspaceSnapshotResetPreserve = true;
        if (deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot restore"
                         << " mode=nonPreedit"
                         << " shown=" << nonPreeditState.shownText
                         << " raw=" << nonPreeditState.rawAsciiBuffer
                         << " rewritten="
                         << nonPreeditState.hasRewrittenCurrentWord;
        }
        clearBackspaceSnapshot(nonPreeditState);
        nonPreeditState.allowBackspaceSnapshotResetPreserve = true;
        return true;
    }

    void clearComposeState(NonPreeditDeltaRewriteState &nonPreeditState,
                           const char *reason = "unknown",
                           bool clearSnapshot = true) const {
        
        if (deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: nonPreedit clear"
                        << " reason=" << reason
                        << " shown=" << nonPreeditState.shownText
                        << " pending=" << nonPreeditState.nonPreeditKeys.size()
                        << " rewriteLock=" << nonPreeditState.rewriteLock
                        << " waitingAck=" << nonPreeditState.waitingBackspaceAck
                        << " remotePending=" << nonPreeditState.remoteRewritePending;
        }
        nonPreeditState.shownText.clear();
        nonPreeditState.rawAsciiBuffer.clear();
        nonPreeditState.hasRewrittenCurrentWord = false;
        nonPreeditState.restoredFromBackspaceSnapshot = false;
        nonPreeditState.allowBackspaceSnapshotResetPreserve = false;
        nonPreeditState.allowTransientResetPreserve = false;
        nonPreeditState.rewriteLock = false;
        nonPreeditState.waitingBackspaceAck = false;
        nonPreeditState.processingNonPreedit = false;
        nonPreeditState.expectedBackspaces = 0;
        nonPreeditState.seenBackspaces = 0;
        nonPreeditState.lateBackspaceBudget = 0;
        nonPreeditState.nonPreeditKeys.clear();
        nonPreeditState.commitTimer = 0;
        nonPreeditState.lateBackspaceTimeoutTimer = 0;
        nonPreeditState.ackTimeoutTimer = 0;
        nonPreeditState.pendingConvertedText.clear();
        nonPreeditState.pendingShownTextAfterCommit.clear();
        nonPreeditState.remotePendingTxId = 0;
        nonPreeditState.remoteRewritePending = false;
        if (clearSnapshot) {
            clearBackspaceSnapshot(nonPreeditState);
        }
    }

    void finishPendingBackspaceCommit(std::shared_ptr<RewriteContext> context,
                                      FcitxOpenKeyState &state) {
        auto &nonPreeditState = state.nonPreeditDelta;
        const std::string commitText = std::move(nonPreeditState.pendingConvertedText);
        const std::string shownAfter =
            std::move(nonPreeditState.pendingShownTextAfterCommit);
        context->imContext().cancelTimer(nonPreeditState.commitTimer);
        nonPreeditState.pendingConvertedText.clear();
        nonPreeditState.pendingShownTextAfterCommit.clear();

        if (!commitText.empty()) {
            context->imContext().commitString(commitText);
        }
        nonPreeditState.shownText = shownAfter;
        nonPreeditState.allowTransientResetPreserve =
            !nonPreeditState.shownText.empty();
        nonPreeditState.waitingBackspaceAck = false;
        nonPreeditState.expectedBackspaces = 0;
        nonPreeditState.seenBackspaces = 0;
        context->imContext().cancelTimer(nonPreeditState.ackTimeoutTimer);
        nonPreeditState.remotePendingTxId = 0;
        nonPreeditState.remoteRewritePending = false;
        if (nonPreeditState.shownText.empty()) {
            nonPreeditState.rawAsciiBuffer.clear();
            nonPreeditState.hasRewrittenCurrentWord = false;
        }

        if (!commitText.empty() && schedulePostCommitPump(context, state)) {
            return;
        }

        finishPostCommitPump(context, state);
    }

    bool schedulePostCommitPump(std::shared_ptr<RewriteContext> context,
                                FcitxOpenKeyState &state) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (kNonPreeditPostCommitPumpDelayUsec == 0) {
            return false;
        }

        nonPreeditState.commitTimer = context->imContext().scheduleOnce(
            kNonPreeditPostCommitPumpDelayUsec, [this, context, &state]() {
                state.nonPreeditDelta.commitTimer = 0;
                finishPostCommitPump(context, state);
            });
        return nonPreeditState.commitTimer != 0;
    }

    void finishPostCommitPump(std::shared_ptr<RewriteContext> context,
                              FcitxOpenKeyState &state) {
        auto &nonPreeditState = state.nonPreeditDelta;
        context->imContext().cancelTimer(nonPreeditState.commitTimer);
        nonPreeditState.rewriteLock = false;
        pumpNonPreedit(context, state, deps_.adapter,
                  deps_.debugEnabled ? deps_.debugEnabled() : false);
    }

    bool applyWordDelta(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state,
                        bool debug, const std::string &newWord, char asciiChar,
                        const char *reason,
                        bool compareWithRawAppend = true) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (!fcitx::utf8::validate(nonPreeditState.shownText) ||
            !fcitx::utf8::validate(newWord)) {
            clearComposeState(nonPreeditState, "invalid-utf8");
            return false;
        }

        const std::string oldShown = nonPreeditState.shownText;
        const std::string rawAppend =
            compareWithRawAppend ? oldShown + asciiChar : oldShown;
        const RewriteTiming timing =
            nonPreeditTimingFor(context->imContext(), state.program);
        const std::size_t prefixLen =
            commonPrefixBytesUTF8Boundary(nonPreeditState.shownText, newWord);
        unsigned int deleteCount =
            utf8CharCount(nonPreeditState.shownText.substr(prefixLen));
        std::string commitText = newWord.substr(prefixLen);
        const bool browserAutocomplete =
            deleteCount > 0 &&
            isBrowserLikeProgram(state.program) &&
            !nonPreeditState.hasRewrittenCurrentWord &&
            context->looksLikeAutocomplete(nonPreeditState.shownText);
        if (browserAutocomplete) {
            deleteCount += 1;
        }
        if (deleteCount > 128) {
            deleteCount = utf8CharCount(nonPreeditState.shownText);
            commitText = newWord;
        }

        if (debug) {
            FCITX_INFO() << "openkey: nonPreedit program=" << state.program
                         << " reason=" << reason
                         << " from=" << nonPreeditState.shownText
                         << " to=" << newWord
                         << " delete=" << deleteCount
                         << " commit=" << commitText
                         << " inter=" << timing.interKeyUsec
                         << " delay=" << timing.commitDelayUsec
                         << " autocomplete=" << browserAutocomplete
                         << " nonPreeditPending=" << nonPreeditState.nonPreeditKeys.size();
        }

        if (deleteCount == 0) {
            if (!commitText.empty()) {
                context->imContext().commitString(commitText);
            }
            nonPreeditState.shownText = newWord;
            nonPreeditState.hasRewrittenCurrentWord =
                nonPreeditState.hasRewrittenCurrentWord || (newWord != rawAppend);
            nonPreeditState.restoredFromBackspaceSnapshot = false;
            nonPreeditState.allowBackspaceSnapshotResetPreserve = false;
            nonPreeditState.allowTransientResetPreserve = true;
            return true;
        }

        if (deps_.nonPreeditRemoteEnabled && deps_.nonPreeditRemoteEnabled()) {
            nonPreeditState.rewriteLock = true;
            nonPreeditState.waitingBackspaceAck = false;
            nonPreeditState.expectedBackspaces = static_cast<int>(deleteCount);
            nonPreeditState.seenBackspaces = 0;
            nonPreeditState.pendingConvertedText = commitText;
            nonPreeditState.pendingShownTextAfterCommit = newWord;
            nonPreeditState.hasRewrittenCurrentWord =
                nonPreeditState.hasRewrittenCurrentWord || (newWord != rawAppend);
            nonPreeditState.restoredFromBackspaceSnapshot = false;
            nonPreeditState.allowBackspaceSnapshotResetPreserve = false;
            nonPreeditState.allowTransientResetPreserve = true;
            if (context->scheduleRemoteRewrite(deleteCount, timing.interKeyUsec,
                                              timing.commitDelayUsec)) {
                nonPreeditState.remoteRewritePending = true;
                return true;
            }
            nonPreeditState.rewriteLock = false;
            nonPreeditState.pendingConvertedText.clear();
            nonPreeditState.pendingShownTextAfterCommit.clear();
        }

        clearComposeState(nonPreeditState, "default");
        return false;
    }

    bool hasPendingRewrite(const NonPreeditDeltaRewriteState &state) const {
        return state.rewriteLock || state.waitingBackspaceAck ||
               !state.pendingConvertedText.empty() ||
               !state.pendingShownTextAfterCommit.empty() ||
               state.hasRemoteRewritePending();
    }

    bool maybeExpandMacroBeforeBoundary(std::shared_ptr<RewriteContext> context,
                                        FcitxOpenKeyState &state,
                                        const KeyInfo &boundaryKey,
                                        std::shared_ptr<OpenKeyAdapter> adapterShared,
                                        bool debug, char trigger) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (!deps_.enableMacro || !deps_.enableMacro() || !adapterShared ||
            !isMacroTriggerKey(trigger) || nonPreeditState.shownText.empty()) {
            return false;
        }

        adapterShared->setCodeTable(state.codeTable);
        std::string replacement;
        if (!adapterShared->expandMacro(nonPreeditState.shownText, replacement)) {
            return false;
        }

        if (!applyWordDelta(context, state, debug, replacement, trigger, "macro",
                            false)) {
            return false;
        }

        if (hasPendingRewrite(nonPreeditState)) {
            nonPreeditState.nonPreeditKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(nonPreeditState);
            context->imContext().forwardKeyPressAndRelease(boundaryKey);
            clearComposeState(nonPreeditState, "macro-boundary", false);
            nonPreeditState.allowBackspaceSnapshotResetPreserve =
                needsTransientResetPreserve(state.program) && trigger == ' ';
        }
        return true;
    }

    bool maybeRestoreBeforeBoundary(std::shared_ptr<RewriteContext> context,
                                    FcitxOpenKeyState &state,
                                    const KeyInfo &boundaryKey,
                                    std::shared_ptr<OpenKeyAdapter> adapterShared,
                                    bool debug, char trigger) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (!deps_.restoreIfWrongSpelling ||
            !deps_.restoreIfWrongSpelling() || !adapterShared ||
            nonPreeditState.shownText.empty()) {
            return false;
        }

        adapterShared->setCodeTable(state.codeTable);
        std::string restoredWord;
        const bool restored =
            !nonPreeditState.rawAsciiBuffer.empty()
                ? adapterShared->restoreFromRawAsciiOnWordBreak(
                      nonPreeditState.shownText,
                      nonPreeditState.rawAsciiBuffer, trigger, restoredWord)
                : adapterShared->restoreOnWordBreak(nonPreeditState.shownText,
                                                   trigger, restoredWord);
        if (!restored) {
            return false;
        }

        if (!applyWordDelta(context, state, debug, restoredWord, trigger,
                            "restore-boundary", false)) {
            return false;
        }

        if (hasPendingRewrite(nonPreeditState)) {
            nonPreeditState.nonPreeditKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(nonPreeditState);
            context->imContext().forwardKeyPressAndRelease(boundaryKey);
            clearComposeState(nonPreeditState, "restore-boundary", false);
            nonPreeditState.allowBackspaceSnapshotResetPreserve =
                needsTransientResetPreserve(state.program) && trigger == ' ';
        }
        return true;
    }

    bool processNonPreeditKey(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state,
                          const KeyInfo &nonPreeditKey,
                          std::shared_ptr<OpenKeyAdapter> adapterShared,
                          bool debug) {
        auto &nonPreeditState = state.nonPreeditDelta;
        const RewriteTiming timing =
            nonPreeditTimingFor(context->imContext(), state.program);

        if (keyIsCursorMove(nonPreeditKey) || keyIs(nonPreeditKey, kKeyDelete)) {
           clearComposeState(nonPreeditState, "cursor-delete");
            return false;
        }

        if (keyIs(nonPreeditKey, kKeyEscape)) {
           clearComposeState(nonPreeditState, "escape");
            return false;
        }

        if (keyIs(nonPreeditKey, kKeyBackSpace)) {
            if (needsTransientResetPreserve(state.program) &&
                !nonPreeditState.shownText.empty() &&
                !context->trackedWordStillBeforeCursor(
                    nonPreeditState.shownText, false)) {
                clearComposeState(nonPreeditState, "backspace-cursor-mismatch");
                return false;
            }
            if (restoreBackspaceSnapshot(nonPreeditState)) {
                return false;
            }
            if (nonPreeditState.shownText.empty()) {
                return false;
            }
            if (!nonPreeditState.hasRewrittenCurrentWord) {
                clearComposeState(nonPreeditState, "backspace-empty-or-not-rewritten");           
                return false;
            }
            const auto method = context->sendBackspaces(1, timing.interKeyUsec);
            if (method != BackspaceMethod::Uinput) {
                return false;
            }
            if (!nonPreeditState.rawAsciiBuffer.empty()) {
                nonPreeditState.rawAsciiBuffer.pop_back();
            }
            nonPreeditState.shownText = utf8DropLastN(nonPreeditState.shownText, 1);
            if (nonPreeditState.shownText.empty()) {
                nonPreeditState.rawAsciiBuffer.clear();
                nonPreeditState.hasRewrittenCurrentWord = false;
            }
            nonPreeditState.allowTransientResetPreserve =
                !nonPreeditState.shownText.empty();
            return true;
        }

        const uint32_t uni = keyUnicode(nonPreeditKey);
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            // Word boundaries: clear composition state, forward to app
            if (isBoundaryASCII(c) || keyIs(nonPreeditKey, kKeyReturn) ||
                keyIs(nonPreeditKey, kKeyKpEnter) ||
                keyIs(nonPreeditKey, kKeyIsoEnter) ||
                keyIs(nonPreeditKey, kKeyTab)) {
                if (maybeExpandMacroBeforeBoundary(context, state, nonPreeditKey,
                                                   adapterShared, debug, c)) {
                    return true;
                }
                if (maybeRestoreBeforeBoundary(context, state, nonPreeditKey,
                                               adapterShared, debug, c)) {
                    return true;
                }
                rememberBackspaceSnapshot(nonPreeditState);
                clearComposeState(nonPreeditState, "boundary", false);
                nonPreeditState.allowBackspaceSnapshotResetPreserve =
                    needsTransientResetPreserve(state.program) && c == ' ';
                return false;
            }

            // Only composing chars go to the engine.
            // Non-composing non-boundary chars (e.g. !@#$) clear state and forward.
            if (!isComposingASCII(c)) {
                clearComposeState(nonPreeditState, "not-composing-ascii");
                return false;
            }

            if (nonPreeditState.shownText.empty() &&
                nonPreeditState.canReseedFromBackspaceSnapshot) {
                clearBackspaceSnapshot(nonPreeditState);
            }

            if (needsTransientResetPreserve(state.program) &&
                !nonPreeditState.shownText.empty() &&
                !context->trackedWordStillBeforeCursor(
                    nonPreeditState.shownText, false)) {
                clearComposeState(nonPreeditState, "ascii-cursor-mismatch");
                return false;
            }

            if (!adapterShared) {
                clearComposeState(nonPreeditState, "no-adapter");
                return false;
            }
            adapterShared->setCodeTable(state.codeTable);
            const auto r =
                adapterShared->processAsciiKey(nonPreeditState.shownText, c);
            if (!r.handled) {
                clearComposeState(nonPreeditState, "adapter-not-handled");
                return false;
            }
            nonPreeditState.rawAsciiBuffer.push_back(c);
            return applyWordDelta(context, state, debug, r.newWord, c, "ascii");
        }

       clearComposeState(nonPreeditState, "non-ascii-key");
        return false;
    }

    void pumpNonPreedit(std::shared_ptr<RewriteContext> context, FcitxOpenKeyState &state,
                   std::shared_ptr<OpenKeyAdapter> adapterShared, bool debug) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (nonPreeditState.processingNonPreedit || nonPreeditState.rewriteLock) {
            return;
        }
        nonPreeditState.processingNonPreedit = true;
        while (!nonPreeditState.rewriteLock && !nonPreeditState.nonPreeditKeys.empty()) {
            const KeyInfo nonPreeditKey = nonPreeditState.nonPreeditKeys.front();
            nonPreeditState.nonPreeditKeys.pop_front();
            const bool handled =
                processNonPreeditKey(context, state, nonPreeditKey,
                                     adapterShared, debug);
            if (!handled) {
                context->imContext().forwardKeyPressAndRelease(nonPreeditKey);
            }
        }
        nonPreeditState.processingNonPreedit = false;
    }

    BackspaceModeDeps deps_;
};

} // namespace

std::unique_ptr<BackspaceModeHandler> createBackspaceRewriteModeHandler(
    BackspaceModeDeps deps) {
    return std::make_unique<BackspaceRewriteModeHandler>(std::move(deps));
}

std::unique_ptr<BackspaceModeHandler> createNonPreeditBackspaceRewriteModeHandler(
    BackspaceModeDeps deps) {
    return std::make_unique<NonPreeditBackspaceRewriteModeHandler>(
        std::move(deps));
}

} // namespace openkey
