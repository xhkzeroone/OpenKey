#pragma once

#include <memory>
#include <functional>
#include <string_view>

#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>

#include "openkey_platform.h"

namespace openkey {

KeyInfo keyInfoFromFcitxEvent(const fcitx::KeyEvent &event);
fcitx::Key keyInfoToFcitxKey(const KeyInfo &key);

class FcitxIMContext final : public IMContext {
public:
    FcitxIMContext(fcitx::InputContext *ic, fcitx::Instance *instance,
                   std::weak_ptr<void> lifetime);
    ~FcitxIMContext() override;

    void commitString(std::string_view text) override;
    void deleteSurroundingText(int offsetChars, unsigned int nChars) override;
    void forwardKey(const KeyInfo &key, bool isRelease) override;
    void forwardKeyPressAndRelease(const KeyInfo &key) override;

    void setPreedit(std::string_view text, int cursorBytePos) override;
    void clearPreedit() override;
    bool supportsPreedit() const override;

    SurroundingTextInfo getSurroundingText() const override;
    bool supportsSurroundingText() const override;

    TimerHandle scheduleOnce(uint64_t delayUsec,
                             std::function<void()> cb) override;
    void cancelTimer(TimerHandle &handle) override;

    std::string program() const override;
    bool isX11() const override;
    bool isPasswordField() const override;
    bool isLegacyFrontend() const override;

    fcitx::InputContext *nativeInputContext() const { return ic_; }

private:
    fcitx::InputContext *ic_ = nullptr;
    fcitx::Instance *instance_ = nullptr;
    std::weak_ptr<void> lifetime_;
};

class FcitxRewriteContext final : public RewriteContext {
public:
    using BackspaceSender = std::function<BackspaceMethod(int, uint64_t)>;
    using RemoteScheduler =
        std::function<bool(unsigned int, uint64_t, uint64_t)>;
    using WordTracker = std::function<bool(std::string_view, bool)>;
    using AutocompleteDetector = std::function<bool(std::string_view)>;

    FcitxRewriteContext(std::shared_ptr<FcitxIMContext> context,
                        BackspaceSender backspaceSender,
                        RemoteScheduler remoteScheduler,
                        WordTracker wordTracker,
                        AutocompleteDetector autocompleteDetector)
        : context_(std::move(context)),
          backspaceSender_(std::move(backspaceSender)),
          remoteScheduler_(std::move(remoteScheduler)),
          wordTracker_(std::move(wordTracker)),
          autocompleteDetector_(std::move(autocompleteDetector)) {}

    IMContext &imContext() override { return *context_; }
    BackspaceMethod sendBackspaces(int count, uint64_t interKeyUsec) override {
        return backspaceSender_ ? backspaceSender_(count, interKeyUsec)
                                : BackspaceMethod::None;
    }
    bool scheduleRemoteRewrite(unsigned int deleteCount,
                               uint64_t interBackspaceUsec,
                               uint64_t commitDelayUsec) override {
        return remoteScheduler_ ? remoteScheduler_(deleteCount,
                                                   interBackspaceUsec,
                                                   commitDelayUsec)
                                : false;
    }
    bool trackedWordStillBeforeCursor(std::string_view shownText,
                                      bool requireSurroundingText) override {
        return wordTracker_ ? wordTracker_(shownText, requireSurroundingText)
                            : false;
    }
    bool looksLikeAutocomplete(std::string_view shownText) override {
        return autocompleteDetector_ ? autocompleteDetector_(shownText) : false;
    }

    fcitx::InputContext *nativeInputContext() const {
        return context_ ? context_->nativeInputContext() : nullptr;
    }

private:
    std::shared_ptr<FcitxIMContext> context_;
    BackspaceSender backspaceSender_;
    RemoteScheduler remoteScheduler_;
    WordTracker wordTracker_;
    AutocompleteDetector autocompleteDetector_;
};

} // namespace openkey
