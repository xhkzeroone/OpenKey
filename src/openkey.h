#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>

#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>

#include "openkey_config.h"
#include "openkey_platform.h"
#include "openkey_state.h"
#include "openkey_text_mode_handler.h"

namespace fcitx {
class Instance;
}

namespace openkey {

struct FcitxOpenKeyState;
class OpenKeyAdapter;
class FcitxFocusedAppBridge;
class FcitxRemoteNonPreeditCoordinator;
class BackspaceModeHandler;

enum class RuntimeMode {
    Auto,
    BackspaceRewriteDelta,
    NonPreeditBackspaceRewrite,
    SurroundingText,
    Preedit,
    DirectCommit,
};

struct DeltaRewriteState {
    std::string shownText;
    std::string rawAsciiBuffer;
    bool hasRewrittenCurrentWord = false;
    bool rewriteLock = false;
    bool waitingBackspaceAck = false;
    bool processingQueue = false;
    int expectedBackspaces = 0;
    int seenBackspaces = 0;
    std::deque<KeyInfo> queuedKeys;
    TimerHandle commitTimer = 0;
    TimerHandle ackTimeoutTimer = 0;
    std::string pendingConvertedText;
    std::string pendingShownTextAfterCommit;
    std::string backspaceSnapshotShownText;
    std::string backspaceSnapshotRawAsciiBuffer;
    bool backspaceSnapshotHasRewrittenCurrentWord = false;
    bool canReseedFromBackspaceSnapshot = false;
    bool restoredFromBackspaceSnapshot = false;
    bool preserveBackspaceSnapshotAfterBoundaryBackspace = false;
    bool allowBackspaceSnapshotResetPreserve = false;
    bool allowTransientResetPreserve = false;

    bool hasPendingRewrite() const {
        return rewriteLock || waitingBackspaceAck ||
               !pendingConvertedText.empty() ||
               !pendingShownTextAfterCommit.empty();
    }

    void clear() {
        if (hasPendingRewrite()) {
            return;
        }

        shownText.clear();
        rawAsciiBuffer.clear();
        hasRewrittenCurrentWord = false;
        rewriteLock = false;
        waitingBackspaceAck = false;
        processingQueue = false;
        expectedBackspaces = 0;
        seenBackspaces = 0;
        queuedKeys.clear();
        commitTimer = 0;
        ackTimeoutTimer = 0;
        pendingConvertedText.clear();
        pendingShownTextAfterCommit.clear();
        backspaceSnapshotShownText.clear();
        backspaceSnapshotRawAsciiBuffer.clear();
        backspaceSnapshotHasRewrittenCurrentWord = false;
        canReseedFromBackspaceSnapshot = false;
        restoredFromBackspaceSnapshot = false;
        preserveBackspaceSnapshotAfterBoundaryBackspace = false;
        allowBackspaceSnapshotResetPreserve = false;
        allowTransientResetPreserve = false;
    }
};

struct NonPreeditDeltaRewriteState {
    std::string shownText;
    std::string rawAsciiBuffer;
    bool hasRewrittenCurrentWord = false;
    bool rewriteLock = false;
    bool waitingBackspaceAck = false;
    bool processingNonPreedit = false;
    int expectedBackspaces = 0;
    int seenBackspaces = 0;
    int lateBackspaceBudget = 0;
    std::deque<KeyInfo> nonPreeditKeys;
    TimerHandle commitTimer = 0;
    TimerHandle lateBackspaceTimeoutTimer = 0;
    TimerHandle ackTimeoutTimer = 0;
    std::string pendingConvertedText;
    std::string pendingShownTextAfterCommit;
    uint64_t remoteSessionId = 0;
    uint64_t remoteNextTxId = 1;
    uint64_t remotePendingTxId = 0;
    bool remoteRewritePending = false;
    std::string backspaceSnapshotShownText;
    std::string backspaceSnapshotRawAsciiBuffer;
    bool backspaceSnapshotHasRewrittenCurrentWord = false;
    bool canReseedFromBackspaceSnapshot = false;
    bool restoredFromBackspaceSnapshot = false;
    bool preserveBackspaceSnapshotAfterBoundaryBackspace = false;
    bool allowBackspaceSnapshotResetPreserve = false;
    bool allowTransientResetPreserve = false;

    bool hasRemoteRewritePending() const {
        return remoteRewritePending || remotePendingTxId != 0;
    }

    void clear() {
        if (hasRemoteRewritePending()) {
            return;
        }

        shownText.clear();
        rawAsciiBuffer.clear();
        hasRewrittenCurrentWord = false;
        rewriteLock = false;
        waitingBackspaceAck = false;
        processingNonPreedit = false;
        expectedBackspaces = 0;
        seenBackspaces = 0;
        lateBackspaceBudget = 0;
        nonPreeditKeys.clear();
        commitTimer = 0;
        lateBackspaceTimeoutTimer = 0;
        ackTimeoutTimer = 0;
        pendingConvertedText.clear();
        pendingShownTextAfterCommit.clear();
        remotePendingTxId = 0;
        remoteRewritePending = false;
        backspaceSnapshotShownText.clear();
        backspaceSnapshotRawAsciiBuffer.clear();
        backspaceSnapshotHasRewrittenCurrentWord = false;
        canReseedFromBackspaceSnapshot = false;
        restoredFromBackspaceSnapshot = false;
        preserveBackspaceSnapshotAfterBoundaryBackspace = false;
        allowBackspaceSnapshotResetPreserve = false;
        allowTransientResetPreserve = false;
    }
};

struct FcitxOpenKeyState : public fcitx::InputContextProperty,
                            public OpenKeyTextState {
    DeltaRewriteState delta;
    NonPreeditDeltaRewriteState nonPreeditDelta;
    std::unique_ptr<fcitx::EventSourceTime> modeInfoTimer;
    RuntimeMode mode = RuntimeMode::SurroundingText;
    RuntimeMode autoMode = RuntimeMode::SurroundingText;
    bool manualMode = false;
    bool modeDecided = false;
};

class FcitxOpenKeyEngine final : public fcitx::InputMethodEngineV2 {
public:
    explicit FcitxOpenKeyEngine(fcitx::Instance *instance);
    ~FcitxOpenKeyEngine() override;

    void keyEvent(const fcitx::InputMethodEntry &entry,
                  fcitx::KeyEvent &keyEvent) override;
    std::string subMode(const fcitx::InputMethodEntry &entry,
                        fcitx::InputContext &inputContext) override;
    std::string subModeLabelImpl(const fcitx::InputMethodEntry &entry,
                                 fcitx::InputContext &ic) override;
    void activate(const fcitx::InputMethodEntry &entry,
                  fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry,
                    fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry,
               fcitx::InputContextEvent &event) override;

    void reloadConfig() override;
    void save() override;
    const fcitx::Configuration *getConfig() const override;
    void setConfig(const fcitx::RawConfig &config) override;
    const fcitx::Configuration *getSubConfig(const std::string &path) const override;
    void setSubConfig(const std::string &path, const fcitx::RawConfig &config) override;

private:
    fcitx::Instance *instance_;
    std::shared_ptr<void> lifetime_;
    OpenKeyConfig config_;
    OpenKeyMacroTable macroTables_;

    std::unordered_map<std::string, RuntimeMode> x11AppModeMap_;
    std::unordered_map<std::string, RuntimeMode> waylandAppModeMap_;

    fcitx::SimpleInputContextPropertyFactory<FcitxOpenKeyState> factory_;

    // Core adapter.
    std::shared_ptr<OpenKeyAdapter> adapter_;
    std::unique_ptr<FcitxRemoteNonPreeditCoordinator> remoteNonPreeditCoordinator_;

    // Optional bridge to GNOME Shell extension to resolve app id/name when
    // InputContext::program() is empty (common on Wayland for some clients).
    std::unique_ptr<FcitxFocusedAppBridge> focusedAppBridge_;

    std::unique_ptr<TextModeHandler> preeditHandler_;
    std::unique_ptr<TextModeHandler> surroundingTextHandler_;
    std::unique_ptr<BackspaceModeHandler> backspaceRewriteHandler_;
    std::unique_ptr<BackspaceModeHandler> nonPreeditBackspaceRewriteHandler_;

    bool debugEnabled() const;
    void loadAppModes();
    void persistAppModes();
    void setAppModeForProgram(fcitx::InputContext *ic,
                              const std::string &program, RuntimeMode mode);
    std::unordered_map<std::string, RuntimeMode> &appModeMapFor(
        fcitx::InputContext *ic);
    void applyConfig();
    void persistConfig();
    bool nonPreeditServerAvailable();

    FcitxOpenKeyState *stateFor(fcitx::InputContext *ic);

    RuntimeMode decideMode(fcitx::InputContext *ic, FcitxOpenKeyState &state,
                             bool writeBack = true);
    RuntimeMode firstManualMode() const;
    bool handleBackspaceRewrite(fcitx::InputContext *ic,
                                fcitx::KeyEvent &event, FcitxOpenKeyState &state);
    bool scheduleRemoteNonPreeditRewrite(fcitx::InputContext *ic, FcitxOpenKeyState &state,
                                    unsigned int deleteCount,
                                    uint64_t interBackspaceUsec,
                                    uint64_t commitDelayUsec);
    void handleRemoteNonPreeditDone(fcitx::InputContext *ic, uint64_t sessionId,
                               uint64_t txId);
};

} // namespace openkey
