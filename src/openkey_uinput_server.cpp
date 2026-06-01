#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/uinput.h>
#endif

namespace {

std::atomic_bool g_running{true};

void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_running.store(false);
    }
}

std::string currentUsername() {
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
    return username;
}

uid_t uidForUsername(const std::string &username) {
    struct passwd pwd {};
    struct passwd *result = nullptr;
    char buf[4096];
    const int res =
        getpwnam_r(username.c_str(), &pwd, buf, sizeof(buf), &result);
    if (res == 0 && result) {
        return result->pw_uid;
    }
    return static_cast<uid_t>(-1);
}

std::string buildSocketName(const std::string &username) {
    std::string name;
    name.reserve(64);
    name += "openkeysocket-";
    name += username;
    name += "-kb_socket";
    // sun_path is typically 108 bytes. Keep some margin.
    constexpr std::size_t kMax = 100;
    if (name.size() > kMax) {
        name.resize(kMax);
    }
    return name;
}

#ifdef __linux__
class UinputDevice {
public:
    ~UinputDevice() {
        if (fd_ >= 0) {
            (void)ioctl(fd_, UI_DEV_DESTROY);
            ::close(fd_);
        }
        fd_ = -1;
    }

    bool initialize() {
        fd_ = ::open("/dev/uinput", O_WRONLY);
        if (fd_ < 0) {
            return false;
        }

        if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(fd_, UI_SET_KEYBIT, KEY_BACKSPACE) < 0) {
            return false;
        }

        struct uinput_setup usetup {};
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x1234;
        usetup.id.product = 0x5678;
        std::snprintf(usetup.name, sizeof(usetup.name),
                      "OpenKey-Uinput-Server");
        if (ioctl(fd_, UI_DEV_SETUP, &usetup) < 0 ||
            ioctl(fd_, UI_DEV_CREATE) < 0) {
            return false;
        }

        // Let the virtual device settle.
        ::sleep(1);
        return true;
    }

    void sendBackspace() const {
        if (fd_ < 0) {
            return;
        }
        struct input_event ev[4] {};
        ev[0].type = EV_KEY;
        ev[0].code = KEY_BACKSPACE;
        ev[0].value = 1;
        ev[2].type = EV_KEY;
        ev[2].code = KEY_BACKSPACE;
        ev[2].value = 0;
        const ssize_t w = ::write(fd_, ev, sizeof(ev));
        (void)w;
    }

private:
    int fd_ = -1;
};
#endif

} // namespace

int main(int argc, char **argv) {
#ifndef __linux__
    (void)argc;
    (void)argv;
    std::fprintf(stderr, "openkey-uinput-server: linux only\n");
    return 1;
#else
    std::string targetUser;
    if (argc == 3 && std::strcmp(argv[1], "-u") == 0) {
        targetUser = argv[2];
    } else {
        targetUser = currentUsername();
    }
    const uid_t expectedUid = uidForUsername(targetUser);
    if (expectedUid == static_cast<uid_t>(-1)) {
        std::fprintf(stderr, "openkey-uinput-server: unknown user: %s\n",
                     targetUser.c_str());
        return 1;
    }

    UinputDevice uinput;
    if (!uinput.initialize()) {
        std::fprintf(stderr,
                     "openkey-uinput-server: failed to init /dev/uinput: %s\n",
                     std::strerror(errno));
        return 1;
    }

    const std::string socketName = buildSocketName(targetUser);
    int serverFd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (serverFd < 0) {
        std::fprintf(stderr, "openkey-uinput-server: socket() failed: %s\n",
                     std::strerror(errno));
        return 1;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    std::memcpy(&addr.sun_path[1], socketName.data(), socketName.size());
    const socklen_t addrLen =
        static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                               socketName.size() + 1);
    if (::bind(serverFd, reinterpret_cast<struct sockaddr *>(&addr), addrLen) !=
        0) {
        std::fprintf(stderr, "openkey-uinput-server: bind() failed: %s\n",
                     std::strerror(errno));
        ::close(serverFd);
        return 1;
    }
    (void)::listen(serverFd, 5);

    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)::sigaction(SIGTERM, &sa, nullptr);
    (void)::sigaction(SIGINT, &sa, nullptr);

    int clientFd = -1;
    int pendingBackspaces = 0;

    while (g_running.load()) {
        struct pollfd fds[2] {};
        fds[0].fd = serverFd;
        fds[0].events = POLLIN;
        fds[1].fd = clientFd;
        fds[1].events = POLLIN;

        const int timeoutMs = (pendingBackspaces > 0) ? 1 : -1;
        const int ret = ::poll(fds, 2, timeoutMs);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (ret == 0) {
            if (pendingBackspaces > 0) {
                uinput.sendBackspace();
                --pendingBackspaces;
            }
            continue;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            int newFd = ::accept4(serverFd, nullptr, nullptr, SOCK_NONBLOCK);
            if (newFd >= 0) {
                struct ucred cred {};
                socklen_t len = sizeof(cred);
                bool authorized = false;
                if (::getsockopt(newFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) ==
                    0) {
                    authorized = (cred.uid == expectedUid);
                }
                if (!authorized) {
                    ::close(newFd);
                } else {
                    if (clientFd >= 0) {
                        ::close(clientFd);
                    }
                    clientFd = newFd;
                }
            }
        }

        if (clientFd >= 0 &&
            (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            int count = 0;
            const ssize_t n = ::recv(clientFd, &count, sizeof(count), 0);
            if (n <= 0) {
                ::close(clientFd);
                clientFd = -1;
            } else if (count > 0) {
                pendingBackspaces += (count - 1);
                uinput.sendBackspace();
            }
        }
    }

    if (clientFd >= 0) {
        ::close(clientFd);
    }
    ::close(serverFd);
    return 0;
#endif
}
