#include "openkey.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <unordered_set>
#include <vector>

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

static bool startsWithASCIIInsensitive(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    const size_t n = std::strlen(prefix);
    for (size_t i = 0; i < n; i++) {
        if (!s[i]) return false;
        if (toLowerASCII(s[i]) != toLowerASCII(prefix[i])) return false;
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

static bool isBrowserProgram(const std::string &program) {
    if (program.empty()) {
        return false;
    }

    const std::string base = asciiLower(programBaseName(program));

    static const std::vector<std::string> kBrowserPatterns = {
        "chrome",
        "chromium",
        "edge",
        "msedge",
        "brave",
        "vivaldi",
        "opera",
        "coccoc",
        "yandex",
        "firefox",
        "librewolf",
        "waterfox",
        "floorp",
        "zen",
        "epiphany",
        "falkon",
        "midori",
        "qutebrowser",
        "palemoon",
        "basilisk",
        "nyxt",
        "otter",
        "dooble",
        "messenger"
    };

    for (const auto &pattern : kBrowserPatterns) {
    if (base.find(asciiLower(pattern)) != std::string::npos) {
        return true;
    }
}

    return false;
}


static bool isX11Backend(fcitx::InputContext *ic) {
    const char *frontend = ic ? ic->frontend() : nullptr;
    if (frontend) {
        if (startsWithASCIIInsensitive(frontend, "wayland")) {
            return false;
        }
        if (equalsASCIIInsensitive(frontend, "xim")) {
            return true;
        }
    }
    const char *sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType && sessionType[0]) {
        if (equalsASCIIInsensitive(sessionType, "x11")) {
            return true;
        }
        if (equalsASCIIInsensitive(sessionType, "wayland")) {
            return false;
        }
    }
    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay && waylandDisplay[0]) {
        return false;
    }
    const char *display = std::getenv("DISPLAY");
    return display && display[0];
}


static bool shouldUseDST(fcitx::InputContext *ic, int count) {
    if (!ic) return false;

    // Rule 1: app bị skip thủ công
    if (isBrowserProgram(ic->program())) return false;

    // Rule 3: app phải support surrounding text
    if (ic->program().empty() &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        // Trường hợp này gần như chỉ xảy ra với Chrome/Chromium trên Wayland
        return false;
    }

    const auto &st = ic->surroundingText();

    // Rule 4: surrounding text phải hợp lệ
    if (!st.isValid()) return false;

    // Rule 5: không xử lý khi đang select text
    if (st.cursor() != st.anchor()) return false;

    // Rule 6: đủ ký tự để xóa
    if (static_cast<unsigned int>(count) > st.cursor()) return false;

    return true;
}

class BackspaceInjector {
public:
    ~BackspaceInjector() {
        destroyServer();
        destroyUinput();
    }

    enum class Method {
        DeleteSurroundingText,
        Uinput,
        None,
    };

    Method sendBackspaces(fcitx::InputContext *ic, int count, bool debug,
                          uint64_t uinputInterKeyUsec = 1500) {
        (void)ic;
        if (count <= 0) {
            return Method::None;
        }
            // Dùng deleteSurroundingText cho app reliable, trừ Chrome
        if (shouldUseDST(ic, count)) {
            ic->deleteSurroundingText(-count, count);
            return Method::DeleteSurroundingText;
        }

        if (ensureServer(debug)) {
            if (sendBackspacesServer(count, debug)) {
                return Method::Uinput;
            }
        }
        if (ensureUinput(debug)) {
            sendBackspacesUinput(count, uinputInterKeyUsec);
            return Method::Uinput;
        }
        return Method::None;
    }

    bool uinputAvailable(bool debug) { return ensureUinput(debug); }

private:
    int fd_ = -1;
    bool tried_ = false;
    int serverFd_ = -1;

    void destroyServer() {
#ifdef __linux__
        if (serverFd_ >= 0) {
            ::close(serverFd_);
        }
#endif
        serverFd_ = -1;
    }

    static std::string buildServerSocketName() {
        struct passwd pwd {};
        struct passwd *result = nullptr;
        long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufSize <= 0) {
            bufSize = 16384;
        }
        std::vector<char> buf(static_cast<std::size_t>(bufSize));
        std::string username;
        const int res =
            getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result);
        if (res == 0 && result) {
            username = result->pw_name;
        } else {
            username = "unknown";
        }

        std::string name;
        name.reserve(64);
        name += "openkeysocket-";
        name += username;
        name += "-kb_socket";
        constexpr std::size_t kMax = 100;
        if (name.size() > kMax) {
            name.resize(kMax);
        }
        return name;
    }

    bool ensureServer(bool debug) {
#ifndef __linux__
        (void)debug;
        return false;
#else
        (void)debug;
        if (serverFd_ >= 0) {
            return true;
        }

        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            return false;
        }

        const std::string socketName = buildServerSocketName();
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';
        std::memcpy(&addr.sun_path[1], socketName.data(), socketName.size());
        const socklen_t len = static_cast<socklen_t>(
            offsetof(struct sockaddr_un, sun_path) + socketName.size() + 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), len) !=
            0) {
            ::close(fd);
            return false;
        }
        serverFd_ = fd;
        return true;
#endif
    }

    bool sendBackspacesServer(int count, bool debug) {
#ifndef __linux__
        (void)count;
        (void)debug;
        return false;
#else
        if (serverFd_ < 0) {
            return false;
        }
        const ssize_t n =
            ::send(serverFd_, &count, sizeof(count), MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(sizeof(count))) {
            return true;
        }
        if (debug) {
            FCITX_WARN() << "openkey: uinput-server send failed; reconnect";
        }
        destroyServer();
        return false;
#endif
    }

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
                FCITX_WARN() << "openkey: UI_DEV_CREATE failed";
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

    void sendBackspacesUinput(int count, uint64_t interKeyUsec) {
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
                if (interKeyUsec > 0) {
                    ::usleep(static_cast<useconds_t>(
                        std::min<uint64_t>(interKeyUsec, 1000000)));
                }
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
    std::function<uint64_t()> bsRewriteCommitExtraUsec;
    std::function<uint64_t()> bsRewriteCommitCapUsec;
    std::function<uint64_t()> bsRewriteUinputInterKeyUsec;
};

class BackspaceRewriteModeHandler final : public InputModeHandler {
public:
    explicit BackspaceRewriteModeHandler(ModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                   OpenKeyState &state) override {
        auto key = event.key().normalize();

        if (event.isRelease()) {
            return false;
        }

        const uint64_t nowUsec = fcitx::now(CLOCK_MONOTONIC);

        // Do not swallow application shortcuts.
        if (hasCtrlAltSuperMeta(key)) {
            return false;
        }

        const auto icRef = ic->watch();
        const std::weak_ptr<void> lifetimeWeak = deps_.lifetimeWeak;
        const auto adapterShared = deps_.adapter;
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        const uint64_t uinputInterKeyUsec =
            deps_.bsRewriteUinputInterKeyUsec
                ? deps_.bsRewriteUinputInterKeyUsec()
                : 1500;
        auto *loop = deps_.instance ? &deps_.instance->eventLoop() : nullptr;

        auto stateFor = [this](fcitx::InputContext *ic2) -> OpenKeyState * {
            if (!ic2 || !deps_.factory) {
                return nullptr;
            }
            return ic2->propertyFor(deps_.factory);
        };

        auto clearWordState = [&state]() {
            state.shownText.clear();
            state.hasRewrittenCurrentWord = false;
            state.waitingBackspaceAck = false;
            state.expectedBackspaces = 0;
            state.seenBackspaces = 0;
            state.pendingConvertedText.clear();
            state.pendingShownTextAfterCommit.clear();
            state.pendingKeys.clear();
            state.commitTimer.reset();
            state.rewriteLock = false;
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
                const bool handled = handleKey(ic2, synthetic, *st);
                if (!handled && !synthetic.accepted()) {
                    ic2->forwardKey(k);
                }
            }
        };

        auto scheduleCommitAfterBackspace =
            [this, icRef, lifetimeWeak, loop, stateFor, &state,
             drainPendingKeys](uint64_t delayUsec) {
                state.commitTimer.reset();
                if (!loop) {
                    return;
                }
                const uint64_t deadline =
                    fcitx::now(CLOCK_MONOTONIC) + delayUsec;
                state.commitTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [this, icRef, lifetimeWeak, stateFor,
                     drainPendingKeys](fcitx::EventSourceTime *, uint64_t) {
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
                        auto _timer = std::move(st->commitTimer);

                        const std::string commitText =
                            std::move(st->pendingConvertedText);
                        const std::string shownAfter =
                            std::move(st->pendingShownTextAfterCommit);
                        st->pendingConvertedText.clear();
                        st->pendingShownTextAfterCommit.clear();

                        if (!commitText.empty()) {
                            ic2->commitString(commitText);
                        }
                        st->shownText = shownAfter;
                        st->hasRewrittenCurrentWord = !st->shownText.empty();
                        st->rewriteLock = false;
                        st->waitingBackspaceAck = false;
                        st->expectedBackspaces = 0;
                        st->seenBackspaces = 0;

                        drainPendingKeys();
                        return false;
                    });
                if (state.commitTimer) {
                    state.commitTimer->setOneShot();
                }
            };

auto applyWordDelta = [&, this](const std::string &newWord,
                                const char *reason) -> bool {
    if (!deps_.backspaceInjector) {
        return false;
    }
    if (!fcitx::utf8::validate(state.shownText) ||
        !fcitx::utf8::validate(newWord)) {
        clearWordState();
        return false;
    }

    const std::size_t prefixLen =
        commonPrefixBytesUTF8Boundary(state.shownText, newWord);
    unsigned int deleteCount =
        utf8CharCount(state.shownText.substr(prefixLen));
    std::string commitText = newWord.substr(prefixLen);
    if (deleteCount > 128) {
        deleteCount = utf8CharCount(state.shownText);
        commitText = newWord;
    }

    if (debug) {
        FCITX_INFO() << "openkey: bs-delta program=" << state.program
                     << " reason=" << reason
                     << " from=" << state.shownText
                     << " to=" << newWord
                     << " delete=" << deleteCount
                     << " commit=" << commitText;
    }

    if (deleteCount == 0) {
        if (!commitText.empty()) {
            ic->commitString(commitText);
        }
        state.shownText = newWord;
        state.hasRewrittenCurrentWord = !state.shownText.empty();
        event.filterAndAccept();
        return true;
    }

    // Thử DeleteSurroundingText trước (GTK, Qt app reliable)
    const auto method = deps_.backspaceInjector->sendBackspaces(
        ic, static_cast<int>(deleteCount), debug, uinputInterKeyUsec);

    if (method == BackspaceInjector::Method::DeleteSurroundingText) {
        // Không cần chờ ack, commit ngay
        if (!commitText.empty()) {
            ic->commitString(commitText);
        }
        state.shownText = newWord;
        state.hasRewrittenCurrentWord = !state.shownText.empty();
        event.filterAndAccept();
        return true;
    }

    if (method == BackspaceInjector::Method::Uinput) {
        // Inject thêm 1 backspace extra làm trigger ack
        // (N backspace xóa text + 1 backspace loop back báo xong)
        deps_.backspaceInjector->sendBackspaces(
            ic, 1, debug, uinputInterKeyUsec);

        state.rewriteLock = true;
        state.waitingBackspaceAck = true;
        state.expectedBackspaces = static_cast<int>(deleteCount) + 1;
        state.seenBackspaces = 0;
        state.pendingConvertedText = std::move(commitText);
        state.pendingShownTextAfterCommit = newWord;
        event.filterAndAccept();
        return true;
    }

    // Uinput không available
    clearWordState();
    return false;
};
        if (state.waitingBackspaceAck) {
            if (key.check(FcitxKey_BackSpace) && !hasCtrlAltSuperMeta(key)) {
                state.seenBackspaces++;
                if (state.seenBackspaces < state.expectedBackspaces) {
                    // Let the injected backspaces reach the app to delete.
                    return false;
                }

                // Filter the final trigger backspace (extra +1).
                event.filterAndAccept();

                const uint64_t extraUsec = deps_.bsRewriteCommitExtraUsec
                                               ? deps_.bsRewriteCommitExtraUsec()
                                               : 0;
                scheduleCommitAfterBackspace(extraUsec);
                return true;
            }

            // Buffer any other keys during deletion and replay after commit.
            state.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

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

        // Escape ends current composing word.
        if (key.check(FcitxKey_Escape)) {
            clearWordState();
            return false;
        }

        // Physical BackSpace: delete one visible rune.
        if (key.check(FcitxKey_BackSpace)) {
            if (state.shownText.empty()) {
                return false;
            }
            const auto method = deps_.backspaceInjector->sendBackspaces(
                ic, 1, debug, uinputInterKeyUsec);
            if (method != BackspaceInjector::Method::Uinput) {
                clearWordState();
                return false;
            }
            event.filterAndAccept();
            state.shownText = utf8DropLastN(state.shownText, 1);
            state.hasRewrittenCurrentWord = !state.shownText.empty();
            return true;
        }

        // Printable ASCII path.
        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            // Boundary keys: end current word and let application handle it.
            if (isBoundaryASCII(c) || key.check(FcitxKey_Return) ||
                key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_ISO_Enter) ||
                key.check(FcitxKey_Tab)) {
                clearWordState();
                return false;
            }

            // Word characters: apply OpenKey changes immediately via delta.
            if (isComposingASCII(c)) {
                if (!adapterShared) {
                    return false;
                }
                adapterShared->setCodeTable(state.codeTable);
                const auto r =
                    adapterShared->processAsciiKey(state.shownText, c);
                if (!r.handled) {
                    return false;
                }
                state.lastPhysicalKeyUsec = nowUsec;
                return applyWordDelta(r.newWord, "ascii");
            }

            // Other printable ASCII: treat as boundary of current word.
            clearWordState();
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
    deps.bsRewriteCommitExtraUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteCommitExtraUsec.value()));
    };
    deps.bsRewriteCommitCapUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteCommitCapUsec.value()));
    };
    deps.bsRewriteUinputInterKeyUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteUinputInterKeyUsec.value()));
    };
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
        case RuntimeMode::BackspaceRewriteDelta:
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
    case RuntimeMode::BackspaceRewriteDelta:
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
    state->shownText.clear();
    state->hasRewrittenCurrentWord = false;
    state->rewriteLock = false;
    state->waitingBackspaceAck = false;
    state->expectedBackspaces = 0;
    state->seenBackspaces = 0;
    state->pendingKeys.clear();
    state->rewriteTimer.reset();
    state->commitTimer.reset();
    state->pendingConvertedText.clear();
    state->pendingShownTextAfterCommit.clear();
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
    state->shownText.clear();
    state->hasRewrittenCurrentWord = false;
    state->rewriteLock = false;
    state->waitingBackspaceAck = false;
    state->expectedBackspaces = 0;
    state->seenBackspaces = 0;
    state->pendingKeys.clear();
    state->rewriteTimer.reset();
    state->commitTimer.reset();
    state->pendingConvertedText.clear();
    state->pendingShownTextAfterCommit.clear();
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

    // X11 browser fields (omnibox/search boxes) are often fragile with
    // surrounding-text/backspace rewrite. Force preedit for better UX.
    if (isX11Backend(ic) && isBrowserProgram(s.program)) {
        return RuntimeMode::Preedit;
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
    const bool backspaceRewriteSupported =
        g_backspaceInjector.uinputAvailable(debugEnabled());

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
    case ModeOverride::ForceBackspaceRewriteDelta:
        return backspaceRewriteSupported ? RuntimeMode::BackspaceRewriteDelta
                                        : RuntimeMode::Preedit;
    case ModeOverride::Auto:
        break;
    }

    // Auto mode priority: SurroundingText -> BackspaceRewrite -> Preedit -> DirectCommit.
    if (canUseSurroundingText()) {
        return RuntimeMode::SurroundingText;
    }
    if (backspaceRewriteSupported) {
        return RuntimeMode::BackspaceRewriteDelta;
    }
    if (clientPreeditSupported) {
        return RuntimeMode::Preedit;
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

    // Hard force: X11 browser text fields are fragile with non-preedit modes.
    // Even if user sets ForceBackspaceRewriteDelta, keep preedit for browsers.
    if (isX11Backend(ic) && isBrowserProgram(state->program)) {
        const RuntimeMode desired =
            ic->capabilityFlags().test(fcitx::CapabilityFlag::Password)
                ? RuntimeMode::DirectCommit
                : RuntimeMode::Preedit;
        if (!state->modeDecided || !state->manualMode || state->mode != desired) {
            state->shownText.clear();
            state->hasRewrittenCurrentWord = false;
            state->rewriteLock = false;
            state->waitingBackspaceAck = false;
            state->expectedBackspaces = 0;
            state->seenBackspaces = 0;
            state->pendingKeys.clear();
            state->rewriteTimer.reset();
            state->commitTimer.reset();
            state->pendingConvertedText.clear();
            state->pendingShownTextAfterCommit.clear();
            state->hasPendingBoundaryKey = false;
            state->composing.clear();
            state->macroBuffer.clear();
            state->rollbackWord.clear();
            state->rollbackDisplay.clear();
            state->noSeedNextWord = false;
            state->surroundingFailures = 0;
            state->manualMode = true;
            state->modeDecided = true;
            state->mode = desired;
            state->autoMode = desired;
            updatePreeditUI(ic, *state);
            if (debugEnabled()) {
                FCITX_INFO() << "openkey: force preedit for x11 browser program="
                             << state->program
                             << " mode=" << static_cast<int>(state->mode);
            }
        }
    }

    // Switch composition mode hotkey (Auto -> ST -> Backspace -> Preedit -> Direct -> Auto).
        if (key.checkKeyList(config_.switchModeKey.value()) && key.sym() != FcitxKey_None) {
            auto clearComposingState = [this, ic, state]() {
            state->shownText.clear();
            state->hasRewrittenCurrentWord = false;
            state->rewriteLock = false;
            state->waitingBackspaceAck = false;
            state->expectedBackspaces = 0;
            state->seenBackspaces = 0;
            state->pendingKeys.clear();
            state->rewriteTimer.reset();
            state->commitTimer.reset();
            state->pendingConvertedText.clear();
            state->pendingShownTextAfterCommit.clear();
            state->hasPendingBoundaryKey = false;
            state->composing.clear();
            state->macroBuffer.clear();
            state->rollbackWord.clear();
            state->rollbackDisplay.clear();
            state->noSeedNextWord = false;
            state->surroundingFailures = 0;
            updatePreeditUI(ic, *state);
        };

        // X11 browsers: always stay in preedit mode (disable other runtime
        // mode options).
        if (isX11Backend(ic) && isBrowserProgram(state->program)) {
            state->manualMode = true;
            state->modeDecided = true;
            if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Password)) {
                state->mode = RuntimeMode::DirectCommit;
                state->autoMode = RuntimeMode::DirectCommit;
            } else {
                state->mode = RuntimeMode::Preedit;
                state->autoMode = RuntimeMode::Preedit;
            }
            if (debugEnabled()) {
                FCITX_INFO() << "openkey: force mode for x11 browser program="
                             << state->program
                             << " mode=" << static_cast<int>(state->mode);
            }
            clearComposingState();
            if (instance_) {
                instance_->showInputMethodInformation(ic);
            }
            // Continue with the existing toast code path below.
        }
        // Wine/Proton clients: always stay in preedit mode (disable other
        // runtime mode options).
        else if (isWineProgram(state->program)) {
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
            if (canUseSurroundingText()) {
                state->mode = RuntimeMode::SurroundingText;
            } else if (g_backspaceInjector.uinputAvailable(debugEnabled())) {
                state->mode = RuntimeMode::BackspaceRewriteDelta;
            } else {
                state->mode = RuntimeMode::Preedit;
            }
        } else if (state->mode == RuntimeMode::SurroundingText) {
            state->mode = g_backspaceInjector.uinputAvailable(debugEnabled())
                              ? RuntimeMode::BackspaceRewriteDelta
                              : RuntimeMode::Preedit;
        } else if (state->mode == RuntimeMode::BackspaceRewriteDelta) {
            state->mode = RuntimeMode::Preedit;
        } else if (state->mode == RuntimeMode::Preedit) {
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
                case RuntimeMode::BackspaceRewriteDelta:
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
    case RuntimeMode::BackspaceRewriteDelta:
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
