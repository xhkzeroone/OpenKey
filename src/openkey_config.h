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
    OpenKeyMacroItem,
    fcitx::Option<std::string> key{this, "Key", N_("Từ viết tắt"), ""};
    fcitx::Option<std::string> value{this, "Value", N_("Cụm từ thay thế"), ""};
);

FCITX_CONFIGURATION(
    OpenKeyMacroTable,
    fcitx::OptionWithAnnotation<std::vector<OpenKeyMacroItem>,
                                fcitx::ListDisplayOptionAnnotation>
        macros{this,
               "Macro",
               N_("Danh sách gõ tắt"),
               {},
               fcitx::NoConstrain<std::vector<OpenKeyMacroItem>>(),
               fcitx::DefaultMarshaller<std::vector<OpenKeyMacroItem>>(),
               fcitx::ListDisplayOptionAnnotation("Key")};
);

FCITX_CONFIGURATION(
    OpenKeyConfig,
    fcitx::OptionWithAnnotation<InputType, InputTypeI18NAnnotation>
        inputType{this, "InputType", N_("Kiểu gõ"), InputType::Telex};
    fcitx::OptionWithAnnotation<CodeTable, CodeTableI18NAnnotation>
        codeTable{this, "CodeTable", N_("Bảng mã"), CodeTable::Unicode};
    fcitx::Option<bool> checkSpelling{
        this, "CheckSpelling", N_("Kiểm tra chính tả"), true};
    fcitx::Option<bool> enableMacro{this, "EnableMacro", N_("Cho phép gõ tắt"), false};
    fcitx::Option<bool> freeMark{
        this, "FreeMark", N_("Cho phép đặt dấu tự do"), true};
    fcitx::Option<bool> restoreIfWrongSpelling{
        this,
        "RestoreIfWrongSpelling",
        N_("Tự trả phím khi gõ sai chính tả"),
        true};
    fcitx::Option<bool> enableBackspaceSnapshot{
        this,
        "EnableBackspaceSnapshot",
        N_("Cho phép Backspace quay lại sửa từ vừa gõ"),
        true};
    fcitx::Option<bool> enableRawBackspaceRewrite{
        this,
        "EnableRawBackspaceRewrite",
        N_("Bật xoá chậm rãi (xoá từng ký tự raw)"),
        false};
    fcitx::Option<bool> enableSurroundingFastPath{
        this,
        "EnableSurroundingFastPath",
        N_("Dùng Surrounding để xoá chữ không dấu (nhanh hơn)"),
        true};
    fcitx::Option<bool> useModernOrthography{
        this, "UseModernOrthography", N_("Đặt dấu oà, uý (thay vì òa, úy)"), false};
    fcitx::Option<bool> literalWAtWordStart{
        this,
        "LiteralWAtWordStart",
        N_("Giữ nguyên W ở đầu từ"),
        true};
    fcitx::Option<bool> allowConsonantZFWJ{
        this, "AllowConsonantZFWJ", N_("Cho phép gõ phụ âm Z, F, W, J"), false};
    fcitx::SubConfigOption macroEditor{
        this, "MacroEditor", N_("Trình chỉnh sửa gõ tắt"), "fcitx://config/addon/openkey/openkey-macro"};
    fcitx::Option<bool> debug{this, "Debug", N_("Ghi nhật ký gỡ lỗi (debug)"), false};
    fcitx::KeyListOption switchModeKey{
        this,
        "SwitchModeKey",
        N_("Phím tắt chuyển chế độ gõ"),
        {fcitx::Key("Alt+space")},
        fcitx::KeyListConstrain(
            fcitx::KeyConstrainFlag::AllowModifierLess)};);

} // namespace openkey
