#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

namespace fcitx {
class Instance;
}

namespace openkey {

struct OpenKeyState;
class OpenKeyAdapter;

class InputModeHandler {
public:
    virtual ~InputModeHandler() = default;
    virtual bool handleKey(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                           OpenKeyState &state) = 0;
    virtual void reset(OpenKeyState &) {}
};

enum class ModeOverride {
    Auto,
    ForceBackspaceRewrite,
    ForceSurroundingText,
    ForcePreedit,
    DirectCommit
};
FCITX_CONFIG_ENUM_NAME_WITH_I18N(ModeOverride, N_("Auto"),
                                 N_("Force Backspace Rewrite"),
                                 N_("Force Surrounding Text"),
                                 N_("Force Preedit"), N_("Direct Commit"));

enum class InputType { Telex, VNI, SimpleTelex1, SimpleTelex2 };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(InputType, N_("Telex"), N_("VNI"),
                                 N_("Simple Telex 1"), N_("Simple Telex 2"));

enum class CodeTable { Unicode, TCVN3, VNIWindows };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(CodeTable, N_("Unicode"),
                                 N_("TCVN3 (ABC)"),
                                 N_("VNI Windows"));

FCITX_CONFIGURATION(OpenKeyConfig,
                    fcitx::KeyListOption
                        switchModeKey{this,
                                      "SwitchModeKey",
                                      N_("Switch composition mode hotkey"),
                                      {fcitx::Key("Alt+Super+space")},
                                      fcitx::KeyListConstrain(
                                          fcitx::KeyConstrainFlag::
                                              AllowModifierLess)};
                    fcitx::OptionWithAnnotation<ModeOverride,
                                                ModeOverrideI18NAnnotation>
                        mode{this,
                             "Mode",
                             N_("Composition Mode"),
                             ModeOverride::Auto};
                    fcitx::OptionWithAnnotation<InputType,
                                                InputTypeI18NAnnotation>
                        inputType{this,
                                  "InputType",
                                  N_("Input Type"),
                                  InputType::Telex};
                    fcitx::OptionWithAnnotation<CodeTable,
                                                CodeTableI18NAnnotation>
                        codeTable{this,
                                  "CodeTable",
                                  N_("Output Code Table"),
                                  CodeTable::Unicode};
                    fcitx::Option<bool> freeMark{this,
                                                 "FreeMark",
                                                 N_("Free Mark"),
                                                 true};
                    fcitx::Option<bool>
                        checkSpelling{this,
                                      "CheckSpelling",
                                      N_("Spell Check"),
                                      true};
                    fcitx::Option<bool>
                        useModernOrthography{this,
                                             "UseModernOrthography",
                                             N_("Use modern orthography (oà, uý)"),
                                             true};
                    fcitx::Option<bool>
                        quickTelex{this,
                                  "QuickTelex",
                                  N_("Quick Telex (cc=ch, gg=gi, ...)"),
                                  false};
                    fcitx::Option<bool>
                        restoreIfWrongSpelling{this,
                                               "RestoreIfWrongSpelling",
                                               N_("Restore key on wrong spelling"),
                                               false};
                    fcitx::Option<bool>
                        fixRecommendBrowser{this,
                                            "FixRecommendBrowser",
                                            N_("Fix browser/Excel quirks"),
                                            true};
                    fcitx::Option<bool>
                        upperCaseFirstChar{this,
                                           "UpperCaseFirstChar",
                                           N_("Uppercase first letter after sentence"),
                                           false};
                    fcitx::Option<bool>
                        allowConsonantZFWJ{this,
                                           "AllowConsonantZFWJ",
                                           N_("Allow consonants z, w, j, f"),
                                           true};
                    fcitx::Option<bool>
                        quickStartConsonant{this,
                                           "QuickStartConsonant",
                                           N_("Quick start consonant (f->ph, j->gi, w->qu)"),
                                           false};
                    fcitx::Option<bool>
                        quickEndConsonant{this,
                                         "QuickEndConsonant",
                                         N_("Quick end consonant (g->ng, h->nh, k->ch)"),
                                         false};
                    fcitx::Option<bool>
                        otherLanguage{this,
                                     "OtherLanguage",
                                     N_("Disable Vietnamese in other languages"),
                                     false};
                    fcitx::Option<bool>
                        tempOffSpelling{this,
                                        "TempOffSpelling",
                                        N_("Temporarily toggle spell check with Ctrl"),
                                        false};
                    fcitx::Option<bool>
                        tempOffOpenKey{this,
                                       "TempOffOpenKey",
                                       N_("Temporarily toggle OpenKey with Alt"),
                                       false};
                    fcitx::Option<bool>
                        enableMacro{this,
                                    "EnableMacro",
                                    N_("Enable macros (shortcuts)"),
                                    false};
                    fcitx::Option<bool>
                        autoCapsMacro{this,
                                     "AutoCapsMacro",
                                     N_("Auto-capitalize macro results"),
                                     false};
                    fcitx::Option<std::string>
                        macroFile{this,
                                  "MacroFile",
                                  N_("Macro file path"),
                                  ""};
                    fcitx::Option<bool>
                        debug{this,
                              "Debug",
                              N_("Debug Logging"),
                              false};
                    fcitx::Option<std::string>
                        surroundingTextBlacklist{this,
                                                 "SurroundingTextBlacklist",
                                                 N_("Surrounding Text "
                                                    "Blacklist"),
                                                 ""};);

enum class RuntimeMode {
    BackspaceRewrite,
    SurroundingText,
    Preedit,
    DirectCommit,
};

struct OpenKeyState : public fcitx::InputContextProperty {
    // Backspace-rewrite mode state.
    std::string rawBuffer;
    std::string shownText;
    bool hasRewrittenCurrentWord = false;
    bool rewriteLock = false;
    bool isInjecting = false;
    uint64_t injectingUntilUsec = 0;
    std::vector<fcitx::Key> pendingKeys;
    uint64_t lastPhysicalKeyUsec = 0;
    std::unique_ptr<fcitx::EventSourceTime> rewriteTimer;
    std::unique_ptr<fcitx::EventSourceTime> commitTimer;
    std::unique_ptr<fcitx::EventSourceTime> modeInfoTimer;
    std::string pendingConvertedText;
    fcitx::Key pendingBoundaryKey;
    bool hasPendingBoundaryKey = false;

    std::string composing;
    std::string lastCommitted;
    std::string macroBuffer;
    // For surrounding-text "direct rollback" mode: track the current word we
    // have rewritten so we don't rely on surrounding text contents being fresh.
    std::string rollbackWord;
    std::string rollbackDisplay;
    bool noSeedNextWord = false;
    RuntimeMode mode = RuntimeMode::SurroundingText;
    RuntimeMode autoMode = RuntimeMode::SurroundingText;
    bool manualMode = false;
    bool modeDecided = false;
    bool surroundingReliable = true;
    fcitx::CapabilityFlags lastCapability;
    std::string program;
    int surroundingFailures = 0;
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

private:
    fcitx::Instance *instance_;
    std::shared_ptr<void> lifetime_;
    OpenKeyConfig config_;

    std::unordered_set<std::string> surroundingBlacklist_;
    bool blacklistDirty_ = false;

    fcitx::SimpleInputContextPropertyFactory<OpenKeyState> factory_;

    // Core adapter.
    std::shared_ptr<OpenKeyAdapter> adapter_;

    std::unique_ptr<InputModeHandler> backspaceRewriteHandler_;

    bool debugEnabled() const;
    void rebuildBlacklist();
    void applyConfig();
    void persistConfig();
    void addProgramToBlacklist(const std::string &program);

    OpenKeyState *stateFor(fcitx::InputContext *ic);

    RuntimeMode decideMode(fcitx::InputContext *ic, OpenKeyState &state);
    bool handlePreedit(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                       OpenKeyState &state);
    bool handleSurroundingText(fcitx::InputContext *ic, fcitx::KeyEvent &event,
                               OpenKeyState &state);
    bool handleBackspaceRewrite(fcitx::InputContext *ic,
                                fcitx::KeyEvent &event, OpenKeyState &state);

    void updatePreeditUI(fcitx::InputContext *ic, const OpenKeyState &state);
    void commitAndClearPreedit(fcitx::InputContext *ic, OpenKeyState &state);
};

} // namespace openkey
