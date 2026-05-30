#include "Engine.h"

// OpenKey core expects these globals to be defined by the embedding application.
// For fcitx5 addon, we define sensible defaults:
// - Vietnamese, Telex, Unicode (precomposed)
// - Prefer "Unicode dựng sẵn" output (vCodeTable = 0)

int vLanguage = 1;
int vInputType = vTelex;
int vFreeMark = 1;
int vCodeTable = 0;
int vSwitchKeyStatus = 0;
int vCheckSpelling = 1;
int vUseModernOrthography = 1;
int vQuickTelex = 0;
int vRestoreIfWrongSpelling = 0;
int vFixRecommendBrowser = 0;
int vUseMacro = 0;
int vUseMacroInEnglishMode = 0;
int vAutoCapsMacro = 0;
int vUseSmartSwitchKey = 0;
int vUpperCaseFirstChar = 0;
int vTempOffSpelling = 0;
int vAllowConsonantZFWJ = 1;
int vQuickStartConsonant = 0;
int vQuickEndConsonant = 0;
int vRememberCode = 0;
int vOtherLanguage = 0;
int vTempOffOpenKey = 0;

