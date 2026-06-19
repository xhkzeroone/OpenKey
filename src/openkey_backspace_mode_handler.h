#pragma once

#include <functional>
#include <memory>

#include "openkey_platform.h"

namespace openkey {

class OpenKeyAdapter;
struct FcitxOpenKeyState;

struct BackspaceModeDeps {
    std::shared_ptr<OpenKeyAdapter> adapter;
    std::function<bool()> debugEnabled;
    std::function<bool()> enableMacro;
    std::function<bool()> restoreIfWrongSpelling;
    std::function<bool()> enableBackspaceSnapshot;
    std::function<bool()> nonPreeditRemoteEnabled;
};

class BackspaceModeHandler {
public:
    virtual ~BackspaceModeHandler() = default;
    virtual bool handleKey(std::shared_ptr<RewriteContext> context,
                           const KeyInfo &key,
                           FcitxOpenKeyState &state) = 0;
    virtual void handleRemoteCommitAction(
        std::shared_ptr<RewriteContext>, FcitxOpenKeyState &, uint64_t) {}
};

std::unique_ptr<BackspaceModeHandler> createBackspaceRewriteModeHandler(
    BackspaceModeDeps deps);
std::unique_ptr<BackspaceModeHandler> createNonPreeditBackspaceRewriteModeHandler(
    BackspaceModeDeps deps);

} // namespace openkey
