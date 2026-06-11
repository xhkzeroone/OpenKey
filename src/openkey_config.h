#pragma once

#include <string>

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

namespace openkey {

enum class InputType { Telex, VNI, SimpleTelex1, SimpleTelex2 };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(InputType, N_("Telex"), N_("VNI"),
                                 N_("Simple Telex 1"), N_("Simple Telex 2"));

enum class CodeTable {
    Unicode,
    TCVN3,
    VNIWindows,
    UnicodeCompound,
    VietnameseLocaleCP1258,
};
FCITX_CONFIG_ENUM_NAME_WITH_I18N(CodeTable, N_("Unicode"),
                                 N_("TCVN3 (ABC)"),
                                 N_("VNI Windows"),
                                 N_("Unicode tổ hợp"),
                                 N_("Vietnamese Locale CP 1258"));

FCITX_CONFIGURATION(
    OpenKeyConfig,
    fcitx::OptionWithAnnotation<InputType, InputTypeI18NAnnotation>
        inputType{this, "InputType", N_("Kiểu gõ"), InputType::Telex};
    fcitx::OptionWithAnnotation<CodeTable, CodeTableI18NAnnotation>
        codeTable{this, "CodeTable", N_("Bảng mã đầu ra"), CodeTable::Unicode};
    fcitx::Option<bool> checkSpelling{
        this, "CheckSpelling", N_("Kiểm tra chính tả"), true};
    fcitx::Option<bool> enableMacro{this, "EnableMacro", N_("Gõ tắt"), false};
    fcitx::Option<bool> freeMark{
        this, "FreeMark", N_("Xử lý W ở đầu từ"), true};
    fcitx::Option<bool> restoreIfWrongSpelling{
        this,
        "RestoreIfWrongSpelling",
        N_("Tự trả phím khi từ không hợp lệ"),
        false};
    fcitx::Option<bool> useModernOrthography{
        this, "UseModernOrthography", N_("Dùng oà, uý thay vì òa, úy"), true};
    fcitx::Option<bool> allowConsonantZFWJ{
        this, "AllowConsonantZFWJ", N_("Cho phép gõ tự do hơn"), true};
    fcitx::Option<std::string> macroFile{
        this, "MacroFile", N_("Đường dẫn file gõ tắt"), ""};
    fcitx::Option<bool> debug{this, "Debug", N_("Ghi log debug"), false};
    fcitx::KeyListOption switchModeKey{
        this,
        "SwitchModeKey",
        N_("Phím chuyển chế độ gõ"),
        {fcitx::Key("Alt+space")},
        fcitx::KeyListConstrain(
            fcitx::KeyConstrainFlag::AllowModifierLess)};);

} // namespace openkey
