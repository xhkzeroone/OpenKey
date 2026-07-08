#include "openkey.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iterator>
#include <mutex>
#include <signal.h>
#include <sstream>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

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

#ifdef __linux__
constexpr int kRewriteServerNiceValue = -10;

static bool rewriteServerPriorityEnabled() {
  const char *env = std::getenv("OPENKEY_NONPREEDIT_SERVER_PRIORITY");
  if (!env || !*env) {
    return true;
  }
  return std::strcmp(env, "0") != 0 && strcasecmp(env, "false") != 0 &&
         strcasecmp(env, "no") != 0 && strcasecmp(env, "off") != 0;
}
#endif

class RemoteRewriteCoordinator {
public:
  using DoneCallback =
      std::function<void(fcitx::InputContext *, uint64_t, uint64_t)>;

  RemoteRewriteCoordinator(fcitx::EventLoop *loop, DoneCallback onDone,
                           std::function<bool()> debugEnabled)
      : onDone_(std::move(onDone)), debugEnabled_(std::move(debugEnabled)) {
    if (loop) {
      dispatcher_.attach(loop);
    }
    const char *env = std::getenv("OPENKEY_NONPREEDIT_SERVER_SOCK");
    socketPath_ = (env && *env) ? env : "/tmp/openkey-nonpreedit.sock";
    const char *serverEnv = std::getenv("OPENKEY_NONPREEDIT_SERVER_BIN");
    serverCommand_ =
        (serverEnv && *serverEnv) ? serverEnv : OPENKEY_NONPREEDIT_SERVER_PATH;
  }

  ~RemoteRewriteCoordinator() {
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
    return sendLineLocked(
        "PLAN " + std::to_string(sessionId) + " " + std::to_string(txId) + " " +
        std::to_string(backspaces) + " " + std::to_string(interKeyUsec) + " " +
        std::to_string(commitDelayUsec) + "\n");
  }

  bool scheduleWait(uint64_t sessionId, uint64_t txId, uint64_t delayUsec) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (!ensureConnectedLocked(false)) {
      return false;
    }
    if (debugEnabled_ && debugEnabled_()) {
      std::string msg = "schedule WAIT session=" + std::to_string(sessionId) +
                        " tx=" + std::to_string(txId) +
                        " delay=" + std::to_string(delayUsec);
      maybeLog(msg.c_str());
    }
    return sendLineLocked("WAIT " + std::to_string(sessionId) + " " +
                          std::to_string(txId) + " " +
                          std::to_string(delayUsec) + "\n");
  }

  bool cancelDone(uint64_t sessionId, uint64_t txId) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (!ensureConnectedLocked(false)) {
      return false;
    }
    return sendLineLocked("CANCELDONE " + std::to_string(sessionId) + " " +
                          std::to_string(txId) + "\n");
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

    sockaddr_un addr{};
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
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
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
      FCITX_INFO() << "openkey: remote-rewrite starting server"
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
      if (rewriteServerPriorityEnabled()) {
        (void)setpriority(PRIO_PROCESS, 0, kRewriteServerNiceValue);
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
      FCITX_INFO() << "openkey: remote-rewrite " << reason
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

static size_t utf8ByteOffsetForCharIndex(const std::string &s,
                                         size_t charIndex) {
  const auto len = fcitx::utf8::length(s);
  if (charIndex >= len) {
    return s.size();
  }
  auto it = fcitx::utf8::nextNChar(s.begin(), charIndex);
  return static_cast<size_t>(std::distance(s.begin(), it));
}

static size_t utf8CharIndexForByteOffset(const std::string &s,
                                         size_t byteOffset) {
  byteOffset = std::min(byteOffset, s.size());
  return fcitx::utf8::length(std::string(s.begin(), s.begin() + byteOffset));
}

static bool hasCtrlAltSuperMeta(const fcitx::Key &key) {
  const auto states = key.states();
  return states.test(fcitx::KeyState::Ctrl) ||
         states.test(fcitx::KeyState::Alt) ||
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
  // Keep the persisted token stable for existing per-app mode files.
  switch (mode) {
  case RuntimeMode::Auto:
    return "auto";
  case RuntimeMode::Preedit:
    return "preedit";
  case RuntimeMode::BackspaceRewrite:
    return "nonPreedit";
  case RuntimeMode::DirectCommit:
    return "direct";
  }
  return "auto";
}

static bool runtimeModeFromString(const std::string &mode, RuntimeMode &out) {
  // Accept the legacy persisted token for backward compatibility.
  if (equalsASCIIInsensitive(mode, "auto")) {
    out = RuntimeMode::Auto;
    return true;
  }
  if (equalsASCIIInsensitive(mode, "preedit")) {
    out = RuntimeMode::Preedit;
    return true;
  }
  if (equalsASCIIInsensitive(mode, "nonPreedit")) {
    out = RuntimeMode::BackspaceRewrite;
    return true;
  }
  if (equalsASCIIInsensitive(mode, "direct")) {
    out = RuntimeMode::DirectCommit;
    return true;
  }
  return false;
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
  case CodeTable::UnicodeCompound:
    return 3;
  case CodeTable::VietnameseLocaleCP1258:
    return 4;
  }
  return 0;
}

struct RewriteTiming {
  uint64_t interKeyUsec = 2000;
  uint64_t commitDelayUsec = 60000;
};

static constexpr RewriteTiming kBackspaceRewriteWaylandTiming{1000, 40000};
static constexpr RewriteTiming kBackspaceRewriteWaylandFirefoxFamilyTiming{
    1000, 40000};
static constexpr RewriteTiming kBackspaceRewriteX11Timing{1000, 80000};
static constexpr RewriteTiming kBackspaceRewriteX11BrowserTiming{1000, 80000};
static constexpr RewriteTiming kBackspaceRewriteX11FirefoxFamilyTiming{1000,
                                                                       80000};
static constexpr uint64_t kBackspaceRewritePostCommitPumpDelayUsec = 10000;

static bool isRunningOnX11() {

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

static std::string getCombinedProgramInfo(const OpenKeyState &state) {
  return normalizedProgramName(state.program) + " " +
         asciiLower(state.windowTitle);
}

static bool isBrowserLikeProgram(const OpenKeyState &state);

static bool isMetaAppOrWeb(const OpenKeyState &state) {
  if (state.program.empty() && state.windowTitle.empty()) {
    return true;
  }
  // Nếu là trình duyệt nhưng không có windowTitle (do tắt extension),
  // ta không thể biết web gì, nên mặc định trả về true để dùng delay an toàn (40ms/30ms)
  if (state.windowTitle.empty() && isBrowserLikeProgram(state)) {
    return true;
  }
  const std::string base = asciiLower(getCombinedProgramInfo(state));
  static const std::vector<std::string> kMetaPatterns = {
      "messenger", "facebook", "instagram", "whatsapp", "threads",
  };
  for (const auto &pattern : kMetaPatterns) {
    if (base.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

static bool isFirefoxLikeProgram(const OpenKeyState &state) {
  if (state.program.empty() && state.windowTitle.empty()) {
    return false;
  }

  const std::string base = asciiLower(getCombinedProgramInfo(state));
  static const std::vector<std::string> kFirefoxPatterns = {
      "firefox", "librewolf",   "waterfox", "floorp",
      "zen",     "tor-browser", "mullvad",  "icecat",
  };

  for (const auto &pattern : kFirefoxPatterns) {
    if (base.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

static bool needsTransientResetPreserve(const OpenKeyState &state) {
  return isFirefoxLikeProgram(state);
}

static bool isBrowserLikeProgram(const OpenKeyState &state) {
  if (state.program.empty() && state.windowTitle.empty()) {
    return true; // Assume browser-like if program name is unknown.
  }

  const std::string base = asciiLower(getCombinedProgramInfo(state));
  std::vector<std::string> kBrowserPatterns = {
      // Chromium
      "chrome",
      "google-chrome",
      "chromium",
      "chromium-browser",
      "edge",
      "msedge",
      "brave",
      "vivaldi",
      "opera",
      "opera-beta",
      "opera-developer",
      "coccoc",
      "yandex",

      // Firefox family
      "firefox",
      "librewolf",
      "waterfox",
      "floorp",
      "zen",
      "tor-browser",
      "torbrowser",

      // WebKit / Qt
      "epiphany",
      "falkon",
      "midori",
      "qutebrowser",

      // Other browsers
      "palemoon",
      "basilisk",
      "nyxt",
      "otter",
      "dooble",
      "arc",
      "helium",
      "mullvad",
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

  if (cursor > textLen || anchor > textLen || shownLen == 0 ||
      shownLen > textLen) {
    return false;
  }

  unsigned int prefixCursor = cursor;
  if (cursor != anchor) {
    const unsigned int selectionStart = std::min(cursor, anchor);
    const unsigned int selectionEnd = std::max(cursor, anchor);
    if (selectionEnd == cursor) {
      prefixCursor = selectionStart;
    }
  }

  const size_t rangeStart =
      prefixCursor >= shownLen ? prefixCursor - shownLen : 0;

  bool samePrefix = false;
  for (size_t pb = text.find(shownText); pb != std::string::npos;
       pb = text.find(shownText, pb + 1)) {
    const size_t pbChar = utf8CharIndexForByteOffset(text, pb);
    if (pbChar >= rangeStart && pbChar + shownLen == prefixCursor) {
      samePrefix = true;
      break;
    }
  }

  if (!samePrefix) {
    return false;
  }

  auto hasNewlineBetween = [&](size_t from, size_t to) {
    if (from > to) {
      std::swap(from, to);
    }
    const size_t fromByte = utf8ByteOffsetForCharIndex(text, from);
    const size_t toByte = utf8ByteOffsetForCharIndex(text, to);
    const size_t p = text.find('\n', fromByte);
    return p != std::string::npos && p < toByte;
  };

  // Case 1: omnibox/autocomplete thường select phần phía sau cursor tới cuối
  // dòng.
  if (cursor != anchor) {
    unsigned int selectionStart = std::min(cursor, anchor);
    unsigned int selectionEnd = std::max(cursor, anchor);

    bool selectionTouchesCursor =
        selectionStart == cursor || selectionEnd == cursor ||
        (selectionStart < cursor && selectionEnd > cursor);

    const size_t selectionStartByte =
        utf8ByteOffsetForCharIndex(text, selectionStart);
    const size_t nextLineBreak = text.find('\n', selectionStartByte);
    const size_t lineEnd =
        nextLineBreak == std::string::npos
            ? textLen
            : utf8CharIndexForByteOffset(text, nextLineBreak);
    const bool selectionGoesToLineEnd =
        static_cast<size_t>(selectionEnd) == lineEnd;

    return selectionTouchesCursor && selectionGoesToLineEnd &&
           !hasNewlineBetween(selectionStart, selectionEnd);
  }

  // Case 2: không selection nhưng sau cursor có text tự mọc thêm.
  // Giống browser search/address autocomplete.
  if (cursor < textLen) {
    const size_t cursorByte = utf8ByteOffsetForCharIndex(text, cursor);
    if (text.find('\n', cursorByte) != std::string::npos) {
      return false;
    }

    // Sau cursor còn ít nhất 2 ký tự thì mới coi là autocomplete,
    // tránh nhầm khi user sửa giữa từ thường.
    return textLen >= static_cast<size_t>(cursor) + 2;
  }

  return false;
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

static RewriteTiming backspaceRewriteTimingFor(bool x11,
                                               const OpenKeyState &state) {

  if (x11 && isFirefoxLikeProgram(state)) {
    return kBackspaceRewriteX11FirefoxFamilyTiming;
  }

  if (!x11 && isFirefoxLikeProgram(state)) {
    return kBackspaceRewriteWaylandFirefoxFamilyTiming;
  }

  if (x11 && isBrowserLikeProgram(state)) {
    return kBackspaceRewriteX11BrowserTiming;
  }

  if (x11) {
    return kBackspaceRewriteX11Timing;
  }
  return kBackspaceRewriteWaylandTiming;
}

class BackspaceInjector {
public:
  ~BackspaceInjector() { destroyUinput(); }

  enum class Method {
    Uinput,
    None,
  };

  Method sendBackspaces(fcitx::InputContext *ic, const std::string &program,
                        int count, bool debug,
                        uint64_t uinputInterKeyUsec = 1500) {
    if (count <= 0) {
      return Method::None;
    }
    if (!ensureUinput(debug)) {
      if (debug) {
        FCITX_INFO() << "openkey: backspace method=uinput-none"
                     << " program=" << program << " frontend="
                     << (ic && ic->frontend() ? ic->frontend() : "")
                     << " count=" << count << " reason=uinput-unavailable";
      }
      return Method::None;
    }
    if (debug) {
      FCITX_INFO() << "openkey: backspace method=uinput"
                   << " program=" << program << " frontend="
                   << (ic && ic->frontend() ? ic->frontend() : "")
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

    struct uinput_user_dev uidev{};
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
    struct input_event ev{};
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

static void forwardKeyPressAndRelease(fcitx::InputContext *ic,
                                      const fcitx::Key &key) {
  if (!ic) {
    return;
  }
  ic->forwardKey(key);
  ic->forwardKey(key, true);
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

} // namespace

namespace {
constexpr const char *kBridgeBusName = "org.openkey.Bridge";
constexpr const char *kBridgeObjectPath = "/org/openkey/Bridge";
constexpr const char *kBridgeInterface = "org.openkey.Bridge1";
} // namespace

class FocusedAppBridge {
public:
  FocusedAppBridge(fcitx::EventLoop *loop, std::function<bool()> debugEnabled)
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

  // Returns GNOME Shell focused app id (usually "*.desktop") and window title.
  std::pair<std::string, std::string> focusedAppInfo() { return query(); }

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

struct BackspaceRewriteDeps {
  fcitx::Instance *instance = nullptr;
  fcitx::SimpleInputContextPropertyFactory<OpenKeyState> *factory = nullptr;
  std::shared_ptr<OpenKeyAdapter> adapter;
  BackspaceInjector *backspaceInjector = nullptr;
  std::weak_ptr<void> lifetimeWeak;
  std::function<bool()> debugEnabled;
  std::function<bool()> enableMacro;
  std::function<bool()> restoreIfWrongSpelling;
  std::function<bool()> enableBackspaceSnapshot;
  std::function<bool()> rawBackspaceRewriteEnabled;
  std::function<bool()> remoteEnabled;
  std::function<bool(fcitx::InputContext *, OpenKeyState &, unsigned int,
                     uint64_t, uint64_t)>
      remoteSchedule;
  std::function<bool(OpenKeyState &, uint64_t)> remoteScheduleWait;
  std::function<bool(uint64_t, uint64_t)> remoteCancelDone;
};

struct SimpleModeHandlerDeps {
  std::shared_ptr<OpenKeyAdapter> adapter;
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
      if (!deps_.restoreIfWrongSpelling || !deps_.restoreIfWrongSpelling() ||
          state.composing.empty()) {
        return false;
      }
      std::string restoredWord;
      const bool restored = !state.preeditKeyBuffer.empty()
                                ? deps_.adapter->restoreFromRawAsciiOnWordBreak(
                                      state.composing, state.preeditKeyBuffer,
                                      trigger, restoredWord)
                                : deps_.adapter->restoreOnWordBreak(
                                      state.composing, trigger, restoredWord);
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
    text.append(state.composing);
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
    state.composing.clear();
    state.preeditKeyBuffer.clear();
    updatePreeditUI(ic, state);
  }

  SimpleModeHandlerDeps deps_;
};

class BackspaceRewriteModeHandler final : public InputModeHandler {
public:
  explicit BackspaceRewriteModeHandler(BackspaceRewriteDeps deps)
      : deps_(std::move(deps)) {}

  bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                 OpenKeyState &state) override {
    auto key = event.rawKey();
    auto normKey = event.key().normalize();
    const auto adapterShared = deps_.adapter;
    const bool debug = deps_.debugEnabled ? deps_.debugEnabled() : false;
    auto &rewriteState = state.rewriteState;

    auto isBackspace = [&]() {
      return key.check(FcitxKey_BackSpace) || normKey.check(FcitxKey_BackSpace);
    };

    if (event.isRelease()) {
      if (isBackspace() && rewriteState.rawBackspaceAwaitingRelease) {
        rewriteState.rawBackspaceAwaitingRelease = false;
        if (!deps_.remoteEnabled || !deps_.remoteEnabled() ||
            !deps_.remoteSchedule) {
          return false;
        }
        adapterShared->setCodeTable(state.codeTable);
        const std::string converted =
            adapterShared->convertRawBuffer(rewriteState.rawAsciiBuffer);
        applyWordDelta(ic, state, debug, converted, 0, "raw-backspace", false);
        return false; // let the release go to the app
      }
      return false;
    }

    if (hasCtrlAltSuperMeta(key)) {
      return false;
    }

    if (rewriteState.rewriteLock) {
      if (isBackspace()) {
        if (rewriteState.waitingBackspaceAck) {
          rewriteState.seenBackspaces++;
          if (rewriteState.seenBackspaces < rewriteState.expectedBackspaces) {
            return false;
          }
          rewriteState.waitingBackspaceAck = false;
          if (debug) {
            FCITX_INFO() << "openkey: ack full, backspaces="
                         << rewriteState.seenBackspaces;
          }
          if (deps_.remoteScheduleWait) {
            // Ứng dụng thuộc họ Meta (Messenger, Facebook, WhatsApp...): X11 =
            // 40ms, Wayland = 30ms
            if (isMetaAppOrWeb(state)) {
              deps_.remoteScheduleWait(state,
                                       state.isX11Environment ? 40000 : 30000);
            } else {
              deps_.remoteScheduleWait(state,
                                       state.isX11Environment ? 20000 : 15000);
            }
          }
          event.filterAndAccept();
          return true;
        }
        return false;
      }

      rewriteState.queuedKeys.push_back(key);
      event.filterAndAccept();
      return true;
    }

    if (isBackspace()) {
      if (needsTransientResetPreserve(state) &&
          !rewriteState.shownText.empty() &&
          !trackedWordStillBeforeCursor(ic, rewriteState.shownText, false)) {
        clearComposeState(rewriteState, "backspace-cursor-mismatch");
        return false;
      }
      if (restoreBackspaceSnapshot(rewriteState)) {
        return true;
      }

      bool rawBackspaceRewrite = deps_.rawBackspaceRewriteEnabled &&
                                 deps_.rawBackspaceRewriteEnabled();
      if (rawBackspaceRewrite && !rewriteState.shownText.empty() &&
          !rewriteState.rawAsciiBuffer.empty()) {
        rewriteState.rawBackspaceAwaitingRelease = true;
        rewriteState.shownText = utf8DropLastN(rewriteState.shownText, 1);
        rewriteState.rawAsciiBuffer.pop_back();
        if (rewriteState.shownText.empty()) {
          rewriteState.rawAsciiBuffer.clear();
          rewriteState.hasRewrittenCurrentWord = false;
        }
        rewriteState.allowTransientResetPreserve =
            !rewriteState.shownText.empty();
        return false;
      }

      // Fallback for non-raw-backspace
      if (!rewriteState.shownText.empty()) {
        rewriteState.shownText = utf8DropLastN(rewriteState.shownText, 1);
        if (!rewriteState.rawAsciiBuffer.empty()) {
          rewriteState.rawAsciiBuffer.pop_back();
        }
        if (rewriteState.shownText.empty()) {
          rewriteState.rawAsciiBuffer.clear();
          rewriteState.hasRewrittenCurrentWord = false;
        }
        rewriteState.allowTransientResetPreserve =
            !rewriteState.shownText.empty();
      }
      return false; // để app tự xóa ký tự trên màn hình
    }

    if (key.check(FcitxKey_Shift_L) || key.check(FcitxKey_Shift_R) ||
        normKey.check(FcitxKey_Shift_L) || normKey.check(FcitxKey_Shift_R)) {
      return false;
    }

    const uint32_t uni = fcitx::Key::keySymToUnicode(normKey.sym());

    if (!(uni >= 0x20 && uni <= 0x7E)) {
      clearComposeState(rewriteState, "non-printable-boundary");
      return false;
    }

    if (uni >= 0x20 && uni <= 0x7E) {
      const char c = static_cast<char>(uni);
      if (!isComposingASCII(c)) {
        const bool handled =
            processQueuedKey(ic, state, key, adapterShared, debug);
        if (handled) {
          event.filterAndAccept();
        }
        return handled;
      }

      rewriteState.queuedKeys.push_back(key);
      event.filterAndAccept();
      pumpQueue(ic, state, adapterShared, debug);
      return true;
    }

    if (key.isCursorMove() || normKey.isCursorMove() ||
        key.check(FcitxKey_Delete) || normKey.check(FcitxKey_Delete) ||
        key.check(FcitxKey_Escape) || normKey.check(FcitxKey_Escape)) {
      clearComposeState(rewriteState, "cursor-delete");
    }

    return false;
  }

  void handleRemoteCommitAction(fcitx::InputContext *ic, OpenKeyState &state,
                                uint64_t txId) {
    auto &rewriteState = state.rewriteState;
    if (rewriteState.remotePendingTxId != txId) {
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
    return !deps_.enableBackspaceSnapshot || deps_.enableBackspaceSnapshot();
  }

  void clearBackspaceSnapshot(BackspaceRewriteState &rewriteState) const {
    const bool hadSnapshot =
        rewriteState.canReseedFromBackspaceSnapshot ||
        !rewriteState.backspaceSnapshotShownText.empty() ||
        !rewriteState.backspaceSnapshotRawAsciiBuffer.empty() ||
        rewriteState.preserveBackspaceSnapshotAfterBoundaryBackspace;
    if (hadSnapshot && deps_.debugEnabled && deps_.debugEnabled()) {
      FCITX_INFO()
          << "openkey: bs-snapshot clear"
          << " mode=backspaceRewrite"
          << " shown=" << rewriteState.shownText
          << " snapshotShown=" << rewriteState.backspaceSnapshotShownText
          << " snapshotRaw=" << rewriteState.backspaceSnapshotRawAsciiBuffer
          << " canReseed=" << rewriteState.canReseedFromBackspaceSnapshot
          << " preserveAfterBoundaryBackspace="
          << rewriteState.preserveBackspaceSnapshotAfterBoundaryBackspace;
    }
    rewriteState.backspaceSnapshotShownText.clear();
    rewriteState.backspaceSnapshotRawAsciiBuffer.clear();
    rewriteState.backspaceSnapshotHasRewrittenCurrentWord = false;
    rewriteState.canReseedFromBackspaceSnapshot = false;
    rewriteState.preserveBackspaceSnapshotAfterBoundaryBackspace = false;
    rewriteState.allowBackspaceSnapshotResetPreserve = false;
  }

  void rememberBackspaceSnapshot(BackspaceRewriteState &rewriteState) const {
    if (!backspaceSnapshotEnabled()) {
      clearBackspaceSnapshot(rewriteState);
      return;
    }
    if (rewriteState.shownText.empty()) {
      clearBackspaceSnapshot(rewriteState);
      return;
    }
    rewriteState.backspaceSnapshotShownText = rewriteState.shownText;
    rewriteState.backspaceSnapshotRawAsciiBuffer = rewriteState.rawAsciiBuffer;
    rewriteState.backspaceSnapshotHasRewrittenCurrentWord =
        rewriteState.hasRewrittenCurrentWord;
    rewriteState.canReseedFromBackspaceSnapshot = true;
    rewriteState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
    if (deps_.debugEnabled && deps_.debugEnabled()) {
      FCITX_INFO() << "openkey: bs-snapshot remember"
                   << " mode=backspaceRewrite"
                   << " shown=" << rewriteState.shownText
                   << " raw=" << rewriteState.rawAsciiBuffer
                   << " rewritten=" << rewriteState.hasRewrittenCurrentWord;
    }
  }

  bool restoreBackspaceSnapshot(BackspaceRewriteState &rewriteState) const {
    if (!backspaceSnapshotEnabled()) {
      clearBackspaceSnapshot(rewriteState);
      return false;
    }
    if (!rewriteState.canReseedFromBackspaceSnapshot ||
        !rewriteState.shownText.empty() ||
        rewriteState.backspaceSnapshotShownText.empty()) {
      if (deps_.debugEnabled && deps_.debugEnabled()) {
        FCITX_INFO() << "openkey: bs-snapshot restore-miss"
                     << " mode=backspaceRewrite"
                     << " shown=" << rewriteState.shownText << " snapshotShown="
                     << rewriteState.backspaceSnapshotShownText
                     << " snapshotRaw="
                     << rewriteState.backspaceSnapshotRawAsciiBuffer
                     << " canReseed="
                     << rewriteState.canReseedFromBackspaceSnapshot;
      }
      return false;
    }
    rewriteState.shownText = rewriteState.backspaceSnapshotShownText;
    rewriteState.rawAsciiBuffer = rewriteState.backspaceSnapshotRawAsciiBuffer;
    rewriteState.hasRewrittenCurrentWord =
        rewriteState.backspaceSnapshotHasRewrittenCurrentWord;
    rewriteState.restoredFromBackspaceSnapshot = true;
    rewriteState.allowBackspaceSnapshotResetPreserve = true;
    if (deps_.debugEnabled && deps_.debugEnabled()) {
      FCITX_INFO() << "openkey: bs-snapshot restore"
                   << " mode=backspaceRewrite"
                   << " shown=" << rewriteState.shownText
                   << " raw=" << rewriteState.rawAsciiBuffer
                   << " rewritten=" << rewriteState.hasRewrittenCurrentWord;
    }
    clearBackspaceSnapshot(rewriteState);
    rewriteState.allowBackspaceSnapshotResetPreserve = true;
    return true;
  }

  void clearComposeState(BackspaceRewriteState &rewriteState,
                         const char *reason = "unknown",
                         bool clearSnapshot = true) const {

    if (deps_.debugEnabled && deps_.debugEnabled()) {
      FCITX_INFO() << "openkey: backspace-rewrite clear"
                   << " reason=" << reason
                   << " shown=" << rewriteState.shownText
                   << " pending=" << rewriteState.queuedKeys.size()
                   << " rewriteLock=" << rewriteState.rewriteLock
                   << " waitingAck=" << rewriteState.waitingBackspaceAck
                   << " remotePending=" << rewriteState.remoteRewritePending;
    }
    rewriteState.shownText.clear();
    rewriteState.rawAsciiBuffer.clear();
    rewriteState.hasRewrittenCurrentWord = false;
    rewriteState.restoredFromBackspaceSnapshot = false;
    rewriteState.allowBackspaceSnapshotResetPreserve = false;
    rewriteState.allowTransientResetPreserve = false;
    rewriteState.rewriteLock = false;
    rewriteState.waitingBackspaceAck = false;
    rewriteState.processingQueue = false;
    rewriteState.expectedBackspaces = 0;
    rewriteState.seenBackspaces = 0;
    rewriteState.lateBackspaceBudget = 0;
    rewriteState.queuedKeys.clear();
    rewriteState.commitTimer.reset();
    rewriteState.pendingConvertedText.clear();
    rewriteState.pendingShownTextAfterCommit.clear();

    rewriteState.remotePendingTxId = 0;
    rewriteState.remoteRewritePending = false;
    if (clearSnapshot) {
      clearBackspaceSnapshot(rewriteState);
    }
  }

  void finishPendingBackspaceCommit(fcitx::InputContext *ic,
                                    OpenKeyState &state) {
    auto &rewriteState = state.rewriteState;
    const std::string commitText = std::move(rewriteState.pendingConvertedText);
    const std::string shownAfter =
        std::move(rewriteState.pendingShownTextAfterCommit);
    rewriteState.commitTimer.reset();
    rewriteState.pendingConvertedText.clear();
    rewriteState.pendingShownTextAfterCommit.clear();

    if (!commitText.empty()) {
      ic->commitString(commitText);
    }
    rewriteState.shownText = shownAfter;
    rewriteState.allowTransientResetPreserve = !rewriteState.shownText.empty();
    rewriteState.waitingBackspaceAck = false;
    rewriteState.expectedBackspaces = 0;
    rewriteState.seenBackspaces = 0;
    rewriteState.remotePendingTxId = 0;
    rewriteState.remoteRewritePending = false;
    if (rewriteState.shownText.empty()) {
      rewriteState.rawAsciiBuffer.clear();
      rewriteState.hasRewrittenCurrentWord = false;
    }

    if (!commitText.empty() && schedulePostCommitPump(ic, state)) {
      return;
    }

    finishPostCommitPump(ic, state);
  }

  bool schedulePostCommitPump(fcitx::InputContext *ic, OpenKeyState &state) {
    auto &rewriteState = state.rewriteState;
    if (!deps_.instance || kBackspaceRewritePostCommitPumpDelayUsec == 0) {
      return false;
    }

    const auto icRef = ic->watch();
    const std::weak_ptr<void> lifetimeWeak = deps_.lifetimeWeak;
    auto *loop = &deps_.instance->eventLoop();
    const uint64_t deadline =
        fcitx::now(CLOCK_MONOTONIC) + kBackspaceRewritePostCommitPumpDelayUsec;

    rewriteState.commitTimer = loop->addTimeEvent(
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

          auto _timer = std::move(st->rewriteState.commitTimer);
          finishPostCommitPump(ic2, *st);
          return false;
        });

    if (!rewriteState.commitTimer) {
      return false;
    }

    rewriteState.commitTimer->setOneShot();
    return true;
  }

  void finishPostCommitPump(fcitx::InputContext *ic, OpenKeyState &state) {
    auto &rewriteState = state.rewriteState;
    rewriteState.commitTimer.reset();
    rewriteState.rewriteLock = false;
    pumpQueue(ic, state, deps_.adapter,
              deps_.debugEnabled ? deps_.debugEnabled() : false);
  }

  bool applyWordDelta(fcitx::InputContext *ic, OpenKeyState &state, bool debug,
                      const std::string &newWord, char asciiChar,
                      const char *reason, bool compareWithRawAppend = true) {
    auto &rewriteState = state.rewriteState;
    if (!deps_.backspaceInjector) {
      return false;
    }
    if (!fcitx::utf8::validate(rewriteState.shownText) ||
        !fcitx::utf8::validate(newWord)) {
      clearComposeState(rewriteState, "invalid-utf8");
      return false;
    }

    const std::string oldShown = rewriteState.shownText;
    const std::string rawAppend =
        compareWithRawAppend ? oldShown + asciiChar : oldShown;
    const RewriteTiming timing =
        backspaceRewriteTimingFor(state.isX11Environment, state);
    const std::size_t prefixLen =
        commonPrefixBytesUTF8Boundary(rewriteState.shownText, newWord);
    unsigned int deleteCount =
        utf8CharCount(rewriteState.shownText.substr(prefixLen));
    std::string commitText = newWord.substr(prefixLen);

    const std::string expectedPrefixAfterDelete =
        rewriteState.shownText.substr(0, prefixLen);
    const bool browserAutocomplete =
        deleteCount > 0 && isBrowserLikeProgram(state) &&
        !rewriteState.hasRewrittenCurrentWord &&
        looksLikeBrowserAutocomplete(ic, rewriteState.shownText);
    if (browserAutocomplete) {
      deleteCount += 1;
    }
    if (deleteCount > 128) {
      deleteCount = utf8CharCount(rewriteState.shownText);
      commitText = newWord;
    }

    if (debug) {
      FCITX_INFO() << "openkey: backspace-rewrite program=" << state.program
                   << " reason=" << reason << " from=" << rewriteState.shownText
                   << " to=" << newWord << " delete=" << deleteCount
                   << " commit=" << commitText
                   << " inter=" << timing.interKeyUsec
                   << " delay=" << timing.commitDelayUsec
                   << " autocomplete=" << browserAutocomplete
                   << " rewritePending=" << rewriteState.queuedKeys.size();
    }

    if (deleteCount == 0) {
      if (!commitText.empty()) {
        ic->commitString(commitText);
      }
      rewriteState.shownText = newWord;
      rewriteState.hasRewrittenCurrentWord =
          rewriteState.hasRewrittenCurrentWord || (newWord != rawAppend);
      rewriteState.restoredFromBackspaceSnapshot = false;
      rewriteState.allowBackspaceSnapshotResetPreserve = false;
      rewriteState.allowTransientResetPreserve = true;
      return true;
    }

    if (deps_.remoteEnabled && deps_.remoteEnabled() && deps_.remoteSchedule) {
      const uint64_t keysTime = timing.interKeyUsec * (deleteCount + 1);
      const uint64_t serverCommitDelay = timing.commitDelayUsec > keysTime
                                             ? timing.commitDelayUsec - keysTime
                                             : 0;

      rewriteState.rewriteLock = true;
      rewriteState.waitingBackspaceAck = true;
      rewriteState.expectedBackspaces = static_cast<int>(deleteCount) + 1;
      rewriteState.seenBackspaces = 0;
      rewriteState.pendingConvertedText = commitText;
      rewriteState.pendingShownTextAfterCommit = newWord;
      rewriteState.hasRewrittenCurrentWord =
          rewriteState.hasRewrittenCurrentWord || (newWord != rawAppend);
      rewriteState.restoredFromBackspaceSnapshot = false;
      rewriteState.allowBackspaceSnapshotResetPreserve = false;
      rewriteState.allowTransientResetPreserve = true;
      if (deps_.remoteSchedule(ic, state, deleteCount, timing.interKeyUsec,
                               serverCommitDelay)) {
        rewriteState.remoteRewritePending = true;
        return true;
      }
      rewriteState.rewriteLock = false;
      rewriteState.waitingBackspaceAck = false;
      rewriteState.expectedBackspaces = 0;
      rewriteState.pendingConvertedText.clear();
      rewriteState.pendingShownTextAfterCommit.clear();
    }

    clearComposeState(rewriteState, "default");
    return false;
  }

  bool hasPendingRewrite(const BackspaceRewriteState &state) const {
    return state.rewriteLock || state.waitingBackspaceAck ||
           !state.pendingConvertedText.empty() ||
           !state.pendingShownTextAfterCommit.empty() ||
           state.hasRemoteRewritePending();
  }

  bool
  maybeExpandMacroBeforeBoundary(fcitx::InputContext *ic, OpenKeyState &state,
                                 const fcitx::Key &boundaryKey,
                                 std::shared_ptr<OpenKeyAdapter> adapterShared,
                                 bool debug, char trigger) {
    auto &rewriteState = state.rewriteState;
    if (!deps_.enableMacro || !deps_.enableMacro() || !adapterShared ||
        !isMacroTriggerKey(trigger) || rewriteState.shownText.empty()) {
      return false;
    }

    adapterShared->setCodeTable(state.codeTable);
    std::string replacement;
    if (!adapterShared->expandMacro(rewriteState.shownText, replacement)) {
      return false;
    }

    if (!applyWordDelta(ic, state, debug, replacement, trigger, "macro",
                        false)) {
      return false;
    }

    if (hasPendingRewrite(rewriteState)) {
      rewriteState.queuedKeys.push_front(boundaryKey);
    } else {
      rememberBackspaceSnapshot(rewriteState);
      forwardKeyPressAndRelease(ic, boundaryKey);
      clearComposeState(rewriteState, "macro-boundary", false);
      rewriteState.allowBackspaceSnapshotResetPreserve =
          needsTransientResetPreserve(state) && trigger == ' ';
    }
    return true;
  }

  bool maybeRestoreBeforeBoundary(fcitx::InputContext *ic, OpenKeyState &state,
                                  const fcitx::Key &boundaryKey,
                                  std::shared_ptr<OpenKeyAdapter> adapterShared,
                                  bool debug, char trigger) {
    auto &rewriteState = state.rewriteState;
    if (!deps_.restoreIfWrongSpelling || !deps_.restoreIfWrongSpelling() ||
        !adapterShared || rewriteState.shownText.empty()) {
      return false;
    }

    adapterShared->setCodeTable(state.codeTable);
    std::string restoredWord;
    const bool restored =
        !rewriteState.rawAsciiBuffer.empty()
            ? adapterShared->restoreFromRawAsciiOnWordBreak(
                  rewriteState.shownText, rewriteState.rawAsciiBuffer, trigger,
                  restoredWord)
            : adapterShared->restoreOnWordBreak(rewriteState.shownText, trigger,
                                                restoredWord);
    if (!restored) {
      return false;
    }

    if (!applyWordDelta(ic, state, debug, restoredWord, trigger,
                        "restore-boundary", false)) {
      return false;
    }

    if (hasPendingRewrite(rewriteState)) {
      rewriteState.queuedKeys.push_front(boundaryKey);
    } else {
      rememberBackspaceSnapshot(rewriteState);
      forwardKeyPressAndRelease(ic, boundaryKey);
      clearComposeState(rewriteState, "restore-boundary", false);
      rewriteState.allowBackspaceSnapshotResetPreserve =
          needsTransientResetPreserve(state) && trigger == ' ';
    }
    return true;
  }

  bool processQueuedKey(fcitx::InputContext *ic, OpenKeyState &state,
                        const fcitx::Key &queuedKey,
                        std::shared_ptr<OpenKeyAdapter> adapterShared,
                        bool debug) {
    auto &rewriteState = state.rewriteState;
    const RewriteTiming timing =
        backspaceRewriteTimingFor(state.isX11Environment, state);
    if (hasCtrlAltSuperMeta(queuedKey)) {
      clearComposeState(rewriteState, "ctrl-alt-super");
      return false;
    }

    if (queuedKey.isCursorMove() || queuedKey.check(FcitxKey_Delete)) {
      clearComposeState(rewriteState, "cursor-delete");
      return false;
    }

    if (queuedKey.check(FcitxKey_Escape)) {
      clearComposeState(rewriteState, "escape");
      return false;
    }

    if (queuedKey.check(FcitxKey_BackSpace)) {
      if (needsTransientResetPreserve(state) &&
          !rewriteState.shownText.empty() &&
          !trackedWordStillBeforeCursor(ic, rewriteState.shownText, false)) {
        clearComposeState(rewriteState, "backspace-cursor-mismatch");
        return false;
      }
      if (restoreBackspaceSnapshot(rewriteState)) {
        return false;
      }
      if (rewriteState.shownText.empty()) {
        return false;
      }
      if (!rewriteState.hasRewrittenCurrentWord) {
        clearComposeState(rewriteState, "backspace-empty-or-not-rewritten");
        return false;
      }
      const auto method = deps_.backspaceInjector->sendBackspaces(
          ic, state.program, 1, debug, timing.interKeyUsec);
      if (method != BackspaceInjector::Method::Uinput) {
        return false;
      }
      if (!rewriteState.rawAsciiBuffer.empty()) {
        rewriteState.rawAsciiBuffer.pop_back();
      }
      rewriteState.shownText = utf8DropLastN(rewriteState.shownText, 1);
      if (rewriteState.shownText.empty()) {
        rewriteState.rawAsciiBuffer.clear();
        rewriteState.hasRewrittenCurrentWord = false;
      }
      rewriteState.allowTransientResetPreserve =
          !rewriteState.shownText.empty();
      return true;
    }

    const auto normalizedKey = queuedKey.normalize();
    const uint32_t uni = fcitx::Key::keySymToUnicode(normalizedKey.sym());
    if (uni >= 0x20 && uni <= 0x7E) {
      const char c = static_cast<char>(uni);

      // Word boundaries: clear composition state, forward to app
      if (isBoundaryASCII(c) || queuedKey.check(FcitxKey_Return) ||
          queuedKey.check(FcitxKey_KP_Enter) ||
          queuedKey.check(FcitxKey_ISO_Enter) ||
          queuedKey.check(FcitxKey_Tab)) {
        if (maybeExpandMacroBeforeBoundary(ic, state, queuedKey, adapterShared,
                                           debug, c)) {
          return true;
        }
        if (maybeRestoreBeforeBoundary(ic, state, queuedKey, adapterShared,
                                       debug, c)) {
          return true;
        }
        rememberBackspaceSnapshot(rewriteState);
        clearComposeState(rewriteState, "boundary", false);
        rewriteState.allowBackspaceSnapshotResetPreserve =
            needsTransientResetPreserve(state) && c == ' ';
        return false;
      }

      // Only composing chars go to the engine.
      // Non-composing non-boundary chars (e.g. !@#$) clear state and forward.
      if (!isComposingASCII(c)) {
        clearComposeState(rewriteState, "not-composing-ascii");
        return false;
      }

      if (rewriteState.shownText.empty() &&
          rewriteState.canReseedFromBackspaceSnapshot) {
        clearBackspaceSnapshot(rewriteState);
      }

      if (needsTransientResetPreserve(state) &&
          !rewriteState.shownText.empty() &&
          !trackedWordStillBeforeCursor(ic, rewriteState.shownText, false)) {
        clearComposeState(rewriteState, "ascii-cursor-mismatch");
        return false;
      }

      if (!adapterShared) {
        clearComposeState(rewriteState, "no-adapter");
        return false;
      }
      adapterShared->setCodeTable(state.codeTable);
      const auto r = adapterShared->processAsciiKey(rewriteState.shownText, c);
      if (!r.handled) {
        clearComposeState(rewriteState, "adapter-not-handled");
        return false;
      }
      rewriteState.rawAsciiBuffer.push_back(c);
      return applyWordDelta(ic, state, debug, r.newWord, c, "ascii");
    }

    clearComposeState(rewriteState, "non-ascii-key");
    return false;
  }

  void pumpQueue(fcitx::InputContext *ic, OpenKeyState &state,
                 std::shared_ptr<OpenKeyAdapter> adapterShared, bool debug) {
    auto &rewriteState = state.rewriteState;
    if (rewriteState.processingQueue || rewriteState.rewriteLock) {
      return;
    }
    rewriteState.processingQueue = true;
    while (!rewriteState.rewriteLock && !rewriteState.queuedKeys.empty()) {
      const fcitx::Key queuedKey = rewriteState.queuedKeys.front();
      rewriteState.queuedKeys.pop_front();
      const bool handled =
          processQueuedKey(ic, state, queuedKey, adapterShared, debug);
      if (!handled) {
        forwardKeyPressAndRelease(ic, queuedKey);
      }
    }
    rewriteState.processingQueue = false;
  }

  BackspaceRewriteDeps deps_;
};

} // namespace

OpenKeyEngine::OpenKeyEngine(fcitx::Instance *instance)
    : instance_(instance), adapter_(std::make_shared<OpenKeyAdapter>()) {
  isX11Environment_ = isRunningOnX11();
  lifetime_ = std::make_shared<int>(1);
  instance_->inputContextManager().registerProperty("openkeyState", &factory_);
  remoteRewriteCoordinator_ = std::make_unique<RemoteRewriteCoordinator>(
      instance_ ? &instance_->eventLoop() : nullptr,
      [this](fcitx::InputContext *ic, uint64_t sessionId, uint64_t txId) {
        handleRemoteRewriteDone(ic, sessionId, txId);
      },
      [this]() { return debugEnabled(); });
  focusedAppBridge_ = std::make_unique<FocusedAppBridge>(
      instance_ ? &instance_->eventLoop() : nullptr,
      [this]() { return debugEnabled(); });
  BackspaceRewriteDeps rewriteDeps;
  rewriteDeps.instance = instance_;
  rewriteDeps.factory = &factory_;
  rewriteDeps.adapter = adapter_;
  rewriteDeps.backspaceInjector = &g_backspaceInjector;
  rewriteDeps.lifetimeWeak = lifetime_;
  rewriteDeps.debugEnabled = [this]() { return debugEnabled(); };
  rewriteDeps.enableMacro = [this]() { return config_.enableMacro.value(); };
  rewriteDeps.restoreIfWrongSpelling = [this]() {
    return config_.restoreIfWrongSpelling.value();
  };
  rewriteDeps.enableBackspaceSnapshot = [this]() {
    return config_.enableBackspaceSnapshot.value();
  };
  rewriteDeps.rawBackspaceRewriteEnabled = [this]() {
    return config_.enableRawBackspaceRewrite.value();
  };
  rewriteDeps.remoteEnabled = [this]() {
    return remoteRewriteCoordinator_ && remoteRewriteCoordinator_->enabled();
  };
  rewriteDeps.remoteSchedule =
      [this](fcitx::InputContext *ic, OpenKeyState &state,
             unsigned int deleteCount, uint64_t interBackspaceUsec,
             uint64_t commitDelayUsec) {
        return scheduleRemoteRewrite(ic, state, deleteCount, interBackspaceUsec,
                                     commitDelayUsec);
      };
  rewriteDeps.remoteScheduleWait = [this](OpenKeyState &state,
                                          uint64_t delayUsec) {
    if (!remoteRewriteCoordinator_) {
      return false;
    }
    return remoteRewriteCoordinator_->scheduleWait(
        state.rewriteState.remoteSessionId,
        state.rewriteState.remotePendingTxId, delayUsec);
  };
  rewriteDeps.remoteCancelDone = [this](uint64_t sessionId, uint64_t txId) {
    return remoteRewriteCoordinator_ &&
           remoteRewriteCoordinator_->cancelDone(sessionId, txId);
  };
  backspaceRewriteHandler_ =
      std::make_unique<BackspaceRewriteModeHandler>(std::move(rewriteDeps));

  SimpleModeHandlerDeps simpleDeps;
  simpleDeps.adapter = adapter_;
  simpleDeps.enableMacro = [this]() { return config_.enableMacro.value(); };
  simpleDeps.restoreIfWrongSpelling = [this]() {
    return config_.restoreIfWrongSpelling.value();
  };
  preeditHandler_ = std::make_unique<PreeditModeHandler>(std::move(simpleDeps));
  reloadConfig();
  if (remoteRewriteCoordinator_) {
    remoteRewriteCoordinator_->ensureAvailableOrStartOnce();
  }

  // Warm up uinput ngay khi load để tránh delay lần đầu gõ
  g_backspaceInjector.uinputAvailable(debugEnabled());
}

OpenKeyEngine::~OpenKeyEngine() {
  focusedAppBridge_.reset();
  remoteRewriteCoordinator_.reset();
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
    case RuntimeMode::Preedit:
      return "Preedit";
    case RuntimeMode::BackspaceRewrite:
      return "Non Preedit";
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
  case RuntimeMode::Preedit:
    return "Preedit";
  case RuntimeMode::BackspaceRewrite:
    return "Non Preedit";
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

bool OpenKeyEngine::rewriteServerAvailable() {
  return remoteRewriteCoordinator_ && remoteRewriteCoordinator_->available();
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
  auto persistOne =
      [](const char *path,
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

std::unordered_map<std::string, RuntimeMode> &
OpenKeyEngine::appModeMapFor(fcitx::InputContext * /* ic */) {
  return isX11Environment_ ? x11AppModeMap_ : waylandAppModeMap_;
}

OpenKeyState *OpenKeyEngine::stateFor(fcitx::InputContext *ic) {
  return ic->propertyFor(&factory_);
}

bool OpenKeyEngine::scheduleRemoteRewrite(fcitx::InputContext *ic,
                                          OpenKeyState &state,
                                          unsigned int deleteCount,
                                          uint64_t interBackspaceUsec,
                                          uint64_t commitDelayUsec) {
  if (!remoteRewriteCoordinator_ || !remoteRewriteCoordinator_->enabled() ||
      !ic) {
    return false;
  }

  auto &rewriteState = state.rewriteState;
  if (rewriteState.remoteSessionId == 0) {
    rewriteState.remoteSessionId =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ic));
    if (rewriteState.remoteSessionId == 0) {
      rewriteState.remoteSessionId = 1;
    }
  }
  const uint64_t txId = rewriteState.remoteNextTxId++;
  rewriteState.remotePendingTxId = txId;
  rewriteState.remoteRewritePending = true;
  remoteRewriteCoordinator_->bindSession(rewriteState.remoteSessionId,
                                         ic->watch());

  if (!remoteRewriteCoordinator_->schedule(
          rewriteState.remoteSessionId, txId, static_cast<int>(deleteCount),
          interBackspaceUsec, commitDelayUsec)) {
    rewriteState.remotePendingTxId = 0;
    rewriteState.remoteRewritePending = false;
    return false;
  }
  return true;
}

void OpenKeyEngine::handleRemoteRewriteDone(fcitx::InputContext *ic,
                                            uint64_t sessionId, uint64_t txId) {
  auto *state = stateFor(ic);
  if (!state) {
    return;
  }
  auto &rewriteState = state->rewriteState;
  if (rewriteState.remoteSessionId != sessionId ||
      rewriteState.remotePendingTxId != txId || !rewriteState.rewriteLock ||
      !rewriteState.remoteRewritePending) {
    return;
  }

  if (debugEnabled()) {
    FCITX_INFO() << "openkey: remote DONE session=" << sessionId
                 << " txId=" << txId;
  }

  auto *rewriteHandler = dynamic_cast<BackspaceRewriteModeHandler *>(
      backspaceRewriteHandler_.get());
  if (!rewriteHandler) {
    return;
  }
  rewriteHandler->handleRemoteCommitAction(ic, *state, txId);
}

const fcitx::Configuration *OpenKeyEngine::getConfig() const {
  return &config_;
}

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
  state->isX11Environment = isX11Environment_;

  state->rewriteState.clear();
  state->composing.clear();
  state->preeditKeyBuffer.clear();
  state->manualMode = false;
  state->modeDecided = false;
  state->program = ic->program();
  state->windowTitle.clear();
  if (focusedAppBridge_) {
    const auto info = focusedAppBridge_->focusedAppInfo();
    if (state->program.empty() && !info.first.empty()) {
      state->program = info.first;
    }
    state->windowTitle = info.second;
    if (debugEnabled()) {
      FCITX_INFO() << "openkey: bridge program=" << state->program
                   << " title=" << state->windowTitle;
    }
  }

  // Lấy thời gian hiện tại để kiểm tra khoảng cách từ lần gõ phím cuối
  uint64_t nowTime = fcitx::now(CLOCK_MONOTONIC);
  // Nếu đã hơn 2 giây kể từ lần nhấn phím cuối cùng, coi như đây thực sự là bắt
  // đầu gõ "từ đầu tiên". Nếu dưới 2 giây, có khả năng là ứng dụng đang tự động
  // focus lại ngầm ở giữa một từ đang gõ dở, nên giữ nguyên trạng thái cũ.
  if (nowTime - lastKeyTime_ > 2000000) { // 2 seconds
    // Mặc định là false
    state->x11FirstWordPreedit = false;
    // Kiểm tra ưu tiên theo ý muốn: Chỉ áp dụng trên X11 và nếu là Trình duyệt
    if (state->isX11Environment && isBrowserLikeProgram(*state)) {
      // Nếu là trình duyệt nhưng KHÔNG hỗ trợ SurroundingText thì mới bật cờ
      if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        state->x11FirstWordPreedit = true;
      }
    }
  } else {
    state->x11FirstWordPreedit = false;
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
  state->rewriteState.clear();
  reset(entry, event);
}

void OpenKeyEngine::reset(const fcitx::InputMethodEntry &,
                          fcitx::InputContextEvent &event) {
  auto *ic = event.inputContext();
  auto *state = stateFor(ic);
  const bool snapshotEnabled = config_.enableBackspaceSnapshot.value();

  const bool transientResetKeepRewrite =
      needsTransientResetPreserve(*state) &&
      state->mode == RuntimeMode::BackspaceRewrite &&
      !state->rewriteState.shownText.empty() &&
      !state->rewriteState.rawAsciiBuffer.empty() &&
      state->rewriteState.allowTransientResetPreserve &&
      !state->rewriteState.rewriteLock;

  const bool preserveRewrite =
      transientResetKeepRewrite ||
      (snapshotEnabled && state->rewriteState.restoredFromBackspaceSnapshot &&
       !state->rewriteState.shownText.empty() &&
       state->rewriteState.allowBackspaceSnapshotResetPreserve);

  const bool preserveRewriteSnapshot =
      snapshotEnabled && !preserveRewrite &&
      state->rewriteState.allowBackspaceSnapshotResetPreserve &&
      state->rewriteState.preserveBackspaceSnapshotAfterBoundaryBackspace &&
      state->rewriteState.canReseedFromBackspaceSnapshot &&
      !state->rewriteState.backspaceSnapshotShownText.empty();

  const std::string rewriteShown = state->rewriteState.shownText;
  const std::string rewriteRaw = state->rewriteState.rawAsciiBuffer;
  const bool rewriteRewritten = state->rewriteState.hasRewrittenCurrentWord;
  const bool rewriteAllowTransientResetPreserve =
      state->rewriteState.allowTransientResetPreserve;
  const std::string rewriteSnapshotShown =
      state->rewriteState.backspaceSnapshotShownText;
  const std::string rewriteSnapshotRaw =
      state->rewriteState.backspaceSnapshotRawAsciiBuffer;
  const bool rewriteSnapshotRewritten =
      state->rewriteState.backspaceSnapshotHasRewrittenCurrentWord;

  if (debugEnabled()) {
    FCITX_INFO() << "openkey: reset"
                 << " program=" << state->program
                 << " snapshotEnabled=" << snapshotEnabled
                 << " transientResetKeepRewrite=" << transientResetKeepRewrite
                 << " preserveRewrite=" << preserveRewrite
                 << " preserveRewriteSnapshot=" << preserveRewriteSnapshot
                 << " rewriteAllowPreserve="
                 << state->rewriteState.allowBackspaceSnapshotResetPreserve
                 << " rewriteShown=" << state->rewriteState.shownText
                 << " rewriteRaw=" << state->rewriteState.rawAsciiBuffer
                 << " rewriteAllowTransientResetPreserve="
                 << state->rewriteState.allowTransientResetPreserve;
  }

  state->rewriteState.clear();

  if (preserveRewrite) {
    state->rewriteState.shownText = rewriteShown;
    state->rewriteState.rawAsciiBuffer = rewriteRaw;
    state->rewriteState.hasRewrittenCurrentWord = rewriteRewritten;
    state->rewriteState.allowTransientResetPreserve =
        rewriteAllowTransientResetPreserve;
    state->rewriteState.restoredFromBackspaceSnapshot =
        false; // Transient reset preserve, not a snapshot restore.
    state->rewriteState.allowBackspaceSnapshotResetPreserve = false;
    if (debugEnabled()) {
      FCITX_INFO() << "openkey: reset preserve"
                   << " mode=backspaceRewrite"
                   << " shown=" << state->rewriteState.shownText
                   << " raw=" << state->rewriteState.rawAsciiBuffer
                   << " rewritten="
                   << state->rewriteState.hasRewrittenCurrentWord;
    }
  }

  if (preserveRewriteSnapshot) {
    state->rewriteState.backspaceSnapshotShownText = rewriteSnapshotShown;
    state->rewriteState.backspaceSnapshotRawAsciiBuffer = rewriteSnapshotRaw;
    state->rewriteState.backspaceSnapshotHasRewrittenCurrentWord =
        rewriteSnapshotRewritten;
    state->rewriteState.canReseedFromBackspaceSnapshot = true;
    state->rewriteState.preserveBackspaceSnapshotAfterBoundaryBackspace = true;
    state->rewriteState.allowBackspaceSnapshotResetPreserve = false;
    if (debugEnabled()) {
      FCITX_INFO() << "openkey: reset preserve-snapshot"
                   << " mode=backspaceRewrite"
                   << " snapshotShown="
                   << state->rewriteState.backspaceSnapshotShownText
                   << " snapshotRaw="
                   << state->rewriteState.backspaceSnapshotRawAsciiBuffer;
    }
  }

  state->composing.clear();
  state->preeditKeyBuffer.clear();

  ic->inputPanel().reset();
  ic->updatePreedit();
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
}

RuntimeMode OpenKeyEngine::decideMode(fcitx::InputContext *ic, OpenKeyState &s,
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
    const bool hasRewriteServer = rewriteServerAvailable();
    if (it->second == RuntimeMode::BackspaceRewrite && hasRewriteServer) {
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

  RuntimeMode mode;
  if (isFirefoxLikeProgram(s)) {
    mode = RuntimeMode::Preedit;
  } else {
    mode = rewriteServerAvailable() ? RuntimeMode::BackspaceRewrite
                                    : RuntimeMode::Preedit;
  }
  if (writeBack && !normalizedProgram.empty()) {
    appModeMap[normalizedProgram] = mode;
    persistAppModes();
  }
  return mode;
}

RuntimeMode OpenKeyEngine::firstManualMode() const {
  return RuntimeMode::BackspaceRewrite;
}

void OpenKeyEngine::keyEvent(const fcitx::InputMethodEntry &,
                             fcitx::KeyEvent &event) {
  auto *ic = event.inputContext();
  auto *state = stateFor(ic);

  // Cập nhật lại thời điểm gõ phím toàn cục cho engine
  lastKeyTime_ = fcitx::now(CLOCK_MONOTONIC);

  if (event.isRelease()) {
    if (state && state->mode == RuntimeMode::BackspaceRewrite &&
        backspaceRewriteHandler_) {
      backspaceRewriteHandler_->handleKey(ic, event, *state);
    }
    return;
  }

  const auto key = event.key().normalize();

  // Ensure we have a baseline auto mode for this context.
  if (!state->modeDecided) {
    state->mode = decideMode(ic, *state, false);
    state->autoMode = state->mode;
    state->modeDecided = true;
  }

  if (key.checkKeyList(config_.switchModeKey.value()) &&
      key.sym() != FcitxKey_None) {
    // DirectCommit is reserved for protected contexts such as password
    // fields and is intentionally excluded from manual mode switching.
    if (state->mode == RuntimeMode::DirectCommit &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::Password)) {
      return;
    }

    auto clearComposingState = [this, ic, state]() {
      state->rewriteState.clear();
      state->composing.clear();
      state->preeditKeyBuffer.clear();
      ic->inputPanel().reset();
      ic->updatePreedit();
      ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
    };

    bool returnToAuto = false;
    RuntimeMode nextMode;
    if (!state->manualMode) {
      nextMode = RuntimeMode::BackspaceRewrite;
    } else if (state->mode == RuntimeMode::BackspaceRewrite) {
      nextMode = RuntimeMode::Preedit;
    } else if (state->mode == RuntimeMode::Preedit) {
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
        setAppModeForProgram(ic, state->program, nextMode);
        persistAppModes();
      }
    }

    if (debugEnabled()) {
      FCITX_INFO() << "openkey: switch mode hotkey program=" << state->program
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
        case RuntimeMode::Preedit:
          return "Preedit";
        case RuntimeMode::BackspaceRewrite:
          return "Non Preedit";
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
    RuntimeMode checkMode = state->mode;
    // Fix kẹt chữ: Mặc dù mode hiện tại là BackspaceRewrite, nhưng nếu cờ
    // x11FirstWordPreedit đang bật thì bộ gõ thực chất đang chạy ngầm bằng
    // Preedit. Ta phải báo cho hệ thống biết nó là Preedit để dọn dẹp chữ gõ
    // dở, nếu không chữ sẽ bị dính vĩnh viễn trên màn hình.
    if (state->isX11Environment &&
        state->mode == RuntimeMode::BackspaceRewrite &&
        state->x11FirstWordPreedit) {
      checkMode = RuntimeMode::Preedit;
    }

    // Nếu đang có chữ gõ dở (!composing.empty()) mà người dùng bấm phím tắt
    // (vd: Ctrl+C, Ctrl+S) thì xóa sạch chữ gõ dở đó đi (gọi reset).
    if (state->modeDecided && checkMode == RuntimeMode::Preedit &&
        !state->composing.empty()) {
      preeditHandler_->reset(*state);
    }

    // Bấm phím tắt đồng nghĩa với việc người dùng đã ngắt quãng việc gõ từ hiện
    // tại (break). Hủy cờ Preedit tạm thời để chữ tiếp theo sau khi bấm phím
    // tắt sẽ gõ bằng BackspaceRewrite.
    if (state->x11FirstWordPreedit) {
      state->x11FirstWordPreedit = false;
    }
    return;
  }

  RuntimeMode effectiveMode = state->mode;
  // Áp dụng tạm thời mode Preedit nếu cờ first word đang bật
  if (state->isX11Environment &&
      effectiveMode == RuntimeMode::BackspaceRewrite &&
      state->x11FirstWordPreedit) {
    effectiveMode = RuntimeMode::Preedit;
  }

  bool wasComposing = !state->composing.empty();

  switch (effectiveMode) {
  case RuntimeMode::Auto:
    return;
  case RuntimeMode::DirectCommit:
    return;
  case RuntimeMode::BackspaceRewrite:
    if (backspaceRewriteHandler_) {
      backspaceRewriteHandler_->handleKey(ic, event, *state);
    }
    return;
  case RuntimeMode::Preedit:
    adapter_->setCodeTable(state->codeTable);
    if (preeditHandler_) {
      preeditHandler_->handleKey(ic, event, *state);
    }
    break;
  }

  // Kiểm tra xem sự kiện vừa rồi có phải là một khoảng nghỉ (break) để thoát
  // khỏi Preedit hay không
  if (state->x11FirstWordPreedit && effectiveMode == RuntimeMode::Preedit) {
    bool isBreak = false;
    if (wasComposing && state->composing.empty()) {
      // Nếu trước đó đang gõ chữ mà giờ trống trơn, ngoại trừ trường hợp bị
      // người dùng chủ động xóa hết bằng Backspace/Escape.
      if (!key.check(FcitxKey_BackSpace) && !key.check(FcitxKey_Escape)) {
        isBreak = true;
      }
    } else if (!wasComposing && state->composing.empty()) {
      // Nếu không phải đang gõ chữ, mọi thao tác ngoài modifier hay phím xóa
      // đều coi là tạo ra khoảng nghỉ.
      if (!key.isModifier() && !key.check(FcitxKey_BackSpace) &&
          !key.check(FcitxKey_Escape) && !key.check(FcitxKey_Delete)) {
        isBreak = true;
      }
    }
    // Gỡ cờ first word và trở về lại với chế độ BackspaceRewrite cho các chữ
    // tiếp theo.
    if (isBreak) {
      state->x11FirstWordPreedit = false;
    }
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
