#include "openkey.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/action.h>
#include <fcitx-config/iniparser.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/utf8.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/trackableobject.h>

#include "Macro.h"
#include "openkey_adapter.h"
#include "surrounding_text_utils.h"

#ifdef __linux__
// OpenKey core defines X11 keycode-style KEY_* macros in
// `Sources/OpenKey/engine/platforms/linux.h`. They conflict with Linux input
// KEY_* macros required by uinput. Undefine the OpenKey ones before including
// uinput headers to avoid redefinition warnings.
#ifdef KEY_ESC
#undef KEY_ESC
#endif
#ifdef KEY_DELETE
#undef KEY_DELETE
#endif
#ifdef KEY_TAB
#undef KEY_TAB
#endif
#ifdef KEY_ENTER
#undef KEY_ENTER
#endif
#ifdef KEY_RETURN
#undef KEY_RETURN
#endif
#ifdef KEY_SPACE
#undef KEY_SPACE
#endif
#ifdef KEY_LEFT
#undef KEY_LEFT
#endif
#ifdef KEY_RIGHT
#undef KEY_RIGHT
#endif
#ifdef KEY_DOWN
#undef KEY_DOWN
#endif
#ifdef KEY_UP
#undef KEY_UP
#endif
#ifdef KEY_DOT
#undef KEY_DOT
#endif
#ifdef KEY_MINUS
#undef KEY_MINUS
#endif
#ifdef KEY_SEMICOLON
#undef KEY_SEMICOLON
#endif
#ifdef KEY_COMMA
#undef KEY_COMMA
#endif
#ifdef KEY_SLASH
#undef KEY_SLASH
#endif

#include <linux/uinput.h>
#endif

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

static bool hasCtrlAltSuperMeta(const fcitx::Key &key) {
    const auto states = key.states();
    return states.test(fcitx::KeyState::Ctrl) || states.test(fcitx::KeyState::Alt) ||
           states.test(fcitx::KeyState::Super) ||
           states.test(fcitx::KeyState::Meta) ||
           states.test(fcitx::KeyState::Hyper) ||
           states.test(fcitx::KeyState::Super2) ||
           states.test(fcitx::KeyState::Hyper2);
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

static bool equalsASCIIInsensitive(const std::string &a, const char *b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (toLowerASCII(a[i]) != toLowerASCII(b[i])) {
            return false;
        }
    }
    return true;
}

static bool isWineProgram(const std::string &program) {
    if (program.empty()) {
        return false;
    }
    // Wine/Proton Windows programs often show up as "*.exe" here.
    if (endsWithASCIIInsensitive(program, ".exe")) {
        return true;
    }
    // Some toolkits report the wine loader instead of the actual exe.
    static const char *kWineProgramNames[] = {
        "wine", "wine64", "wine-preloader", "wine64-preloader", "wineserver",
    };
    for (const char *name : kWineProgramNames) {
        if (equalsASCIIInsensitive(program, name)) {
            return true;
        }
    }
    return false;
}

static bool shouldTriggerRewriteOnChunk(const std::string &rawBuffer) {
    // Tone keys.
    if (!rawBuffer.empty()) {
        const char last = toLowerASCII(rawBuffer.back());
        if (last == 's' || last == 'f' || last == 'r' || last == 'x' ||
            last == 'j') {
            return true;
        }
    }

    // Shape chunks.
    static const char *kSuffixes[] = {"aa", "aw", "ee", "oo", "ow", "uw", "dd",
                                      "iee", "yee", "uyee", "uow", "uoow",
                                      "aau", "aay"};
    for (const char *suf : kSuffixes) {
        if (endsWithASCIIInsensitive(rawBuffer, suf)) {
            return true;
        }
    }
    return false;
}

class BackspaceInjector {
public:
    ~BackspaceInjector() { destroyUinput(); }

    enum class Method {
        DeleteSurroundingText,
        Uinput,
        ForwardKey,
        None,
    };

    Method sendBackspaces(fcitx::InputContext *ic, int count, bool debug) {
        if (!ic || count <= 0) {
            return Method::None;
        }
        // Prefer framework-level deletion when available (works better for
        // some toolkits / fields like browser omnibox).
        if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
            const auto &st = ic->surroundingText();
            if (st.isValid() && st.cursor() == st.anchor() &&
                fcitx::utf8::validate(st.text()) &&
                st.cursor() <= fcitx::utf8::length(st.text()) &&
                static_cast<unsigned int>(count) <= st.cursor()) {
                ic->deleteSurroundingText(-count,
                                          static_cast<unsigned int>(count));
                return Method::DeleteSurroundingText;
            }
        }
        if (ensureUinput(debug)) {
            sendBackspacesUinput(count);
            return Method::Uinput;
        }
        // Fallback: forward BackSpace key to client.
        for (int i = 0; i < count; i++) {
            ic->forwardKey(fcitx::Key(FcitxKey_BackSpace));
        }
        return Method::ForwardKey;
    }

private:
    int fd_ = -1;
    bool tried_ = false;

    void destroyUinput() {
#ifdef __linux__
        if (fd_ >= 0) {
            (void)ioctl(fd_, UI_DEV_DESTROY);
            ::close(fd_);
        }
#endif
        fd_ = -1;
        tried_ = false;
    }

    bool ensureUinput(bool debug) {
#ifndef __linux__
        (void)debug;
        return false;
#else
        if (fd_ >= 0) {
            return true;
        }
        if (tried_) {
            return false;
        }
        tried_ = true;

        fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd_ < 0) {
            if (debug) {
                FCITX_WARN() << "openkey: uinput unavailable";
            }
            return false;
        }

        (void)ioctl(fd_, UI_SET_EVBIT, EV_KEY);
        (void)ioctl(fd_, UI_SET_KEYBIT, KEY_BACKSPACE);

        struct uinput_user_dev uidev {};
        std::snprintf(uidev.name, sizeof(uidev.name), "fcitx5-openkey");
        uidev.id.bustype = BUS_USB;
        uidev.id.vendor = 0x1234;
        uidev.id.product = 0x5678;
        uidev.id.version = 1;
        const ssize_t w = ::write(fd_, &uidev, sizeof(uidev));
        (void)w;

        if (ioctl(fd_, UI_DEV_CREATE) < 0) {
            ::close(fd_);
            fd_ = -1;
            if (debug) {
                FCITX_WARN() << "openkey: UI_DEV_CREATE failed; fallback to "
                                "forwardKey backspace";
            }
            return false;
        }
        return true;
#endif
    }

#ifdef __linux__
    static void emitEvent(int fd, int type, int code, int value) {
        struct input_event ev {};
        ::gettimeofday(&ev.time, nullptr);
        ev.type = static_cast<decltype(ev.type)>(type);
        ev.code = static_cast<decltype(ev.code)>(code);
        ev.value = value;
        const ssize_t w = ::write(fd, &ev, sizeof(ev));
        (void)w;
    }

    void sendBackspacesUinput(int count) {
        if (fd_ < 0 || count <= 0) {
            return;
        }
        for (int i = 0; i < count; i++) {
            emitEvent(fd_, EV_KEY, KEY_BACKSPACE, 1);
            emitEvent(fd_, EV_SYN, SYN_REPORT, 0);
            emitEvent(fd_, EV_KEY, KEY_BACKSPACE, 0);
            emitEvent(fd_, EV_SYN, SYN_REPORT, 0);
            // Some clients/input stacks are flaky if events are injected in very
            // tight bursts. Add a tiny delay to improve reliability under fast
            // typing.
            if (i + 1 < count) {
                ::usleep(1500); // 1.5ms
            }
        }
    }
#endif
};

static BackspaceInjector g_backspaceInjector;

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

static bool shouldCommitBoundaryAsText(const fcitx::Key &rawKey) {
    const uint32_t uni = fcitx::Key::keySymToUnicode(rawKey.sym());
    if (uni >= 0x20 && uni <= 0x7E) {
        // Space and common ASCII punctuation should be safe to commit as text.
        return true;
    }
    return false;
}

static void applyBoundaryKey(fcitx::InputContext *ic, const fcitx::Key &rawKey) {
    if (!ic) {
        return;
    }
    if (shouldCommitBoundaryAsText(rawKey)) {
        const std::string utf8 = fcitx::Key::keySymToUTF8(rawKey.sym());
        if (!utf8.empty()) {
            ic->commitString(utf8);
            return;
        }
    }
    ic->forwardKey(rawKey);
}

static int toOpenKeyInputType(InputType type) {
    switch (type) {
    case InputType::Telex:
        return 0;
    case InputType::VNI:
        return 1;
    case InputType::SimpleTelex1:
        return 2;
    case InputType::SimpleTelex2:
        return 3;
    }
    return 0;
}

static int toOpenKeyCodeTable(CodeTable table) {
    switch (table) {
    case CodeTable::Unicode:
        return 0;
    case CodeTable::TCVN3:
        return 1;
    case CodeTable::VNIWindows:
        return 2;
    }
    return 0;
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

} // namespace

namespace {

struct ModeDeps {
    fcitx::Instance *instance = nullptr;
    fcitx::SimpleInputContextPropertyFactory<OpenKeyState> *factory = nullptr;
    std::shared_ptr<OpenKeyAdapter> adapter;
    BackspaceInjector *backspaceInjector = nullptr;
    std::weak_ptr<void> lifetimeWeak;
    std::function<bool()> debugEnabled;
};

class BackspaceRewriteModeHandler final : public InputModeHandler {
public:
    explicit BackspaceRewriteModeHandler(ModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                   OpenKeyState &state) override {
        // This body is moved from OpenKeyEngine::handleBackspaceRewrite with
        // minimal structural changes.
        auto key = event.key().normalize();

        // Never do anything on key release in this mode.
        if (event.isRelease()) {
            return false;
        }

        const uint64_t nowUsec = fcitx::now(CLOCK_MONOTONIC);

        // Guard: ignore injected backspace events that might loop back (uinput).
        if (state.isInjecting && nowUsec <= state.injectingUntilUsec &&
            key.check(FcitxKey_BackSpace) && !hasCtrlAltSuperMeta(key)) {
            return false;
        }

        // Do not swallow application shortcuts.
        if (hasCtrlAltSuperMeta(key)) {
            return false;
        }

        auto clearWordState = [&state]() {
            state.rawBuffer.clear();
            state.shownText.clear();
            state.hasRewrittenCurrentWord = false;
            state.rewriteTimer.reset();
        };

        const auto icRef = ic->watch();
        const std::weak_ptr<void> lifetimeWeak = deps_.lifetimeWeak;
        const auto adapterShared = deps_.adapter;
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        auto *loop = deps_.instance ? &deps_.instance->eventLoop() : nullptr;

        auto stateFor = [this](fcitx::InputContext *ic2) -> OpenKeyState * {
            if (!ic2 || !deps_.factory) {
                return nullptr;
            }
            return ic2->propertyFor(deps_.factory);
        };

        auto drainPendingKeys = [this, icRef, lifetimeWeak, stateFor]() {
            if (lifetimeWeak.expired()) {
                return;
            }
            auto *ic2 = icRef.get();
            if (!ic2) {
                return;
            }
            auto *st = stateFor(ic2);
            if (!st || st->pendingKeys.empty()) {
                return;
            }
            // Move out to avoid re-entrancy issues.
            auto keys = std::move(st->pendingKeys);
            st->pendingKeys.clear();
            for (const auto &k : keys) {
                fcitx::KeyEvent synthetic(ic2, k, false /* release */,
                                          0 /* time */);
                handleKey(ic2, synthetic, *st);
            }
        };

        auto scheduleCommitAfterBackspace = [this, &state, icRef, lifetimeWeak,
                                             loop, stateFor,
                                             drainPendingKeys](uint64_t delayUsec) {
            state.commitTimer.reset();
            if (!loop) {
                return;
            }
            const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
            state.commitTimer = loop->addTimeEvent(
                CLOCK_MONOTONIC, deadline, 0,
                [this, icRef, lifetimeWeak, stateFor,
                 drainPendingKeys](fcitx::EventSourceTime *, uint64_t) {
                    // One-shot.
                    if (lifetimeWeak.expired()) {
                        return false;
                    }
                    auto *ic2 = icRef.get();
                    if (!ic2) {
                        return false;
                    }
                    auto *statePtr = stateFor(ic2);
                    if (!statePtr) {
                        return false;
                    }
                    // Keep the EventSourceTime alive until callback returns.
                    auto _timer = std::move(statePtr->commitTimer);

                    const std::string converted = statePtr->pendingConvertedText;
                    statePtr->pendingConvertedText.clear();

                    // Commit converted Vietnamese text.
                    if (!converted.empty()) {
                        ic2->commitString(converted);
                        statePtr->shownText = converted;
                        statePtr->hasRewrittenCurrentWord = true;
                    }

                    // Forward boundary key if requested.
                    if (statePtr->hasPendingBoundaryKey) {
                        applyBoundaryKey(ic2, statePtr->pendingBoundaryKey);
                        statePtr->pendingBoundaryKey = fcitx::Key(FcitxKey_None);
                        statePtr->hasPendingBoundaryKey = false;
                        // Boundary ends the current word.
                        statePtr->rawBuffer.clear();
                        statePtr->shownText.clear();
                        statePtr->hasRewrittenCurrentWord = false;
                    }

                    statePtr->isInjecting = false;
                    statePtr->injectingUntilUsec = 0;
                    statePtr->rewriteLock = false;

                    drainPendingKeys();
                    return false;
                });
            if (state.commitTimer) {
                state.commitTimer->setOneShot();
            }
        };

        auto beginRewrite = [this, ic, &state, scheduleCommitAfterBackspace,
                             nowUsec, adapterShared, debug](const char *reason) {
            if (state.rewriteLock) {
                return;
            }
            if (state.rawBuffer.empty()) {
                return;
            }
            if (!adapterShared) {
                return;
            }

            adapterShared->setCodeTable(state.codeTable);
            const std::string converted =
                adapterShared->convertRawBuffer(state.rawBuffer);
            if (converted.empty() || converted == state.shownText) {
                return;
            }

            if (debug) {
                FCITX_INFO() << "openkey: bs-rewrite start program=" << state.program
                             << " reason=" << reason
                             << " rawBuffer=" << state.rawBuffer
                             << " shownText=" << state.shownText
                             << " converted=" << converted;
            }

            state.rewriteTimer.reset();
            state.rewriteLock = true;
            state.isInjecting = true;
            state.pendingConvertedText = converted;

            const int bsCount =
                static_cast<int>(fcitx::utf8::length(state.shownText));
            if (bsCount > 0) {
                const auto method =
                    deps_.backspaceInjector->sendBackspaces(ic, bsCount, debug);
                uint64_t commitDelayUsec = 15000;
                // NOTE: deleting via client protocol (deleteSurroundingText) or
                // forwarded keys tends to be more asynchronous. uinput is also
                // asynchronous when the target app is busy. Use a slightly
                // longer delay for stability when typing fast.
                if (method == BackspaceInjector::Method::DeleteSurroundingText ||
                    method == BackspaceInjector::Method::ForwardKey) {
                    commitDelayUsec = 45000; // 45ms
                } else if (method == BackspaceInjector::Method::Uinput) {
                    // Scale with deletion size, capped.
                    commitDelayUsec = std::min<uint64_t>(
                        60000, 25000 + static_cast<uint64_t>(bsCount) * 2000);
                }
                // Allow enough time for injected backspaces to loop back
                // (uinput), with extra slack for busy clients.
                state.injectingUntilUsec = nowUsec +
                                           std::min<uint64_t>(250000,
                                                              commitDelayUsec + 80000);
                scheduleCommitAfterBackspace(commitDelayUsec);
                return;
            }

            // Commit after a brief delay so deletion is processed first.
            state.injectingUntilUsec = nowUsec + 50000; // 50ms
            scheduleCommitAfterBackspace(15000); // 15ms
        };

        auto scheduleRewrite =
            [this, icRef, lifetimeWeak, adapterShared, debug, loop, stateFor,
             &state](uint64_t delayUsec, const char *reason) {
                if (state.rawBuffer.empty()) {
                    return;
                }
                state.rewriteTimer.reset();
                if (!loop) {
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
                state.rewriteTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [this, icRef, lifetimeWeak, adapterShared, debug, loop,
                     stateFor, reason](fcitx::EventSourceTime *, uint64_t) {
                        if (lifetimeWeak.expired() || !adapterShared) {
                            return false;
                        }
                        auto *ic2 = icRef.get();
                        if (!ic2) {
                            return false;
                        }
                        auto *statePtr = stateFor(ic2);
                        if (!statePtr) {
                            return false;
                        }
                        // Keep the EventSourceTime alive until callback returns.
                        auto _timer = std::move(statePtr->rewriteTimer);
                        if (statePtr->rewriteLock) {
                            return false;
                        }
                        // beginRewrite is local; replicate minimal call here.
                        adapterShared->setCodeTable(statePtr->codeTable);
                        const std::string converted =
                            adapterShared->convertRawBuffer(
                                statePtr->rawBuffer);
                        if (converted.empty() ||
                            converted == statePtr->shownText) {
                            return false;
                        }
                        if (debug) {
                            FCITX_INFO()
                                << "openkey: bs-rewrite timer program="
                                << statePtr->program << " reason=" << reason
                                << " rawBuffer=" << statePtr->rawBuffer
                                << " shownText=" << statePtr->shownText
                                << " converted=" << converted;
                        }
                        statePtr->rewriteLock = true;
                        statePtr->isInjecting = true;
                        const uint64_t now2 = fcitx::now(CLOCK_MONOTONIC);
                        statePtr->pendingConvertedText = converted;
                        const int bsCount = static_cast<int>(
                            fcitx::utf8::length(statePtr->shownText));
                        uint64_t commitDelayUsec = 15000;
                        if (bsCount > 0) {
                            const auto method = deps_.backspaceInjector->sendBackspaces(
                                ic2, bsCount, debug);
                            if (method ==
                                    BackspaceInjector::Method::DeleteSurroundingText ||
                                method == BackspaceInjector::Method::ForwardKey) {
                                commitDelayUsec = 45000;
                            } else if (method == BackspaceInjector::Method::Uinput) {
                                commitDelayUsec = std::min<uint64_t>(
                                    60000,
                                    25000 + static_cast<uint64_t>(bsCount) * 2000);
                            }
                        }
                        statePtr->injectingUntilUsec =
                            now2 + std::min<uint64_t>(250000, commitDelayUsec + 80000);
                        // Commit after a brief delay.
                        statePtr->commitTimer.reset();
                        OpenKeyState *st2 = statePtr;
                        const uint64_t commitDeadline =
                            fcitx::now(CLOCK_MONOTONIC) + commitDelayUsec;
                        st2->commitTimer = loop->addTimeEvent(
                            CLOCK_MONOTONIC, commitDeadline, 0,
                            [this, icRef, lifetimeWeak, st2, stateFor](
                                fcitx::EventSourceTime *, uint64_t) {
                                if (lifetimeWeak.expired()) {
                                    return false;
                                }
                                auto *ic3 = icRef.get();
                                if (!ic3 || !st2) {
                                    return false;
                                }
                                // Keep the EventSourceTime alive until callback
                                // returns.
                                auto _timer = std::move(st2->commitTimer);
                                const std::string conv =
                                    st2->pendingConvertedText;
                                st2->pendingConvertedText.clear();
                                if (!conv.empty()) {
                                    ic3->commitString(conv);
                                    st2->shownText = conv;
                                    st2->hasRewrittenCurrentWord = true;
                                }
                                st2->isInjecting = false;
                                st2->injectingUntilUsec = 0;
                                st2->rewriteLock = false;
                                // Drain any keys collected during lock.
                                auto keys = std::move(st2->pendingKeys);
                                st2->pendingKeys.clear();
                                for (const auto &k : keys) {
                                    fcitx::KeyEvent syn(ic3, k, false, 0);
                                    handleKey(ic3, syn, *st2);
                                }
                                return false;
                            });
                        if (st2->commitTimer) {
                            st2->commitTimer->setOneShot();
                        }
                        return false;
                    });
                if (state.rewriteTimer) {
                    state.rewriteTimer->setOneShot();
                }

                if (deps_.debugEnabled && deps_.debugEnabled()) {
                    FCITX_INFO()
                        << "openkey: bs-rewrite schedule program=" << state.program
                        << " reason=" << reason << " delayUsec=" << delayUsec
                        << " rawBuffer=" << state.rawBuffer
                        << " shownText=" << state.shownText;
                }
            };

        // If we are currently rewriting, queue and swallow all physical keys.
        if (state.rewriteLock) {
            state.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        // Cursor move / delete: end composing state.
        if (key.isCursorMove() || key.check(FcitxKey_Delete)) {
            clearWordState();
            return false;
        }

        // Escape cancels current composing word (boundary) but should not be
        // swallowed (let application handle Escape).
        if (key.check(FcitxKey_Escape)) {
            clearWordState();
            return false;
        }

        // Physical BackSpace: delete one visible rune and resync rawBuffer.
        if (key.check(FcitxKey_BackSpace)) {
            if (state.rawBuffer.empty() && state.shownText.empty()) {
                return false;
            }
            event.filterAndAccept();

            deps_.backspaceInjector->sendBackspaces(ic, 1, debug);
            if (!state.shownText.empty()) {
                state.shownText = utf8DropLastN(state.shownText, 1);
            }

            if (!state.rawBuffer.empty()) {
                state.rawBuffer.pop_back();
            }
            // Best-effort: trim rawBuffer until conversion matches shownText.
            for (int i = 0; i < 16 && !state.rawBuffer.empty(); i++) {
                adapterShared->setCodeTable(state.codeTable);
                const std::string converted =
                    adapterShared->convertRawBuffer(state.rawBuffer);
                if (converted == state.shownText) {
                    break;
                }
                state.rawBuffer.pop_back();
            }

            if (state.rawBuffer.empty()) {
                state.hasRewrittenCurrentWord = false;
            }
            return true;
        }

        // Printable ASCII path.
        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            // Boundary keys: rewrite immediately (if needed), then forward the
            // original boundary key and clear word state.
            if (isBoundaryASCII(c) || key.check(FcitxKey_Return) ||
                key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_ISO_Enter) ||
                key.check(FcitxKey_Tab)) {
                if (!state.rawBuffer.empty()) {
                    event.filterAndAccept();
                    // Stash boundary key and rewrite immediately.
                    state.hasPendingBoundaryKey = true;
                    state.pendingBoundaryKey = event.rawKey();
                    beginRewrite("boundary");
                    // If rewrite didn't happen, still forward boundary and clear.
                    if (!state.rewriteLock) {
                        applyBoundaryKey(ic, event.rawKey());
                        clearWordState();
                    }
                    return true;
                }
                return false;
            }

            // Word characters: update state and commit raw char to client.
            if (isComposingASCII(c)) {
                state.rawBuffer.push_back(c);
                const std::string vis = fcitx::Key::keySymToUTF8(key.sym());
                state.lastPhysicalKeyUsec = nowUsec;

                event.filterAndAccept();
                if (!state.hasRewrittenCurrentWord) {
                    if (!vis.empty()) {
                        state.shownText += vis;
                        ic->commitString(vis);
                    }

                    // Schedule rewrites based on chunks and idle.
                    if (shouldTriggerRewriteOnChunk(state.rawBuffer)) {
                        scheduleRewrite(30000, "chunk"); // 30ms
                    } else {
                         scheduleRewrite(100000, "idle"); // 100ms
                    }
                    return true;
                }

                // Converted phase: keep the word active and reconvert the full
                // rawBuffer until a boundary is typed. Do NOT commit the raw
                // character; a short rewrite will update the visible word.
                scheduleRewrite(30000, "post_rewrite"); // 30ms
                return true;
            }

            // Other printable ASCII: treat as boundary of current word.
            if (!state.rawBuffer.empty()) {
                event.filterAndAccept();
                state.hasPendingBoundaryKey = true;
                state.pendingBoundaryKey = event.rawKey();
                beginRewrite("other_ascii");
                if (!state.rewriteLock) {
                    applyBoundaryKey(ic, event.rawKey());
                    clearWordState();
                }
                return true;
            }
            return false;
        }

        // Any other key: end composing state, let application handle.
        clearWordState();
        return false;
    }

private:
    ModeDeps deps_;
};

} // namespace

OpenKeyEngine::OpenKeyEngine(fcitx::Instance *instance)
    : instance_(instance), adapter_(std::make_shared<OpenKeyAdapter>()) {
    lifetime_ = std::make_shared<int>(1);
    instance_->inputContextManager().registerProperty("openkeyState", &factory_);
    ModeDeps deps;
    deps.instance = instance_;
    deps.factory = &factory_;
    deps.adapter = adapter_;
    deps.backspaceInjector = &g_backspaceInjector;
    deps.lifetimeWeak = lifetime_;
    deps.debugEnabled = [this]() { return debugEnabled(); };
    backspaceRewriteHandler_ =
        std::make_unique<BackspaceRewriteModeHandler>(std::move(deps));
    reloadConfig();
}

OpenKeyEngine::~OpenKeyEngine() {
    adapter_.reset();
    lifetime_.reset();
}

std::string OpenKeyEngine::subModeLabelImpl(const fcitx::InputMethodEntry &,
                                           fcitx::InputContext &ic) {
    auto *state = stateFor(&ic);

    auto inputTypeLabel = [&](InputType t) -> std::string {
        switch (t) {
        case InputType::Telex:
            return "Telex";
        case InputType::VNI:
            return "VNI";
        case InputType::SimpleTelex1:
            return "Simple Telex 1";
        case InputType::SimpleTelex2:
            return "Simple Telex 2";
        }
        return "Telex";
    };

    auto runtimeModeLabel = [&](RuntimeMode m) -> std::string {
        switch (m) {
        case RuntimeMode::SurroundingText:
            return "Surrounding";
        case RuntimeMode::Preedit:
            return "Preedit";
        case RuntimeMode::BackspaceRewrite:
            return "Backspace";
        case RuntimeMode::DirectCommit:
            return "Direct";
        }
        return "Preedit";
    };

    std::string label = inputTypeLabel(config_.inputType.value());
    if (state) {
        label += " \xC2\xB7 "; // middle dot
        label += runtimeModeLabel(state->mode);
    }
    return label;
}

std::string OpenKeyEngine::subMode(const fcitx::InputMethodEntry &,
                                  fcitx::InputContext &ic) {
    auto *state = stateFor(&ic);
    if (!state) {
        return {};
    }
    switch (state->mode) {
    case RuntimeMode::SurroundingText:
        return "Surrounding";
    case RuntimeMode::Preedit:
        return "Preedit";
    case RuntimeMode::BackspaceRewrite:
        return "Backspace";
    case RuntimeMode::DirectCommit:
        return "Direct";
    }
    return {};
}

bool OpenKeyEngine::debugEnabled() const {
    if (config_.debug.value()) {
        return true;
    }
    const char *env = std::getenv("FCITX_OPENKEY_DEBUG");
    return env && env[0] && env[0] != '0';
}

void OpenKeyEngine::rebuildBlacklist() {
    surroundingBlacklist_.clear();
    for (auto &part : fcitx::stringutils::split(
             config_.surroundingTextBlacklist.value(), ",",
             fcitx::stringutils::SplitBehavior::SkipEmpty)) {
        auto s = fcitx::stringutils::trim(part);
        if (!s.empty()) {
            surroundingBlacklist_.insert(std::move(s));
        }
    }
}

OpenKeyState *OpenKeyEngine::stateFor(fcitx::InputContext *ic) {
    return ic->propertyFor(&factory_);
}

const fcitx::Configuration *OpenKeyEngine::getConfig() const { return &config_; }

void OpenKeyEngine::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    applyConfig();
}

void OpenKeyEngine::reloadConfig() {
    fcitx::readAsIni(config_, fcitx::StandardPath::Type::PkgConfig,
                     "conf/openkey.conf");
    applyConfig();
}

void OpenKeyEngine::applyConfig() {
    rebuildBlacklist();
    adapter_->setInputType(toOpenKeyInputType(config_.inputType.value()));
    adapter_->setFreeMark(config_.freeMark.value());
    adapter_->setCodeTable(toOpenKeyCodeTable(config_.codeTable.value()));
    adapter_->setCheckSpelling(config_.checkSpelling.value());
    adapter_->setUseModernOrthography(config_.useModernOrthography.value());
    adapter_->setQuickTelex(config_.quickTelex.value());
    adapter_->setRestoreIfWrongSpelling(config_.restoreIfWrongSpelling.value());
    adapter_->setFixRecommendBrowser(config_.fixRecommendBrowser.value());
    adapter_->setUpperCaseFirstChar(config_.upperCaseFirstChar.value());
    adapter_->setAllowConsonantZFWJ(config_.allowConsonantZFWJ.value());
    adapter_->setQuickStartConsonant(config_.quickStartConsonant.value());
    adapter_->setQuickEndConsonant(config_.quickEndConsonant.value());
    adapter_->setUseMacro(config_.enableMacro.value());
    adapter_->setUseMacroInEnglishMode(false);
    adapter_->setAutoCapsMacro(config_.autoCapsMacro.value());
    adapter_->setUseSmartSwitchKey(false);
    adapter_->setRememberCode(false);
    adapter_->setOtherLanguage(config_.otherLanguage.value());
    adapter_->setTempOffSpelling(config_.tempOffSpelling.value());
    adapter_->setTempOffOpenKey(config_.tempOffOpenKey.value());

    // (Re)load macro file if enabled.
    initMacroMap(nullptr, 0);
    if (config_.enableMacro.value() && !config_.macroFile.value().empty()) {
        readFromFile(config_.macroFile.value(), false);
    }
}

void OpenKeyEngine::persistConfig() {
    blacklistDirty_ = false;
    fcitx::safeSaveAsIni(config_, fcitx::StandardPath::Type::PkgConfig,
                         "conf/openkey.conf");
}

void OpenKeyEngine::save() { persistConfig(); }

void OpenKeyEngine::activate(const fcitx::InputMethodEntry &,
                             fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    state->rawBuffer.clear();
    state->shownText.clear();
    state->hasRewrittenCurrentWord = false;
    state->rewriteLock = false;
    state->isInjecting = false;
    state->injectingUntilUsec = 0;
    state->pendingKeys.clear();
    state->rewriteTimer.reset();
    state->commitTimer.reset();
    state->pendingConvertedText.clear();
    state->hasPendingBoundaryKey = false;
    state->composing.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    state->noSeedNextWord = false;
    state->surroundingFailures = 0;
    state->manualMode = false;
    state->modeDecided = false;
    state->program = ic->program();

    state->codeTable = toOpenKeyCodeTable(config_.codeTable.value());

    updatePreeditUI(ic, *state);
}

void OpenKeyEngine::deactivate(const fcitx::InputMethodEntry &entry,
                               fcitx::InputContextEvent &event) {
    reset(entry, event);
}

void OpenKeyEngine::reset(const fcitx::InputMethodEntry &,
                          fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    state->rawBuffer.clear();
    state->shownText.clear();
    state->hasRewrittenCurrentWord = false;
    state->rewriteLock = false;
    state->isInjecting = false;
    state->injectingUntilUsec = 0;
    state->pendingKeys.clear();
    state->rewriteTimer.reset();
    state->commitTimer.reset();
    state->pendingConvertedText.clear();
    state->hasPendingBoundaryKey = false;
    state->composing.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    updatePreeditUI(ic, *state);
}

RuntimeMode OpenKeyEngine::decideMode(fcitx::InputContext *ic, OpenKeyState &s) {
    // Password field should never attempt to do any composition.
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Password)) {
        return RuntimeMode::DirectCommit;
    }

    // Wine/Proton clients are very sensitive to surrounding-text/backspace
    // rewriting. Force preedit unconditionally for these applications.
    if (isWineProgram(s.program)) {
        return RuntimeMode::Preedit;
    }

    const auto canUseSurroundingText = [&]() -> bool {
        const auto &st = ic->surroundingText();
        if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
            return false;
        }
        if (!st.isValid() || st.cursor() != st.anchor()) {
            return false;
        }
        if (!s.program.empty() &&
            surroundingBlacklist_.find(s.program) != surroundingBlacklist_.end()) {
            return false;
        }
        if (!fcitx::utf8::validate(st.text())) {
            return false;
        }
        if (st.cursor() > fcitx::utf8::length(st.text())) {
            return false;
        }
        return true;
    };

    const bool clientPreeditSupported =
        ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit);
    const bool backspaceRewriteSupported = true;

    // Explicit overrides.
    switch (config_.mode.value()) {
    case ModeOverride::DirectCommit:
        return RuntimeMode::DirectCommit;
    case ModeOverride::ForceSurroundingText:
        // If forced surrounding-text can't be used, fallback to preedit.
        return canUseSurroundingText() ? RuntimeMode::SurroundingText
                                       : RuntimeMode::Preedit;
    case ModeOverride::ForcePreedit:
        return RuntimeMode::Preedit;
    case ModeOverride::ForceBackspaceRewrite:
        // If backspace rewrite can't be used for any reason, fallback to preedit.
        return backspaceRewriteSupported ? RuntimeMode::BackspaceRewrite
                                        : RuntimeMode::Preedit;
    case ModeOverride::Auto:
        break;
    }

    // Auto mode priority: SurroundingText -> Preedit -> BackspaceRewrite -> DirectCommit.
    if (canUseSurroundingText()) {
        return RuntimeMode::SurroundingText;
    }
    if (clientPreeditSupported) {
        return RuntimeMode::Preedit;
    }
    if (backspaceRewriteSupported) {
        return RuntimeMode::BackspaceRewrite;
    }
    return RuntimeMode::DirectCommit;
}

void OpenKeyEngine::addProgramToBlacklist(const std::string &program) {
    if (program.empty() || surroundingBlacklist_.find(program) != surroundingBlacklist_.end()) {
        return;
    }
    surroundingBlacklist_.insert(program);

    // Persist to config as comma-separated list.
    std::string merged = config_.surroundingTextBlacklist.value();
    if (!merged.empty() && merged.back() != ',') {
        merged.push_back(',');
    }
    merged += program;
    config_.surroundingTextBlacklist.setValue(std::move(merged));
    blacklistDirty_ = true;
    persistConfig();
}

void OpenKeyEngine::updatePreeditUI(fcitx::InputContext *ic,
                                   const OpenKeyState &state) {
    auto &panel = ic->inputPanel();
    panel.reset();

    if (state.composing.empty()) {
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
        return;
    }

    fcitx::Text text;
    text.append(state.composing, fcitx::TextFormatFlag::Underline);
    text.setCursor(static_cast<int>(state.composing.size()));

    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
        panel.setClientPreedit(text);
    } else {
        panel.setPreedit(text);
    }

    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
}

void OpenKeyEngine::commitAndClearPreedit(fcitx::InputContext *ic,
                                         OpenKeyState &state) {
    if (state.composing.empty()) {
        return;
    }
    ic->commitString(state.composing);
    state.lastCommitted = state.composing;
    state.composing.clear();
    updatePreeditUI(ic, state);
}

bool OpenKeyEngine::handlePreedit(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                                 OpenKeyState &state) {
    auto key = event.key().normalize();

    auto commitPreeditAndMaybeAppend = [&](const std::string &suffixUtf8) {
        if (state.composing.empty()) {
            return false;
        }
        std::string out = state.composing;
        out += suffixUtf8;
        ic->commitString(out);
        state.lastCommitted = state.composing;
        state.composing.clear();
        updatePreeditUI(ic, state);
        event.filterAndAccept();
        return true;
    };

    // Navigation / editing keys should terminate preedit but still be handled
    // by application.
    if (key.isCursorMove() || key.check(FcitxKey_Delete) ||
        key.check(FcitxKey_Tab)) {
        commitAndClearPreedit(ic, state);
        return false;
    }

    if (key.check(FcitxKey_BackSpace)) {
        if (!state.composing.empty()) {
            // delete one UTF-8 char
            const auto len = fcitx::utf8::length(state.composing);
            if (len > 0) {
                auto it = fcitx::utf8::nextNChar(state.composing.begin(), len - 1);
                state.composing.erase(it, state.composing.end());
            } else {
                state.composing.clear();
            }
            updatePreeditUI(ic, state);
            event.filterAndAccept();
            return true;
        }
        return false;
    }

    if (key.check(FcitxKey_Escape)) {
        if (!state.composing.empty()) {
            state.composing.clear();
            updatePreeditUI(ic, state);
            event.filterAndAccept();
            return true;
        }
        return false;
    }

    if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter) ||
        key.check(FcitxKey_ISO_Enter)) {
        // Commit composing, then let client handle Enter.
        commitAndClearPreedit(ic, state);
        return false;
    }

    const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
    const std::string utf8 = fcitx::Key::keySymToUTF8(key.sym());

    // Commit composing before any non-ascii printable char.
    if (!state.composing.empty() && (utf8.empty() || uni > 0x7F)) {
        commitAndClearPreedit(ic, state);
        return false;
    }

    if (uni >= 0x20 && uni <= 0x7E) {
        const char c = static_cast<char>(uni);

        // Break keys.
        //
        // On some frontends (notably Wayland), the boundary key that ended the
        // preedit may not reliably reach the client. Commit the boundary
        // character together with the preedit string to avoid requiring users
        // to press the key twice and to avoid selection/range edge cases with
        // multiple consecutive commitString() calls.
        if (c == ' ') {
            if (config_.enableMacro.value() && !state.composing.empty()) {
                std::string replacement;
                if (adapter_->expandMacro(state.composing, replacement)) {
                    state.composing = std::move(replacement);
                }
            }
            return commitPreeditAndMaybeAppend(" ");
        }
        if (!isComposingASCII(c)) {
            const std::string boundaryUtf8 = fcitx::Key::keySymToUTF8(key.sym());
            if (config_.enableMacro.value() && isMacroTriggerKey(c) &&
                !state.composing.empty()) {
                std::string replacement;
                if (adapter_->expandMacro(state.composing, replacement)) {
                    state.composing = std::move(replacement);
                }
            }
            if (!boundaryUtf8.empty()) {
                return commitPreeditAndMaybeAppend(boundaryUtf8);
            }
            commitAndClearPreedit(ic, state);
            return false;
        }

        auto r = adapter_->processAsciiKey(state.composing, c);
        if (!r.handled) {
            return false;
        }
        state.composing = std::move(r.newWord);
        updatePreeditUI(ic, state);
        event.filterAndAccept();
        return true;
    }

    return false;
}

bool OpenKeyEngine::handleSurroundingText(fcitx::InputContext *ic,
                                         fcitx::KeyEvent &event,
                                         OpenKeyState &state) {
    auto key = event.key().normalize();

    // Preedit buffer should be empty in surrounding mode.
    state.composing.clear();

    if (debugEnabled()) {
        FCITX_INFO() << "openkey: st key program=" << state.program
                     << " sym=" << key.sym()
                     << " rollbackDisplay=" << state.rollbackDisplay
                     << " rollbackWord=" << state.rollbackWord;
    }

    if (key.check(FcitxKey_Delete) ||
        key.isCursorMove()) {
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.noSeedNextWord = false;
        return false;
    }

    if (key.check(FcitxKey_BackSpace)) {
        if (!state.rollbackWord.empty()) {
            if (!fcitx::utf8::validate(state.rollbackWord) ||
                !fcitx::utf8::validate(state.rollbackDisplay)) {
                state.rollbackWord.clear();
                state.rollbackDisplay.clear();
                return false;
            }
            // delete one UTF-8 char from rollbackWord
            const auto len = fcitx::utf8::length(state.rollbackWord);
            if (len > 0) {
                auto it =
                    fcitx::utf8::nextNChar(state.rollbackWord.begin(), len - 1);
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
                state.surroundingFailures++;
                return false;
            }
            if (deleteChars > 0) {
                ic->deleteSurroundingText(-static_cast<int>(deleteChars),
                                          deleteChars);
            }
            if (newDisplay.size() > prefixLen) {
                ic->commitString(newDisplay.substr(prefixLen));
            }
            state.rollbackDisplay = std::move(newDisplay);
            if (debugEnabled()) {
                FCITX_INFO() << "openkey: st bs program=" << state.program
                             << " deleteChars=" << deleteChars
                             << " newDisplay=" << state.rollbackDisplay;
            }
            event.filterAndAccept();
            return true;
        }
        if (!state.macroBuffer.empty()) {
            state.macroBuffer.pop_back();
        }
        // no rollback state: let app handle backspace
        return false;
    }

    if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter) ||
        key.check(FcitxKey_ISO_Enter)) {
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.noSeedNextWord = false;
        return false;
    }

    const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
    if (!(uni >= 0x20 && uni <= 0x7E)) {
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.noSeedNextWord = false;
        return false;
    }
    const char c = static_cast<char>(uni);

    // Keep rollback state in sync with actual surrounding text. Some clients
    // (notably Chromium/Electron) may report surrounding text but ignore
    // deleteSurroundingText in some fields, causing duplicate characters if we
    // keep applying deltas. Detect the mismatch and count it as a failure so
    // we can fallback to preedit mode for that app.
    if (isComposingASCII(c) && !state.rollbackDisplay.empty()) {
        const auto &st = ic->surroundingText();
        if (!st.isValid() || st.cursor() != st.anchor() ||
            !fcitx::utf8::validate(st.text()) ||
            st.cursor() > fcitx::utf8::length(st.text())) {
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.surroundingFailures += 3;
            return false;
        }
        WordSegment seg;
        if (!extractWordBeforeCursor(st.text(), st.cursor(), seg) ||
            seg.word != state.rollbackDisplay) {
            if (debugEnabled()) {
                FCITX_INFO() << "openkey: st desync program=" << state.program
                             << " expected=" << state.rollbackDisplay
                             << " actual=" << seg.word;
            }
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            // Desync is a strong signal the client can't reliably apply our
            // deleteSurroundingText deltas. Escalate quickly to fallback mode.
            state.surroundingFailures += 3;
            return false;
        }
    }

    // Do not try to "compose" across whitespace.
    if (c == ' ' || c == '\t') {
        if (config_.enableMacro.value() && !state.rollbackDisplay.empty()) {
            std::string replacement;
            if (adapter_->expandMacro(state.rollbackDisplay, replacement)) {
                const unsigned int deleteChars =
                    utf8CharCount(state.rollbackDisplay);
                if (deleteChars > 0 && deleteChars <= 128) {
                    ic->deleteSurroundingText(-static_cast<int>(deleteChars),
                                              deleteChars);
                    ic->commitString(replacement);
                }
            }
        }
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.noSeedNextWord = true;
        return false;
    }

    if (!isComposingASCII(c)) {
        if (config_.enableMacro.value() && isMacroTriggerKey(c) &&
            !state.rollbackDisplay.empty()) {
            std::string replacement;
            if (adapter_->expandMacro(state.rollbackDisplay, replacement)) {
                const unsigned int deleteChars =
                    utf8CharCount(state.rollbackDisplay);
                if (deleteChars > 0 && deleteChars <= 128) {
                    ic->deleteSurroundingText(-static_cast<int>(deleteChars),
                                              deleteChars);
                    ic->commitString(replacement);
                }
            }
        }
        state.macroBuffer.clear();
        // Let application handle punctuation and break keys.
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.noSeedNextWord = true;
        return false;
    }

    if (state.noSeedNextWord) {
        // We just ended a word boundary (space/punctuation) and the next
        // composing key should start a new word. Some clients (e.g. LibreOffice)
        // may not report trailing whitespace in surrounding text reliably, so
        // seeding from surrounding text would accidentally reuse previous word.
        state.noSeedNextWord = false;
    } else if (state.rollbackWord.empty()) {
        auto &st = ic->surroundingText();
        if (st.isValid() && st.cursor() == st.anchor()) {
            WordSegment seg;
            if (extractWordBeforeCursor(st.text(), st.cursor(), seg)) {
                state.rollbackWord = seg.word;
                state.rollbackDisplay = seg.word;
                if (debugEnabled()) {
                    FCITX_INFO() << "openkey: st seed program=" << state.program
                                 << " st.cursor=" << st.cursor()
                                 << " st.textLen=" << fcitx::utf8::length(st.text())
                                 << " seed=" << seg.word;
                }
            }
        }
    }

    auto r = adapter_->processAsciiKey(state.rollbackWord, c);
    if (!r.handled) {
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        return false;
    }
    if (!fcitx::utf8::validate(r.newWord)) {
        if (debugEnabled()) {
            FCITX_WARN() << "openkey: invalid utf8 from adapter program="
                         << state.program;
        }
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        return false;
    }

    // Direct rollback: only delete the delta part and commit the delta part.
    const std::size_t prefixLen =
        commonPrefixBytesUTF8Boundary(state.rollbackDisplay, r.newWord);
    const unsigned int deleteChars =
        utf8CharCount(state.rollbackDisplay.substr(prefixLen));
    if (deleteChars > 128) {
        // Safety guard: avoid deleting an unreasonable range if client state is
        // out-of-sync.
        if (debugEnabled()) {
            FCITX_WARN() << "openkey: deleteChars too large program="
                         << state.program << " deleteChars=" << deleteChars
                         << " rollbackDisplay=" << state.rollbackDisplay
                         << " newWord=" << r.newWord;
        }
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.surroundingFailures++;
        return false;
    }
    if (deleteChars > 0) {
        ic->deleteSurroundingText(-static_cast<int>(deleteChars), deleteChars);
    }
    if (r.newWord.size() > prefixLen) {
        ic->commitString(r.newWord.substr(prefixLen));
    }
    if (debugEnabled()) {
        FCITX_INFO() << "openkey: st apply program=" << state.program
                     << " deleteChars=" << deleteChars
                     << " commitDelta=" << r.newWord.substr(prefixLen)
                     << " newWord=" << r.newWord;
    }

    state.rollbackWord = r.newWord;
    state.rollbackDisplay = r.newWord;
    state.lastCommitted = state.rollbackDisplay;
    state.surroundingFailures = 0;
    event.filterAndAccept();
    return true;
}

bool OpenKeyEngine::handleBackspaceRewrite(fcitx::InputContext *ic,
                                          fcitx::KeyEvent &event,
                                          OpenKeyState &state) {
    // Kept for compatibility during refactor; keyEvent() routes to the
    // strategy handler instead of calling this directly.
    if (backspaceRewriteHandler_) {
        return backspaceRewriteHandler_->handleKey(ic, event, state);
    }
    auto key = event.key().normalize();

    // Never do anything on key release in this mode.
    if (event.isRelease()) {
        return false;
    }

    const uint64_t nowUsec = fcitx::now(CLOCK_MONOTONIC);

    // Guard: ignore injected backspace events that might loop back (uinput).
    if (state.isInjecting && nowUsec <= state.injectingUntilUsec &&
        key.check(FcitxKey_BackSpace) && !hasCtrlAltSuperMeta(key)) {
        return false;
    }

    // Do not swallow application shortcuts.
    if (hasCtrlAltSuperMeta(key)) {
        return false;
    }

    auto clearWordState = [&state]() {
        state.rawBuffer.clear();
        state.shownText.clear();
        state.hasRewrittenCurrentWord = false;
        state.rewriteTimer.reset();
    };

    const auto icRef = ic->watch();
    const std::weak_ptr<void> lifetimeWeak = lifetime_;
    const auto adapterShared = adapter_;
    const bool debug = debugEnabled();
    auto *loop = &instance_->eventLoop();

    auto drainPendingKeys = [this, icRef, lifetimeWeak]() {
        if (lifetimeWeak.expired()) {
            return;
        }
        auto *ic2 = icRef.get();
        if (!ic2) {
            return;
        }
        auto *st = stateFor(ic2);
        if (!st || st->pendingKeys.empty()) {
            return;
        }
        // Move out to avoid re-entrancy issues.
        auto keys = std::move(st->pendingKeys);
        st->pendingKeys.clear();
        for (const auto &k : keys) {
            fcitx::KeyEvent synthetic(ic2, k, false /* release */,
                                      0 /* time */);
            handleBackspaceRewrite(ic2, synthetic, *st);
        }
    };

    auto scheduleCommitAfterBackspace = [this, &state, icRef, lifetimeWeak, loop,
                                         drainPendingKeys](uint64_t delayUsec) {
        state.commitTimer.reset();
        const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
        state.commitTimer = loop->addTimeEvent(
            CLOCK_MONOTONIC, deadline, 0,
            [this, icRef, lifetimeWeak, drainPendingKeys](fcitx::EventSourceTime *,
                                            uint64_t) {
                // One-shot.
                if (lifetimeWeak.expired()) {
                    return false;
                }
                auto *ic2 = icRef.get();
                if (!ic2) {
                    return false;
                }
                auto *statePtr = stateFor(ic2);
                if (!statePtr) {
                    return false;
                }
                // Keep the EventSourceTime alive until callback returns.
                auto _timer = std::move(statePtr->commitTimer);

                const std::string converted = statePtr->pendingConvertedText;
                statePtr->pendingConvertedText.clear();

                // Commit converted Vietnamese text.
                if (!converted.empty()) {
                    ic2->commitString(converted);
                    statePtr->shownText = converted;
                    statePtr->hasRewrittenCurrentWord = true;
                }

                // Forward boundary key if requested.
                if (statePtr->hasPendingBoundaryKey) {
                    applyBoundaryKey(ic2, statePtr->pendingBoundaryKey);
                    statePtr->pendingBoundaryKey = fcitx::Key(FcitxKey_None);
                    statePtr->hasPendingBoundaryKey = false;
                    // Boundary ends the current word.
                    statePtr->rawBuffer.clear();
                    statePtr->shownText.clear();
                    statePtr->hasRewrittenCurrentWord = false;
                }

                statePtr->isInjecting = false;
                statePtr->injectingUntilUsec = 0;
                statePtr->rewriteLock = false;

                drainPendingKeys();
                return false;
            });
        state.commitTimer->setOneShot();
    };

    auto beginRewrite = [this, ic, &state, scheduleCommitAfterBackspace,
                         nowUsec](const char *reason) {
        if (state.rewriteLock) {
            return;
        }
        if (state.rawBuffer.empty()) {
            return;
        }

        adapter_->setCodeTable(state.codeTable);
        const std::string converted = adapter_->convertRawBuffer(state.rawBuffer);
        if (converted.empty() || converted == state.shownText) {
            return;
        }

        if (debugEnabled()) {
            FCITX_INFO() << "openkey: bs-rewrite start program=" << state.program
                         << " reason=" << reason
                         << " rawBuffer=" << state.rawBuffer
                         << " shownText=" << state.shownText
                         << " converted=" << converted;
        }

        state.rewriteTimer.reset();
        state.rewriteLock = true;
        state.isInjecting = true;
        state.pendingConvertedText = converted;

        const int bsCount = static_cast<int>(fcitx::utf8::length(state.shownText));
        if (bsCount > 0) {
            const auto method =
                g_backspaceInjector.sendBackspaces(ic, bsCount, debugEnabled());
            uint64_t commitDelayUsec = 15000;
            if (method == BackspaceInjector::Method::DeleteSurroundingText ||
                method == BackspaceInjector::Method::ForwardKey) {
                commitDelayUsec = 45000;
            } else if (method == BackspaceInjector::Method::Uinput) {
                commitDelayUsec = std::min<uint64_t>(
                    60000, 25000 + static_cast<uint64_t>(bsCount) * 2000);
            }
            state.injectingUntilUsec =
                nowUsec + std::min<uint64_t>(250000, commitDelayUsec + 80000);
            scheduleCommitAfterBackspace(commitDelayUsec);
            return;
        }

        // Commit after a brief delay so deletion is processed first.
        state.injectingUntilUsec = nowUsec + 50000;
        scheduleCommitAfterBackspace(15000); // 15ms
    };

    auto scheduleRewrite = [this, icRef, lifetimeWeak, adapterShared, debug, loop, &state](
                               uint64_t delayUsec,
                                             const char *reason) {
        if (state.rawBuffer.empty()) {
            return;
        }
        state.rewriteTimer.reset();
        const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
        state.rewriteTimer = loop->addTimeEvent(
            CLOCK_MONOTONIC, deadline, 0,
            [this, icRef, lifetimeWeak, adapterShared, debug, loop, reason](fcitx::EventSourceTime *, uint64_t) {
                if (lifetimeWeak.expired() || !adapterShared) {
                    return false;
                }
                auto *ic2 = icRef.get();
                if (!ic2) {
                    return false;
                }
                auto *statePtr = stateFor(ic2);
                if (!statePtr) {
                    return false;
                }
                // Keep the EventSourceTime alive until callback returns.
                auto _timer = std::move(statePtr->rewriteTimer);
                if (statePtr->rewriteLock) {
                    return false;
                }
                // beginRewrite is local; replicate minimal call here.
                adapterShared->setCodeTable(statePtr->codeTable);
                const std::string converted =
                    adapterShared->convertRawBuffer(statePtr->rawBuffer);
                if (converted.empty() || converted == statePtr->shownText) {
                    return false;
                }
                if (debug) {
                    FCITX_INFO() << "openkey: bs-rewrite timer program="
                                 << statePtr->program << " reason=" << reason
                                 << " rawBuffer=" << statePtr->rawBuffer
                                 << " shownText=" << statePtr->shownText
                                 << " converted=" << converted;
                }
                statePtr->rewriteLock = true;
                statePtr->isInjecting = true;
                const uint64_t now2 = fcitx::now(CLOCK_MONOTONIC);
                statePtr->pendingConvertedText = converted;
                const int bsCount =
                    static_cast<int>(fcitx::utf8::length(statePtr->shownText));
                uint64_t commitDelayUsec = 15000;
                if (bsCount > 0) {
                    const auto method =
                        g_backspaceInjector.sendBackspaces(ic2, bsCount, debug);
                    if (method == BackspaceInjector::Method::DeleteSurroundingText ||
                        method == BackspaceInjector::Method::ForwardKey) {
                        commitDelayUsec = 45000;
                    } else if (method == BackspaceInjector::Method::Uinput) {
                        commitDelayUsec = std::min<uint64_t>(
                            60000,
                            25000 + static_cast<uint64_t>(bsCount) * 2000);
                    }
                }
                statePtr->injectingUntilUsec =
                    now2 + std::min<uint64_t>(250000, commitDelayUsec + 80000);
                // Commit after a brief delay.
                statePtr->commitTimer.reset();
                OpenKeyState *st2 = statePtr;
                const uint64_t commitDeadline =
                    fcitx::now(CLOCK_MONOTONIC) + commitDelayUsec;
                // Use global event loop pointer; do not dereference `this`
                // after engine is unloaded.
                st2->commitTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, commitDeadline, 0,
                    [this, icRef, lifetimeWeak, st2](fcitx::EventSourceTime *, uint64_t) {
                        if (lifetimeWeak.expired()) {
                            return false;
                        }
                        auto *ic3 = icRef.get();
                        if (!ic3 || !st2) {
                            return false;
                        }
                        // Keep the EventSourceTime alive until callback returns.
                        auto _timer = std::move(st2->commitTimer);
                        const std::string conv = st2->pendingConvertedText;
                        st2->pendingConvertedText.clear();
                        if (!conv.empty()) {
                            ic3->commitString(conv);
                            st2->shownText = conv;
                            st2->hasRewrittenCurrentWord = true;
                        }
                        st2->isInjecting = false;
                        st2->injectingUntilUsec = 0;
                        st2->rewriteLock = false;
                        // Drain any keys collected during lock.
                        auto keys = std::move(st2->pendingKeys);
                        st2->pendingKeys.clear();
                        for (const auto &k : keys) {
                            fcitx::KeyEvent syn(ic3, k, false, 0);
                            handleBackspaceRewrite(ic3, syn, *st2);
                        }
                        return false;
                    });
                st2->commitTimer->setOneShot();
                return false;
            });
        state.rewriteTimer->setOneShot();

        if (debugEnabled()) {
            FCITX_INFO() << "openkey: bs-rewrite schedule program=" << state.program
                         << " reason=" << reason << " delayUsec=" << delayUsec
                         << " rawBuffer=" << state.rawBuffer
                         << " shownText=" << state.shownText;
        }
    };

    // If we are currently rewriting, queue and swallow all physical keys.
    if (state.rewriteLock) {
        state.pendingKeys.push_back(key);
        event.filterAndAccept();
        return true;
    }

    // Cursor move / delete: end composing state.
    if (key.isCursorMove() || key.check(FcitxKey_Delete)) {
        clearWordState();
        return false;
    }

    // Physical BackSpace: delete one visible rune and resync rawBuffer.
    if (key.check(FcitxKey_BackSpace)) {
        if (state.rawBuffer.empty() && state.shownText.empty()) {
            return false;
        }
        event.filterAndAccept();

        g_backspaceInjector.sendBackspaces(ic, 1, debugEnabled());
        if (!state.shownText.empty()) {
            state.shownText = utf8DropLastN(state.shownText, 1);
        }

        if (!state.rawBuffer.empty()) {
            state.rawBuffer.pop_back();
        }
        // Best-effort: trim rawBuffer until conversion matches shownText.
        for (int i = 0; i < 16 && !state.rawBuffer.empty(); i++) {
            adapter_->setCodeTable(state.codeTable);
            const std::string converted =
                adapter_->convertRawBuffer(state.rawBuffer);
            if (converted == state.shownText) {
                break;
            }
            state.rawBuffer.pop_back();
        }

        if (state.rawBuffer.empty()) {
            state.hasRewrittenCurrentWord = false;
        }
        return true;
    }

    // Printable ASCII path.
    const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
    if (uni >= 0x20 && uni <= 0x7E) {
        const char c = static_cast<char>(uni);

        // Boundary keys: rewrite immediately (if needed), then forward the
        // original boundary key and clear word state.
        if (isBoundaryASCII(c) || key.check(FcitxKey_Return) ||
            key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_ISO_Enter) ||
            key.check(FcitxKey_Tab)) {
            if (!state.rawBuffer.empty()) {
                event.filterAndAccept();
                // Stash boundary key and rewrite immediately.
                state.hasPendingBoundaryKey = true;
                state.pendingBoundaryKey = event.rawKey();
                beginRewrite("boundary");
                // If rewrite didn't happen, still forward boundary and clear.
                if (!state.rewriteLock) {
                    applyBoundaryKey(ic, event.rawKey());
                    clearWordState();
                }
                return true;
            }
            return false;
        }

        // Word characters: update state and commit raw char to client.
        if (isComposingASCII(c)) {
            state.rawBuffer.push_back(c);
            const std::string vis = fcitx::Key::keySymToUTF8(key.sym());
            if (!vis.empty()) {
                state.shownText += vis;
            }
            state.lastPhysicalKeyUsec = nowUsec;

            event.filterAndAccept();
            if (!vis.empty()) {
                ic->commitString(vis);
            }

            // Schedule rewrites based on chunks and idle.
            if (shouldTriggerRewriteOnChunk(state.rawBuffer)) {
                scheduleRewrite(30000, "chunk"); // 30ms
            }
            // TODO: idle-based rewrite is not working well currently: some apps (e.g. Slack) may delay key events when we have pending timers, which causes noticeable input lag. We may want to revisit this in the future with a more robust implementation (e.g. using a separate thread and timer).
            // } else {
            //     scheduleRewrite(500000, "idle"); // 500ms
            // }
            return true;
        }

        // Other printable ASCII: treat as boundary of current word.
        if (!state.rawBuffer.empty()) {
            event.filterAndAccept();
            state.hasPendingBoundaryKey = true;
            state.pendingBoundaryKey = event.rawKey();
            beginRewrite("other_ascii");
            if (!state.rewriteLock) {
                applyBoundaryKey(ic, event.rawKey());
                clearWordState();
            }
            return true;
        }
        return false;
    }

    // Any other key: end composing state, let application handle.
    clearWordState();
    return false;
}

void OpenKeyEngine::keyEvent(const fcitx::InputMethodEntry &,
                             fcitx::KeyEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    if (state->program.empty()) {
        state->program = ic->program();
    }

    if (event.isRelease()) {
        return;
    }

    const auto key = event.key().normalize();

    // Switch composition mode hotkey (Auto -> ST -> Preedit -> Backspace -> Direct -> Auto).
    if (key.checkKeyList(config_.switchModeKey.value()) && key.sym() != FcitxKey_None) {
        auto clearComposingState = [this, ic, state]() {
            state->rawBuffer.clear();
            state->shownText.clear();
            state->hasRewrittenCurrentWord = false;
            state->rewriteLock = false;
            state->isInjecting = false;
            state->injectingUntilUsec = 0;
            state->pendingKeys.clear();
            state->rewriteTimer.reset();
            state->commitTimer.reset();
            state->pendingConvertedText.clear();
            state->hasPendingBoundaryKey = false;
            state->composing.clear();
            state->macroBuffer.clear();
            state->rollbackWord.clear();
            state->rollbackDisplay.clear();
            state->noSeedNextWord = false;
            state->surroundingFailures = 0;
            updatePreeditUI(ic, *state);
        };

        // Wine/Proton clients: always stay in preedit mode (disable other
        // runtime mode options).
        if (isWineProgram(state->program)) {
            state->manualMode = true;
            state->modeDecided = true;
            // Keep password fields safe: never attempt composition there.
            if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Password)) {
                state->mode = RuntimeMode::DirectCommit;
                state->autoMode = RuntimeMode::DirectCommit;
            } else {
                state->mode = RuntimeMode::Preedit;
                state->autoMode = RuntimeMode::Preedit;
            }
            if (debugEnabled()) {
                FCITX_INFO() << "openkey: force mode for wine program="
                             << state->program
                             << " mode=" << static_cast<int>(state->mode);
            }
            clearComposingState();
            if (instance_) {
                instance_->showInputMethodInformation(ic);
            }
            // Continue with the existing toast code path below.
        } else {
        // Ensure we have a baseline auto mode for this context.
        if (!state->modeDecided) {
            state->lastCapability = ic->capabilityFlags();
            state->mode = decideMode(ic, *state);
            state->autoMode = state->mode;
            state->modeDecided = true;
        }

        const auto canUseSurroundingText = [&]() -> bool {
            const auto &st = ic->surroundingText();
            if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
                return false;
            }
            if (!st.isValid() || st.cursor() != st.anchor()) {
                return false;
            }
            if (!state->program.empty() &&
                surroundingBlacklist_.find(state->program) != surroundingBlacklist_.end()) {
                return false;
            }
            if (!fcitx::utf8::validate(st.text())) {
                return false;
            }
            if (st.cursor() > fcitx::utf8::length(st.text())) {
                return false;
            }
            return true;
        };

        if (!state->manualMode) {
            // Enter manual mode.
            state->manualMode = true;
            state->mode = canUseSurroundingText() ? RuntimeMode::SurroundingText
                                                  : RuntimeMode::Preedit;
        } else if (state->mode == RuntimeMode::SurroundingText) {
            state->mode = RuntimeMode::Preedit;
        } else if (state->mode == RuntimeMode::Preedit) {
            state->mode = RuntimeMode::BackspaceRewrite;
        } else if (state->mode == RuntimeMode::BackspaceRewrite) {
            state->mode = RuntimeMode::DirectCommit;
        } else if (state->mode == RuntimeMode::DirectCommit) {
            // Leave manual mode and go back to auto decision.
            state->manualMode = false;
            state->modeDecided = false;
            state->lastCapability = ic->capabilityFlags();
            state->mode = decideMode(ic, *state);
            state->autoMode = state->mode;
            state->modeDecided = true;
        }

        if (debugEnabled()) {
            FCITX_INFO() << "openkey: switch mode hotkey program=" << state->program
                         << " manual=" << (state->manualMode ? 1 : 0)
                         << " mode=" << static_cast<int>(state->mode);
        }
        clearComposingState();

        // Show mode information immediately when hotkey is pressed.
        if (instance_) {
            instance_->showInputMethodInformation(ic);
        }
        }
        // Fallback "toast" in input panel for UIs that don't display the popup.
        {
            auto inputTypeLabel = [&](InputType t) -> std::string {
                switch (t) {
                case InputType::Telex:
                    return "Telex";
                case InputType::VNI:
                    return "VNI";
                case InputType::SimpleTelex1:
                    return "Simple Telex 1";
                case InputType::SimpleTelex2:
                    return "Simple Telex 2";
                }
                return "Telex";
            };
            auto runtimeModeLabel = [&](RuntimeMode m) -> std::string {
                switch (m) {
                case RuntimeMode::SurroundingText:
                    return "Surrounding";
                case RuntimeMode::Preedit:
                    return "Preedit";
                case RuntimeMode::BackspaceRewrite:
                    return "Backspace";
                case RuntimeMode::DirectCommit:
                    return "Direct";
                }
                return "Preedit";
            };

            const std::string toast =
                std::string("OpenKey (") + inputTypeLabel(config_.inputType.value()) +
                " \xC2\xB7 " + runtimeModeLabel(state->mode) + ")";

            fcitx::Text aux;
            aux.append(toast);
            ic->inputPanel().setAuxUp(aux);
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);

            state->modeInfoTimer.reset();
            const auto icRef = ic->watch();
            const std::weak_ptr<void> lifetimeWeak = lifetime_;
            const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + 800000; // 800ms
            state->modeInfoTimer = instance_->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, deadline, 0,
                [this, icRef, lifetimeWeak, toast](fcitx::EventSourceTime *,
                                                  uint64_t) {
                    if (lifetimeWeak.expired()) {
                        return false;
                    }
                    auto *ic2 = icRef.get();
                    if (!ic2) {
                        return false;
                    }
                    auto *st = stateFor(ic2);
                    if (!st) {
                        return false;
                    }
                    auto _timer = std::move(st->modeInfoTimer);

                    // Clear only if still showing our toast.
                    const auto &current = ic2->inputPanel().auxUp().toString();
                    if (current == toast) {
                        ic2->inputPanel().setAuxUp(fcitx::Text());
                        ic2->updateUserInterface(
                            fcitx::UserInterfaceComponent::InputPanel, true);
                    }
                    return false;
                });
        }

        event.filterAndAccept();
        return;
    }

    // Do not swallow application shortcuts, but ensure we don't leave a stale
    // composing buffer in preedit mode.
    if (hasCtrlAltSuperMeta(key)) {
        if (state->modeDecided && state->mode == RuntimeMode::Preedit &&
            !state->composing.empty()) {
            commitAndClearPreedit(ic, *state);
        }
        return;
    }

    // Decide mode per input context.
    if (!state->modeDecided) {
        state->lastCapability = ic->capabilityFlags();
        state->mode = decideMode(ic, *state);
        state->autoMode = state->mode;
        state->modeDecided = true;

        if (debugEnabled()) {
            FCITX_INFO() << "openkey: detect mode program=" << state->program
                         << " cap=" << ic->capabilityFlags()
                         << " mode=" << static_cast<int>(state->mode);
        }
    }

    bool handled = false;
    switch (state->mode) {
    case RuntimeMode::DirectCommit:
        return;
    case RuntimeMode::BackspaceRewrite:
        if (backspaceRewriteHandler_) {
            handled = backspaceRewriteHandler_->handleKey(ic, event, *state);
        }
        return;
    case RuntimeMode::Preedit:
        adapter_->setCodeTable(state->codeTable);
        handled = handlePreedit(ic, event, *state);
        return;
    case RuntimeMode::SurroundingText:
        adapter_->setCodeTable(state->codeTable);
        handled = handleSurroundingText(ic, event, *state);
        if (!handled) {
            // If surrounding text is flaky for this app, fallback to preedit.
            // Heuristic: repeated failures to use surrounding text.
            if (state->surroundingFailures >= 3) {
                if (debugEnabled()) {
                    FCITX_INFO() << "openkey: fallback to preedit program="
                                 << state->program
                                 << " reason=surrounding_failures";
                }
                addProgramToBlacklist(state->program);
                state->mode = RuntimeMode::Preedit;
                // Re-handle this key in preedit mode immediately so the key
                // doesn't leak to application as raw ASCII when we fallback.
                adapter_->setCodeTable(state->codeTable);
                (void)handlePreedit(ic, event, *state);
            }
        }
        return;
    }
}

class OpenKeyEngineFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new OpenKeyEngine(manager->instance());
    }
};

} // namespace openkey

FCITX_ADDON_FACTORY(openkey::OpenKeyEngineFactory)
