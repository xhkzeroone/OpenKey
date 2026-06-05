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
#include <fcitx-utils/dbus/bus.h>

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

static std::string runtimeModeToString(RuntimeMode mode) {
    switch (mode) {
    case RuntimeMode::Auto:
        return "auto";
    case RuntimeMode::Browser:
        return "browser";
    case RuntimeMode::BrowserX11:
        return "browser-x11";
    case RuntimeMode::Preedit:
        return "preedit";
    case RuntimeMode::SurroundingText:
        return "surrounding";
    case RuntimeMode::BackspaceRewriteDelta:
        return "backspace";
    case RuntimeMode::DirectCommit:
        return "direct";
    }
    return "auto";
}

static bool runtimeModeFromString(const std::string &mode, RuntimeMode &out) {
    if (equalsASCIIInsensitive(mode, "auto")) {
        out = RuntimeMode::Auto;
        return true;
    }
    if (equalsASCIIInsensitive(mode, "browser")) {
        out = RuntimeMode::Browser;
        return true;
    }
    if (equalsASCIIInsensitive(mode, "browser-x11")) {
        out = RuntimeMode::BrowserX11;
        return true;
    }
    if (equalsASCIIInsensitive(mode, "preedit")) {
        out = RuntimeMode::Preedit;
        return true;
    }
    if (equalsASCIIInsensitive(mode, "surrounding")) {
        out = RuntimeMode::SurroundingText;
        return true;
    }
    if (equalsASCIIInsensitive(mode, "backspace")) {
        out = RuntimeMode::BackspaceRewriteDelta;
        return true;
    }
    if (equalsASCIIInsensitive(mode, "direct")) {
        out = RuntimeMode::DirectCommit;
        return true;
    }
    return false;
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
        "messenger",
        "helium",
        "window:", // common prefix in Wayland for webview-based clients
    };

    for (const auto &pattern : kBrowserPatterns) {
    if (base.find(asciiLower(pattern)) != std::string::npos) {
        return true;
    }
}

    return false;
}

static bool isElectronLikeProgram(const std::string &program) {
    if (program.empty()) {
        return false;
    }

    const std::string base = asciiLower(programBaseName(program));

    // Common executable names.
    if (base == "electron" || base == "code" || base == "code-oss" ||
        base == "codium" || base == "vscode") {
        return true;
    }

    // Common desktop ids / app ids.
    //
    // Keep this list conservative: we only use it to avoid
    // deleteSurroundingText in web/electron-like text fields where it can be
    // ignored, causing duplicated commits in BackspaceRewriteDelta mode.
    static const std::vector<std::string> kElectronPatterns = {
        "visualstudio.code",
        "vscodium",
        "slack",
        "discord",
        "teams",
        "notion",
        "obsidian",
        "postman",
        "insomnia",
        "mattermost",
        "signal",
        "element",
        "skypeforlinux",
        "zalo",
        "youtube",
        "outlook",
    };

    for (const auto &pattern : kElectronPatterns) {
        if (base.find(pattern) != std::string::npos) {
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

static bool isWaylandBackend(fcitx::InputContext *ic) {
    const char *frontend = ic ? ic->frontend() : nullptr;
    if (frontend && startsWithASCIIInsensitive(frontend, "wayland")) {
        return true;
    }
    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay && waylandDisplay[0]) {
        return !isX11Backend(ic);
    }
    const char *sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType && sessionType[0] &&
        equalsASCIIInsensitive(sessionType, "wayland")) {
        return !isX11Backend(ic);
    }
    return false;
}

static bool shouldUsePreeditForX11Browser(fcitx::InputContext *ic,
                                          const std::string &program) {
    return isX11Backend(ic) && isBrowserProgram(program);
}

static bool hasReliableSurroundingText(fcitx::InputContext *ic) {
    if (!ic) {
        return false;
    }
    const auto &st = ic->surroundingText();
    if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return false;
    }
    if (!st.isValid() || st.cursor() != st.anchor()) {
        return false;
    }
    if (!fcitx::utf8::validate(st.text())) {
        return false;
    }
    if (st.cursor() > fcitx::utf8::length(st.text())) {
        return false;
    }
    return true;
}

static bool shouldHideBrowserModesForProgram(fcitx::InputContext *ic,
                                             const std::string &program) {
    if (isBrowserProgram(program)) {
        return false;
    }
    return hasReliableSurroundingText(ic);
}

static bool shouldUseDST(fcitx::InputContext *ic, const std::string &program,
                         int count) {
    if (!ic) return false;

    if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return false;
    }

    // Rule 1: app bị skip thủ công
    if (isBrowserProgram(program) || isElectronLikeProgram(program)) return false;

    // If we can't identify the program (common on Wayland for some clients),
    // deleteSurroundingText is often unreliable in web/electron text fields.
    // Prefer uinput/server backspace in that case.
    if (program.empty()) return false;

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
    ~BackspaceInjector() { destroyUinput(); }

    enum class Method {
        DeleteSurroundingText,
        Uinput,
        None,
    };

    Method sendBackspaces(fcitx::InputContext *ic, const std::string &program,
                          int count, bool debug,
                          uint64_t uinputInterKeyUsec = 1500) {
        if (count <= 0) {
            return Method::None;
        }

        // Dùng deleteSurroundingText cho app reliable (non-browser/electron).
        if (shouldUseDST(ic, program, count)) {
            ic->deleteSurroundingText(-count, count);
            return Method::DeleteSurroundingText;
        }

        if (ensureUinput(debug)) {
            sendBackspacesUinput(count, uinputInterKeyUsec);
            return Method::Uinput;
        }
        return Method::None;
    }

    Method sendBackspacesUinputOnly(fcitx::InputContext *ic,
                                    const std::string &program, int count,
                                    bool debug,
                                    uint64_t uinputInterKeyUsec = 1500) {
        if (count <= 0) {
            return Method::None;
        }
        if (!ensureUinput(debug)) {
            if (debug) {
                FCITX_INFO() << "openkey: backspace method=browser-uinput-none"
                             << " program=" << program
                             << " frontend=" << (ic && ic->frontend() ? ic->frontend() : "")
                             << " count=" << count
                             << " reason=uinput-unavailable";
            }
            return Method::None;
        }
        if (debug) {
            FCITX_INFO() << "openkey: backspace method=browser-uinput"
                         << " program=" << program
                         << " frontend=" << (ic && ic->frontend() ? ic->frontend() : "")
                         << " count=" << count;
        }
        sendBackspacesUinput(count, uinputInterKeyUsec);
        return Method::Uinput;
    }

    bool uinputAvailable(bool debug) { return ensureUinput(debug); }

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
            FCITX_WARN() << "openkey: UI_DEV_CREATE failed";
        }
        return false;
    }

    // Đợi kernel register virtual device xong trước khi dùng
    ::usleep(200000); // 200ms

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
constexpr const char *kBridgeBusName = "org.openkey.Bridge";
constexpr const char *kBridgeObjectPath = "/org/openkey/Bridge";
constexpr const char *kBridgeInterface = "org.openkey.Bridge1";
} // namespace

class FocusedAppBridge {
public:
    FocusedAppBridge(fcitx::EventLoop *loop,
                     std::function<bool()> debugEnabled)
        : bus_(fcitx::dbus::BusType::Session),
          debugEnabled_(std::move(debugEnabled)) {
        if (bus_.isOpen() && loop) {
            bus_.attachEventLoop(loop);
        }
    }

    ~FocusedAppBridge() {
        if (bus_.isOpen()) {
            bus_.detachEventLoop();
        }
    }

    // Returns GNOME Shell focused app id (usually "*.desktop"), or empty.
    std::string focusedAppId() { return query().first; }

    // Returns GNOME Shell focused app name, or empty.
    std::string focusedAppName() { return query().second; }

private:
    std::pair<std::string, std::string> query() {
        if (!bus_.isOpen()) {
            return {};
        }

        auto msg = bus_.createMethodCall(kBridgeBusName, kBridgeObjectPath,
                                         kBridgeInterface, "GetFocusedApp");
        auto reply = msg.call(20000); // 20ms
        if (!reply || reply.isError()) {
            if (debugEnabled_ && debugEnabled_()) {
                FCITX_INFO() << "openkey: bridge GetFocusedApp unavailable";
            }
            return {};
        }
        std::string appId;
        std::string appName;
        reply >> appId >> appName;
        return {std::move(appId), std::move(appName)};
    }

    fcitx::dbus::Bus bus_;
    std::function<bool()> debugEnabled_;
};

namespace {

struct BrowserModeDeps {
    fcitx::Instance *instance = nullptr;
    fcitx::SimpleInputContextPropertyFactory<OpenKeyState> *factory = nullptr;
    std::shared_ptr<OpenKeyAdapter> adapter;
    BackspaceInjector *backspaceInjector = nullptr;
    std::weak_ptr<void> lifetimeWeak;
    std::function<bool()> debugEnabled;
    std::function<uint64_t()> bsRewriteUinputInterKeyUsec;
    std::function<uint64_t()> browserRewriteCommitDelayUsec;
};

struct DeltaModeDeps {
    fcitx::Instance *instance = nullptr;
    fcitx::SimpleInputContextPropertyFactory<OpenKeyState> *factory = nullptr;
    std::shared_ptr<OpenKeyAdapter> adapter;
    BackspaceInjector *backspaceInjector = nullptr;
    std::weak_ptr<void> lifetimeWeak;
    std::function<bool()> debugEnabled;
    std::function<uint64_t()> bsRewriteCommitExtraUsec;
    std::function<uint64_t()> bsRewriteUinputInterKeyUsec;
};

class BrowserModeHandler final : public InputModeHandler {
public:
    explicit BrowserModeHandler(BrowserModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                   OpenKeyState &state) override {
        auto key = event.key().normalize();
        if (event.isRelease()) {
            return false;
        }
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
        const uint64_t browserCommitDelayUsec =
            deps_.browserRewriteCommitDelayUsec
                ? deps_.browserRewriteCommitDelayUsec()
                : 150000;
        auto *loop = deps_.instance ? &deps_.instance->eventLoop() : nullptr;
        auto &browserState = state.browser;

        auto stateFor = [this](fcitx::InputContext *ic2) -> OpenKeyState * {
            if (!ic2 || !deps_.factory) {
                return nullptr;
            }
            return ic2->propertyFor(deps_.factory);
        };

        auto clearWordState = [&browserState]() {
            browserState.clear();
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
            if (!st || st->browser.pendingKeys.empty()) {
                return;
            }
            auto keys = std::move(st->browser.pendingKeys);
            st->browser.pendingKeys.clear();
            for (const auto &k : keys) {
                fcitx::KeyEvent synthetic(ic2, k, false, 0);
                const bool handled = handleKey(ic2, synthetic, *st);
                if (!handled && !synthetic.accepted()) {
                    ic2->forwardKey(k);
                }
            }
        };

        // Không replay pending key ngay trong callback timer để tránh
        // re-entrancy khi key replay khởi tạo transaction Browser mới.
        auto scheduleDrainPendingKeys =
            [icRef, lifetimeWeak, loop, stateFor,
             drainPendingKeys](BrowserRewriteState &browserState2) {
                browserState2.drainPendingTimer.reset();
                if (!loop) {
                    drainPendingKeys();
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + 1;
                browserState2.drainPendingTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
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
                        auto _timer = std::move(st->browser.drainPendingTimer);
                        drainPendingKeys();
                        return false;
                    });
                if (browserState2.drainPendingTimer) {
                    browserState2.drainPendingTimer->setOneShot();
                    return;
                }
                drainPendingKeys();
            };

        auto scheduleLateBudgetTimeoutDrain =
            [icRef, lifetimeWeak, loop, stateFor,
             scheduleDrainPendingKeys](BrowserRewriteState &browserState2) {
                if (browserState2.lateBackspaceBudget == 0) {
                    scheduleDrainPendingKeys(browserState2);
                    return;
                }
                // Budget chưa về 0: đợi tối đa 200ms rồi force drain.
                browserState2.lateBackspaceTimeoutTimer.reset();
                if (!loop) {
                    browserState2.lateBackspaceBudget = 0;
                    scheduleDrainPendingKeys(browserState2);
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + 200000;
                browserState2.lateBackspaceTimeoutTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
                     scheduleDrainPendingKeys](fcitx::EventSourceTime *,
                                              uint64_t) {
                        if (lifetimeWeak.expired()) {
                            return false;
                        }
                        auto *ic3 = icRef.get();
                        if (!ic3) {
                            return false;
                        }
                        auto *st3 = stateFor(ic3);
                        if (!st3) {
                            return false;
                        }
                        auto _timer =
                            std::move(st3->browser.lateBackspaceTimeoutTimer);
                        if (st3->browser.lateBackspaceBudget > 0) {
                            FCITX_INFO()
                                << "openkey: browser late-bs timeout"
                                << " budget=" << st3->browser.lateBackspaceBudget
                                << " force-drain";
                            st3->browser.lateBackspaceBudget = 0;
                        }
                        scheduleDrainPendingKeys(st3->browser);
                        return false;
                    });
                if (browserState2.lateBackspaceTimeoutTimer) {
                    browserState2.lateBackspaceTimeoutTimer->setOneShot();
                    return;
                }
                browserState2.lateBackspaceBudget = 0;
                scheduleDrainPendingKeys(browserState2);
            };

        auto finishPendingBackspaceCommit = [icRef, lifetimeWeak, stateFor]() {
            if (lifetimeWeak.expired()) {
                return;
            }
            auto *ic2 = icRef.get();
            if (!ic2) {
                return;
            }
            auto *st = stateFor(ic2);
            if (!st) {
                return;
            }

            auto &browserState2 = st->browser;
            const std::string commitText =
                std::move(browserState2.pendingConvertedText);
            const std::string shownAfter =
                std::move(browserState2.pendingShownTextAfterCommit);
            browserState2.commitTimer.reset();
            browserState2.pendingConvertedText.clear();
            browserState2.pendingShownTextAfterCommit.clear();

            if (!commitText.empty()) {
                ic2->commitString(commitText);
            }
            browserState2.shownText = shownAfter;
            // Không clear lock, không drain ở đây.
        };

        auto scheduleCommitAfterBackspace =
            [icRef, lifetimeWeak, loop, stateFor, &browserState,
             scheduleLateBudgetTimeoutDrain, finishPendingBackspaceCommit](
                uint64_t delayUsec) {
                browserState.commitTimer.reset();
                if (!loop) {
                    finishPendingBackspaceCommit();
                    browserState.lateBackspaceBudget =
                        browserState.expectedBackspaces;
                    browserState.expectedBackspaces = 0;
                    browserState.rewriteLock = false;
                    scheduleLateBudgetTimeoutDrain(browserState);
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
                browserState.commitTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
                     scheduleLateBudgetTimeoutDrain,
                     finishPendingBackspaceCommit](fcitx::EventSourceTime *,
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
                        auto _timer = std::move(st->browser.commitTimer);
                        finishPendingBackspaceCommit();
                        st->browser.lateBackspaceBudget =
                            st->browser.expectedBackspaces;
                        st->browser.expectedBackspaces = 0;
                        st->browser.rewriteLock = false;
                        scheduleLateBudgetTimeoutDrain(st->browser);
                        return false;
                    });
                if (browserState.commitTimer) {
                    browserState.commitTimer->setOneShot();
                }
            };

        auto applyWordDelta = [&, this](const std::string &newWord,
                                        char asciiChar,
                                        const char *reason) -> bool {
            if (!deps_.backspaceInjector) {
                return false;
            }
            if (browserState.rewriteLock ||
                browserState.lateBackspaceBudget > 0) {
                browserState.pendingKeys.push_back(key);
                event.filterAndAccept();
                return true;
            }
            if (!fcitx::utf8::validate(browserState.shownText) ||
                !fcitx::utf8::validate(newWord)) {
                clearWordState();
                return false;
            }

            const std::string oldShown = browserState.shownText;
            const std::string rawAppend = oldShown + asciiChar;

            const std::size_t prefixLen =
                commonPrefixBytesUTF8Boundary(browserState.shownText, newWord);
            unsigned int deleteCount =
                utf8CharCount(browserState.shownText.substr(prefixLen));
            std::string commitText = newWord.substr(prefixLen);
            if (deleteCount > 128) {
                deleteCount = utf8CharCount(browserState.shownText);
                commitText = newWord;
            }

            if (debug) {
                FCITX_INFO() << "openkey: bs-delta program=" << state.program
                             << " reason=" << reason
                             << " from=" << browserState.shownText
                             << " to=" << newWord
                             << " delete=" << deleteCount
                             << " commit=" << commitText;
            }

            if (deleteCount == 0) {
                if (!commitText.empty()) {
                    ic->commitString(commitText);
                }
                browserState.shownText = newWord;
                browserState.hasRewrittenCurrentWord =
                    browserState.hasRewrittenCurrentWord ||
                    (newWord != rawAppend);
                event.filterAndAccept();
                return true;
            }

            const std::string programForInjector = state.program;
            const auto method = deps_.backspaceInjector->sendBackspacesUinputOnly(
                ic, programForInjector, static_cast<int>(deleteCount), debug,
                uinputInterKeyUsec);
            if (method != BackspaceInjector::Method::Uinput) {
                clearWordState();
                return false;
            }

            browserState.rewriteLock = true;
            browserState.expectedBackspaces = static_cast<int>(deleteCount);
            browserState.pendingConvertedText = std::move(commitText);
            browserState.pendingShownTextAfterCommit = newWord;
            browserState.hasRewrittenCurrentWord =
                browserState.hasRewrittenCurrentWord || (newWord != rawAppend);
            event.filterAndAccept();
            scheduleCommitAfterBackspace(browserCommitDelayUsec);
            return true;
        };

        if (browserState.rewriteLock) {
            if (browserState.expectedBackspaces > 0 &&
                key.check(FcitxKey_BackSpace) &&
                !hasCtrlAltSuperMeta(key)) {
                browserState.expectedBackspaces--;
                return false;
            }
            browserState.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (browserState.lateBackspaceBudget > 0 &&
            key.check(FcitxKey_BackSpace) &&
            !hasCtrlAltSuperMeta(key)) {
            // Swallow backspace uinput về muộn sau khi transaction đã mở lock.
            browserState.lateBackspaceBudget--;
            event.filterAndAccept();
            if (browserState.lateBackspaceBudget == 0) {
                browserState.lateBackspaceTimeoutTimer.reset();
                scheduleDrainPendingKeys(browserState);
            }
            return true;
        }

        if (key.isCursorMove() || key.check(FcitxKey_Delete)) {
            clearWordState();
            return false;
        }

        if (key.check(FcitxKey_Escape)) {
            clearWordState();
            return false;
        }

        if (key.check(FcitxKey_BackSpace)) {
            if (browserState.shownText.empty()) {
                return false;
            }
            if (!browserState.hasRewrittenCurrentWord) {
                clearWordState();
                return false;
            }
            browserState.shownText = utf8DropLastN(browserState.shownText, 1);
            if (browserState.shownText.empty()) {
                browserState.hasRewrittenCurrentWord = false;
            }
            return false;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            if (isBoundaryASCII(c) || key.check(FcitxKey_Return) ||
                key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_ISO_Enter) ||
                key.check(FcitxKey_Tab)) {
                clearWordState();
                return false;
            }

            if (isComposingASCII(c)) {
                if (!adapterShared) {
                    return false;
                }
                adapterShared->setCodeTable(state.codeTable);
                const auto r =
                    adapterShared->processAsciiKey(browserState.shownText, c);
                if (!r.handled) {
                    return false;
                }
                return applyWordDelta(r.newWord, c, "ascii");
            }

            clearWordState();
            return false;
        }

        clearWordState();
        return false;
    }

private:
    BrowserModeDeps deps_;
};

class BrowserX11ModeHandler final : public InputModeHandler {
public:
    explicit BrowserX11ModeHandler(DeltaModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                   OpenKeyState &state) override {
        auto key = event.key().normalize();
        if (event.isRelease()) {
            return false;
        }
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
        constexpr uint64_t kBrowserX11CommitSlackUsec = 5000;
        constexpr uint64_t kDeltaAckTimeoutUsec = 200000;
        constexpr uint64_t kDeltaLateBackspaceTimeoutUsec = 200000;
        auto *loop = deps_.instance ? &deps_.instance->eventLoop() : nullptr;
        auto &deltaState = state.delta;
        state.browser.clear();

        auto stateFor = [this](fcitx::InputContext *ic2) -> OpenKeyState * {
            if (!ic2 || !deps_.factory) {
                return nullptr;
            }
            return ic2->propertyFor(deps_.factory);
        };

        auto clearWordState = [&deltaState]() {
            deltaState.clear();
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
            if (!st || st->delta.pendingKeys.empty()) {
                return;
            }
            auto keys = std::move(st->delta.pendingKeys);
            st->delta.pendingKeys.clear();
            for (const auto &k : keys) {
                fcitx::KeyEvent synthetic(ic2, k, false, 0);
                const bool handled = handleKey(ic2, synthetic, *st);
                if (!handled && !synthetic.accepted()) {
                    ic2->forwardKey(k);
                }
            }
        };

        auto scheduleDrainPendingKeys =
            [icRef, lifetimeWeak, loop, stateFor,
             drainPendingKeys](DeltaRewriteState &deltaState2) {
                deltaState2.drainPendingTimer.reset();
                if (!loop) {
                    drainPendingKeys();
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + 1;
                deltaState2.drainPendingTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
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
                        auto _timer = std::move(st->delta.drainPendingTimer);
                        drainPendingKeys();
                        return false;
                    });
                if (deltaState2.drainPendingTimer) {
                    deltaState2.drainPendingTimer->setOneShot();
                    return;
                }
                drainPendingKeys();
            };

        auto scheduleLateBudgetTimeoutDrain =
            [icRef, lifetimeWeak, loop, stateFor,
             scheduleDrainPendingKeys](DeltaRewriteState &deltaState2) {
                if (deltaState2.lateBackspaceBudget == 0) {
                    scheduleDrainPendingKeys(deltaState2);
                    return;
                }
                deltaState2.lateBackspaceTimeoutTimer.reset();
                if (!loop) {
                    deltaState2.lateBackspaceBudget = 0;
                    scheduleDrainPendingKeys(deltaState2);
                    return;
                }
                const uint64_t deadline =
                    fcitx::now(CLOCK_MONOTONIC) +
                    kDeltaLateBackspaceTimeoutUsec;
                deltaState2.lateBackspaceTimeoutTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
                     scheduleDrainPendingKeys](fcitx::EventSourceTime *,
                                              uint64_t) {
                        if (lifetimeWeak.expired()) {
                            return false;
                        }
                        auto *ic3 = icRef.get();
                        if (!ic3) {
                            return false;
                        }
                        auto *st3 = stateFor(ic3);
                        if (!st3) {
                            return false;
                        }
                        auto _timer =
                            std::move(st3->delta.lateBackspaceTimeoutTimer);
                        if (st3->delta.lateBackspaceBudget > 0) {
                            if (st3->delta.expectedBackspaces > 0 ||
                                st3->delta.seenBackspaces > 0) {
                                FCITX_INFO()
                                    << "openkey: delta late-bs timeout"
                                    << " budget="
                                    << st3->delta.lateBackspaceBudget;
                            }
                            st3->delta.lateBackspaceBudget = 0;
                        }
                        scheduleDrainPendingKeys(st3->delta);
                        return false;
                    });
                if (deltaState2.lateBackspaceTimeoutTimer) {
                    deltaState2.lateBackspaceTimeoutTimer->setOneShot();
                    return;
                }
                deltaState2.lateBackspaceBudget = 0;
                scheduleDrainPendingKeys(deltaState2);
            };

        auto finishPendingBackspaceCommit =
            [icRef, lifetimeWeak, stateFor, drainPendingKeys]() {
                if (lifetimeWeak.expired()) {
                    return;
                }
                auto *ic2 = icRef.get();
                if (!ic2) {
                    return;
                }
                auto *st = stateFor(ic2);
                if (!st) {
                    return;
                }

                auto &deltaState2 = st->delta;
                const std::string commitText =
                    std::move(deltaState2.pendingConvertedText);
                const std::string shownAfter =
                    std::move(deltaState2.pendingShownTextAfterCommit);
                deltaState2.commitTimer.reset();
                deltaState2.pendingConvertedText.clear();
                deltaState2.pendingShownTextAfterCommit.clear();

                if (!commitText.empty()) {
                    ic2->commitString(commitText);
                }
                deltaState2.shownText = shownAfter;
                deltaState2.rewriteLock = false;
                deltaState2.waitingBackspaceAck = false;
                deltaState2.expectedBackspaces = 0;
                deltaState2.seenBackspaces = 0;
                deltaState2.ackTimeoutTimer.reset();

                drainPendingKeys();
            };

        auto scheduleCommitAfterBackspace =
            [icRef, lifetimeWeak, loop, stateFor, &deltaState,
             scheduleLateBudgetTimeoutDrain,
             finishPendingBackspaceCommit](uint64_t delayUsec) {
                deltaState.commitTimer.reset();
                if (!loop) {
                    const int remainingBackspaces =
                        std::max(0, deltaState.expectedBackspaces -
                                        deltaState.seenBackspaces);
                    finishPendingBackspaceCommit();
                    deltaState.lateBackspaceBudget = remainingBackspaces;
                    scheduleLateBudgetTimeoutDrain(deltaState);
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
                deltaState.commitTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
                     scheduleLateBudgetTimeoutDrain,
                     finishPendingBackspaceCommit](fcitx::EventSourceTime *,
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
                        const int remainingBackspaces =
                            std::max(0, st->delta.expectedBackspaces -
                                            st->delta.seenBackspaces);
                        auto _timer = std::move(st->delta.commitTimer);
                        finishPendingBackspaceCommit();
                        st->delta.lateBackspaceBudget = remainingBackspaces;
                        scheduleLateBudgetTimeoutDrain(st->delta);
                        return false;
                    });
                if (deltaState.commitTimer) {
                    deltaState.commitTimer->setOneShot();
                }
            };

        auto scheduleAckTimeout =
            [this, icRef, lifetimeWeak, loop, stateFor, &deltaState,
             scheduleCommitAfterBackspace]() {
                deltaState.ackTimeoutTimer.reset();
                if (!loop) {
                    return;
                }
                const uint64_t deadline =
                    fcitx::now(CLOCK_MONOTONIC) + kDeltaAckTimeoutUsec;
                deltaState.ackTimeoutTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [this, icRef, lifetimeWeak, stateFor,
                     scheduleCommitAfterBackspace](fcitx::EventSourceTime *,
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
                        auto _timer = std::move(st->delta.ackTimeoutTimer);
                        if (!st->delta.waitingBackspaceAck) {
                            return false;
                        }
                        FCITX_INFO() << "openkey: bs-x11 ack timeout"
                                     << " program=" << st->program
                                     << " seen=" << st->delta.seenBackspaces
                                     << " expected="
                                     << st->delta.expectedBackspaces;
                        const uint64_t extraUsec =
                            deps_.bsRewriteCommitExtraUsec
                                ? deps_.bsRewriteCommitExtraUsec()
                                : 0;
                        scheduleCommitAfterBackspace(
                            extraUsec + kBrowserX11CommitSlackUsec);
                        return false;
                    });
                if (deltaState.ackTimeoutTimer) {
                    deltaState.ackTimeoutTimer->setOneShot();
                }
            };

        auto applyWordDelta = [&, this](const std::string &newWord,
                                        char asciiChar,
                                        const char *reason) -> bool {
            if (!deps_.backspaceInjector) {
                return false;
            }
            if (deltaState.rewriteLock) {
                deltaState.pendingKeys.push_back(key);
                event.filterAndAccept();
                return true;
            }
            if (!fcitx::utf8::validate(deltaState.shownText) ||
                !fcitx::utf8::validate(newWord)) {
                clearWordState();
                return false;
            }

            const std::string oldShown = deltaState.shownText;
            const std::string rawAppend = oldShown + asciiChar;

            const std::size_t prefixLen =
                commonPrefixBytesUTF8Boundary(deltaState.shownText, newWord);
            unsigned int deleteCount =
                utf8CharCount(deltaState.shownText.substr(prefixLen));
            std::string commitText = newWord.substr(prefixLen);
            if (deleteCount > 128) {
                deleteCount = utf8CharCount(deltaState.shownText);
                commitText = newWord;
            }

            if (debug) {
                FCITX_INFO() << "openkey: bs-x11 program=" << state.program
                             << " reason=" << reason
                             << " from=" << deltaState.shownText
                             << " to=" << newWord
                             << " delete=" << deleteCount
                             << " commit=" << commitText;
            }

            if (deleteCount == 0) {
                if (!commitText.empty()) {
                    ic->commitString(commitText);
                }
                deltaState.shownText = newWord;
                deltaState.hasRewrittenCurrentWord =
                    deltaState.hasRewrittenCurrentWord ||
                    (newWord != rawAppend);
                event.filterAndAccept();
                return true;
            }

            const std::string programForInjector = state.program;
            const int ackBackspaceCount =
                static_cast<int>(deleteCount) + 1;
            const auto method =
                deps_.backspaceInjector->sendBackspacesUinputOnly(
                    ic, programForInjector, ackBackspaceCount, debug,
                    uinputInterKeyUsec);

            if (method == BackspaceInjector::Method::Uinput) {
                deltaState.rewriteLock = true;
                deltaState.waitingBackspaceAck = true;
                deltaState.expectedBackspaces = ackBackspaceCount;
                deltaState.seenBackspaces = 0;
                deltaState.pendingConvertedText = std::move(commitText);
                deltaState.pendingShownTextAfterCommit = newWord;
                event.filterAndAccept();
                scheduleAckTimeout();
                return true;
            }

            clearWordState();
            return false;
        };

        if (deltaState.waitingBackspaceAck) {
            if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter) ||
                key.check(FcitxKey_ISO_Enter) || key.isCursorMove() ||
                key.check(FcitxKey_Delete) || key.check(FcitxKey_Tab) ||
                key.check(FcitxKey_Escape)) {
                finishPendingBackspaceCommit();
                deltaState.pendingKeys.clear();
                return false;
            }
            if (key.check(FcitxKey_BackSpace) && !hasCtrlAltSuperMeta(key)) {
                deltaState.seenBackspaces++;
                if (deltaState.seenBackspaces < deltaState.expectedBackspaces) {
                    return false;
                }

                deltaState.ackTimeoutTimer.reset();
                deltaState.waitingBackspaceAck = false;
                const uint64_t extraUsec = deps_.bsRewriteCommitExtraUsec
                                               ? deps_.bsRewriteCommitExtraUsec()
                                               : 0;
                scheduleCommitAfterBackspace(
                    extraUsec + kBrowserX11CommitSlackUsec);
                event.filterAndAccept();
                return true;
            }

            deltaState.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (deltaState.lateBackspaceBudget > 0) {
            if (key.check(FcitxKey_BackSpace) && !hasCtrlAltSuperMeta(key)) {
                deltaState.lateBackspaceBudget--;
                event.filterAndAccept();
                if (deltaState.lateBackspaceBudget == 0) {
                    deltaState.lateBackspaceTimeoutTimer.reset();
                    scheduleDrainPendingKeys(deltaState);
                }
                return true;
            }
            deltaState.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (deltaState.rewriteLock) {
            deltaState.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (key.isCursorMove() || key.check(FcitxKey_Delete)) {
            clearWordState();
            return false;
        }

        if (key.check(FcitxKey_Escape)) {
            clearWordState();
            return false;
        }

        if (key.check(FcitxKey_BackSpace)) {
            if (deltaState.shownText.empty()) {
                return false;
            }
            if (!deltaState.hasRewrittenCurrentWord) {
                clearWordState();
                return false;
            }
            const std::string programForInjector = state.program;
            const auto method = deps_.backspaceInjector->sendBackspaces(
                ic, programForInjector, 1, debug, uinputInterKeyUsec);
            if (method != BackspaceInjector::Method::Uinput) {
                clearWordState();
                return false;
            }
            event.filterAndAccept();
            deltaState.shownText = utf8DropLastN(deltaState.shownText, 1);
            return true;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            if (isBoundaryASCII(c) || key.check(FcitxKey_Return) ||
                key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_ISO_Enter) ||
                key.check(FcitxKey_Tab)) {
                clearWordState();
                return false;
            }

            if (isComposingASCII(c)) {
                if (!adapterShared) {
                    return false;
                }
                adapterShared->setCodeTable(state.codeTable);
                const auto r =
                    adapterShared->processAsciiKey(deltaState.shownText, c);
                if (!r.handled) {
                    return false;
                }
                return applyWordDelta(r.newWord, c, "ascii");
            }

            clearWordState();
            return false;
        }

        clearWordState();
        return false;
    }

private:
    DeltaModeDeps deps_;
};

class BackspaceRewriteModeHandler final : public InputModeHandler {
public:
    explicit BackspaceRewriteModeHandler(DeltaModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                   OpenKeyState &state) override {
        auto key = event.key().normalize();
        if (event.isRelease()) {
            return false;
        }
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
        constexpr uint64_t kDeltaAckTimeoutUsec = 200000;
        constexpr uint64_t kDeltaLateBackspaceTimeoutUsec = 200000;
        auto *loop = deps_.instance ? &deps_.instance->eventLoop() : nullptr;
        auto &deltaState = state.delta;

        auto stateFor = [this](fcitx::InputContext *ic2) -> OpenKeyState * {
            if (!ic2 || !deps_.factory) {
                return nullptr;
            }
            return ic2->propertyFor(deps_.factory);
        };

        auto clearWordState = [&deltaState]() {
            deltaState.clear();
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
            if (!st || st->delta.pendingKeys.empty()) {
                return;
            }
            auto keys = std::move(st->delta.pendingKeys);
            st->delta.pendingKeys.clear();
            for (const auto &k : keys) {
                fcitx::KeyEvent synthetic(ic2, k, false, 0);
                const bool handled = handleKey(ic2, synthetic, *st);
                if (!handled && !synthetic.accepted()) {
                    ic2->forwardKey(k);
                }
            }
        };

        auto scheduleDrainPendingKeys =
            [icRef, lifetimeWeak, loop, stateFor,
             drainPendingKeys](DeltaRewriteState &deltaState2) {
                deltaState2.drainPendingTimer.reset();
                if (!loop) {
                    drainPendingKeys();
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + 1;
                deltaState2.drainPendingTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
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
                        auto _timer = std::move(st->delta.drainPendingTimer);
                        drainPendingKeys();
                        return false;
                    });
                if (deltaState2.drainPendingTimer) {
                    deltaState2.drainPendingTimer->setOneShot();
                    return;
                }
                drainPendingKeys();
            };

        auto scheduleLateBudgetTimeoutDrain =
            [icRef, lifetimeWeak, loop, stateFor,
             scheduleDrainPendingKeys](DeltaRewriteState &deltaState2) {
                if (deltaState2.lateBackspaceBudget == 0) {
                    scheduleDrainPendingKeys(deltaState2);
                    return;
                }
                deltaState2.lateBackspaceTimeoutTimer.reset();
                if (!loop) {
                    deltaState2.lateBackspaceBudget = 0;
                    scheduleDrainPendingKeys(deltaState2);
                    return;
                }
                const uint64_t deadline =
                    fcitx::now(CLOCK_MONOTONIC) +
                    kDeltaLateBackspaceTimeoutUsec;
                deltaState2.lateBackspaceTimeoutTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
                     scheduleDrainPendingKeys](fcitx::EventSourceTime *,
                                              uint64_t) {
                        if (lifetimeWeak.expired()) {
                            return false;
                        }
                        auto *ic3 = icRef.get();
                        if (!ic3) {
                            return false;
                        }
                        auto *st3 = stateFor(ic3);
                        if (!st3) {
                            return false;
                        }
                        auto _timer =
                            std::move(st3->delta.lateBackspaceTimeoutTimer);
                        if (st3->delta.lateBackspaceBudget > 0) {
                            FCITX_INFO()
                                << "openkey: bs-delta late-bs timeout"
                                << " program=" << st3->program
                                << " budget="
                                << st3->delta.lateBackspaceBudget;
                            st3->delta.lateBackspaceBudget = 0;
                        }
                        scheduleDrainPendingKeys(st3->delta);
                        return false;
                    });
                if (deltaState2.lateBackspaceTimeoutTimer) {
                    deltaState2.lateBackspaceTimeoutTimer->setOneShot();
                    return;
                }
                deltaState2.lateBackspaceBudget = 0;
                scheduleDrainPendingKeys(deltaState2);
            };

        auto finishPendingBackspaceCommit =
            [icRef, lifetimeWeak, stateFor, drainPendingKeys]() {
                if (lifetimeWeak.expired()) {
                    return;
                }
                auto *ic2 = icRef.get();
                if (!ic2) {
                    return;
                }
                auto *st = stateFor(ic2);
                if (!st) {
                    return;
                }

                auto &deltaState2 = st->delta;
                const std::string commitText =
                    std::move(deltaState2.pendingConvertedText);
                const std::string shownAfter =
                    std::move(deltaState2.pendingShownTextAfterCommit);
                deltaState2.commitTimer.reset();
                deltaState2.pendingConvertedText.clear();
                deltaState2.pendingShownTextAfterCommit.clear();

                if (!commitText.empty()) {
                    ic2->commitString(commitText);
                }
                deltaState2.shownText = shownAfter;
                deltaState2.rewriteLock = false;
                deltaState2.waitingBackspaceAck = false;
                deltaState2.expectedBackspaces = 0;
                deltaState2.seenBackspaces = 0;
                deltaState2.ackTimeoutTimer.reset();

                drainPendingKeys();
            };

        auto scheduleCommitAfterBackspace =
            [icRef, lifetimeWeak, loop, stateFor, &deltaState,
             scheduleLateBudgetTimeoutDrain,
             finishPendingBackspaceCommit](uint64_t delayUsec) {
                deltaState.commitTimer.reset();
                if (!loop) {
                    const int remainingBackspaces =
                        std::max(0, deltaState.expectedBackspaces -
                                        deltaState.seenBackspaces);
                    finishPendingBackspaceCommit();
                    deltaState.lateBackspaceBudget = remainingBackspaces;
                    scheduleLateBudgetTimeoutDrain(deltaState);
                    return;
                }
                const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
                deltaState.commitTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [icRef, lifetimeWeak, stateFor,
                     scheduleLateBudgetTimeoutDrain,
                     finishPendingBackspaceCommit](fcitx::EventSourceTime *,
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
                        const int remainingBackspaces =
                            std::max(0, st->delta.expectedBackspaces -
                                            st->delta.seenBackspaces);
                        auto _timer = std::move(st->delta.commitTimer);
                        finishPendingBackspaceCommit();
                        st->delta.lateBackspaceBudget = remainingBackspaces;
                        scheduleLateBudgetTimeoutDrain(st->delta);
                        return false;
                    });
                if (deltaState.commitTimer) {
                    deltaState.commitTimer->setOneShot();
                }
            };

        auto scheduleAckTimeout =
            [this, icRef, lifetimeWeak, loop, stateFor, &deltaState,
             scheduleCommitAfterBackspace]() {
                deltaState.ackTimeoutTimer.reset();
                if (!loop) {
                    return;
                }
                const uint64_t deadline =
                    fcitx::now(CLOCK_MONOTONIC) + kDeltaAckTimeoutUsec;
                deltaState.ackTimeoutTimer = loop->addTimeEvent(
                    CLOCK_MONOTONIC, deadline, 0,
                    [this, icRef, lifetimeWeak, stateFor,
                     scheduleCommitAfterBackspace](fcitx::EventSourceTime *,
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
                        auto _timer = std::move(st->delta.ackTimeoutTimer);
                        if (!st->delta.waitingBackspaceAck) {
                            return false;
                        }
                        FCITX_INFO() << "openkey: bs-delta ack timeout"
                                     << " program=" << st->program
                                     << " seen=" << st->delta.seenBackspaces
                                     << " expected="
                                     << st->delta.expectedBackspaces;
                        const uint64_t extraUsec =
                            deps_.bsRewriteCommitExtraUsec
                                ? deps_.bsRewriteCommitExtraUsec()
                                : 0;
                        scheduleCommitAfterBackspace(extraUsec);
                        return false;
                    });
                if (deltaState.ackTimeoutTimer) {
                    deltaState.ackTimeoutTimer->setOneShot();
                }
            };

        auto applyWordDelta = [&, this](const std::string &newWord,
                                        char asciiChar,
                                        const char *reason) -> bool {
            if (!deps_.backspaceInjector) {
                return false;
            }
            if (deltaState.rewriteLock) {
                deltaState.pendingKeys.push_back(key);
                event.filterAndAccept();
                return true;
            }
            if (!fcitx::utf8::validate(deltaState.shownText) ||
                !fcitx::utf8::validate(newWord)) {
                clearWordState();
                return false;
            }

            const std::string oldShown = deltaState.shownText;
            const std::string rawAppend = oldShown + asciiChar;

            const std::size_t prefixLen =
                commonPrefixBytesUTF8Boundary(deltaState.shownText, newWord);
            unsigned int deleteCount =
                utf8CharCount(deltaState.shownText.substr(prefixLen));
            std::string commitText = newWord.substr(prefixLen);
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
                             << " commit=" << commitText;
            }

            if (deleteCount == 0) {
                if (!commitText.empty()) {
                    ic->commitString(commitText);
                }
                deltaState.shownText = newWord;
                deltaState.hasRewrittenCurrentWord =
                    deltaState.hasRewrittenCurrentWord ||
                    (newWord != rawAppend);
                event.filterAndAccept();
                return true;
            }

            const std::string programForInjector = state.program;
            const auto method = deps_.backspaceInjector->sendBackspaces(
                ic, programForInjector, static_cast<int>(deleteCount), debug,
                uinputInterKeyUsec);

            if (method == BackspaceInjector::Method::DeleteSurroundingText) {
                if (!commitText.empty()) {
                    ic->commitString(commitText);
                }
                deltaState.shownText = newWord;
                deltaState.hasRewrittenCurrentWord =
                    deltaState.hasRewrittenCurrentWord ||
                    (newWord != rawAppend);
                event.filterAndAccept();
                return true;
            }

            if (method == BackspaceInjector::Method::Uinput) {
                deps_.backspaceInjector->sendBackspaces(
                    ic, programForInjector, 1, debug, uinputInterKeyUsec);

                deltaState.rewriteLock = true;
                deltaState.waitingBackspaceAck = true;
                deltaState.expectedBackspaces =
                    static_cast<int>(deleteCount) + 1;
                deltaState.seenBackspaces = 0;
                deltaState.pendingConvertedText = std::move(commitText);
                deltaState.pendingShownTextAfterCommit = newWord;
                event.filterAndAccept();
                scheduleAckTimeout();
                return true;
            }

            clearWordState();
            return false;
        };

        if (deltaState.waitingBackspaceAck) {
            if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter) ||
                key.check(FcitxKey_ISO_Enter) || key.isCursorMove() ||
                key.check(FcitxKey_Delete) || key.check(FcitxKey_Tab) ||
                key.check(FcitxKey_Escape)) {
                finishPendingBackspaceCommit();
                deltaState.pendingKeys.clear();
                return false;
            }
            if (key.check(FcitxKey_BackSpace) && !hasCtrlAltSuperMeta(key)) {
                deltaState.seenBackspaces++;
                if (deltaState.seenBackspaces < deltaState.expectedBackspaces) {
                    return false;
                }

                deltaState.ackTimeoutTimer.reset();
                event.filterAndAccept();
                const uint64_t extraUsec = deps_.bsRewriteCommitExtraUsec
                                               ? deps_.bsRewriteCommitExtraUsec()
                                               : 0;
                scheduleCommitAfterBackspace(extraUsec);
                return true;
            }

            deltaState.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (deltaState.lateBackspaceBudget > 0) {
            if (key.check(FcitxKey_BackSpace) && !hasCtrlAltSuperMeta(key)) {
                deltaState.lateBackspaceBudget--;
                event.filterAndAccept();
                if (deltaState.lateBackspaceBudget == 0) {
                    deltaState.lateBackspaceTimeoutTimer.reset();
                    scheduleDrainPendingKeys(deltaState);
                }
                return true;
            }
            deltaState.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (deltaState.rewriteLock) {
            deltaState.pendingKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (key.isCursorMove() || key.check(FcitxKey_Delete)) {
            clearWordState();
            return false;
        }

        if (key.check(FcitxKey_Escape)) {
            clearWordState();
            return false;
        }

        if (key.check(FcitxKey_BackSpace)) {
            if (deltaState.shownText.empty()) {
                return false;
            }
            if (!deltaState.hasRewrittenCurrentWord) {
                clearWordState();
                return false;
            }
            const std::string programForInjector = state.program;
            const auto method = deps_.backspaceInjector->sendBackspaces(
                ic, programForInjector, 1, debug, uinputInterKeyUsec);
            if (method != BackspaceInjector::Method::Uinput) {
                clearWordState();
                return false;
            }
            event.filterAndAccept();
            deltaState.shownText = utf8DropLastN(deltaState.shownText, 1);
            return true;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            if (isBoundaryASCII(c) || key.check(FcitxKey_Return) ||
                key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_ISO_Enter) ||
                key.check(FcitxKey_Tab)) {
                clearWordState();
                return false;
            }

            if (isComposingASCII(c)) {
                if (!adapterShared) {
                    return false;
                }
                adapterShared->setCodeTable(state.codeTable);
                const auto r =
                    adapterShared->processAsciiKey(deltaState.shownText, c);
                if (!r.handled) {
                    return false;
                }
                return applyWordDelta(r.newWord, c, "ascii");
            }

            clearWordState();
            return false;
        }

        clearWordState();
        return false;
    }

private:
    DeltaModeDeps deps_;
};

} // namespace

OpenKeyEngine::OpenKeyEngine(fcitx::Instance *instance)
    : instance_(instance), adapter_(std::make_shared<OpenKeyAdapter>()) {
    lifetime_ = std::make_shared<int>(1);
    instance_->inputContextManager().registerProperty("openkeyState", &factory_);
    focusedAppBridge_ = std::make_unique<FocusedAppBridge>(
        instance_ ? &instance_->eventLoop() : nullptr,
        [this]() { return debugEnabled(); });
    BrowserModeDeps browserDeps;
    browserDeps.instance = instance_;
    browserDeps.factory = &factory_;
    browserDeps.adapter = adapter_;
    browserDeps.backspaceInjector = &g_backspaceInjector;
    browserDeps.lifetimeWeak = lifetime_;
    browserDeps.debugEnabled = [this]() { return debugEnabled(); };
    browserDeps.bsRewriteUinputInterKeyUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteUinputInterKeyUsec.value()));
    };
    browserDeps.browserRewriteCommitDelayUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.browserRewriteCommitDelayUsec.value()));
    };

    DeltaModeDeps deltaDeps;
    deltaDeps.instance = instance_;
    deltaDeps.factory = &factory_;
    deltaDeps.adapter = adapter_;
    deltaDeps.backspaceInjector = &g_backspaceInjector;
    deltaDeps.lifetimeWeak = lifetime_;
    deltaDeps.debugEnabled = [this]() { return debugEnabled(); };
    deltaDeps.bsRewriteCommitExtraUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteCommitExtraUsec.value()));
    };
    deltaDeps.bsRewriteUinputInterKeyUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteUinputInterKeyUsec.value()));
    };
    browserRewriteHandler_ =
        std::make_unique<BrowserModeHandler>(std::move(browserDeps));
    DeltaModeDeps browserX11Deps;
    browserX11Deps.instance = instance_;
    browserX11Deps.factory = &factory_;
    browserX11Deps.adapter = adapter_;
    browserX11Deps.backspaceInjector = &g_backspaceInjector;
    browserX11Deps.lifetimeWeak = lifetime_;
    browserX11Deps.debugEnabled = [this]() { return debugEnabled(); };
    browserX11Deps.bsRewriteCommitExtraUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteCommitExtraUsec.value()));
    };
    browserX11Deps.bsRewriteUinputInterKeyUsec = [this]() -> uint64_t {
        return static_cast<uint64_t>(
            std::max(0, config_.bsRewriteUinputInterKeyUsec.value()));
    };
    browserX11RewriteHandler_ =
        std::make_unique<BrowserX11ModeHandler>(std::move(browserX11Deps));
    backspaceRewriteHandler_ =
        std::make_unique<BackspaceRewriteModeHandler>(std::move(deltaDeps));
    reloadConfig();

    // Warm up uinput ngay khi load để tránh delay lần đầu gõ
    g_backspaceInjector.uinputAvailable(debugEnabled());
}

OpenKeyEngine::~OpenKeyEngine() {
    focusedAppBridge_.reset();
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
        case RuntimeMode::Auto:
            return "Auto";
        case RuntimeMode::Browser:
            return "Browser";
        case RuntimeMode::BrowserX11:
            return "Browser X11";
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
    case RuntimeMode::Auto:
        return "Auto";
    case RuntimeMode::Browser:
        return "Browser";
    case RuntimeMode::BrowserX11:
        return "BrowserX11";
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


void OpenKeyEngine::loadAppModes() {
    appModeMap_.clear();
    fcitx::RawConfig raw;
    fcitx::readAsIni(raw, fcitx::StandardPath::Type::PkgConfig,
                     "conf/openkey-appmodes.conf");
    for (const auto &section : raw.subItems()) {
        const auto normalized = normalizedProgramName(section);
        if (normalized.empty()) {
            continue;
        }
        auto sectionConfig = raw.get(section);
        if (!sectionConfig) {
            continue;
        }
        const std::string *value = sectionConfig->valueByPath("mode");
        if (!value) {
            continue;
        }
        RuntimeMode mode;
        if (!runtimeModeFromString(*value, mode)) {
            continue;
        }
        appModeMap_[normalized] = mode;
    }
}

void OpenKeyEngine::persistAppModes() {
    fcitx::RawConfig raw;
    for (auto &[program, mode] : appModeMap_) {
        if (program.empty()) {
            continue;
        }
        raw[program]["mode"] = runtimeModeToString(mode);
    }
    fcitx::safeSaveAsIni(raw, fcitx::StandardPath::Type::PkgConfig,
                         "conf/openkey-appmodes.conf");
}

void OpenKeyEngine::setAppModeForProgram(const std::string &program,
                                         RuntimeMode mode) {
    const auto normalized = normalizedProgramName(program);
    if (normalized.empty()) {
        return;
    }
    appModeMap_[normalized] = mode;
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
    loadAppModes();
    applyConfig();
}

void OpenKeyEngine::applyConfig() {
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
    fcitx::safeSaveAsIni(config_, fcitx::StandardPath::Type::PkgConfig,
                         "conf/openkey.conf");
}

void OpenKeyEngine::save() { persistConfig(); }

void OpenKeyEngine::activate(const fcitx::InputMethodEntry &,
                             fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    state->browser.clear();
    state->delta.clear();
    state->composing.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    state->noSeedNextWord = false;
    state->surroundingFailures = 0;
    state->manualMode = false;
    state->modeDecided = false;
    state->program = ic->program();
    if (state->program.empty() && focusedAppBridge_) {
        const std::string bridged = focusedAppBridge_->focusedAppId();
        if (!bridged.empty()) {
            state->program = bridged;
            if (debugEnabled()) {
                FCITX_INFO() << "openkey: bridge program=" << state->program;
            }
        }
    }

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
    state->browser.clear();
    state->delta.clear();
    state->composing.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    updatePreeditUI(ic, *state);
}

RuntimeMode OpenKeyEngine::decideMode(fcitx::InputContext *ic,
                                         OpenKeyState &s,
                                         bool writeBack) {
    // Password field should never attempt to do any composition.
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Password)) {
        return RuntimeMode::DirectCommit;
    }

    const auto normalizedProgram = normalizedProgramName(s.program);
    const bool browserDisabled = isX11Backend(ic);
    const bool hideBrowserModes =
        shouldHideBrowserModesForProgram(ic, s.program);
    auto it = appModeMap_.find(normalizedProgram);
    if (!normalizedProgram.empty() && it != appModeMap_.end() &&
        it->second != RuntimeMode::Auto) {
        if (((it->second == RuntimeMode::Browser ||
              it->second == RuntimeMode::BrowserX11) &&
             hideBrowserModes) ||
            (it->second == RuntimeMode::Browser && browserDisabled) ||
            (it->second == RuntimeMode::BrowserX11 && !browserDisabled)) {
            // Ignore backend-specific browser mode on the wrong backend and
            // fall back to auto decision for this session.
        } else {
            return it->second;
        }
    }

    // Browser backspace-rewrite is for Wayland browser quirks. On X11,
    // browsers can drop commitString() after uinput backspace, so prefer
    // preedit and avoid the browser transaction entirely.
    if (shouldUsePreeditForX11Browser(ic, s.program)) {
        return RuntimeMode::Preedit;
    }

    // Wayland browsers default to delta rewrite. The dedicated Browser
    // transaction remains available as an explicit per-app/manual mode.
    if (!browserDisabled && isWaylandBackend(ic) &&
        isBrowserProgram(s.program)) {
        const auto mode = RuntimeMode::BackspaceRewriteDelta;
        if (writeBack && !normalizedProgram.empty()) {
            appModeMap_[normalizedProgram] = mode;
            persistAppModes();
        }
        return mode;
    }

    if (hasReliableSurroundingText(ic)) {
        const auto mode = RuntimeMode::SurroundingText;
        if (writeBack && !normalizedProgram.empty()) {
            appModeMap_[normalizedProgram] = mode;
            persistAppModes();
        }
        return mode;
    }

    if (s.program.empty()) {
        return RuntimeMode::Preedit;
    }

    const auto mode = RuntimeMode::BackspaceRewriteDelta;
    if (writeBack && !normalizedProgram.empty()) {
        appModeMap_[normalizedProgram] = mode;
        persistAppModes();
    }
    return mode;
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
    if (state.mode == RuntimeMode::Browser && browserRewriteHandler_) {
        return browserRewriteHandler_->handleKey(ic, event, state);
    }
    if (state.mode == RuntimeMode::BrowserX11 && browserX11RewriteHandler_) {
        return browserX11RewriteHandler_->handleKey(ic, event, state);
    }
    if (state.mode == RuntimeMode::BackspaceRewriteDelta &&
        backspaceRewriteHandler_) {
        return backspaceRewriteHandler_->handleKey(ic, event, state);
    }
    return false;
}

void OpenKeyEngine::keyEvent(const fcitx::InputMethodEntry &,
                             fcitx::KeyEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);

    if (event.isRelease()) {
        return;
    }

    const auto key = event.key().normalize();

    // Ensure we have a baseline auto mode for this context.
    if (!state->modeDecided) {
        state->lastCapability = ic->capabilityFlags();
        state->mode = decideMode(ic, *state);
        state->autoMode = state->mode;
        state->modeDecided = true;
    }
    const bool browserDisabled = isX11Backend(ic);
    if (browserDisabled && state->mode == RuntimeMode::Browser) {
        state->browser.clear();
        state->mode = decideMode(ic, *state, false);
        state->autoMode = state->mode;
        state->manualMode = false;
    }
    if (!browserDisabled && state->mode == RuntimeMode::BrowserX11) {
        state->browser.clear();
        state->mode = decideMode(ic, *state, false);
        state->autoMode = state->mode;
        state->manualMode = false;
    }

    if (key.checkKeyList(config_.switchModeKey.value()) && key.sym() != FcitxKey_None) {
        auto clearComposingState = [this, ic, state]() {
            state->browser.clear();
            state->delta.clear();
            state->composing.clear();
            state->macroBuffer.clear();
            state->rollbackWord.clear();
            state->rollbackDisplay.clear();
            state->noSeedNextWord = false;
            state->surroundingFailures = 0;
            updatePreeditUI(ic, *state);
        };

        const bool hideBrowserModes =
            shouldHideBrowserModesForProgram(ic, state->program);
        bool returnToAuto = false;
        RuntimeMode nextMode;
        if (!state->manualMode) {
            // First manual override: always offer SurroundingText first.
            nextMode = RuntimeMode::SurroundingText;
        } else if (state->mode == RuntimeMode::SurroundingText) {
            nextMode = hideBrowserModes
                           ? RuntimeMode::BackspaceRewriteDelta
                           : (browserDisabled ? RuntimeMode::BrowserX11
                                              : RuntimeMode::Browser);
        } else if (state->mode == RuntimeMode::Browser) {
            nextMode = RuntimeMode::BackspaceRewriteDelta;
        } else if (state->mode == RuntimeMode::BrowserX11) {
            nextMode = RuntimeMode::BackspaceRewriteDelta;
        } else if (state->mode == RuntimeMode::BackspaceRewriteDelta) {
            nextMode = RuntimeMode::Preedit;
        } else if (state->mode == RuntimeMode::Preedit) {
            nextMode = RuntimeMode::DirectCommit;
        } else {
            returnToAuto = true;
        }

        if (returnToAuto) {
            state->manualMode = false;
            state->modeDecided = false;
            state->lastCapability = ic->capabilityFlags();
            state->mode = decideMode(ic, *state, false);
            state->autoMode = state->mode;
            state->modeDecided = true;
            if (!state->program.empty()) {
                setAppModeForProgram(state->program, RuntimeMode::Auto);
                persistAppModes();
            }
        } else {
            state->manualMode = true;
            state->mode = nextMode;
            if (!state->program.empty()) {
                setAppModeForProgram(state->program, nextMode);
                persistAppModes();
            }
        }

        if (debugEnabled()) {
            FCITX_INFO() << "openkey: switch mode hotkey program="
                         << state->program
                         << " manual=" << (state->manualMode ? 1 : 0)
                         << " mode=" << static_cast<int>(state->mode);
        }
        clearComposingState();
        if (instance_) {
            instance_->showInputMethodInformation(ic);
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
                case RuntimeMode::Auto:
                    return "Auto";
                case RuntimeMode::Browser:
                    return "Browser";
                case RuntimeMode::BrowserX11:
                    return "Browser X11";
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

    bool handled = false;
    switch (state->mode) {
    case RuntimeMode::Auto:
        return;
    case RuntimeMode::DirectCommit:
        return;
    case RuntimeMode::Browser:
        if (browserRewriteHandler_) {
            handled = browserRewriteHandler_->handleKey(ic, event, *state);
        }
        return;
    case RuntimeMode::BrowserX11:
        if (browserX11RewriteHandler_) {
            handled = browserX11RewriteHandler_->handleKey(ic, event, *state);
        }
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
            // If surrounding text is flaky for this app, blacklist it and
            // demote to backspace rewrite. This is a final fallback.
            if (state->surroundingFailures >= 3) {
                if (debugEnabled()) {
                    FCITX_INFO() << "openkey: fallback to backspace program="
                                 << state->program
                                 << " reason=surrounding_failures";
                }
                state->mode = RuntimeMode::BackspaceRewriteDelta;
                if (!state->program.empty()) {
                    setAppModeForProgram(state->program,
                                         RuntimeMode::BackspaceRewriteDelta);
                    persistAppModes();
                }
                (void)handleBackspaceRewrite(ic, event, *state);
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
