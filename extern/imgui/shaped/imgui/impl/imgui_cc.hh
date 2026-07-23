#pragma once

#include <imgui/imgui.h>

#include <clean-core/common/assert.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// clean-core interop for Dear ImGui — the display and editing overloads behind the <imgui/imgui_sc.hh> umbrella.
///
/// Header-only on purpose: the definitions touch clean-core, and imgui is an extern target added below the libs, so a compiled unit here would depend upward on clean-core.
///
/// cc::string converts implicitly to cc::string_view, so the string_view overloads take both.
/// Display goes through cc::string_view: TextUnformatted takes a [begin, end) range, so the view passes untouched — no null terminator, no copy.
/// Editing binds a cc::string directly: InputText grows and shrinks it as the user types, the same contract as upstream's imgui_stdlib InputText(std::string*).
/// Labels and hints stay const char* — imgui reads them up to a '\0', which a cc::string_view has no terminator to give (use cc::string::c_str_materialize()).

namespace ImGui
{
/// Draws the view's bytes verbatim, without a null terminator or a copy.
inline void TextUnformatted(cc::string_view text) { TextUnformatted(text.begin(), text.end()); }

namespace impl
{
/// Bridges an ImGui input widget's resize requests to a cc::string, optionally chaining a caller's callback.
struct input_text_cc_data
{
    cc::string* str = nullptr;
    ImGuiInputTextCallback chain_callback = nullptr;
    void* chain_callback_user_data = nullptr;
};

/// On CallbackResize, grows the string to the length imgui asks for and hands it back the (possibly moved) buffer.
/// The extra reserved byte is imgui's trailing '\0', which sits past size() and outside the string's own length.
inline int input_text_cc_callback(ImGuiInputTextCallbackData* data)
{
    auto* const ud = static_cast<input_text_cc_data*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        cc::string* const str = ud->str;
        str->resize_to_uninitialized(data->BufTextLen);
        str->reserve_back(1);
        data->Buf = str->data();
    }
    else if (ud->chain_callback != nullptr)
    {
        data->UserData = ud->chain_callback_user_data;
        return ud->chain_callback(data);
    }
    return 0;
}

/// Presents `str` to imgui as a null-terminated buffer whose room grows through input_text_cc_callback.
/// buf_size is the current content plus imgui's '\0'; the first keystroke past it triggers a resize.
/// `invoke` is the widget call — InputText / InputTextMultiline / InputTextWithHint — bound to its label and options.
inline bool input_text_cc(cc::string& str,
                          ImGuiInputTextFlags flags,
                          ImGuiInputTextCallback callback,
                          void* user_data,
                          auto&& invoke)
{
    CC_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0, "the cc::string overloads own CallbackResize");
    flags |= ImGuiInputTextFlags_CallbackResize;

    str.c_str_materialize(); // null-terminate the initial content and secure the '\0' byte
    input_text_cc_data cc_data{.str = &str, .chain_callback = callback, .chain_callback_user_data = user_data};
    return invoke(str.data(), size_t(str.size()) + 1, flags, &input_text_cc_callback, &cc_data);
}
} // namespace impl

/// Edits `str` in place, resizing it as the text grows or shrinks.
/// Takes the string by reference — imgui's own convention is a pointer, but a reference reads better on our side.
inline bool InputText(char const* label,
                      cc::string& str,
                      ImGuiInputTextFlags flags = 0,
                      ImGuiInputTextCallback callback = nullptr,
                      void* user_data = nullptr)
{
    return impl::input_text_cc(str, flags, callback, user_data,
                               [&](char* buf, size_t size, ImGuiInputTextFlags f, ImGuiInputTextCallback cb, void* ud)
                               { return InputText(label, buf, size, f, cb, ud); });
}

/// Multi-line variant of InputText for a cc::string.
inline bool InputTextMultiline(char const* label,
                               cc::string& str,
                               ImVec2 const& size = ImVec2(0, 0),
                               ImGuiInputTextFlags flags = 0,
                               ImGuiInputTextCallback callback = nullptr,
                               void* user_data = nullptr)
{
    return impl::input_text_cc(str, flags, callback, user_data,
                               [&](char* buf, size_t bufsize, ImGuiInputTextFlags f, ImGuiInputTextCallback cb, void* ud)
                               { return InputTextMultiline(label, buf, bufsize, size, f, cb, ud); });
}

/// InputText with placeholder hint text, editing a cc::string.
inline bool InputTextWithHint(char const* label,
                              char const* hint,
                              cc::string& str,
                              ImGuiInputTextFlags flags = 0,
                              ImGuiInputTextCallback callback = nullptr,
                              void* user_data = nullptr)
{
    return impl::input_text_cc(str, flags, callback, user_data,
                               [&](char* buf, size_t size, ImGuiInputTextFlags f, ImGuiInputTextCallback cb, void* ud)
                               { return InputTextWithHint(label, hint, buf, size, f, cb, ud); });
}
} // namespace ImGui
