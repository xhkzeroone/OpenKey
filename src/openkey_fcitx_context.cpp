#include "openkey_fcitx_context.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace openkey {
namespace {

static char toLowerASCII(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

static std::string asciiLower(std::string s) {
    for (char &c : s) {
        c = toLowerASCII(c);
    }
    return s;
}

static std::atomic<TimerHandle> gNextTimerHandle{1};
static std::mutex gTimerMutex;
static std::unordered_map<TimerHandle, std::unique_ptr<fcitx::EventSourceTime>>
    gTimers;

} // namespace

KeyInfo keyInfoFromFcitxEvent(const fcitx::KeyEvent &event) {
    const auto normalized = event.key().normalize();
    const auto raw = event.rawKey();
    KeyInfo info;
    info.sym = static_cast<uint32_t>(normalized.sym());
    info.rawSym = static_cast<uint32_t>(raw.sym());
    info.code = static_cast<uint32_t>(raw.code());
    info.modifiers = static_cast<uint32_t>(raw.states().toInteger());
    info.isRelease = event.isRelease();
    return info;
}

fcitx::Key keyInfoToFcitxKey(const KeyInfo &key) {
    return fcitx::Key(static_cast<fcitx::KeySym>(key.rawSym ? key.rawSym : key.sym),
                      fcitx::KeyStates(key.modifiers));
}

FcitxIMContext::FcitxIMContext(fcitx::InputContext *ic,
                               fcitx::Instance *instance,
                               std::weak_ptr<void> lifetime)
    : ic_(ic), instance_(instance), lifetime_(std::move(lifetime)) {}

FcitxIMContext::~FcitxIMContext() = default;

void FcitxIMContext::commitString(std::string_view text) {
    if (ic_) {
        ic_->commitString(std::string(text));
    }
}

void FcitxIMContext::deleteSurroundingText(int offsetChars,
                                           unsigned int nChars) {
    if (ic_) {
        ic_->deleteSurroundingText(offsetChars, nChars);
    }
}

void FcitxIMContext::forwardKey(const KeyInfo &key, bool isRelease) {
    if (ic_) {
        ic_->forwardKey(keyInfoToFcitxKey(key), isRelease);
    }
}

void FcitxIMContext::forwardKeyPressAndRelease(const KeyInfo &key) {
    forwardKey(key, false);
    forwardKey(key, true);
}

void FcitxIMContext::setPreedit(std::string_view text, int cursorBytePos) {
    if (!ic_) {
        return;
    }

    auto &panel = ic_->inputPanel();
    panel.reset();

    if (!text.empty()) {
        fcitx::Text preedit;
        preedit.append(std::string(text));
        preedit.setCursor(cursorBytePos < 0 ? static_cast<int>(text.size())
                                            : cursorBytePos);
        if (supportsPreedit()) {
            panel.setClientPreedit(preedit);
        } else {
            panel.setPreedit(preedit);
        }
    }

    ic_->updatePreedit();
    ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
}

void FcitxIMContext::clearPreedit() {
    if (!ic_) {
        return;
    }
    ic_->inputPanel().reset();
    ic_->updatePreedit();
    ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel, true);
}

bool FcitxIMContext::supportsPreedit() const {
    return ic_ && ic_->capabilityFlags().test(fcitx::CapabilityFlag::Preedit);
}

SurroundingTextInfo FcitxIMContext::getSurroundingText() const {
    SurroundingTextInfo result;
    if (!ic_) {
        return result;
    }
    const auto &st = ic_->surroundingText();
    result.valid = st.isValid();
    result.text = st.text();
    result.cursor = st.cursor();
    result.anchor = st.anchor();
    return result;
}

bool FcitxIMContext::supportsSurroundingText() const {
    return ic_ && ic_->capabilityFlags().test(
                      fcitx::CapabilityFlag::SurroundingText);
}

TimerHandle FcitxIMContext::scheduleOnce(uint64_t delayUsec,
                                         std::function<void()> cb) {
    if (!ic_ || !instance_ || !cb) {
        return 0;
    }
    const TimerHandle handle = gNextTimerHandle.fetch_add(1);
    const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) + delayUsec;
    const std::weak_ptr<void> lifetime = lifetime_;
    const auto icRef = ic_->watch();
    auto timer = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, deadline, 0,
        [handle, lifetime, icRef, cb = std::move(cb)](
            fcitx::EventSourceTime *, uint64_t) {
            std::unique_ptr<fcitx::EventSourceTime> currentTimer;
            {
                std::lock_guard<std::mutex> lock(gTimerMutex);
                auto it = gTimers.find(handle);
                if (it != gTimers.end()) {
                    currentTimer = std::move(it->second);
                    gTimers.erase(it);
                }
            }
            if (!lifetime.expired() && icRef.get()) {
                cb();
            }
            return false;
        });
    if (!timer) {
        return 0;
    }
    timer->setOneShot();
    {
        std::lock_guard<std::mutex> lock(gTimerMutex);
        gTimers[handle] = std::move(timer);
    }
    return handle;
}

void FcitxIMContext::cancelTimer(TimerHandle &handle) {
    if (handle != 0) {
        std::lock_guard<std::mutex> lock(gTimerMutex);
        gTimers.erase(handle);
        handle = 0;
    }
}

std::string FcitxIMContext::program() const {
    return ic_ ? ic_->program() : std::string();
}

bool FcitxIMContext::isX11() const {
    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    const char *x11Display = std::getenv("DISPLAY");
    if (waylandDisplay && *waylandDisplay) {
        return false;
    }
    return x11Display && *x11Display;
}

bool FcitxIMContext::isPasswordField() const {
    return ic_ && ic_->capabilityFlags().test(fcitx::CapabilityFlag::Password);
}

bool FcitxIMContext::isLegacyFrontend() const {
    return ic_ && ic_->frontend() &&
           asciiLower(ic_->frontend()).find("fcitx4") != std::string::npos;
}

} // namespace openkey
