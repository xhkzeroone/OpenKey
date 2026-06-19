#pragma once

#include <functional>
#include <memory>

#include "openkey_platform.h"
#include "openkey_state.h"

namespace openkey {

class OpenKeyAdapter;

class TextModeHandler {
public:
    virtual ~TextModeHandler() = default;
    virtual bool handleKey(IMContext &context, const KeyInfo &key,
                           OpenKeyTextState &state) = 0;
    virtual void reset(OpenKeyTextState &) {}
};

struct TextModeHandlerDeps {
    std::shared_ptr<OpenKeyAdapter> adapter;
    std::function<bool()> enableMacro;
    std::function<bool()> restoreIfWrongSpelling;
    std::function<bool()> enableBackspaceSnapshot;
};

class PreeditModeHandler final : public TextModeHandler {
public:
    explicit PreeditModeHandler(TextModeHandlerDeps deps);

    bool handleKey(IMContext &context, const KeyInfo &key,
                   OpenKeyTextState &state) override;
    void reset(OpenKeyTextState &state) override;

private:
    void updatePreeditUI(IMContext &context, const OpenKeyTextState &state);
    void commitAndClearPreedit(IMContext &context, OpenKeyTextState &state);

    TextModeHandlerDeps deps_;
};

class SurroundingTextModeHandler final : public TextModeHandler {
public:
    explicit SurroundingTextModeHandler(TextModeHandlerDeps deps);

    bool handleKey(IMContext &context, const KeyInfo &key,
                   OpenKeyTextState &state) override;
    void reset(OpenKeyTextState &state) override;

private:
    void clearRollbackSnapshot(OpenKeyTextState &state) const;
    void rememberRollbackSnapshot(OpenKeyTextState &state) const;
    bool restoreRollbackSnapshotAfterBoundary(OpenKeyTextState &state) const;

    TextModeHandlerDeps deps_;
};

} // namespace openkey
