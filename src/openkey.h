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

namespace fcitx {
class Instance;
}

namespace openkey {

struct OpenKeyState;
class OpenKeyAdapter;
class FocusedAppBridge;
class RemoteRewriteCoordinator;

class InputModeHandler {
public:
    virtual ~InputModeHandler() = default;
    virtual bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                           OpenKeyState &state) = 0;
    virtual void reset(OpenKeyState &) {}
};

enum class RuntimeMode {
    Auto,
    BackspaceRewrite,
    SurroundingText,
    Preedit,
    DirectCommit,
};

struct BackspaceRewriteState {
    std::string shownText;
    std::string rawAsciiBuffer;
    bool hasRewrittenCurrentWord = false;
    bool rewriteLock = false;
    bool waitingBackspaceAck = false;
    bool processingQueue = false;
    int expectedBackspaces = 0;
    int seenBackspaces = 0;
    int lateBackspaceBudget = 0;
    std::deque<fcitx::Key> queuedKeys;
    std::unique_ptr<fcitx::EventSourceTime> commitTimer;
    std::unique_ptr<fcitx::EventSourceTime> lateBackspaceTimeoutTimer;
    std::unique_ptr<fcitx::EventSourceTime> ackTimeoutTimer;
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
        processingQueue = false;
        expectedBackspaces = 0;
        seenBackspaces = 0;
        lateBackspaceBudget = 0;
        queuedKeys.clear();
        commitTimer.reset();
        lateBackspaceTimeoutTimer.reset();
        ackTimeoutTimer.reset();
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

struct OpenKeyState : public fcitx::InputContextProperty {
    BackspaceRewriteState rewriteState;
    std::unique_ptr<fcitx::EventSourceTime> modeInfoTimer;

    std::string composing;
    std::string preeditKeyBuffer;
    std::string lastCommitted;
    std::string macroBuffer;
    // For surrounding-text "direct rollback" mode: track the current word we
    // have rewritten so we don't rely on surrounding text contents being fresh.
    std::string rollbackWord;
    std::string rollbackDisplay;
    std::string rollbackRawBuffer;
    std::string rollbackSnapshotWord;
    std::string rollbackSnapshotDisplay;
    std::string rollbackSnapshotRawBuffer;
    bool canReseedRollbackSnapshot = false;
    bool noSeedNextWord = false;
    RuntimeMode mode = RuntimeMode::SurroundingText;
    RuntimeMode autoMode = RuntimeMode::SurroundingText;
    bool manualMode = false;
    bool modeDecided = false;
    std::string program;
    int codeTable = 0;
};

class OpenKeyEngine final : public fcitx::InputMethodEngineV2 {
public:
    explicit OpenKeyEngine(fcitx::Instance *instance);
    ~OpenKeyEngine() override;

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

    fcitx::SimpleInputContextPropertyFactory<OpenKeyState> factory_;

    // Core adapter.
    std::shared_ptr<OpenKeyAdapter> adapter_;
    std::unique_ptr<RemoteRewriteCoordinator> remoteRewriteCoordinator_;

    // Optional bridge to GNOME Shell extension to resolve app id/name when
    // InputContext::program() is empty (common on Wayland for some clients).
    std::unique_ptr<FocusedAppBridge> focusedAppBridge_;

    std::unique_ptr<InputModeHandler> preeditHandler_;
    std::unique_ptr<InputModeHandler> surroundingTextHandler_;
    std::unique_ptr<InputModeHandler> backspaceRewriteHandler_;

    bool debugEnabled() const;
    void loadAppModes();
    void persistAppModes();
    void setAppModeForProgram(fcitx::InputContext *ic,
                              const std::string &program, RuntimeMode mode);
    std::unordered_map<std::string, RuntimeMode> &appModeMapFor(
        fcitx::InputContext *ic);
    void applyConfig();
    void persistConfig();
    bool rewriteServerAvailable();

    OpenKeyState *stateFor(fcitx::InputContext *ic);

    RuntimeMode decideMode(fcitx::InputContext *ic, OpenKeyState &state,
                             bool writeBack = true);
    RuntimeMode firstManualMode() const;
    bool scheduleRemoteRewrite(fcitx::InputContext *ic, OpenKeyState &state,
                                    unsigned int deleteCount,
                                    uint64_t interBackspaceUsec,
                                    uint64_t commitDelayUsec);
    void handleRemoteRewriteDone(fcitx::InputContext *ic, uint64_t sessionId,
                               uint64_t txId);
};

} // namespace openkey
