#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include "openkey_config.h"

namespace fcitx {
class Instance;
class SimpleAction;
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
  BackspaceRewriteNoSurr,
  Preedit,
  DirectCommit,
  Surrounding,
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
  std::string pendingConvertedText;
  std::string pendingShownTextAfterCommit;
  bool rawBackspaceAwaitingRelease = false;
  bool suppressBackspaceRelease = false;
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
    pendingConvertedText.clear();
    pendingShownTextAfterCommit.clear();
    rawBackspaceAwaitingRelease = false;
    remotePendingTxId = 0;
    remoteRewritePending = false;
    backspaceSnapshotShownText.clear();
    backspaceSnapshotRawAsciiBuffer.clear();
    backspaceSnapshotHasRewrittenCurrentWord = false;
    canReseedFromBackspaceSnapshot = false;
    restoredFromBackspaceSnapshot = false;
    preserveBackspaceSnapshotAfterBoundaryBackspace = false;
  }
};

struct OpenKeyState : public fcitx::InputContextProperty {
  BackspaceRewriteState rewriteState;
  std::unique_ptr<fcitx::EventSourceTime> modeInfoTimer;
  std::unique_ptr<fcitx::EventSourceTime> lazyResetTimer;

  std::string composing;
  std::string preeditKeyBuffer;
  RuntimeMode mode = RuntimeMode::Preedit;
  RuntimeMode autoMode = RuntimeMode::Preedit;
  bool manualMode = false;
  bool modeDecided = false;
  std::string program;
  std::string windowTitle;
  int codeTable = 0;
  bool isX11Environment = false;
  // Cờ đánh dấu sử dụng tạm Preedit cho từ đầu tiên trên X11 để tránh lỗi hiển thị.
  bool x11FirstWordPreedit = false;
  // SurroundingTextModeHandler states
  std::string macroBuffer;
  std::string rollbackWord;
  std::string rollbackDisplay;
  std::string rollbackRawBuffer;
  std::string rollbackSnapshotWord;
  std::string rollbackSnapshotDisplay;
  std::string rollbackSnapshotRawBuffer;
  std::string lastCommitted;
  bool canReseedRollbackSnapshot = false;
  bool noSeedNextWord = false;
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
  const fcitx::Configuration *
  getSubConfig(const std::string &path) const override;
  void setSubConfig(const std::string &path,
                    const fcitx::RawConfig &config) override;

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
  std::unique_ptr<InputModeHandler> backspaceRewriteHandler_;
  std::unique_ptr<InputModeHandler> surroundingHandler_;
  std::unique_ptr<fcitx::SimpleAction> modeAutoAction_;
  std::unique_ptr<fcitx::SimpleAction> modeNonPreeditAction_;
  std::unique_ptr<fcitx::SimpleAction> modeFixNonPreeditAction_;
  std::unique_ptr<fcitx::SimpleAction> modePreeditAction_;
  std::unique_ptr<fcitx::SimpleAction> modeSurroundingAction_;
  std::unique_ptr<fcitx::SimpleAction> modeDirectAction_;

  // Lưu lại thời điểm gõ phím cuối cùng toàn cục (tránh bị reset khi app gọi activate liên tục)
  uint64_t lastKeyTime_ = 0;
  bool isX11Environment_ = false;

  bool debugEnabled() const;
  void loadAppModes();
  void persistAppModes();
  void setAppModeForProgram(fcitx::InputContext *ic, const std::string &program,
                            RuntimeMode mode);
  std::unordered_map<std::string, RuntimeMode> &
  appModeMapFor(fcitx::InputContext *ic);
  void applyConfig();
  void persistConfig();
  bool rewriteServerAvailable();

  OpenKeyState *stateFor(fcitx::InputContext *ic);

  RuntimeMode decideMode(fcitx::InputContext *ic, OpenKeyState &state,
                         bool writeBack = true);
  RuntimeMode firstManualMode() const;
  void setupModeMenuActions();
  void addModeMenuToStatusArea(fcitx::InputContext *ic);
  void setModeFromMenu(fcitx::InputContext *ic, RuntimeMode mode);
  void refreshModeMenu(fcitx::InputContext *ic);
  bool scheduleRemoteRewrite(fcitx::InputContext *ic, OpenKeyState &state,
                             unsigned int deleteCount,
                             uint64_t interBackspaceUsec,
                             uint64_t commitDelayUsec);
  void handleRemoteRewriteDone(fcitx::InputContext *ic, uint64_t sessionId,
                               uint64_t txId);
};

} // namespace openkey
