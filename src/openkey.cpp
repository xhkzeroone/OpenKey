#include "openkey.h"

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <thread>
#include <mutex>
#include <sstream>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <strings.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>
#include <utility>
#include <unordered_set>
#include <vector>

#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx-config/iniparser.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/utf8.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx-utils/dbus/bus.h>

#include "Macro.h"
#include "openkey_adapter.h"
#include "openkey_backspace_mode_handler.h"
#include "openkey_fcitx_context.h"
#include "openkey_platform.h"
#include "surrounding_text_utils.h"

#ifndef OPENKEY_NONPREEDIT_SERVER_PATH
#define OPENKEY_NONPREEDIT_SERVER_PATH "openkey-nonpreedit-server"
#endif

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

#ifdef __linux__
constexpr int kNonPreeditServerNiceValue = -10;

static bool nonPreeditServerPriorityEnabled() {
    const char *env = std::getenv("OPENKEY_NONPREEDIT_SERVER_PRIORITY");
    if (!env || !*env) {
        return true;
    }
    return std::strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 &&
           strcasecmp(env, "no") != 0 &&
           strcasecmp(env, "off") != 0;
}
#endif

class FcitxRemoteNonPreeditCoordinator {
public:
    using DoneCallback =
        std::function<void(fcitx::InputContext *, uint64_t, uint64_t)>;

    FcitxRemoteNonPreeditCoordinator(fcitx::EventLoop *loop, DoneCallback onDone,
                           std::function<bool()> debugEnabled)
        : onDone_(std::move(onDone)),
          debugEnabled_(std::move(debugEnabled)) {
        if (loop) {
            dispatcher_.attach(loop);
        }
        const char *env = std::getenv("OPENKEY_NONPREEDIT_SERVER_SOCK");
        socketPath_ = (env && *env) ? env : "/tmp/openkey-nonpreedit.sock";
        const char *serverEnv = std::getenv("OPENKEY_NONPREEDIT_SERVER_BIN");
        serverCommand_ =
            (serverEnv && *serverEnv) ? serverEnv : OPENKEY_NONPREEDIT_SERVER_PATH;
    }

    ~FcitxRemoteNonPreeditCoordinator() {
        stop();
        dispatcher_.detach();
    }

    bool enabled() const { return !socketPath_.empty(); }

    bool available() {
        if (socketPath_.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(ioMutex_);
        return ensureConnectedLocked(false);
    }

    bool ensureAvailableOrStartOnce() {
        if (available()) {
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(ioMutex_);
            if (startAttempted_ || socketPath_.empty() || serverCommand_.empty()) {
                return false;
            }
            startAttempted_ = true;
        }

        if (!spawnServer()) {
            return false;
        }

        for (int i = 0; i < 10; i++) {
            usleep(20000);
            if (available()) {
                return true;
            }
        }
        return false;
    }

    void bindSession(uint64_t sessionId,
                     fcitx::TrackableObjectReference<fcitx::InputContext> icRef) {
        sessionRefs_[sessionId] = icRef;
    }

    bool schedule(uint64_t sessionId, uint64_t txId, int backspaces,
                  uint64_t interKeyUsec, uint64_t commitDelayUsec) {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (!ensureConnectedLocked(false)) {
            return false;
        }
        return sendLineLocked("PLAN " + std::to_string(sessionId) + " " +
                              std::to_string(txId) + " " +
                              std::to_string(backspaces) + " " +
                              std::to_string(interKeyUsec) + " " +
                              std::to_string(commitDelayUsec) + "\n");
    }

private:
    bool ensureConnectedLocked(bool logFailures) {
        reapReaderThreadLocked();
        if (fd_ != -1) {
            return true;
        }
        if (stop_) {
            return false;
        }

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            if (logFailures) {
                maybeLog("socket create failed");
            }
            return false;
        }

        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (socketPath_.size() >= sizeof(addr.sun_path)) {
            if (logFailures) {
                maybeLog("socket path too long");
            }
            ::close(fd);
            return false;
        }
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
                      socketPath_.c_str());
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
            0) {
            if (logFailures) {
                maybeLog("connect failed");
            }
            ::close(fd);
            return false;
        }

        fd_ = fd;
        readerThread_ = std::thread([this]() { readerLoop(); });
        maybeLog("connected");
        return true;
    }

    bool spawnServer() {
        if (debugEnabled_ && debugEnabled_()) {
            FCITX_INFO() << "openkey: remote-nonPreedit starting server"
                         << " command=" << serverCommand_
                         << " socket=" << socketPath_;
        }

        const pid_t child = fork();
        if (child < 0) {
            maybeLog("fork failed");
            return false;
        }

        if (child == 0) {
#ifdef __linux__
            (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() == 1) {
                _exit(0);
            }
#endif
            setsid();
#ifdef __linux__
            if (nonPreeditServerPriorityEnabled()) {
                (void)setpriority(PRIO_PROCESS, 0, kNonPreeditServerNiceValue);
            }
#endif
            const int nullFd = ::open("/dev/null", O_RDWR);
            if (nullFd >= 0) {
                dup2(nullFd, STDIN_FILENO);
                dup2(nullFd, STDOUT_FILENO);
                dup2(nullFd, STDERR_FILENO);
                if (nullFd > STDERR_FILENO) {
                    ::close(nullFd);
                }
            }

            execlp(serverCommand_.c_str(), serverCommand_.c_str(), "-socket",
                   socketPath_.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        serverPid_ = child;
        return true;
    }

    void reapReaderThreadLocked() {
        if (fd_ == -1 && readerThread_.joinable()) {
            readerThread_.join();
        }
    }

    bool sendLineLocked(const std::string &line) {
        if (fd_ < 0) {
            return false;
        }
        const char *data = line.data();
        size_t left = line.size();
        while (left > 0) {
            const ssize_t written = ::write(fd_, data, left);
            if (written <= 0) {
                maybeLog("write failed");
                ::shutdown(fd_, SHUT_RDWR);
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            data += written;
            left -= static_cast<size_t>(written);
        }
        return true;
    }

    void stop() {
        pid_t serverPid = -1;
        {
            std::lock_guard<std::mutex> lock(ioMutex_);
            stop_ = true;
            if (fd_ != -1) {
                ::shutdown(fd_, SHUT_RDWR);
                ::close(fd_);
                fd_ = -1;
            }
            serverPid = serverPid_;
            serverPid_ = -1;
        }
        if (readerThread_.joinable()) {
            readerThread_.join();
        }
        if (serverPid > 0) {
            ::kill(serverPid, SIGTERM);
            int status = 0;
            while (waitpid(serverPid, &status, 0) < 0) {
                if (errno != EINTR) {
                    break;
                }
            }
        }
    }

    void readerLoop() {
        std::string pending;
        char buffer[512];
        while (true) {
            int fd = -1;
            {
                std::lock_guard<std::mutex> lock(ioMutex_);
                fd = fd_;
            }
            if (fd < 0) {
                return;
            }

            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n <= 0) {
                std::lock_guard<std::mutex> lock(ioMutex_);
                if (fd_ != -1) {
                    ::close(fd_);
                    fd_ = -1;
                }
                maybeLog("disconnected");
                return;
            }

            pending.append(buffer, buffer + n);
            size_t pos = 0;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                handleServerLine(std::move(line));
            }
        }
    }

    void handleServerLine(std::string line) {
        if (line.empty()) {
            return;
        }

        std::istringstream iss(line);
        std::string opcode;
        iss >> opcode;
        if (opcode == "DONE") {
            uint64_t sessionId = 0;
            uint64_t txId = 0;
            if (!(iss >> sessionId >> txId)) {
                maybeLog("invalid DONE frame");
                return;
            }
            dispatcher_.schedule([this, sessionId, txId]() {
                auto it = sessionRefs_.find(sessionId);
                if (it == sessionRefs_.end()) {
                    return;
                }
                auto *ic = it->second.get();
                if (!ic || !onDone_) {
                    return;
                }
                onDone_(ic, sessionId, txId);
            });
            return;
        }

        maybeLog("unknown opcode");
    }

    void maybeLog(const char *reason) const {
        if (debugEnabled_ && debugEnabled_()) {
            FCITX_INFO() << "openkey: remote-nonPreedit " << reason
                         << " socket=" << socketPath_;
        }
    }

    fcitx::EventDispatcher dispatcher_;
    DoneCallback onDone_;
    std::function<bool()> debugEnabled_;
    std::string serverCommand_;
    std::string socketPath_;
    std::mutex ioMutex_;
    int fd_ = -1;
    bool stop_ = false;
    bool startAttempted_ = false;
    pid_t serverPid_ = -1;
    std::thread readerThread_;
    std::unordered_map<uint64_t,
                       fcitx::TrackableObjectReference<fcitx::InputContext>>
        sessionRefs_;
};

namespace {

static bool hasCtrlAltSuperMeta(const fcitx::Key &key) {
    const auto states = key.states();
    return states.test(fcitx::KeyState::Ctrl) || states.test(fcitx::KeyState::Alt) ||
           states.test(fcitx::KeyState::Super) ||
           states.test(fcitx::KeyState::Meta) ||
           states.test(fcitx::KeyState::Hyper) ||
           states.test(fcitx::KeyState::Super2) ||
           states.test(fcitx::KeyState::Hyper2);
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
    case RuntimeMode::Auto:           return "auto";
    case RuntimeMode::Preedit:        return "preedit";
    case RuntimeMode::SurroundingText:return "surrounding";
    case RuntimeMode::BackspaceRewriteDelta: return "backspace";
    case RuntimeMode::NonPreeditBackspaceRewrite: return "nonPreedit";
    case RuntimeMode::DirectCommit:   return "direct";
    }
    return "auto";
}

static bool runtimeModeFromString(const std::string &mode, RuntimeMode &out) {
    if (equalsASCIIInsensitive(mode, "auto"))        { out = RuntimeMode::Auto; return true; }
    if (equalsASCIIInsensitive(mode, "preedit"))     { out = RuntimeMode::Preedit; return true; }
    if (equalsASCIIInsensitive(mode, "surrounding")) { out = RuntimeMode::SurroundingText; return true; }
    if (equalsASCIIInsensitive(mode, "backspace"))   { out = RuntimeMode::BackspaceRewriteDelta; return true; }
    if (equalsASCIIInsensitive(mode, "nonPreedit"))  { out = RuntimeMode::NonPreeditBackspaceRewrite; return true; }
    if (equalsASCIIInsensitive(mode, "direct"))      { out = RuntimeMode::DirectCommit; return true; }
    return false;
}

static int toOpenKeyInputType(InputType type) {
    switch (type) {
    case InputType::Telex:       return 0;
    case InputType::VNI:         return 1;
    case InputType::SimpleTelex1:return 2;
    case InputType::SimpleTelex2:return 3;
    }
    return 0;
}

static int toOpenKeyCodeTable(CodeTable table) {
    switch (table) {
    case CodeTable::Unicode:    return 0;
    case CodeTable::TCVN3:      return 1;
    case CodeTable::VNIWindows: return 2;
    case CodeTable::UnicodeCompound: return 3;
    case CodeTable::VietnameseLocaleCP1258: return 4;
    }
    return 0;
}

static bool isRunningOnX11(fcitx::InputContext *ic) {
    (void)ic;

    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    const char *x11Display = std::getenv("DISPLAY");

    if (waylandDisplay && *waylandDisplay) {
        return false;
    }
    if (x11Display && *x11Display) {
        return true;
    }
    return false;
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

static bool looksLikeBrowserAutocomplete(fcitx::InputContext *ic,
                                         const std::string &shownText) {
    if (!ic || shownText.empty()) {
        return false;
    }

    if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return false;
    }

    const auto &st = ic->surroundingText();
    if (!st.isValid()) {
        return false;
    }

    const auto &text = st.text();
    const unsigned int cursor = st.cursor();
    const unsigned int anchor = st.anchor();

    const size_t textLen = fcitx::utf8::length(text);
    const size_t shownLen = fcitx::utf8::length(shownText);

    if (cursor > textLen || anchor > textLen || shownLen == 0 || shownLen > textLen) {
        return false;
    }

    size_t rangeStart = cursor >= shownLen ? cursor - shownLen : 0;
    size_t pb = text.find(shownText);

    bool samePrefix =
        pb != std::string::npos &&
        pb >= rangeStart &&
        pb <= cursor;

    if (!samePrefix) {
        return false;
    }

    auto hasNewlineBetween = [&](size_t from, size_t to) {
        if (from > to) {
            std::swap(from, to);
        }
        size_t p = text.find('\n', from);
        return p != std::string::npos && p < to;
    };

    // Case 1: omnibox/autocomplete thường select phần phía sau cursor tới cuối dòng.
    if (cursor != anchor) {
        unsigned int selectionStart = std::min(cursor, anchor);
        unsigned int selectionEnd = std::max(cursor, anchor);

        bool selectionTouchesCursor =
            selectionStart == cursor ||
            (selectionStart < cursor && selectionEnd > cursor);

        bool selectionGoesToLineEnd =
            selectionEnd == textLen ||
            text.find('\n', selectionEnd) == std::string::npos;

        return selectionTouchesCursor &&
               selectionGoesToLineEnd &&
               !hasNewlineBetween(selectionStart, selectionEnd);
    }

    // Case 2: không selection nhưng sau cursor có text tự mọc thêm.
    // Giống browser search/address autocomplete.
    if (cursor < textLen) {
        if (text.find('\n', cursor) != std::string::npos) {
            return false;
        }

        // Sau cursor còn ít nhất 2 ký tự thì mới coi là autocomplete,
        // tránh nhầm khi user sửa giữa từ thường.
        return textLen >= static_cast<size_t>(cursor) + 2;
    }

    return false;
}

static bool isSurroundingTextAvailable(fcitx::InputContext *ic) {
    if (!ic) {
        return false;
    }

    if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return false;
    }

    const auto &st = ic->surroundingText();

    if (!st.isValid()) {
        return false;
    }

    if (st.cursor() != st.anchor()) {
        return false;
    }

    return true;
}

static bool trackedWordStillBeforeCursor(fcitx::InputContext *ic,
                                         const std::string &shownText,
                                         bool requireSurroundingText) {
    if (!ic || shownText.empty()) {
        return false;
    }

    if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return !requireSurroundingText;
    }

    const auto &st = ic->surroundingText();
    if (!st.isValid()) {
        return !requireSurroundingText;
    }

    if (st.cursor() != st.anchor() ||
        st.cursor() > fcitx::utf8::length(st.text())) {
        return false;
    }

    WordSegment seg;
    return extractWordBeforeCursor(st.text(), st.cursor(), seg) &&
           seg.word == shownText;
}


class FcitxBackspaceInjector {
public:
    ~FcitxBackspaceInjector() { destroyUinput(); }

    enum class Method {
        Uinput,
        None,
    };

    Method sendBackspaces(fcitx::InputContext *ic,
                          const std::string &program, int count,
                          bool debug,
                          uint64_t uinputInterKeyUsec = 1500) {
        if (count <= 0) {
            return Method::None;
        }
        if (!ensureUinput(debug)) {
            if (debug) {
                FCITX_INFO() << "openkey: backspace method=uinput-none"
                             << " program=" << program
                             << " frontend=" << (ic && ic->frontend() ? ic->frontend() : "")
                             << " count=" << count
                             << " reason=uinput-unavailable";
            }
            return Method::None;
        }
        if (debug) {
            FCITX_INFO() << "openkey: backspace method=uinput"
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

static FcitxBackspaceInjector g_backspaceInjector;

} // namespace

namespace {
constexpr const char *kBridgeBusName = "org.openkey.Bridge";
constexpr const char *kBridgeObjectPath = "/org/openkey/Bridge";
constexpr const char *kBridgeInterface = "org.openkey.Bridge1";
} // namespace

class FcitxFocusedAppBridge {
public:
    FcitxFocusedAppBridge(fcitx::EventLoop *loop,
                     std::function<bool()> debugEnabled)
        : bus_(fcitx::dbus::BusType::Session),
          debugEnabled_(std::move(debugEnabled)) {
        if (bus_.isOpen() && loop) {
            bus_.attachEventLoop(loop);
        }
    }

    ~FcitxFocusedAppBridge() {
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



FcitxOpenKeyEngine::FcitxOpenKeyEngine(fcitx::Instance *instance)
    : instance_(instance), adapter_(std::make_shared<OpenKeyAdapter>()) {
    lifetime_ = std::make_shared<int>(1);
    instance_->inputContextManager().registerProperty("openkeyState", &factory_);
    remoteNonPreeditCoordinator_ = std::make_unique<FcitxRemoteNonPreeditCoordinator>(
        instance_ ? &instance_->eventLoop() : nullptr,
        [this](fcitx::InputContext *ic, uint64_t sessionId, uint64_t txId) {
            handleRemoteNonPreeditDone(ic, sessionId, txId);
        },
        [this]() { return debugEnabled(); });
    focusedAppBridge_ = std::make_unique<FcitxFocusedAppBridge>(
        instance_ ? &instance_->eventLoop() : nullptr,
        [this]() { return debugEnabled(); });
    BackspaceModeDeps deltaDeps;
    deltaDeps.adapter = adapter_;
    deltaDeps.debugEnabled = [this]() { return debugEnabled(); };
    deltaDeps.enableMacro = [this]() { return config_.enableMacro.value(); };
    deltaDeps.restoreIfWrongSpelling = [this]() {
        return config_.restoreIfWrongSpelling.value();
    };
    deltaDeps.enableBackspaceSnapshot = [this]() {
        return config_.enableBackspaceSnapshot.value();
    };
    backspaceRewriteHandler_ =
        createBackspaceRewriteModeHandler(std::move(deltaDeps));
    BackspaceModeDeps nonPreeditDeltaDeps;
    nonPreeditDeltaDeps.adapter = adapter_;
    nonPreeditDeltaDeps.debugEnabled = [this]() { return debugEnabled(); };
    nonPreeditDeltaDeps.enableMacro = [this]() {
        return config_.enableMacro.value();
    };
    nonPreeditDeltaDeps.restoreIfWrongSpelling = [this]() {
        return config_.restoreIfWrongSpelling.value();
    };
    nonPreeditDeltaDeps.enableBackspaceSnapshot = [this]() {
        return config_.enableBackspaceSnapshot.value();
    };
    nonPreeditDeltaDeps.nonPreeditRemoteEnabled = [this]() {
        return remoteNonPreeditCoordinator_ && remoteNonPreeditCoordinator_->enabled();
    };
    nonPreeditBackspaceRewriteHandler_ =
        createNonPreeditBackspaceRewriteModeHandler(
            std::move(nonPreeditDeltaDeps));

    TextModeHandlerDeps simpleDeps;
    simpleDeps.adapter = adapter_;
    simpleDeps.enableMacro = [this]() { return config_.enableMacro.value(); };
    simpleDeps.restoreIfWrongSpelling = [this]() {
        return config_.restoreIfWrongSpelling.value();
    };
    simpleDeps.enableBackspaceSnapshot = [this]() {
        return config_.enableBackspaceSnapshot.value();
    };
    preeditHandler_ = std::make_unique<PreeditModeHandler>(simpleDeps);
    surroundingTextHandler_ =
        std::make_unique<SurroundingTextModeHandler>(std::move(simpleDeps));
    reloadConfig();
    if (remoteNonPreeditCoordinator_) {
        remoteNonPreeditCoordinator_->ensureAvailableOrStartOnce();
    }

    // Warm up uinput ngay khi load để tránh delay lần đầu gõ
    g_backspaceInjector.uinputAvailable(debugEnabled());
}

FcitxOpenKeyEngine::~FcitxOpenKeyEngine() {
    focusedAppBridge_.reset();
    remoteNonPreeditCoordinator_.reset();
    adapter_.reset();
    lifetime_.reset();
}

std::string FcitxOpenKeyEngine::subModeLabelImpl(const fcitx::InputMethodEntry &,
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
        case RuntimeMode::SurroundingText:
            return "Non Preedit (Gtk Only)";
        case RuntimeMode::Preedit:
            return "Preedit";
        case RuntimeMode::NonPreeditBackspaceRewrite:
            return "Non Preedit";
        case RuntimeMode::BackspaceRewriteDelta:
            return "Non Preedit (Non Server)";
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

std::string FcitxOpenKeyEngine::subMode(const fcitx::InputMethodEntry &,
                                  fcitx::InputContext &ic) {
    auto *state = stateFor(&ic);
    if (!state) {
        return {};
    }
    switch (state->mode) {
    case RuntimeMode::Auto:
        return "Auto";
    case RuntimeMode::SurroundingText:
        return "Non Preedit (Gtk Only)";
    case RuntimeMode::Preedit:
        return "Preedit";
    case RuntimeMode::NonPreeditBackspaceRewrite:
        return "Non Preedit";
    case RuntimeMode::BackspaceRewriteDelta:
        return "Non Preedit (Non Server)";
    case RuntimeMode::DirectCommit:
        return "Direct";
    }
    return {};
}

bool FcitxOpenKeyEngine::debugEnabled() const {
    if (config_.debug.value()) {
        return true;
    }
    const char *env = std::getenv("FCITX_OPENKEY_DEBUG");
    return env && env[0] && env[0] != '0';
}

bool FcitxOpenKeyEngine::nonPreeditServerAvailable() {
    return remoteNonPreeditCoordinator_ && remoteNonPreeditCoordinator_->available();
}

void FcitxOpenKeyEngine::loadAppModes() {
    auto loadOne = [](const char *path,
                      std::unordered_map<std::string, RuntimeMode> &out) {
        out.clear();
        fcitx::RawConfig raw;
        fcitx::readAsIni(raw, fcitx::StandardPath::Type::PkgConfig, path);
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
            out[normalized] = mode;
        }
    };

    loadOne("conf/openkey-appmodes-x11.conf", x11AppModeMap_);
    loadOne("conf/openkey-appmodes-wayland.conf", waylandAppModeMap_);
}

void FcitxOpenKeyEngine::persistAppModes() {
    auto persistOne = [](const char *path,
                         const std::unordered_map<std::string, RuntimeMode> &map) {
        fcitx::RawConfig raw;
        for (const auto &[program, mode] : map) {
            if (program.empty()) {
                continue;
            }
            raw[program]["mode"] = runtimeModeToString(mode);
        }
        fcitx::safeSaveAsIni(raw, fcitx::StandardPath::Type::PkgConfig, path);
    };

    persistOne("conf/openkey-appmodes-x11.conf", x11AppModeMap_);
    persistOne("conf/openkey-appmodes-wayland.conf", waylandAppModeMap_);
}

void FcitxOpenKeyEngine::setAppModeForProgram(fcitx::InputContext *ic,
                                         const std::string &program,
                                         RuntimeMode mode) {
    const auto normalized = normalizedProgramName(program);
    if (normalized.empty()) {
        return;
    }
    appModeMapFor(ic)[normalized] = mode;
}

std::unordered_map<std::string, RuntimeMode> &FcitxOpenKeyEngine::appModeMapFor(
    fcitx::InputContext *ic) {
    return isRunningOnX11(ic) ? x11AppModeMap_ : waylandAppModeMap_;
}

FcitxOpenKeyState *FcitxOpenKeyEngine::stateFor(fcitx::InputContext *ic) {
    return ic->propertyFor(&factory_);
}

static void cancelRewriteTimers(IMContext &context, FcitxOpenKeyState &state) {
    context.cancelTimer(state.delta.commitTimer);
    context.cancelTimer(state.delta.ackTimeoutTimer);
    context.cancelTimer(state.nonPreeditDelta.commitTimer);
    context.cancelTimer(state.nonPreeditDelta.lateBackspaceTimeoutTimer);
    context.cancelTimer(state.nonPreeditDelta.ackTimeoutTimer);
}

static void clearRewriteState(IMContext &context, FcitxOpenKeyState &state) {
    cancelRewriteTimers(context, state);
    state.delta.clear();
    state.nonPreeditDelta.clear();
}

bool FcitxOpenKeyEngine::scheduleRemoteNonPreeditRewrite(
    fcitx::InputContext *ic, FcitxOpenKeyState &state, unsigned int deleteCount,
    uint64_t interBackspaceUsec, uint64_t commitDelayUsec) {
    if (!remoteNonPreeditCoordinator_ || !remoteNonPreeditCoordinator_->enabled() || !ic) {
        return false;
    }

    auto &nonPreeditState = state.nonPreeditDelta;
    if (nonPreeditState.remoteSessionId == 0) {
        nonPreeditState.remoteSessionId =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ic));
        if (nonPreeditState.remoteSessionId == 0) {
            nonPreeditState.remoteSessionId = 1;
        }
    }
    const uint64_t txId = nonPreeditState.remoteNextTxId++;
    nonPreeditState.remotePendingTxId = txId;
    nonPreeditState.remoteRewritePending = true;
    remoteNonPreeditCoordinator_->bindSession(nonPreeditState.remoteSessionId, ic->watch());

    if (!remoteNonPreeditCoordinator_->schedule(
            nonPreeditState.remoteSessionId, txId, static_cast<int>(deleteCount),
            interBackspaceUsec, commitDelayUsec)) {
        nonPreeditState.remotePendingTxId = 0;
        nonPreeditState.remoteRewritePending = false;
        return false;
    }
    return true;
}

void FcitxOpenKeyEngine::handleRemoteNonPreeditDone(fcitx::InputContext *ic,
                                          uint64_t sessionId, uint64_t txId) {
    auto *state = stateFor(ic);
    if (!state) {
        return;
    }
    auto &nonPreeditState = state->nonPreeditDelta;
    if (nonPreeditState.remoteSessionId != sessionId ||
        nonPreeditState.remotePendingTxId != txId || !nonPreeditState.rewriteLock ||
        !nonPreeditState.remoteRewritePending) {
        return;
    }

    if (!nonPreeditBackspaceRewriteHandler_) {
        return;
    }
    auto imContext = std::make_shared<FcitxIMContext>(ic, instance_, lifetime_);
    auto rewriteContext = std::make_shared<FcitxRewriteContext>(
        imContext,
        [this, ic, state](int count, uint64_t interKeyUsec) {
            const auto method = g_backspaceInjector.sendBackspaces(
                ic, state ? state->program : std::string(), count,
                debugEnabled(), interKeyUsec);
            return method == FcitxBackspaceInjector::Method::Uinput
                       ? BackspaceMethod::Uinput
                       : BackspaceMethod::None;
        },
        [](unsigned int, uint64_t, uint64_t) { return false; },
        [ic](std::string_view shownText, bool requireSurroundingText) {
            return trackedWordStillBeforeCursor(
                ic, std::string(shownText), requireSurroundingText);
        },
        [ic](std::string_view shownText) {
            return looksLikeBrowserAutocomplete(ic, std::string(shownText));
        });
    nonPreeditBackspaceRewriteHandler_->handleRemoteCommitAction(
        rewriteContext, *state, txId);
}

const fcitx::Configuration *FcitxOpenKeyEngine::getConfig() const { return &config_; }

void FcitxOpenKeyEngine::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    applyConfig();
}

void FcitxOpenKeyEngine::reloadConfig() {
    fcitx::readAsIni(config_, fcitx::StandardPath::Type::PkgConfig,
                     "conf/openkey.conf");
    fcitx::readAsIni(macroTables_, fcitx::StandardPath::Type::PkgConfig,
                     "conf/openkey-macro-table.conf");
    loadAppModes();
    applyConfig();
}

void FcitxOpenKeyEngine::applyConfig() {
    adapter_->setInputType(toOpenKeyInputType(config_.inputType.value()));
    adapter_->setFreeMark(config_.freeMark.value());
    adapter_->setCodeTable(toOpenKeyCodeTable(config_.codeTable.value()));
    adapter_->setCheckSpelling(config_.checkSpelling.value());
    adapter_->setUseModernOrthography(config_.useModernOrthography.value());
    adapter_->setLiteralWAtWordStart(config_.literalWAtWordStart.value());
    adapter_->setQuickTelex(false);
    adapter_->setRestoreIfWrongSpelling(config_.restoreIfWrongSpelling.value());
    adapter_->setUpperCaseFirstChar(false);
    adapter_->setAllowConsonantZFWJ(config_.allowConsonantZFWJ.value());
    adapter_->setQuickStartConsonant(false);
    adapter_->setQuickEndConsonant(false);
    adapter_->setUseMacro(config_.enableMacro.value());
    adapter_->setUseMacroInEnglishMode(false);
    adapter_->setAutoCapsMacro(false);
    adapter_->setUseSmartSwitchKey(false);
    adapter_->setRememberCode(false);
    adapter_->setOtherLanguage(false);
    adapter_->setTempOffSpelling(false);
    adapter_->setTempOffOpenKey(false);

    // (Re)load macro table if enabled.
    initMacroMap(nullptr, 0);
    if (config_.enableMacro.value()) {
        for (const auto &keymap : macroTables_.macros.value()) {
            if (!keymap.key.value().empty()) {
                addMacro(keymap.key.value(), keymap.value.value());
            }
        }
    }
}

void FcitxOpenKeyEngine::persistConfig() {
    fcitx::safeSaveAsIni(config_, fcitx::StandardPath::Type::PkgConfig,
                         "conf/openkey.conf");
    fcitx::safeSaveAsIni(macroTables_, fcitx::StandardPath::Type::PkgConfig,
                         "conf/openkey-macro-table.conf");
}

void FcitxOpenKeyEngine::save() { persistConfig(); }

const fcitx::Configuration *
FcitxOpenKeyEngine::getSubConfig(const std::string &path) const {
    if (path == "openkey-macro") {
        return &macroTables_;
    }
    return nullptr;
}

void FcitxOpenKeyEngine::setSubConfig(const std::string &path,
                                 const fcitx::RawConfig &config) {
    if (path == "openkey-macro") {
        macroTables_.load(config, true);
        fcitx::safeSaveAsIni(macroTables_, fcitx::StandardPath::Type::PkgConfig,
                             "conf/openkey-macro-table.conf");
        // Reload macros in memory
        initMacroMap(nullptr, 0);
        if (config_.enableMacro.value()) {
            for (const auto &keymap : macroTables_.macros.value()) {
                if (!keymap.key.value().empty()) {
                    addMacro(keymap.key.value(), keymap.value.value());
                }
            }
        }
    }
}

void FcitxOpenKeyEngine::activate(const fcitx::InputMethodEntry &,
                             fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    FcitxIMContext context(ic, instance_, lifetime_);
    clearRewriteState(context, *state);
    state->composing.clear();
    state->preeditKeyBuffer.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    state->rollbackRawBuffer.clear();
    state->rollbackSnapshotWord.clear();
    state->rollbackSnapshotDisplay.clear();
    state->rollbackSnapshotRawBuffer.clear();
    state->canReseedRollbackSnapshot = false;
    state->noSeedNextWord = false;
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
    state->mode = decideMode(ic, *state);
    state->autoMode = state->mode;
    state->modeDecided = true;

    context.clearPreedit();
}

void FcitxOpenKeyEngine::deactivate(const fcitx::InputMethodEntry &entry,
                               fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    FcitxIMContext context(ic, instance_, lifetime_);
    clearRewriteState(context, *state);
    reset(entry, event);
}

void FcitxOpenKeyEngine::reset(const fcitx::InputMethodEntry &,
                          fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    FcitxIMContext context(ic, instance_, lifetime_);
    const bool snapshotEnabled = config_.enableBackspaceSnapshot.value();

    const bool transientResetKeepDelta =
        needsTransientResetPreserve(state->program) &&
        state->mode == RuntimeMode::BackspaceRewriteDelta &&
        !state->delta.shownText.empty() &&
        !state->delta.rawAsciiBuffer.empty() &&
        state->delta.allowTransientResetPreserve &&
        !state->delta.rewriteLock &&
        !state->delta.waitingBackspaceAck;

    const bool preserveDelta =
        transientResetKeepDelta ||
        (
            snapshotEnabled &&
            state->delta.restoredFromBackspaceSnapshot &&
            !state->delta.shownText.empty() &&
            state->delta.allowBackspaceSnapshotResetPreserve
        );

    const bool preserveDeltaSnapshot =
        snapshotEnabled &&
        !preserveDelta &&
        state->delta.allowBackspaceSnapshotResetPreserve &&
        state->delta.preserveBackspaceSnapshotAfterBoundaryBackspace &&
        state->delta.canReseedFromBackspaceSnapshot &&
        !state->delta.backspaceSnapshotShownText.empty();

    const std::string deltaShown = state->delta.shownText;
    const std::string deltaRaw = state->delta.rawAsciiBuffer;
    const bool deltaRewritten = state->delta.hasRewrittenCurrentWord;
    const bool deltaAllowTransientResetPreserve =
        state->delta.allowTransientResetPreserve;
    const std::string deltaSnapshotShown =
        state->delta.backspaceSnapshotShownText;
    const std::string deltaSnapshotRaw =
        state->delta.backspaceSnapshotRawAsciiBuffer;
    const bool deltaSnapshotRewritten =
        state->delta.backspaceSnapshotHasRewrittenCurrentWord;

    const bool transientResetKeepNonPreedit =
        needsTransientResetPreserve(state->program) &&
        state->mode == RuntimeMode::NonPreeditBackspaceRewrite &&
        !state->nonPreeditDelta.shownText.empty() &&
        !state->nonPreeditDelta.rawAsciiBuffer.empty() &&
        state->nonPreeditDelta.allowTransientResetPreserve &&
        !state->nonPreeditDelta.rewriteLock;

    const bool preserveNonPreedit =
        transientResetKeepNonPreedit ||
        (
            snapshotEnabled &&
            state->nonPreeditDelta.restoredFromBackspaceSnapshot &&
            !state->nonPreeditDelta.shownText.empty() &&
            state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve
        );

    const bool preserveNonPreeditSnapshot =
        snapshotEnabled &&
        !preserveNonPreedit &&
        state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve &&
        state->nonPreeditDelta.preserveBackspaceSnapshotAfterBoundaryBackspace &&
        state->nonPreeditDelta.canReseedFromBackspaceSnapshot &&
        !state->nonPreeditDelta.backspaceSnapshotShownText.empty();

    const std::string nonPreeditShown = state->nonPreeditDelta.shownText;
    const std::string nonPreeditRaw = state->nonPreeditDelta.rawAsciiBuffer;
    const bool nonPreeditRewritten =
        state->nonPreeditDelta.hasRewrittenCurrentWord;
    const bool nonPreeditAllowTransientResetPreserve =
        state->nonPreeditDelta.allowTransientResetPreserve;
    const std::string nonPreeditSnapshotShown =
        state->nonPreeditDelta.backspaceSnapshotShownText;
    const std::string nonPreeditSnapshotRaw =
        state->nonPreeditDelta.backspaceSnapshotRawAsciiBuffer;
    const bool nonPreeditSnapshotRewritten =
        state->nonPreeditDelta.backspaceSnapshotHasRewrittenCurrentWord;

    if (debugEnabled()) {
        FCITX_INFO() << "openkey: reset"
                     << " program=" << state->program
                     << " snapshotEnabled=" << snapshotEnabled
                     << " transientResetKeepDelta="
                     << transientResetKeepDelta
                     << " preserveDelta=" << preserveDelta
                     << " preserveDeltaSnapshot=" << preserveDeltaSnapshot
                     << " deltaAllowPreserve="
                     << state->delta.allowBackspaceSnapshotResetPreserve
                     << " deltaShown=" << state->delta.shownText
                     << " deltaRaw=" << state->delta.rawAsciiBuffer
                     << " deltaAllowTransientResetPreserve="
                     << state->delta.allowTransientResetPreserve
                     << " transientResetKeepNonPreedit="
                     << transientResetKeepNonPreedit
                     << " preserveNonPreedit=" << preserveNonPreedit
                     << " preserveNonPreeditSnapshot="
                     << preserveNonPreeditSnapshot
                     << " nonPreeditAllowPreserve="
                     << state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve
                     << " nonPreeditShown="
                     << state->nonPreeditDelta.shownText
                     << " nonPreeditRaw="
                     << state->nonPreeditDelta.rawAsciiBuffer
                     << " nonPreeditAllowTransientResetPreserve="
                     << state->nonPreeditDelta.allowTransientResetPreserve;
    }

    clearRewriteState(context, *state);

    if (preserveDelta) {
        state->delta.shownText = deltaShown;
        state->delta.rawAsciiBuffer = deltaRaw;
        state->delta.hasRewrittenCurrentWord = deltaRewritten;
        state->delta.allowTransientResetPreserve =
            deltaAllowTransientResetPreserve;
        state->delta.restoredFromBackspaceSnapshot = false;  // Transient reset preserve, not a snapshot restore.
        state->delta.allowBackspaceSnapshotResetPreserve = false;
        if (debugEnabled()) {
            FCITX_INFO() << "openkey: reset preserve"
                         << " mode=backspace"
                         << " shown=" << state->delta.shownText
                         << " raw=" << state->delta.rawAsciiBuffer
                         << " rewritten="
                         << state->delta.hasRewrittenCurrentWord;
        }
    }

    if (preserveDeltaSnapshot) {
        state->delta.backspaceSnapshotShownText = deltaSnapshotShown;
        state->delta.backspaceSnapshotRawAsciiBuffer = deltaSnapshotRaw;
        state->delta.backspaceSnapshotHasRewrittenCurrentWord =
            deltaSnapshotRewritten;
        state->delta.canReseedFromBackspaceSnapshot = true;
        state->delta.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
        state->delta.allowBackspaceSnapshotResetPreserve = false;
        if (debugEnabled()) {
            FCITX_INFO() << "openkey: reset preserve-snapshot"
                         << " mode=backspace"
                         << " snapshotShown="
                         << state->delta.backspaceSnapshotShownText
                         << " snapshotRaw="
                         << state->delta.backspaceSnapshotRawAsciiBuffer;
        }
    }

    if (preserveNonPreedit) {
        state->nonPreeditDelta.shownText = nonPreeditShown;
        state->nonPreeditDelta.rawAsciiBuffer = nonPreeditRaw;
        state->nonPreeditDelta.hasRewrittenCurrentWord = nonPreeditRewritten;
        state->nonPreeditDelta.allowTransientResetPreserve =
            nonPreeditAllowTransientResetPreserve;
        state->nonPreeditDelta.restoredFromBackspaceSnapshot = false;  // Transient reset preserve, not a snapshot restore.
        state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve = false;
        if (debugEnabled()) {
            FCITX_INFO() << "openkey: reset preserve"
                         << " mode=nonPreedit"
                         << " shown=" << state->nonPreeditDelta.shownText
                         << " raw=" << state->nonPreeditDelta.rawAsciiBuffer
                         << " rewritten="
                         << state->nonPreeditDelta.hasRewrittenCurrentWord;
        }
    }

    if (preserveNonPreeditSnapshot) {
        state->nonPreeditDelta.backspaceSnapshotShownText =
            nonPreeditSnapshotShown;
        state->nonPreeditDelta.backspaceSnapshotRawAsciiBuffer =
            nonPreeditSnapshotRaw;
        state->nonPreeditDelta.backspaceSnapshotHasRewrittenCurrentWord =
            nonPreeditSnapshotRewritten;
        state->nonPreeditDelta.canReseedFromBackspaceSnapshot = true;
        state->nonPreeditDelta.preserveBackspaceSnapshotAfterBoundaryBackspace =
            true;
        state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve = false;
        if (debugEnabled()) {
            FCITX_INFO() << "openkey: reset preserve-snapshot"
                         << " mode=nonPreedit"
                         << " snapshotShown="
                         << state->nonPreeditDelta.backspaceSnapshotShownText
                         << " snapshotRaw="
                         << state->nonPreeditDelta.backspaceSnapshotRawAsciiBuffer;
        }
    }

    state->composing.clear();
    state->preeditKeyBuffer.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    state->rollbackRawBuffer.clear();
    state->rollbackSnapshotWord.clear();
    state->rollbackSnapshotDisplay.clear();
    state->rollbackSnapshotRawBuffer.clear();
    state->canReseedRollbackSnapshot = false;

    context.clearPreedit();
}

RuntimeMode FcitxOpenKeyEngine::decideMode(fcitx::InputContext *ic,
                                         FcitxOpenKeyState &s,
                                         bool writeBack) {
    // Password field should never attempt to do any composition.
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Password)) {
        return RuntimeMode::DirectCommit;
    }

    const auto normalizedProgram = normalizedProgramName(s.program);
    auto &appModeMap = appModeMapFor(ic);
    auto it = appModeMap.find(normalizedProgram);
    if (!normalizedProgram.empty() && it != appModeMap.end() &&
        it->second != RuntimeMode::Auto) {
        const bool hasNonPreeditServer = nonPreeditServerAvailable();
        if (it->second == RuntimeMode::NonPreeditBackspaceRewrite &&
            hasNonPreeditServer) {
            return it->second;
        }
        if (it->second == RuntimeMode::BackspaceRewriteDelta &&
            !hasNonPreeditServer) {
            return it->second;
        }
        if (it->second == RuntimeMode::SurroundingText) {
            if (isRunningOnX11(ic) && isSurroundingTextAvailable(ic)) {
                return it->second;
            }
            // Fallback tạm thời, không writeBack để không mất config
            return  nonPreeditServerAvailable() ? RuntimeMode::NonPreeditBackspaceRewrite : RuntimeMode::BackspaceRewriteDelta;
        }
        if (it->second == RuntimeMode::Preedit ||
            it->second == RuntimeMode::DirectCommit) {
            return it->second;
        }
    
        if (writeBack) {
            appModeMap[normalizedProgram] = RuntimeMode::Preedit;
            persistAppModes();
        }
        return RuntimeMode::Preedit;
    }


    RuntimeMode mode;
    if (isFirefoxLikeProgram(s.program)) {
        mode = RuntimeMode::Preedit;
    } else if (isRunningOnX11(ic) && isSurroundingTextAvailable(ic)) {
        mode = RuntimeMode::SurroundingText;
    } else {
        mode = nonPreeditServerAvailable() ? RuntimeMode::NonPreeditBackspaceRewrite : RuntimeMode::BackspaceRewriteDelta;
    }
    if (writeBack && !normalizedProgram.empty()) {
        appModeMap[normalizedProgram] = mode;
        persistAppModes();
    }
    return mode;
}

RuntimeMode FcitxOpenKeyEngine::firstManualMode() const {
    return RuntimeMode::NonPreeditBackspaceRewrite;
}



void FcitxOpenKeyEngine::keyEvent(const fcitx::InputMethodEntry &,
                             fcitx::KeyEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    auto textContext = std::make_shared<FcitxIMContext>(ic, instance_, lifetime_);
    auto backspaceContext = std::make_shared<FcitxRewriteContext>(
        textContext,
        [this, ic, state](int count, uint64_t interKeyUsec) {
            const auto method = g_backspaceInjector.sendBackspaces(
                ic, state ? state->program : std::string(), count,
                debugEnabled(), interKeyUsec);
            return method == FcitxBackspaceInjector::Method::Uinput
                       ? BackspaceMethod::Uinput
                       : BackspaceMethod::None;
        },
        [this, ic, state](unsigned int deleteCount,
                          uint64_t interBackspaceUsec,
                          uint64_t commitDelayUsec) {
            return state && scheduleRemoteNonPreeditRewrite(
                                ic, *state, deleteCount, interBackspaceUsec,
                                commitDelayUsec);
        },
        [ic](std::string_view shownText, bool requireSurroundingText) {
            return trackedWordStillBeforeCursor(
                ic, std::string(shownText), requireSurroundingText);
        },
        [ic](std::string_view shownText) {
            return looksLikeBrowserAutocomplete(ic, std::string(shownText));
        });

    if (event.isRelease()) {
        return;
    }

    const auto key = event.key().normalize();
    const KeyInfo keyInfo = keyInfoFromFcitxEvent(event);

    // Ensure we have a baseline auto mode for this context.
    if (!state->modeDecided) {
        state->mode = decideMode(ic, *state, false);
        state->autoMode = state->mode;
        state->modeDecided = true;
    }

    if (key.checkKeyList(config_.switchModeKey.value()) && key.sym() != FcitxKey_None) {
            auto clearComposingState = [&textContext, state]() {
                clearRewriteState(*textContext, *state);
                state->composing.clear();
                state->preeditKeyBuffer.clear();
                state->macroBuffer.clear();
                state->rollbackWord.clear();
                state->rollbackDisplay.clear();
                state->rollbackRawBuffer.clear();
                state->rollbackSnapshotWord.clear();
                state->rollbackSnapshotDisplay.clear();
                state->rollbackSnapshotRawBuffer.clear();
                state->canReseedRollbackSnapshot = false;
                state->noSeedNextWord = false;
                textContext->clearPreedit();
            };

        bool returnToAuto = false;
        RuntimeMode nextMode;
        if (!state->manualMode) {
            nextMode = RuntimeMode::NonPreeditBackspaceRewrite;
        } else if (state->mode == RuntimeMode::NonPreeditBackspaceRewrite) {
            nextMode = RuntimeMode::BackspaceRewriteDelta;
        } else if (state->mode == RuntimeMode::BackspaceRewriteDelta) {
            nextMode = RuntimeMode::SurroundingText;
        } else if (state->mode == RuntimeMode::SurroundingText) {
            nextMode = RuntimeMode::Preedit;
        } else if (state->mode == RuntimeMode::Preedit) {
            nextMode = RuntimeMode::DirectCommit;
        } else if (state->mode == RuntimeMode::DirectCommit) {
            returnToAuto = true;
        } else {
            nextMode = firstManualMode();
        }

        if (returnToAuto) {
            state->manualMode = false;
            state->mode = state->autoMode;
            state->modeDecided = true;
            if (!state->program.empty()) {
                setAppModeForProgram(ic, state->program, RuntimeMode::Auto);
                persistAppModes();
            }
        } else {
            state->manualMode = true;
            state->mode = nextMode;
            if (!state->program.empty()) {
                setAppModeForProgram(
                    ic, state->program,
                    nextMode == RuntimeMode::SurroundingText
                        ? RuntimeMode::Auto
                        : nextMode);
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
                case RuntimeMode::SurroundingText:
                    return "Non Preedit (Gtk Only)";
                case RuntimeMode::Preedit:
                    return "Preedit";
                case RuntimeMode::NonPreeditBackspaceRewrite:
                    return "Non Preedit";
                case RuntimeMode::BackspaceRewriteDelta:
                    return "Non Preedit (Non Server)";
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
            preeditHandler_->reset(*state);
        }
        return;
    }

    switch (state->mode) {
    case RuntimeMode::Auto:
        return;
    case RuntimeMode::DirectCommit:
        return;
    case RuntimeMode::BackspaceRewriteDelta:
        if (backspaceRewriteHandler_) {
            if (backspaceRewriteHandler_->handleKey(backspaceContext, keyInfo,
                                                    *state)) {
                event.filterAndAccept();
            }
        }
        return;
    case RuntimeMode::NonPreeditBackspaceRewrite:
        if (nonPreeditBackspaceRewriteHandler_) {
            if (nonPreeditBackspaceRewriteHandler_->handleKey(
                    backspaceContext, keyInfo, *state)) {
                event.filterAndAccept();
            }
        }
        return;
    case RuntimeMode::Preedit:
        adapter_->setCodeTable(state->codeTable);
        if (preeditHandler_) {
            if (preeditHandler_->handleKey(*textContext, keyInfo, *state)) {
                event.filterAndAccept();
            }
        }
        return;
    case RuntimeMode::SurroundingText:
        adapter_->setCodeTable(state->codeTable);
        if (surroundingTextHandler_) {
            if (surroundingTextHandler_->handleKey(*textContext, keyInfo,
                                                   *state)) {
                event.filterAndAccept();
            }
        }
        return;
    }
}

class FcitxOpenKeyEngineFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new FcitxOpenKeyEngine(manager->instance());
    }
};

} // namespace openkey

FCITX_ADDON_FACTORY(openkey::FcitxOpenKeyEngineFactory)
