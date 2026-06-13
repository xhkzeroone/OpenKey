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
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
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

class RemoteNonPreeditCoordinator {
public:
    using DoneCallback =
        std::function<void(fcitx::InputContext *, uint64_t, uint64_t)>;

    RemoteNonPreeditCoordinator(fcitx::EventLoop *loop, DoneCallback onDone,
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

    ~RemoteNonPreeditCoordinator() {
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

struct RewriteTiming {
    uint64_t interKeyUsec = 2000;
    uint64_t commitDelayUsec = 60000;
};

struct DeltaRewriteTiming {
    uint64_t interKeyUsec = 1000;
    uint64_t commitDelayUsec = 1000;
};

// Delta timing tuned based on NonPreedit values
// Format: {interKeyUsec, commitDelayUsec}
static constexpr DeltaRewriteTiming kDeltaWaylandTiming{1000, 50000};
static constexpr DeltaRewriteTiming kDeltaWaylandBrowserTiming{1000, 50000};
static constexpr DeltaRewriteTiming kDeltaWaylandElectronTiming{1000, 50000};
static constexpr DeltaRewriteTiming kDeltaX11Timing{1000, 50000};
static constexpr DeltaRewriteTiming kDeltaWaylandFcitx4Timing{1000, 100000};
static constexpr DeltaRewriteTiming kDeltaX11Fcitx4Timing{1000, 100000};
static constexpr DeltaRewriteTiming kDeltaX11BrowserTiming{1000, 100000};
static constexpr uint64_t kDeltaPostCommitPumpDelayUsec = 20000;

static constexpr RewriteTiming kNonPreeditWaylandTiming{1000, 20000};
static constexpr RewriteTiming kNonPreeditX11Timing{1000, 80000};
static constexpr RewriteTiming kNonPreeditX11BrowserTiming{1000, 80000};
static constexpr uint64_t kNonPreeditPostCommitPumpDelayUsec = 20000;

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

static bool isFcitx4Frontend(fcitx::InputContext *ic) {
    if (!ic || !ic->frontend()) {
        return false;
    }
    return asciiLower(ic->frontend()).find("fcitx4") != std::string::npos;
}

static bool isXimFrontend(fcitx::InputContext *ic) {
    if (!ic || !ic->frontend()) {
        return false;
    }
    return asciiLower(ic->frontend()).find("xim") != std::string::npos;
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
        "window:",
    };

    for (const auto &pattern : kBrowserPatterns) {
        if (base.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
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

static DeltaRewriteTiming deltaTimingFor(fcitx::InputContext *ic,
                                         const std::string &program) {
    const bool x11 = isRunningOnX11(ic);
    const bool fcitx4 = isFcitx4Frontend(ic);

    if (x11 && fcitx4) {
        return kDeltaX11Fcitx4Timing;
    }
    if (!x11 && fcitx4) {
        return kDeltaWaylandFcitx4Timing;
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

static RewriteTiming nonPreeditTimingFor(fcitx::InputContext *ic,
                                         const std::string &program) {
    const bool x11 = isRunningOnX11(ic);
    if (x11 && isBrowserLikeProgram(program)) {
        return kNonPreeditX11BrowserTiming;
    }

    if (x11) {
        return kNonPreeditX11Timing;
    }
    return kNonPreeditWaylandTiming;
}

class BackspaceInjector {
public:
    ~BackspaceInjector() { destroyUinput(); }

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

enum class SnapshotBackspaceProbe {
    Unavailable,
    Restore,
    Wait,
    Stale,
};

static bool utf8EndsWith(const std::string &text, const std::string &suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static SnapshotBackspaceProbe probeBackspaceSnapshotWithSurrounding(
    fcitx::InputContext *ic, const std::string &snapshotWord) {
    if (!ic || snapshotWord.empty() ||
        !ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) ||
        !fcitx::utf8::validate(snapshotWord)) {
        return SnapshotBackspaceProbe::Unavailable;
    }

    const auto &st = ic->surroundingText();
    if (!st.isValid() || st.cursor() != st.anchor() ||
        !fcitx::utf8::validate(st.text())) {
        return SnapshotBackspaceProbe::Unavailable;
    }

    const auto textLen = fcitx::utf8::length(st.text());
    if (st.cursor() == 0 || st.cursor() > textLen) {
        return SnapshotBackspaceProbe::Stale;
    }

    const int cursorByte =
        fcitx::utf8::ncharByteLength(st.text().begin(), st.cursor());
    if (cursorByte <= 0) {
        return SnapshotBackspaceProbe::Stale;
    }

    const std::string beforeCursor =
        st.text().substr(0, static_cast<size_t>(cursorByte));
    std::string afterBackspace = utf8DropLastN(beforeCursor, 1);
    if (!fcitx::utf8::validate(afterBackspace)) {
        return SnapshotBackspaceProbe::Stale;
    }

    WordSegment seg;
    const unsigned int afterCursor =
        static_cast<unsigned int>(fcitx::utf8::length(afterBackspace));
    if (extractWordBeforeCursor(afterBackspace, afterCursor, seg) &&
        seg.word == snapshotWord) {
        return SnapshotBackspaceProbe::Restore;
    }

    bool trimmedBoundary = false;
    while (!afterBackspace.empty() && fcitx::utf8::validate(afterBackspace)) {
        const auto len = fcitx::utf8::length(afterBackspace);
        auto it = fcitx::utf8::nextNChar(afterBackspace.begin(), len - 1);
        const std::string lastChar(it, afterBackspace.end());
        if (lastChar.size() != 1 ||
            isComposingASCII(static_cast<char>(lastChar[0]))) {
            break;
        }
        trimmedBoundary = true;
        afterBackspace.erase(it, afterBackspace.end());
    }

    const unsigned int trimmedCursor =
        static_cast<unsigned int>(fcitx::utf8::length(afterBackspace));
    if (trimmedBoundary &&
        extractWordBeforeCursor(afterBackspace, trimmedCursor, seg) &&
        seg.word == snapshotWord && utf8EndsWith(afterBackspace, snapshotWord)) {
        return SnapshotBackspaceProbe::Wait;
    }

    return SnapshotBackspaceProbe::Stale;
}

static bool currentSurroundingAllowsSnapshotPreserve(
    fcitx::InputContext *ic, const std::string &snapshotWord,
    bool snapshotRequiresSurrounding) {
    if (!ic || snapshotWord.empty() || !fcitx::utf8::validate(snapshotWord)) {
        return false;
    }
    if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return !snapshotRequiresSurrounding;
    }

    const auto &st = ic->surroundingText();
    if (!st.isValid() || st.cursor() != st.anchor() ||
        !fcitx::utf8::validate(st.text())) {
        return !snapshotRequiresSurrounding;
    }

    const auto textLen = fcitx::utf8::length(st.text());
    if (st.cursor() > textLen) {
        return false;
    }
    const int cursorByte =
        fcitx::utf8::ncharByteLength(st.text().begin(), st.cursor());
    if (cursorByte < 0) {
        return false;
    }

    std::string beforeCursor =
        st.text().substr(0, static_cast<size_t>(cursorByte));
    WordSegment seg;
    const unsigned int cursor =
        static_cast<unsigned int>(fcitx::utf8::length(beforeCursor));
    if (extractWordBeforeCursor(beforeCursor, cursor, seg) &&
        seg.word == snapshotWord) {
        return true;
    }

    bool trimmedBoundary = false;
    while (!beforeCursor.empty() && fcitx::utf8::validate(beforeCursor)) {
        const auto len = fcitx::utf8::length(beforeCursor);
        auto it = fcitx::utf8::nextNChar(beforeCursor.begin(), len - 1);
        const std::string lastChar(it, beforeCursor.end());
        if (lastChar.size() != 1 ||
            isComposingASCII(static_cast<char>(lastChar[0]))) {
            break;
        }
        trimmedBoundary = true;
        beforeCursor.erase(it, beforeCursor.end());
    }

    const unsigned int trimmedCursor =
        static_cast<unsigned int>(fcitx::utf8::length(beforeCursor));
    return trimmedBoundary &&
           extractWordBeforeCursor(beforeCursor, trimmedCursor, seg) &&
           seg.word == snapshotWord && utf8EndsWith(beforeCursor, snapshotWord);
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



struct DeltaModeDeps {
    fcitx::Instance *instance = nullptr;
    fcitx::SimpleInputContextPropertyFactory<OpenKeyState> *factory = nullptr;
    std::shared_ptr<OpenKeyAdapter> adapter;
    BackspaceInjector *backspaceInjector = nullptr;
    std::weak_ptr<void> lifetimeWeak;
    std::function<bool()> debugEnabled;
    std::function<bool()> enableMacro;
    std::function<bool()> restoreIfWrongSpelling;
    std::function<bool()> enableBackspaceSnapshot;
    std::function<bool()> nonPreeditRemoteEnabled;
    std::function<bool(fcitx::InputContext *, OpenKeyState &, unsigned int,
                       uint64_t, uint64_t)>
        nonPreeditRemoteSchedule;
};

// Deps shared by PreeditModeHandler and SurroundingTextModeHandler.
struct SimpleModeHandlerDeps {
    std::shared_ptr<OpenKeyAdapter> adapter;
    std::function<bool()> debugEnabled;
    std::function<bool()> enableMacro;
    std::function<bool()> restoreIfWrongSpelling;
};

// ---------------------------------------------------------------------------
// PreeditModeHandler
// ---------------------------------------------------------------------------

class PreeditModeHandler final : public InputModeHandler {
public:
    explicit PreeditModeHandler(SimpleModeHandlerDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                   OpenKeyState &state) override {
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
            state.preeditKeyBuffer.clear();
            updatePreeditUI(ic, state);
            event.filterAndAccept();
            return true;
        };

        // Navigation / editing keys terminate preedit but are handled by app.
        if (key.isCursorMove() || key.check(FcitxKey_Delete) ||
            key.check(FcitxKey_Tab)) {
            commitAndClearPreedit(ic, state);
            return false;
        }

        if (key.check(FcitxKey_BackSpace)) {
            if (!state.composing.empty()) {
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
                updatePreeditUI(ic, state);
                event.filterAndAccept();
                return true;
            }
            return false;
        }

        if (key.check(FcitxKey_Escape)) {
            if (!state.composing.empty()) {
                state.composing.clear();
                state.preeditKeyBuffer.clear();
                updatePreeditUI(ic, state);
                event.filterAndAccept();
                return true;
            }
            return false;
        }

        if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter) ||
            key.check(FcitxKey_ISO_Enter)) {
            commitAndClearPreedit(ic, state);
            return false;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        const std::string utf8 = fcitx::Key::keySymToUTF8(key.sym());

        if (!state.composing.empty() && (utf8.empty() || uni > 0x7F)) {
            commitAndClearPreedit(ic, state);
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
            if (!deps_.restoreIfWrongSpelling ||
                !deps_.restoreIfWrongSpelling() || state.composing.empty()) {
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

        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            if (c == ' ') {
                if (!expandMacroBeforeBoundary(c)) {
                    restoreBeforeBoundary(c);
                }
                return commitPreeditAndMaybeAppend(" ");
            }
            if (!isComposingASCII(c)) {
                const std::string boundaryUtf8 = fcitx::Key::keySymToUTF8(key.sym());
                if (!expandMacroBeforeBoundary(c)) {
                    restoreBeforeBoundary(c);
                }
                if (!boundaryUtf8.empty()) {
                    return commitPreeditAndMaybeAppend(boundaryUtf8);
                }
                commitAndClearPreedit(ic, state);
                return false;
            }

            auto r = deps_.adapter->processAsciiKey(state.composing, c);
            if (!r.handled) {
                return false;
            }
            state.composing = std::move(r.newWord);
            state.preeditKeyBuffer.push_back(c);
            updatePreeditUI(ic, state);
            event.filterAndAccept();
            return true;
        }

        return false;
    }

    void reset(OpenKeyState &state) override {
        state.composing.clear();
        state.preeditKeyBuffer.clear();
    }

private:
    void updatePreeditUI(fcitx::InputContext *ic, const OpenKeyState &state) {
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

    void commitAndClearPreedit(fcitx::InputContext *ic, OpenKeyState &state) {
        if (state.composing.empty()) {
            return;
        }
        ic->commitString(state.composing);
        state.lastCommitted = state.composing;
        state.composing.clear();
        state.preeditKeyBuffer.clear();
        updatePreeditUI(ic, state);
    }

    SimpleModeHandlerDeps deps_;
};

// ---------------------------------------------------------------------------
// SurroundingTextModeHandler
// ---------------------------------------------------------------------------

class SurroundingTextModeHandler final : public InputModeHandler {
public:
    explicit SurroundingTextModeHandler(SimpleModeHandlerDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                   OpenKeyState &state) override {
        auto key = event.key().normalize();

        // Preedit buffer should be empty in surrounding mode.
        state.composing.clear();

        const bool debug = deps_.debugEnabled && deps_.debugEnabled();

        if (debug) {
            FCITX_INFO() << "openkey: st key program=" << state.program
                         << " sym=" << key.sym()
                         << " rollbackDisplay=" << state.rollbackDisplay
                         << " rollbackWord=" << state.rollbackWord;
        }

        if (key.check(FcitxKey_Delete) || key.isCursorMove()) {
            state.macroBuffer.clear();
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            state.noSeedNextWord = false;
            return false;
        }

        if (key.check(FcitxKey_BackSpace)) {
            if (!state.rollbackWord.empty()) {
                if (!fcitx::utf8::validate(state.rollbackWord) ||
                    !fcitx::utf8::validate(state.rollbackDisplay)) {
                    state.rollbackWord.clear();
                    state.rollbackDisplay.clear();
                    state.rollbackRawBuffer.clear();
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
                    return false;
                }
                if (deleteChars > 0) {
                    ic->deleteSurroundingText(-static_cast<int>(deleteChars), deleteChars);
                }
                if (newDisplay.size() > prefixLen) {
                    ic->commitString(newDisplay.substr(prefixLen));
                }
                state.rollbackDisplay = std::move(newDisplay);
                if (!state.rollbackRawBuffer.empty()) {
                    state.rollbackRawBuffer.pop_back();
                }
                if (state.rollbackDisplay.empty()) {
                    state.rollbackRawBuffer.clear();
                }
                if (debug) {
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
            return false;
        }

        if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter) ||
            key.check(FcitxKey_ISO_Enter)) {
            state.macroBuffer.clear();
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            state.noSeedNextWord = false;
            return false;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        if (!(uni >= 0x20 && uni <= 0x7E)) {
            state.macroBuffer.clear();
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            state.noSeedNextWord = false;
            return false;
        }
        const char c = static_cast<char>(uni);

        if (isComposingASCII(c) && !state.rollbackDisplay.empty()) {
            const auto &st = ic->surroundingText();
            if (!st.isValid() || st.cursor() != st.anchor() ||
                !fcitx::utf8::validate(st.text()) ||
                st.cursor() > fcitx::utf8::length(st.text())) {
                state.rollbackWord.clear();
                state.rollbackDisplay.clear();
                state.rollbackRawBuffer.clear();
                return false;
            }
            WordSegment seg;
            if (!extractWordBeforeCursor(st.text(), st.cursor(), seg) ||
                seg.word != state.rollbackDisplay) {
                if (debug) {
                    FCITX_INFO() << "openkey: st desync program=" << state.program
                                 << " expected=" << state.rollbackDisplay
                                 << " actual=" << seg.word;
                }
                state.rollbackWord.clear();
                state.rollbackDisplay.clear();
                state.rollbackRawBuffer.clear();
                return false;
            }
        }

        auto replaceRollbackDisplay = [&](const std::string &replacement) {
            const unsigned int deleteChars = utf8CharCount(state.rollbackDisplay);
            if (deleteChars == 0 || deleteChars > 128) {
                return false;
            }
            ic->deleteSurroundingText(-static_cast<int>(deleteChars), deleteChars);
            ic->commitString(replacement);
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
            if (!deps_.restoreIfWrongSpelling ||
                !deps_.restoreIfWrongSpelling() ||
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

        if (c == ' ' || c == '\t') {
            if (!expandMacroBeforeBoundary(c)) {
                restoreBeforeBoundary(c);
            }
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
            state.noSeedNextWord = true;
            return false;
        }

        if (state.noSeedNextWord) {
            state.noSeedNextWord = false;
        } else if (state.rollbackWord.empty()) {
            auto &st = ic->surroundingText();
            if (st.isValid() && st.cursor() == st.anchor()) {
                WordSegment seg;
                if (extractWordBeforeCursor(st.text(), st.cursor(), seg)) {
                    state.rollbackWord = seg.word;
                    state.rollbackDisplay = seg.word;
                    state.rollbackRawBuffer.clear();
                    if (debug) {
                        FCITX_INFO() << "openkey: st seed program=" << state.program
                                     << " st.cursor=" << st.cursor()
                                     << " st.textLen=" << fcitx::utf8::length(st.text())
                                     << " seed=" << seg.word;
                    }
                }
            }
        }

        auto r = deps_.adapter->processAsciiKey(state.rollbackWord, c);
        if (!r.handled) {
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            return false;
        }
        if (!fcitx::utf8::validate(r.newWord)) {
            if (debug) {
                FCITX_WARN() << "openkey: invalid utf8 from adapter program="
                             << state.program;
            }
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            return false;
        }

        const std::size_t prefixLen =
            commonPrefixBytesUTF8Boundary(state.rollbackDisplay, r.newWord);
        const unsigned int deleteChars =
            utf8CharCount(state.rollbackDisplay.substr(prefixLen));
        if (deleteChars > 128) {
            if (debug) {
                FCITX_WARN() << "openkey: deleteChars too large program="
                             << state.program << " deleteChars=" << deleteChars
                             << " rollbackDisplay=" << state.rollbackDisplay
                             << " newWord=" << r.newWord;
            }
            state.rollbackWord.clear();
            state.rollbackDisplay.clear();
            state.rollbackRawBuffer.clear();
            return false;
        }
        if (deleteChars > 0) {
            ic->deleteSurroundingText(-static_cast<int>(deleteChars), deleteChars);
        }
        if (r.newWord.size() > prefixLen) {
            ic->commitString(r.newWord.substr(prefixLen));
        }
        if (debug) {
            FCITX_INFO() << "openkey: st apply program=" << state.program
                         << " deleteChars=" << deleteChars
                         << " commitDelta=" << r.newWord.substr(prefixLen)
                         << " newWord=" << r.newWord;
        }

        state.rollbackWord = r.newWord;
        state.rollbackDisplay = r.newWord;
        state.rollbackRawBuffer.push_back(c);
        state.lastCommitted = state.rollbackDisplay;
        event.filterAndAccept();
        return true;
    }

    void reset(OpenKeyState &state) override {
        state.macroBuffer.clear();
        state.rollbackWord.clear();
        state.rollbackDisplay.clear();
        state.rollbackRawBuffer.clear();
        state.noSeedNextWord = false;
    }

private:
    SimpleModeHandlerDeps deps_;
};

class BackspaceRewriteModeHandler final : public InputModeHandler {
public:
    explicit BackspaceRewriteModeHandler(DeltaModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
               OpenKeyState &state) override {
        auto key = event.rawKey();
        auto normKey = event.key().normalize();

        auto isBackspace = [&]() {
            return key.check(FcitxKey_BackSpace) || normKey.check(FcitxKey_BackSpace);
        };

        if (event.isRelease()) {
            return false;
        }

        if (hasCtrlAltSuperMeta(key)) {
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

                deltaState.ackTimeoutTimer.reset();
                event.filterAndAccept();

                const DeltaRewriteTiming timing = deltaTimingFor(ic, state.program);
                if (isXimFrontend(ic)) {
                    scheduleFinishPendingBackspaceCommit(ic, state, timing.commitDelayUsec);
                } else {
                    finishPendingBackspaceCommit(ic, state, timing.commitDelayUsec);
                }
                return true;
            }

            deltaState.queuedKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (deltaState.rewriteLock) {
            deltaState.queuedKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (isBackspace()) {
            if (!restoreBackspaceSnapshot(ic, deltaState)) {
                clearWordState(deltaState);
            }
            return false;
        }

        if (key.check(FcitxKey_Shift_L) || key.check(FcitxKey_Shift_R) ||
            normKey.check(FcitxKey_Shift_L) || normKey.check(FcitxKey_Shift_R)) {
            return false;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());

        if (!(uni >= 0x20 && uni <= 0x7E)) {
            clearWordState(deltaState);
            return false;
        }

        if (uni >= 0x20 && uni <= 0x7E) {
            deltaState.queuedKeys.push_back(key);
            event.filterAndAccept();
            pumpQueue(ic, state, adapterShared, debug);
            return true;
        }

        if (key.isCursorMove() || normKey.isCursorMove() ||
            key.check(FcitxKey_Delete) || normKey.check(FcitxKey_Delete) ||
            key.check(FcitxKey_Escape) || normKey.check(FcitxKey_Escape)) {
            clearWordState(deltaState);
        }

        return false;
    }

private:
    OpenKeyState *stateFor(fcitx::InputContext *ic) const {
        if (!ic || !deps_.factory) {
            return nullptr;
        }
        return ic->propertyFor(deps_.factory);
    }

    bool backspaceSnapshotEnabled() const {
        return !deps_.enableBackspaceSnapshot ||
               deps_.enableBackspaceSnapshot();
    }

    void clearBackspaceSnapshot(DeltaRewriteState &deltaState) const {
        const bool hadSnapshot =
            deltaState.canReseedFromBackspaceSnapshot ||
            !deltaState.backspaceSnapshotShownText.empty() ||
            !deltaState.backspaceSnapshotRawAsciiBuffer.empty() ||
            deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace ||
            deltaState.backspaceSnapshotUsesSurrounding;
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
                         << " usesSurrounding="
                         << deltaState.backspaceSnapshotUsesSurrounding
                         << " preserveAfterBoundaryBackspace="
                         << deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace;
        }
        deltaState.backspaceSnapshotShownText.clear();
        deltaState.backspaceSnapshotRawAsciiBuffer.clear();
        deltaState.backspaceSnapshotHasRewrittenCurrentWord = false;
        deltaState.canReseedFromBackspaceSnapshot = false;
        deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace = false;
        deltaState.backspaceSnapshotUsesSurrounding = false;
        deltaState.allowBackspaceSnapshotResetPreserve = false;
    }

    void rememberBackspaceSnapshot(fcitx::InputContext *ic,
                                   DeltaRewriteState &deltaState) const {
        if (!backspaceSnapshotEnabled()) {
            clearBackspaceSnapshot(deltaState);
            return;
        }
        const bool useSurrounding =
            ic && ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText);
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
                                 << deltaState.backspaceSnapshotRawAsciiBuffer
                                 << " usesSurrounding="
                                 << deltaState.backspaceSnapshotUsesSurrounding;
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
        deltaState.backspaceSnapshotUsesSurrounding = useSurrounding;
        deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
        if (deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot remember"
                         << " mode=backspace"
                         << " shown=" << deltaState.shownText
                         << " raw=" << deltaState.rawAsciiBuffer
                         << " rewritten="
                         << deltaState.hasRewrittenCurrentWord
                         << " usesSurrounding="
                         << deltaState.backspaceSnapshotUsesSurrounding;
        }
    }

    bool restoreBackspaceSnapshot(fcitx::InputContext *ic,
                                  DeltaRewriteState &deltaState) const {
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
                             << deltaState.canReseedFromBackspaceSnapshot
                             << " usesSurrounding="
                             << deltaState.backspaceSnapshotUsesSurrounding;
            }
            return false;
        }
        const auto probe = probeBackspaceSnapshotWithSurrounding(
            ic, deltaState.backspaceSnapshotShownText);
        if (probe == SnapshotBackspaceProbe::Wait) {
            deltaState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
            deltaState.allowBackspaceSnapshotResetPreserve = true;
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot wait-boundary"
                             << " mode=backspace"
                             << " probe=surrounding"
                             << " snapshotShown="
                             << deltaState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << deltaState.backspaceSnapshotRawAsciiBuffer
                             << " usesSurrounding="
                             << deltaState.backspaceSnapshotUsesSurrounding;
            }
            return true;
        }
        if (probe == SnapshotBackspaceProbe::Stale) {
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot restore-miss"
                             << " mode=backspace"
                             << " probe=surrounding-stale"
                             << " snapshotShown="
                             << deltaState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << deltaState.backspaceSnapshotRawAsciiBuffer;
            }
            return false;
        }
        if (deltaState.backspaceSnapshotUsesSurrounding &&
            probe == SnapshotBackspaceProbe::Unavailable) {
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot restore-miss"
                             << " mode=backspace"
                             << " probe=surrounding-unavailable"
                             << " snapshotShown="
                             << deltaState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << deltaState.backspaceSnapshotRawAsciiBuffer;
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
                         << " probe="
                         << (probe == SnapshotBackspaceProbe::Restore
                                 ? "surrounding"
                                 : "fallback")
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
        deltaState.rewriteLock = false;
        deltaState.waitingBackspaceAck = false;
        deltaState.processingQueue = false;
        deltaState.expectedBackspaces = 0;
        deltaState.seenBackspaces = 0;
        deltaState.queuedKeys.clear();
        deltaState.commitTimer.reset();
        deltaState.ackTimeoutTimer.reset();
        deltaState.pendingConvertedText.clear();
        deltaState.pendingShownTextAfterCommit.clear();
        if (clearSnapshot) {
            clearBackspaceSnapshot(deltaState);
        }
    }

    void scheduleFinishPendingBackspaceCommit(fcitx::InputContext *ic,
                                          OpenKeyState &state,
                                          uint64_t commitDelayUsec) {
        auto &deltaState = state.delta;

        if (!deps_.instance || commitDelayUsec == 0) {
            finishPendingBackspaceCommit(ic, state, 0);
            return;
        }

        const auto icRef = ic->watch();
        const std::weak_ptr<void> lifetimeWeak = deps_.lifetimeWeak;
        auto *loop = &deps_.instance->eventLoop();

        deltaState.ackTimeoutTimer.reset();

        const uint64_t deadline =
            fcitx::now(CLOCK_MONOTONIC) + commitDelayUsec;

        deltaState.ackTimeoutTimer = loop->addTimeEvent(
            CLOCK_MONOTONIC, deadline, 0,
            [this, icRef, lifetimeWeak]
            (fcitx::EventSourceTime *, uint64_t) {
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

                finishPendingBackspaceCommit(ic2, *st, 0);
                return false;
            });

        if (deltaState.ackTimeoutTimer) {
            deltaState.ackTimeoutTimer->setOneShot();
        } else {
            finishPendingBackspaceCommit(ic, state, 0);
        }
    }

    void finishPendingBackspaceCommit(fcitx::InputContext *ic,
                                      OpenKeyState &state,
                                      uint64_t commitDelayUsec) {
        auto &deltaState = state.delta;
        const std::string commitText = std::move(deltaState.pendingConvertedText);
        const std::string shownAfter =
            std::move(deltaState.pendingShownTextAfterCommit);
        deltaState.commitTimer.reset();
        deltaState.pendingConvertedText.clear();
        deltaState.pendingShownTextAfterCommit.clear();

        if (!commitText.empty()) {
            const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
            if (debug) {
                FCITX_INFO() << "openkey: bs-delta commit-delay"
                             << " program=" << state.program
                             << " delay=" << commitDelayUsec;
            }
            if (commitDelayUsec > 0) {
                ::usleep(static_cast<useconds_t>(std::min<uint64_t>(commitDelayUsec, 1000000)));
            }
            ic->commitString(commitText);
        }
        deltaState.shownText = shownAfter;
        deltaState.waitingBackspaceAck = false;
        deltaState.expectedBackspaces = 0;
        deltaState.seenBackspaces = 0;
        deltaState.ackTimeoutTimer.reset();
        if (deltaState.shownText.empty()) {
            deltaState.rawAsciiBuffer.clear();
            deltaState.hasRewrittenCurrentWord = false;
        }

        if (!commitText.empty() && schedulePostCommitPump(ic, state)) {
            return;
        }

        finishPostCommitPump(ic, state);
    }

    bool schedulePostCommitPump(fcitx::InputContext *ic, OpenKeyState &state) {
        auto &deltaState = state.delta;
        if (!deps_.instance || kDeltaPostCommitPumpDelayUsec == 0) {
            return false;
        }

        const auto icRef = ic->watch();
        const std::weak_ptr<void> lifetimeWeak = deps_.lifetimeWeak;
        auto *loop = &deps_.instance->eventLoop();
        const uint64_t deadline =
            fcitx::now(CLOCK_MONOTONIC) + kDeltaPostCommitPumpDelayUsec;

        deltaState.commitTimer = loop->addTimeEvent(
            CLOCK_MONOTONIC, deadline, 0,
            [this, icRef, lifetimeWeak](fcitx::EventSourceTime *, uint64_t) {
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

                auto _timer = std::move(st->delta.commitTimer);
                finishPostCommitPump(ic2, *st);
                return false;
            });

        if (!deltaState.commitTimer) {
            return false;
        }

        deltaState.commitTimer->setOneShot();
        return true;
    }

    void finishPostCommitPump(fcitx::InputContext *ic, OpenKeyState &state) {
        auto &deltaState = state.delta;
        deltaState.commitTimer.reset();
        deltaState.rewriteLock = false;
        pumpQueue(ic, state, deps_.adapter,
                  deps_.debugEnabled ? deps_.debugEnabled() : false);
    }

    void scheduleAckTimeout(fcitx::InputContext *ic, OpenKeyState &state,
                            const DeltaRewriteTiming &timing, int deleteCount) {
        auto *loop = deps_.instance ? &deps_.instance->eventLoop() : nullptr;
        auto &deltaState = state.delta;
        deltaState.ackTimeoutTimer.reset();
        if (!loop) {
            return;
        }

        const auto icRef = ic->watch();
        const std::weak_ptr<void> lifetimeWeak = deps_.lifetimeWeak;
        // Calculate dynamic ACK timeout: (deleteCount - 1) × interKeyUsec + buffer
        // Note: commitDelayUsec is applied separately AFTER receiving ACKs
        const uint64_t injectTime = deleteCount > 1 ? (deleteCount - 1) * timing.interKeyUsec : 0;
        const uint64_t buffer = 50000; // 50ms buffer for app to process backspaces
        const uint64_t dynamicTimeout = injectTime + buffer;
        const uint64_t deadline =
            fcitx::now(CLOCK_MONOTONIC) + dynamicTimeout;
        
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        if (debug) {
            FCITX_INFO() << "openkey: bs-delta ack-timeout scheduled"
                         << " program=" << state.program
                         << " deletes=" << deleteCount
                         << " injectTime=" << injectTime
                         << " buffer=" << buffer
                         << " timeout=" << dynamicTimeout;
        }
        
        deltaState.ackTimeoutTimer = loop->addTimeEvent(
            CLOCK_MONOTONIC, deadline, 0,
            [this, icRef, lifetimeWeak](fcitx::EventSourceTime *, uint64_t) {
                if (lifetimeWeak.expired()) {
                    return false;
                }
                auto *ic2 = icRef.get();
                if (!ic2) {
                    return false;
                }
                auto *st = stateFor(ic2);
                if (!st || !st->delta.waitingBackspaceAck) {
                    return false;
                }
                auto _timer = std::move(st->delta.ackTimeoutTimer);
                FCITX_INFO() << "openkey: bs-delta ack timeout"
                             << " program=" << st->program
                             << " seen=" << st->delta.seenBackspaces
                             << " expected=" << st->delta.expectedBackspaces;
                const DeltaRewriteTiming timing = deltaTimingFor(ic2, st->program);
                if (isXimFrontend(ic2)) {
                    scheduleFinishPendingBackspaceCommit(ic2, *st, timing.commitDelayUsec);
                } else {
                    finishPendingBackspaceCommit(ic2, *st, timing.commitDelayUsec);
                }
                return false;
            });
        if (deltaState.ackTimeoutTimer) {
            deltaState.ackTimeoutTimer->setOneShot();
        }
    }

    bool applyWordDelta(fcitx::InputContext *ic, OpenKeyState &state,
                        bool debug, const std::string &newWord, char asciiChar,
                        const char *reason,
                        bool compareWithRawAppend = true) {
        auto &deltaState = state.delta;
        if (!deps_.backspaceInjector) {
            return false;
        }
        if (!fcitx::utf8::validate(deltaState.shownText) ||
            !fcitx::utf8::validate(newWord)) {
            clearWordState(deltaState);
            return false;
        }

        const std::string oldShown = deltaState.shownText;
        const std::string rawAppend =
            compareWithRawAppend ? oldShown + asciiChar : oldShown;
        const DeltaRewriteTiming timing = deltaTimingFor(ic, state.program);
        const std::size_t prefixLen =
            commonPrefixBytesUTF8Boundary(deltaState.shownText, newWord);
        unsigned int deleteCount =
            utf8CharCount(deltaState.shownText.substr(prefixLen));
        std::string commitText = newWord.substr(prefixLen);
        const bool browserAutocomplete =
            deleteCount > 0 &&
            isBrowserLikeProgram(state.program) &&
            !deltaState.hasRewrittenCurrentWord &&
            looksLikeBrowserAutocomplete(ic, deltaState.shownText);
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
                ic->commitString(commitText);
            }
            deltaState.shownText = newWord;
            deltaState.hasRewrittenCurrentWord =
                deltaState.hasRewrittenCurrentWord || (newWord != rawAppend);
            deltaState.restoredFromBackspaceSnapshot = false;
            deltaState.allowBackspaceSnapshotResetPreserve = false;
            return true;
        }

        const std::string programForInjector = state.program;
        const auto method = deps_.backspaceInjector->sendBackspaces(
            ic, programForInjector, static_cast<int>(deleteCount), debug,
            timing.interKeyUsec);

        if (method == BackspaceInjector::Method::Uinput) {
            // One extra backspace acts as a sentinel: app receives the first N,
            // OpenKey consumes the last one and commits when it arrives.
            deps_.backspaceInjector->sendBackspaces(
                ic, programForInjector, 1, debug, timing.interKeyUsec);
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
            scheduleAckTimeout(ic, state, timing, static_cast<int>(deleteCount));
            return true;
        }

        clearWordState(deltaState);
        return false;
    }

    bool maybeExpandMacroBeforeBoundary(fcitx::InputContext *ic,
                                        OpenKeyState &state,
                                        const fcitx::Key &boundaryKey,
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

        if (!applyWordDelta(ic, state, debug, replacement, trigger, "macro",
                            false)) {
            return false;
        }

        if (deltaState.hasPendingRewrite()) {
            deltaState.queuedKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(ic, deltaState);
            ic->forwardKey(boundaryKey);
            clearWordState(deltaState, false);
        }
        return true;
    }

    bool maybeRestoreBeforeBoundary(fcitx::InputContext *ic,
                                    OpenKeyState &state,
                                    const fcitx::Key &boundaryKey,
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

        if (!applyWordDelta(ic, state, debug, restoredWord, trigger,
                            "restore-boundary", false)) {
            return false;
        }

        if (deltaState.hasPendingRewrite()) {
            deltaState.queuedKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(ic, deltaState);
            ic->forwardKey(boundaryKey);
            clearWordState(deltaState, false);
        }
        return true;
    }

    bool processQueuedKey(fcitx::InputContext *ic, OpenKeyState &state,
                          const fcitx::Key &key,
                          std::shared_ptr<OpenKeyAdapter> adapterShared,
                          bool debug) {
        auto &deltaState = state.delta;

        if (key.isCursorMove() || key.check(FcitxKey_Delete)) {
            clearWordState(deltaState);
            return false;
        }

        if (key.check(FcitxKey_Escape)) {
            clearWordState(deltaState);
            return false;
        }

        if (key.check(FcitxKey_BackSpace)) {
            if (!restoreBackspaceSnapshot(ic, deltaState)) {
                clearWordState(deltaState);
            }
            return false; // let app handle physical Backspace
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            // Word boundaries: clear composition state, forward to app
            if (isBoundaryASCII(c) || key.check(FcitxKey_Return) ||
                key.check(FcitxKey_KP_Enter) || key.check(FcitxKey_ISO_Enter) ||
                key.check(FcitxKey_Tab)) {
                if (maybeExpandMacroBeforeBoundary(ic, state, key, adapterShared,
                                                   debug, c)) {
                    return true;
                }
                if (maybeRestoreBeforeBoundary(ic, state, key, adapterShared,
                                               debug, c)) {
                    return true;
                }
                rememberBackspaceSnapshot(ic, deltaState);
                clearWordState(deltaState, false);
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
            return applyWordDelta(ic, state, debug, r.newWord, c, "ascii");
        }

        clearWordState(deltaState);
        return false;
    }

    void pumpQueue(fcitx::InputContext *ic, OpenKeyState &state,
                   std::shared_ptr<OpenKeyAdapter> adapterShared, bool debug) {
        auto &deltaState = state.delta;
        if (deltaState.processingQueue || deltaState.rewriteLock) {
            return;
        }
        deltaState.processingQueue = true;
        while (!deltaState.rewriteLock && !deltaState.queuedKeys.empty()) {
            const fcitx::Key key = deltaState.queuedKeys.front();
            deltaState.queuedKeys.pop_front();
            const bool handled =
                processQueuedKey(ic, state, key, adapterShared, debug);
            if (!handled) {
                ic->forwardKey(key);
            }
        }
        deltaState.processingQueue = false;
    }

    DeltaModeDeps deps_;
};

class NonPreeditBackspaceRewriteModeHandler final : public InputModeHandler {
public:
    explicit NonPreeditBackspaceRewriteModeHandler(DeltaModeDeps deps)
        : deps_(std::move(deps)) {}

    bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
               OpenKeyState &state) override {
        auto key = event.rawKey();
        auto normKey = event.key().normalize();

        auto isBackspace = [&]() {
            return key.check(FcitxKey_BackSpace) || normKey.check(FcitxKey_BackSpace);
        };

        if (event.isRelease()) {
            return false;
        }

        if (hasCtrlAltSuperMeta(key)) {
            return false;
        }

        const auto adapterShared = deps_.adapter;
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        auto &nonPreeditState = state.nonPreeditDelta;

        state.delta.clear();

        if (nonPreeditState.rewriteLock) {
            if (isBackspace()) {
                return false;
            }

            nonPreeditState.nonPreeditKeys.push_back(key);
            event.filterAndAccept();
            return true;
        }

        if (isBackspace()) {
            if (!restoreBackspaceSnapshot(ic, nonPreeditState)) {
                clearComposeState(nonPreeditState, "backspace");
            }
            return false;
        }

        if (key.check(FcitxKey_Shift_L) || key.check(FcitxKey_Shift_R) ||
            normKey.check(FcitxKey_Shift_L) || normKey.check(FcitxKey_Shift_R)) {
            return false;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(key.sym());

        if (!(uni >= 0x20 && uni <= 0x7E)) {
            clearComposeState(nonPreeditState, "non-printable-boundary");
            return false;
        }

        if (uni >= 0x20 && uni <= 0x7E) {
            nonPreeditState.nonPreeditKeys.push_back(key);
            event.filterAndAccept();
            pumpNonPreedit(ic, state, adapterShared, debug);
            return true;
        }

        if (key.isCursorMove() || normKey.isCursorMove() ||
            key.check(FcitxKey_Delete) || normKey.check(FcitxKey_Delete) ||
            key.check(FcitxKey_Escape) || normKey.check(FcitxKey_Escape)) {
            clearComposeState(nonPreeditState, "cursor-delete");
        }

        return false;
    }

    void handleRemoteBackspaceAction(fcitx::InputContext *ic,
                                     OpenKeyState &state) {
        if (!deps_.backspaceInjector) {
            return;
        }
        const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
        deps_.backspaceInjector->sendBackspaces(
            ic, state.program, 1, debug, 0);
    }

    void handleRemoteCommitAction(fcitx::InputContext *ic, OpenKeyState &state,
                                  uint64_t txId) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (nonPreeditState.remotePendingTxId != txId) {
            return;
        }
        finishPendingBackspaceCommit(ic, state);
    }

private:
    OpenKeyState *stateFor(fcitx::InputContext *ic) const {
        if (!ic || !deps_.factory) {
            return nullptr;
        }
        return ic->propertyFor(deps_.factory);
    }

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
        nonPreeditState.backspaceSnapshotUsesSurrounding = false;
        nonPreeditState.allowBackspaceSnapshotResetPreserve = false;
    }

    void rememberBackspaceSnapshot(
        fcitx::InputContext *ic,
        NonPreeditDeltaRewriteState &nonPreeditState) const {
        if (!backspaceSnapshotEnabled()) {
            clearBackspaceSnapshot(nonPreeditState);
            return;
        }
        const bool useSurrounding =
            ic && ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText);
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
                                 << nonPreeditState.backspaceSnapshotRawAsciiBuffer
                                 << " usesSurrounding="
                                 << nonPreeditState.backspaceSnapshotUsesSurrounding;
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
        nonPreeditState.backspaceSnapshotUsesSurrounding = useSurrounding;
        nonPreeditState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
        if (deps_.debugEnabled && deps_.debugEnabled()) {
            FCITX_INFO() << "openkey: bs-snapshot remember"
                         << " mode=nonPreedit"
                         << " shown=" << nonPreeditState.shownText
                         << " raw=" << nonPreeditState.rawAsciiBuffer
                         << " rewritten="
                         << nonPreeditState.hasRewrittenCurrentWord
                         << " usesSurrounding="
                         << nonPreeditState.backspaceSnapshotUsesSurrounding;
        }
    }

    bool restoreBackspaceSnapshot(
        fcitx::InputContext *ic,
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
                             << nonPreeditState.canReseedFromBackspaceSnapshot
                             << " usesSurrounding="
                             << nonPreeditState.backspaceSnapshotUsesSurrounding;
            }
            return false;
        }
        const auto probe = probeBackspaceSnapshotWithSurrounding(
            ic, nonPreeditState.backspaceSnapshotShownText);
        if (probe == SnapshotBackspaceProbe::Wait) {
            nonPreeditState.preserveBackspaceSnapshotAfterBoundaryBackspace =
                true;
            nonPreeditState.allowBackspaceSnapshotResetPreserve = true;
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot wait-boundary"
                             << " mode=nonPreedit"
                             << " probe=surrounding"
                             << " snapshotShown="
                             << nonPreeditState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << nonPreeditState.backspaceSnapshotRawAsciiBuffer;
            }
            return true;
        }
        if (probe == SnapshotBackspaceProbe::Stale) {
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot restore-miss"
                             << " mode=nonPreedit"
                             << " probe=surrounding-stale"
                             << " snapshotShown="
                             << nonPreeditState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << nonPreeditState.backspaceSnapshotRawAsciiBuffer;
            }
            return false;
        }
        if (nonPreeditState.backspaceSnapshotUsesSurrounding &&
            probe == SnapshotBackspaceProbe::Unavailable) {
            if (deps_.debugEnabled && deps_.debugEnabled()) {
                FCITX_INFO() << "openkey: bs-snapshot restore-miss"
                             << " mode=nonPreedit"
                             << " probe=surrounding-unavailable"
                             << " snapshotShown="
                             << nonPreeditState.backspaceSnapshotShownText
                             << " snapshotRaw="
                             << nonPreeditState.backspaceSnapshotRawAsciiBuffer;
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
                         << " probe="
                         << (probe == SnapshotBackspaceProbe::Restore
                                 ? "surrounding"
                                 : "fallback")
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
        nonPreeditState.rewriteLock = false;
        nonPreeditState.waitingBackspaceAck = false;
        nonPreeditState.processingNonPreedit = false;
        nonPreeditState.expectedBackspaces = 0;
        nonPreeditState.seenBackspaces = 0;
        nonPreeditState.lateBackspaceBudget = 0;
        nonPreeditState.nonPreeditKeys.clear();
        nonPreeditState.commitTimer.reset();
        nonPreeditState.lateBackspaceTimeoutTimer.reset();
        nonPreeditState.ackTimeoutTimer.reset();
        nonPreeditState.pendingConvertedText.clear();
        nonPreeditState.pendingShownTextAfterCommit.clear();
        nonPreeditState.remotePendingTxId = 0;
        nonPreeditState.remoteRewritePending = false;
        if (clearSnapshot) {
            clearBackspaceSnapshot(nonPreeditState);
        }
    }

    void finishPendingBackspaceCommit(fcitx::InputContext *ic,
                                      OpenKeyState &state) {
        auto &nonPreeditState = state.nonPreeditDelta;
        const std::string commitText = std::move(nonPreeditState.pendingConvertedText);
        const std::string shownAfter =
            std::move(nonPreeditState.pendingShownTextAfterCommit);
        nonPreeditState.commitTimer.reset();
        nonPreeditState.pendingConvertedText.clear();
        nonPreeditState.pendingShownTextAfterCommit.clear();

        if (!commitText.empty()) {
            ic->commitString(commitText);
        }
        nonPreeditState.shownText = shownAfter;
        nonPreeditState.waitingBackspaceAck = false;
        nonPreeditState.expectedBackspaces = 0;
        nonPreeditState.seenBackspaces = 0;
        nonPreeditState.ackTimeoutTimer.reset();
        nonPreeditState.remotePendingTxId = 0;
        nonPreeditState.remoteRewritePending = false;
        if (nonPreeditState.shownText.empty()) {
            nonPreeditState.rawAsciiBuffer.clear();
            nonPreeditState.hasRewrittenCurrentWord = false;
        }

        if (!commitText.empty() && schedulePostCommitPump(ic, state)) {
            return;
        }

        finishPostCommitPump(ic, state);
    }

    bool schedulePostCommitPump(fcitx::InputContext *ic, OpenKeyState &state) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (!deps_.instance || kNonPreeditPostCommitPumpDelayUsec == 0) {
            return false;
        }

        const auto icRef = ic->watch();
        const std::weak_ptr<void> lifetimeWeak = deps_.lifetimeWeak;
        auto *loop = &deps_.instance->eventLoop();
        const uint64_t deadline =
            fcitx::now(CLOCK_MONOTONIC) + kNonPreeditPostCommitPumpDelayUsec;

        nonPreeditState.commitTimer = loop->addTimeEvent(
            CLOCK_MONOTONIC, deadline, 0,
            [this, icRef, lifetimeWeak](fcitx::EventSourceTime *, uint64_t) {
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

                auto _timer = std::move(st->nonPreeditDelta.commitTimer);
                finishPostCommitPump(ic2, *st);
                return false;
            });

        if (!nonPreeditState.commitTimer) {
            return false;
        }

        nonPreeditState.commitTimer->setOneShot();
        return true;
    }

    void finishPostCommitPump(fcitx::InputContext *ic, OpenKeyState &state) {
        auto &nonPreeditState = state.nonPreeditDelta;
        nonPreeditState.commitTimer.reset();
        nonPreeditState.rewriteLock = false;
        pumpNonPreedit(ic, state, deps_.adapter,
                  deps_.debugEnabled ? deps_.debugEnabled() : false);
    }

    bool applyWordDelta(fcitx::InputContext *ic, OpenKeyState &state,
                        bool debug, const std::string &newWord, char asciiChar,
                        const char *reason,
                        bool compareWithRawAppend = true) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (!deps_.backspaceInjector) {
            return false;
        }
        if (!fcitx::utf8::validate(nonPreeditState.shownText) ||
            !fcitx::utf8::validate(newWord)) {
            clearComposeState(nonPreeditState, "invalid-utf8");
            return false;
        }

        const std::string oldShown = nonPreeditState.shownText;
        const std::string rawAppend =
            compareWithRawAppend ? oldShown + asciiChar : oldShown;
        const RewriteTiming timing = nonPreeditTimingFor(ic, state.program);
        const std::size_t prefixLen =
            commonPrefixBytesUTF8Boundary(nonPreeditState.shownText, newWord);
        unsigned int deleteCount =
            utf8CharCount(nonPreeditState.shownText.substr(prefixLen));
        std::string commitText = newWord.substr(prefixLen);
        const bool browserAutocomplete =
            deleteCount > 0 &&
            isBrowserLikeProgram(state.program) &&
            !nonPreeditState.hasRewrittenCurrentWord &&
            looksLikeBrowserAutocomplete(ic, nonPreeditState.shownText);
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
                ic->commitString(commitText);
            }
            nonPreeditState.shownText = newWord;
            nonPreeditState.hasRewrittenCurrentWord =
                nonPreeditState.hasRewrittenCurrentWord || (newWord != rawAppend);
            nonPreeditState.restoredFromBackspaceSnapshot = false;
            nonPreeditState.allowBackspaceSnapshotResetPreserve = false;
            return true;
        }

        if (deps_.nonPreeditRemoteEnabled && deps_.nonPreeditRemoteEnabled() &&
            deps_.nonPreeditRemoteSchedule) {
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
            if (deps_.nonPreeditRemoteSchedule(
                    ic, state, deleteCount, timing.interKeyUsec,
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

    bool maybeExpandMacroBeforeBoundary(fcitx::InputContext *ic,
                                        OpenKeyState &state,
                                        const fcitx::Key &boundaryKey,
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

        if (!applyWordDelta(ic, state, debug, replacement, trigger, "macro",
                            false)) {
            return false;
        }

        if (hasPendingRewrite(nonPreeditState)) {
            nonPreeditState.nonPreeditKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(ic, nonPreeditState);
            ic->forwardKey(boundaryKey);
            clearComposeState(nonPreeditState, "macro-boundary", false);
        }
        return true;
    }

    bool maybeRestoreBeforeBoundary(fcitx::InputContext *ic,
                                    OpenKeyState &state,
                                    const fcitx::Key &boundaryKey,
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

        if (!applyWordDelta(ic, state, debug, restoredWord, trigger,
                            "restore-boundary", false)) {
            return false;
        }

        if (hasPendingRewrite(nonPreeditState)) {
            nonPreeditState.nonPreeditKeys.push_front(boundaryKey);
        } else {
            rememberBackspaceSnapshot(ic, nonPreeditState);
            ic->forwardKey(boundaryKey);
            clearComposeState(nonPreeditState, "restore-boundary", false);
        }
        return true;
    }

    bool processNonPreeditKey(fcitx::InputContext *ic, OpenKeyState &state,
                          const fcitx::Key &nonPreeditKey,
                          std::shared_ptr<OpenKeyAdapter> adapterShared,
                          bool debug) {
        auto &nonPreeditState = state.nonPreeditDelta;
        const RewriteTiming timing = nonPreeditTimingFor(ic, state.program);
        if (hasCtrlAltSuperMeta(nonPreeditKey)) {
           clearComposeState(nonPreeditState, "ctrl-alt-super");
            return false;
        }

        if (nonPreeditKey.isCursorMove() || nonPreeditKey.check(FcitxKey_Delete)) {
           clearComposeState(nonPreeditState, "cursor-delete");
            return false;
        }

        if (nonPreeditKey.check(FcitxKey_Escape)) {
           clearComposeState(nonPreeditState, "escape");
            return false;
        }

        if (nonPreeditKey.check(FcitxKey_BackSpace)) {
            if (restoreBackspaceSnapshot(ic, nonPreeditState)) {
                return false;
            }
            if (nonPreeditState.shownText.empty()) {
                return false;
            }
            if (!nonPreeditState.hasRewrittenCurrentWord) {
                clearComposeState(nonPreeditState, "backspace-empty-or-not-rewritten");           
                return false;
            }
            const auto method = deps_.backspaceInjector->sendBackspaces(
                ic, state.program, 1, debug, timing.interKeyUsec);
            if (method != BackspaceInjector::Method::Uinput) {
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
            return true;
        }

        const uint32_t uni = fcitx::Key::keySymToUnicode(nonPreeditKey.sym());
        if (uni >= 0x20 && uni <= 0x7E) {
            const char c = static_cast<char>(uni);

            // Word boundaries: clear composition state, forward to app
            if (isBoundaryASCII(c) || nonPreeditKey.check(FcitxKey_Return) ||
                nonPreeditKey.check(FcitxKey_KP_Enter) ||
                nonPreeditKey.check(FcitxKey_ISO_Enter) ||
                nonPreeditKey.check(FcitxKey_Tab)) {
                if (maybeExpandMacroBeforeBoundary(ic, state, nonPreeditKey,
                                                   adapterShared, debug, c)) {
                    return true;
                }
                if (maybeRestoreBeforeBoundary(ic, state, nonPreeditKey,
                                               adapterShared, debug, c)) {
                    return true;
                }
                rememberBackspaceSnapshot(ic, nonPreeditState);
                clearComposeState(nonPreeditState, "boundary", false);
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
            return applyWordDelta(ic, state, debug, r.newWord, c, "ascii");
        }

       clearComposeState(nonPreeditState, "non-ascii-key");
        return false;
    }

    void pumpNonPreedit(fcitx::InputContext *ic, OpenKeyState &state,
                   std::shared_ptr<OpenKeyAdapter> adapterShared, bool debug) {
        auto &nonPreeditState = state.nonPreeditDelta;
        if (nonPreeditState.processingNonPreedit || nonPreeditState.rewriteLock) {
            return;
        }
        nonPreeditState.processingNonPreedit = true;
        while (!nonPreeditState.rewriteLock && !nonPreeditState.nonPreeditKeys.empty()) {
            const fcitx::Key nonPreeditKey = nonPreeditState.nonPreeditKeys.front();
            nonPreeditState.nonPreeditKeys.pop_front();
            const bool handled =
                processNonPreeditKey(ic, state, nonPreeditKey, adapterShared, debug);
            if (!handled) {
                ic->forwardKey(nonPreeditKey);
            }
        }
        nonPreeditState.processingNonPreedit = false;
    }

    DeltaModeDeps deps_;
};

} // namespace

OpenKeyEngine::OpenKeyEngine(fcitx::Instance *instance)
    : instance_(instance), adapter_(std::make_shared<OpenKeyAdapter>()) {
    lifetime_ = std::make_shared<int>(1);
    instance_->inputContextManager().registerProperty("openkeyState", &factory_);
    remoteNonPreeditCoordinator_ = std::make_unique<RemoteNonPreeditCoordinator>(
        instance_ ? &instance_->eventLoop() : nullptr,
        [this](fcitx::InputContext *ic, uint64_t sessionId, uint64_t txId) {
            handleRemoteNonPreeditDone(ic, sessionId, txId);
        },
        [this]() { return debugEnabled(); });
    focusedAppBridge_ = std::make_unique<FocusedAppBridge>(
        instance_ ? &instance_->eventLoop() : nullptr,
        [this]() { return debugEnabled(); });
    DeltaModeDeps deltaDeps;
    deltaDeps.instance = instance_;
    deltaDeps.factory = &factory_;
    deltaDeps.adapter = adapter_;
    deltaDeps.backspaceInjector = &g_backspaceInjector;
    deltaDeps.lifetimeWeak = lifetime_;
    deltaDeps.debugEnabled = [this]() { return debugEnabled(); };
    deltaDeps.enableMacro = [this]() { return config_.enableMacro.value(); };
    deltaDeps.restoreIfWrongSpelling = [this]() {
        return config_.restoreIfWrongSpelling.value();
    };
    deltaDeps.enableBackspaceSnapshot = [this]() {
        return config_.enableBackspaceSnapshot.value();
    };
    backspaceRewriteHandler_ =
        std::make_unique<BackspaceRewriteModeHandler>(std::move(deltaDeps));
    DeltaModeDeps nonPreeditDeltaDeps;
    nonPreeditDeltaDeps.instance = instance_;
    nonPreeditDeltaDeps.factory = &factory_;
    nonPreeditDeltaDeps.adapter = adapter_;
    nonPreeditDeltaDeps.backspaceInjector = &g_backspaceInjector;
    nonPreeditDeltaDeps.lifetimeWeak = lifetime_;
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
    nonPreeditDeltaDeps.nonPreeditRemoteSchedule =
        [this](fcitx::InputContext *ic, OpenKeyState &state,
               unsigned int deleteCount, uint64_t interBackspaceUsec,
               uint64_t commitDelayUsec) {
            return scheduleRemoteNonPreeditRewrite(ic, state, deleteCount,
                                              interBackspaceUsec,
                                              commitDelayUsec);
        };
    nonPreeditBackspaceRewriteHandler_ =
        std::make_unique<NonPreeditBackspaceRewriteModeHandler>(
            std::move(nonPreeditDeltaDeps));

    SimpleModeHandlerDeps simpleDeps;
    simpleDeps.adapter = adapter_;
    simpleDeps.debugEnabled = [this]() { return debugEnabled(); };
    simpleDeps.enableMacro = [this]() { return config_.enableMacro.value(); };
    simpleDeps.restoreIfWrongSpelling = [this]() {
        return config_.restoreIfWrongSpelling.value();
    };
    preeditHandler_ = std::make_unique<PreeditModeHandler>(simpleDeps);
    surroundingTextHandler_ = std::make_unique<SurroundingTextModeHandler>(std::move(simpleDeps));
    reloadConfig();
    if (remoteNonPreeditCoordinator_) {
        remoteNonPreeditCoordinator_->ensureAvailableOrStartOnce();
    }

    // Warm up uinput ngay khi load để tránh delay lần đầu gõ
    g_backspaceInjector.uinputAvailable(debugEnabled());
}

OpenKeyEngine::~OpenKeyEngine() {
    focusedAppBridge_.reset();
    remoteNonPreeditCoordinator_.reset();
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

std::string OpenKeyEngine::subMode(const fcitx::InputMethodEntry &,
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

bool OpenKeyEngine::debugEnabled() const {
    if (config_.debug.value()) {
        return true;
    }
    const char *env = std::getenv("FCITX_OPENKEY_DEBUG");
    return env && env[0] && env[0] != '0';
}

bool OpenKeyEngine::nonPreeditServerAvailable() {
    return remoteNonPreeditCoordinator_ && remoteNonPreeditCoordinator_->available();
}

void OpenKeyEngine::loadAppModes() {
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

void OpenKeyEngine::persistAppModes() {
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

void OpenKeyEngine::setAppModeForProgram(fcitx::InputContext *ic,
                                         const std::string &program,
                                         RuntimeMode mode) {
    const auto normalized = normalizedProgramName(program);
    if (normalized.empty()) {
        return;
    }
    appModeMapFor(ic)[normalized] = mode;
}

std::unordered_map<std::string, RuntimeMode> &OpenKeyEngine::appModeMapFor(
    fcitx::InputContext *ic) {
    return isRunningOnX11(ic) ? x11AppModeMap_ : waylandAppModeMap_;
}

OpenKeyState *OpenKeyEngine::stateFor(fcitx::InputContext *ic) {
    return ic->propertyFor(&factory_);
}

bool OpenKeyEngine::scheduleRemoteNonPreeditRewrite(
    fcitx::InputContext *ic, OpenKeyState &state, unsigned int deleteCount,
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

void OpenKeyEngine::handleRemoteNonPreeditDone(fcitx::InputContext *ic,
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

    auto *nonPreeditHandler =
        dynamic_cast<NonPreeditBackspaceRewriteModeHandler *>(
            nonPreeditBackspaceRewriteHandler_.get());
    if (!nonPreeditHandler) {
        return;
    }
    nonPreeditHandler->handleRemoteCommitAction(ic, *state, txId);
}

const fcitx::Configuration *OpenKeyEngine::getConfig() const { return &config_; }

void OpenKeyEngine::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    applyConfig();
}

void OpenKeyEngine::reloadConfig() {
    fcitx::readAsIni(config_, fcitx::StandardPath::Type::PkgConfig,
                     "conf/openkey.conf");
    fcitx::readAsIni(macroTables_, fcitx::StandardPath::Type::PkgConfig,
                     "conf/openkey-macro-table.conf");
    loadAppModes();
    applyConfig();
}

void OpenKeyEngine::applyConfig() {
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

void OpenKeyEngine::persistConfig() {
    fcitx::safeSaveAsIni(config_, fcitx::StandardPath::Type::PkgConfig,
                         "conf/openkey.conf");
    fcitx::safeSaveAsIni(macroTables_, fcitx::StandardPath::Type::PkgConfig,
                         "conf/openkey-macro-table.conf");
}

void OpenKeyEngine::save() { persistConfig(); }

const fcitx::Configuration *
OpenKeyEngine::getSubConfig(const std::string &path) const {
    if (path == "openkey-macro") {
        return &macroTables_;
    }
    return nullptr;
}

void OpenKeyEngine::setSubConfig(const std::string &path,
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

void OpenKeyEngine::activate(const fcitx::InputMethodEntry &,
                             fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    state->delta.clear();
    state->nonPreeditDelta.clear();
    state->composing.clear();
    state->preeditKeyBuffer.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    state->rollbackRawBuffer.clear();
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

    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
}

void OpenKeyEngine::deactivate(const fcitx::InputMethodEntry &entry,
                               fcitx::InputContextEvent &event) {
    auto *state = stateFor(event.inputContext());
    state->delta.clear();
    state->nonPreeditDelta.clear();
    reset(entry, event);
}

void OpenKeyEngine::reset(const fcitx::InputMethodEntry &,
                          fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = stateFor(ic);
    const bool hasSurrounding =
        ic && ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText);
    const bool snapshotEnabled = config_.enableBackspaceSnapshot.value();
    const bool preserveDelta =
        snapshotEnabled &&
        state->delta.restoredFromBackspaceSnapshot &&
        !state->delta.shownText.empty() &&
        (state->delta.allowBackspaceSnapshotResetPreserve || hasSurrounding) &&
        currentSurroundingAllowsSnapshotPreserve(
            ic, state->delta.shownText, hasSurrounding);
    const bool preserveDeltaSnapshot =
        snapshotEnabled &&
        !preserveDelta &&
        (state->delta.allowBackspaceSnapshotResetPreserve ||
         state->delta.backspaceSnapshotUsesSurrounding) &&
        state->delta.preserveBackspaceSnapshotAfterBoundaryBackspace &&
        state->delta.canReseedFromBackspaceSnapshot &&
        !state->delta.backspaceSnapshotShownText.empty() &&
        currentSurroundingAllowsSnapshotPreserve(
            ic, state->delta.backspaceSnapshotShownText,
            state->delta.backspaceSnapshotUsesSurrounding);
    const std::string deltaShown = state->delta.shownText;
    const std::string deltaRaw = state->delta.rawAsciiBuffer;
    const bool deltaRewritten = state->delta.hasRewrittenCurrentWord;
    const std::string deltaSnapshotShown =
        state->delta.backspaceSnapshotShownText;
    const std::string deltaSnapshotRaw =
        state->delta.backspaceSnapshotRawAsciiBuffer;
    const bool deltaSnapshotRewritten =
        state->delta.backspaceSnapshotHasRewrittenCurrentWord;
    const bool deltaSnapshotUsesSurrounding =
        state->delta.backspaceSnapshotUsesSurrounding;
    const bool preserveNonPreedit =
        snapshotEnabled &&
        state->nonPreeditDelta.restoredFromBackspaceSnapshot &&
        !state->nonPreeditDelta.shownText.empty() &&
        (state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve ||
         hasSurrounding) &&
        currentSurroundingAllowsSnapshotPreserve(
            ic, state->nonPreeditDelta.shownText, hasSurrounding);
    const bool preserveNonPreeditSnapshot =
        snapshotEnabled &&
        !preserveNonPreedit &&
        (state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve ||
         state->nonPreeditDelta.backspaceSnapshotUsesSurrounding) &&
        state->nonPreeditDelta.preserveBackspaceSnapshotAfterBoundaryBackspace &&
        state->nonPreeditDelta.canReseedFromBackspaceSnapshot &&
        !state->nonPreeditDelta.backspaceSnapshotShownText.empty() &&
        currentSurroundingAllowsSnapshotPreserve(
            ic, state->nonPreeditDelta.backspaceSnapshotShownText,
            state->nonPreeditDelta.backspaceSnapshotUsesSurrounding);
    const std::string nonPreeditShown = state->nonPreeditDelta.shownText;
    const std::string nonPreeditRaw = state->nonPreeditDelta.rawAsciiBuffer;
    const bool nonPreeditRewritten =
        state->nonPreeditDelta.hasRewrittenCurrentWord;
    const std::string nonPreeditSnapshotShown =
        state->nonPreeditDelta.backspaceSnapshotShownText;
    const std::string nonPreeditSnapshotRaw =
        state->nonPreeditDelta.backspaceSnapshotRawAsciiBuffer;
    const bool nonPreeditSnapshotRewritten =
        state->nonPreeditDelta.backspaceSnapshotHasRewrittenCurrentWord;
    const bool nonPreeditSnapshotUsesSurrounding =
        state->nonPreeditDelta.backspaceSnapshotUsesSurrounding;
    if (debugEnabled()) {
        FCITX_INFO() << "openkey: reset"
                     << " program=" << state->program
                     << " snapshotEnabled=" << snapshotEnabled
                     << " preserveDelta=" << preserveDelta
                     << " preserveDeltaSnapshot=" << preserveDeltaSnapshot
                     << " deltaAllowPreserve="
                     << state->delta.allowBackspaceSnapshotResetPreserve
                     << " deltaShown=" << state->delta.shownText
                     << " deltaRaw=" << state->delta.rawAsciiBuffer
                     << " preserveNonPreedit=" << preserveNonPreedit
                     << " preserveNonPreeditSnapshot="
                     << preserveNonPreeditSnapshot
                     << " nonPreeditAllowPreserve="
                     << state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve
                     << " nonPreeditShown="
                     << state->nonPreeditDelta.shownText
                     << " nonPreeditRaw="
                     << state->nonPreeditDelta.rawAsciiBuffer;
    }
    state->delta.clear();
    state->nonPreeditDelta.clear();
    if (preserveDelta) {
        state->delta.shownText = deltaShown;
        state->delta.rawAsciiBuffer = deltaRaw;
        state->delta.hasRewrittenCurrentWord = deltaRewritten;
        state->delta.restoredFromBackspaceSnapshot = true;
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
        state->delta.backspaceSnapshotUsesSurrounding =
            deltaSnapshotUsesSurrounding;
        state->delta.allowBackspaceSnapshotResetPreserve = false;
        if (debugEnabled()) {
            FCITX_INFO() << "openkey: reset preserve-snapshot"
                         << " mode=backspace"
                         << " snapshotShown="
                         << state->delta.backspaceSnapshotShownText
                         << " snapshotRaw="
                         << state->delta.backspaceSnapshotRawAsciiBuffer
                         << " usesSurrounding="
                         << state->delta.backspaceSnapshotUsesSurrounding;
        }
    }
    if (preserveNonPreedit) {
        state->nonPreeditDelta.shownText = nonPreeditShown;
        state->nonPreeditDelta.rawAsciiBuffer = nonPreeditRaw;
        state->nonPreeditDelta.hasRewrittenCurrentWord = nonPreeditRewritten;
        state->nonPreeditDelta.restoredFromBackspaceSnapshot = true;
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
        state->nonPreeditDelta.backspaceSnapshotUsesSurrounding =
            nonPreeditSnapshotUsesSurrounding;
        state->nonPreeditDelta.allowBackspaceSnapshotResetPreserve = false;
        if (debugEnabled()) {
            FCITX_INFO() << "openkey: reset preserve-snapshot"
                         << " mode=nonPreedit"
                         << " snapshotShown="
                         << state->nonPreeditDelta.backspaceSnapshotShownText
                         << " snapshotRaw="
                         << state->nonPreeditDelta.backspaceSnapshotRawAsciiBuffer
                         << " usesSurrounding="
                         << state->nonPreeditDelta.backspaceSnapshotUsesSurrounding;
        }
    }
    state->composing.clear();
    state->preeditKeyBuffer.clear();
    state->macroBuffer.clear();
    state->rollbackWord.clear();
    state->rollbackDisplay.clear();
    state->rollbackRawBuffer.clear();
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
}

RuntimeMode OpenKeyEngine::decideMode(fcitx::InputContext *ic,
                                         OpenKeyState &s,
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

    const auto mode = nonPreeditServerAvailable() ? RuntimeMode::NonPreeditBackspaceRewrite: RuntimeMode::BackspaceRewriteDelta;
    if (writeBack && !normalizedProgram.empty()) {
        appModeMap[normalizedProgram] = mode;
        persistAppModes();
    }
    return mode;
}

RuntimeMode OpenKeyEngine::firstManualMode() const {
    return RuntimeMode::NonPreeditBackspaceRewrite;
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
        state->mode = decideMode(ic, *state, false);
        state->autoMode = state->mode;
        state->modeDecided = true;
    }

    if (key.checkKeyList(config_.switchModeKey.value()) && key.sym() != FcitxKey_None) {
            auto clearComposingState = [this, ic, state]() {
                state->delta.clear();
                state->nonPreeditDelta.clear();
                state->composing.clear();
                state->preeditKeyBuffer.clear();
                state->macroBuffer.clear();
                state->rollbackWord.clear();
                state->rollbackDisplay.clear();
                state->rollbackRawBuffer.clear();
                state->noSeedNextWord = false;
                ic->inputPanel().reset();
                ic->updatePreedit();
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
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
            backspaceRewriteHandler_->handleKey(ic, event, *state);
        }
        return;
    case RuntimeMode::NonPreeditBackspaceRewrite:
        if (nonPreeditBackspaceRewriteHandler_) {
            nonPreeditBackspaceRewriteHandler_->handleKey(ic, event, *state);
        }
        return;
    case RuntimeMode::Preedit:
        adapter_->setCodeTable(state->codeTable);
        if (preeditHandler_) {
            preeditHandler_->handleKey(ic, event, *state);
        }
        return;
    case RuntimeMode::SurroundingText:
        adapter_->setCodeTable(state->codeTable);
        if (surroundingTextHandler_) {
            surroundingTextHandler_->handleKey(ic, event, *state);
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
