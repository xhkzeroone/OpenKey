#pragma once

#include <string>

namespace openkey {

struct OpenKeyTextState {
    std::string composing;
    std::string preeditKeyBuffer;
    std::string lastCommitted;
    std::string macroBuffer;
    std::string rollbackWord;
    std::string rollbackDisplay;
    std::string rollbackRawBuffer;
    std::string rollbackSnapshotWord;
    std::string rollbackSnapshotDisplay;
    std::string rollbackSnapshotRawBuffer;
    bool canReseedRollbackSnapshot = false;
    bool noSeedNextWord = false;
    std::string program;
    int codeTable = 0;
};

} // namespace openkey
