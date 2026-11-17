#include "gpu_capture.hh"

#include <clean-core/common/log.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/conversion.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/context/context.hh>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace sg::impl
{
namespace
{
// ---- RenderDoc's in-application API, the slice of renderdoc_app.h (MIT) this file calls, as of API 1.6.0
// Every member is a pointer, in the header's order; the ones never called are only there to keep the offsets.

#if defined(_WIN32)
#define SG_RENDERDOC_CC __cdecl
#else
#define SG_RENDERDOC_CC
#endif

constexpr int k_renderdoc_api_1_6_0 = 10600;

using renderdoc_unused = void*;
using renderdoc_get_api = int(SG_RENDERDOC_CC*)(int version, void** api);
using renderdoc_set_capture_file_path_template = void(SG_RENDERDOC_CC*)(char const* path_template);
using renderdoc_get_capture = u32(SG_RENDERDOC_CC*)(u32 index, char* path, u32* path_length, u64* timestamp);
using renderdoc_get_num_captures = u32(SG_RENDERDOC_CC*)();
using renderdoc_start_frame_capture = void(SG_RENDERDOC_CC*)(void* device, void* window);
using renderdoc_end_frame_capture = u32(SG_RENDERDOC_CC*)(void* device, void* window);

struct renderdoc_api_1_6_0
{
    renderdoc_unused get_api_version;
    renderdoc_unused set_capture_option_u32;
    renderdoc_unused set_capture_option_f32;
    renderdoc_unused get_capture_option_u32;
    renderdoc_unused get_capture_option_f32;
    renderdoc_unused set_focus_toggle_keys;
    renderdoc_unused set_capture_keys;
    renderdoc_unused get_overlay_bits;
    renderdoc_unused mask_overlay_bits;
    renderdoc_unused remove_hooks;
    renderdoc_unused unload_crash_handler;
    renderdoc_set_capture_file_path_template set_capture_file_path_template;
    renderdoc_unused get_capture_file_path_template;
    renderdoc_get_num_captures get_num_captures;
    renderdoc_get_capture get_capture;
    renderdoc_unused trigger_capture;
    renderdoc_unused is_target_control_connected;
    renderdoc_unused launch_replay_ui;
    renderdoc_unused set_active_window;
    renderdoc_start_frame_capture start_frame_capture;
    renderdoc_unused is_frame_capturing;
    renderdoc_end_frame_capture end_frame_capture;
};

// ---- PIX's programmatic capture, what pix3_win.h's PIXBeginCapture2 and PIXEndCapture call in WinPixGpuCapturer.dll
// The capturer takes the parameters alone: the GPU-or-timing flag picks the library on the caller's side, in pix3_win.h.

#if defined(_WIN32)
/// pix3.h's `PIXCaptureParameters`, a union whose GPU member is the file name alone; the rest only pads it.
struct pix_capture_parameters
{
    wchar_t const* file_name = nullptr;
    u64 padding[8] = {};
};

using pix_begin_capture = HRESULT(WINAPI*)(pix_capture_parameters const* parameters);
using pix_end_capture = HRESULT(WINAPI*)();
#endif

enum class tool
{
    none,
    renderdoc,
    pix,
};

struct capture_state
{
    tool which = tool::none;
    /// The number of advances after which the capture begins: epoch::first plus this is the captured epoch.
    u64 advances = 1;
    cc::string file;

    renderdoc_api_1_6_0* renderdoc = nullptr;
#if defined(_WIN32)
    pix_begin_capture pix_begin = nullptr;
    pix_end_capture pix_end = nullptr;
#endif

    /// The context the capture belongs to: the first to open the epoch asked for, and none other after it.
    sg::context const* owner = nullptr;
    bool is_capturing = false;
    bool is_done = false;
};

cc::mutex<capture_state>& state()
{
    // Never destroyed: a context torn down during static destruction may still end its capture.
    static auto* const s = new cc::mutex<capture_state>();
    return *s;
}

#if defined(_WIN32)
[[nodiscard]] HMODULE load_module(wchar_t const* name, cc::string_view under_program_files)
{
    if (auto const loaded = GetModuleHandleW(name); loaded != nullptr)
        return loaded;
    if (auto const on_path = LoadLibraryW(name); on_path != nullptr)
        return on_path;
    auto const root = cc::environment_variable("ProgramFiles");
    if (!root.has_value())
        return nullptr;
    auto path = cc::utf8_to_utf16(cc::format("{}\\{}", root.value(), under_program_files));
    path.push_back(u'\0');
    return LoadLibraryW(reinterpret_cast<wchar_t const*>(path.data()));
}

/// The newest PIX install, `%ProgramFiles%\Microsoft PIX\<version>\`, as pix3_win.h's loader picks it.
[[nodiscard]] HMODULE load_pix()
{
    if (auto const loaded = GetModuleHandleW(L"WinPixGpuCapturer.dll"); loaded != nullptr)
        return loaded;
    auto const root = cc::environment_variable("ProgramFiles");
    if (!root.has_value())
        return nullptr;
    auto pattern = cc::utf8_to_utf16(cc::format("{}\\Microsoft PIX\\*", root.value()));
    pattern.push_back(u'\0');
    auto found = WIN32_FIND_DATAW{};
    auto const search = FindFirstFileW(reinterpret_cast<wchar_t const*>(pattern.data()), &found);
    if (search == INVALID_HANDLE_VALUE)
        return nullptr;
    auto newest = cc::string();
    do
    {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || found.cFileName[0] == L'.')
            continue;
        auto const name = cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(found.cFileName),
                                                                     isize(wcslen(found.cFileName))));
        // versions are dotted numbers of one width, so the greatest string is the newest
        if (name > newest)
            newest = name;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (newest.empty())
        return nullptr;
    auto path = cc::utf8_to_utf16(cc::format("{}\\Microsoft PIX\\{}\\WinPixGpuCapturer.dll", root.value(), newest));
    path.push_back(u'\0');
    return LoadLibraryW(reinterpret_cast<wchar_t const*>(path.data()));
}
#endif

[[nodiscard]] renderdoc_api_1_6_0* load_renderdoc()
{
    auto get_api = renderdoc_get_api(nullptr);
#if defined(_WIN32)
    if (auto const module = load_module(L"renderdoc.dll", "RenderDoc\\renderdoc.dll"); module != nullptr)
        get_api
            = reinterpret_cast<renderdoc_get_api>(reinterpret_cast<void*>(GetProcAddress(module, "RENDERDOC_GetAPI")));
#elif defined(__linux__)
    auto* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr)
        module = dlopen("librenderdoc.so", RTLD_NOW);
    if (module != nullptr)
        get_api = reinterpret_cast<renderdoc_get_api>(dlsym(module, "RENDERDOC_GetAPI"));
#endif
    if (get_api == nullptr)
        return nullptr;
    auto* api = static_cast<void*>(nullptr);
    if (get_api(k_renderdoc_api_1_6_0, &api) != 1)
        return nullptr;
    return static_cast<renderdoc_api_1_6_0*>(api);
}

void attach(capture_state& s)
{
    auto const named = cc::environment_variable("SC_SG_CAPTURE");
    if (!named.has_value())
        return;
    auto const& name = named.value();
    if (auto const epoch = cc::environment_variable("SC_SG_CAPTURE_EPOCH"); epoch.has_value())
    {
        auto const advances = cc::from_string<u64>(epoch.value());
        if (!advances.has_value())
        {
            CC_LOG_WARNING("SC_SG_CAPTURE_EPOCH is '{}', which is no count of advances; nothing is captured",
                           epoch.value());
            return;
        }
        s.advances = advances.value();
    }
    if (s.advances == 0)
    {
        CC_LOG_WARNING("SC_SG_CAPTURE_EPOCH is 0, but the epoch a context opens at creation cannot be captured; "
                       "nothing is captured");
        return;
    }
    if (auto const file = cc::environment_variable("SC_SG_CAPTURE_FILE"); file.has_value())
        s.file = file.value();

    if (name == "renderdoc")
    {
        s.renderdoc = load_renderdoc();
        if (s.renderdoc == nullptr)
        {
            CC_LOG_WARNING("SC_SG_CAPTURE asks for RenderDoc, but its library could not be loaded; nothing is "
                           "captured");
            return;
        }
        if (!s.file.empty())
            s.renderdoc->set_capture_file_path_template(s.file.c_str_materialize());
        s.which = tool::renderdoc;
        return;
    }
    if (name == "pix")
    {
#if defined(_WIN32)
        auto const module = load_pix();
        if (module != nullptr)
        {
            s.pix_begin = reinterpret_cast<pix_begin_capture>(
                reinterpret_cast<void*>(GetProcAddress(module, "BeginProgrammaticGpuCapture")));
            s.pix_end = reinterpret_cast<pix_end_capture>(
                reinterpret_cast<void*>(GetProcAddress(module, "EndProgrammaticGpuCapture")));
        }
        if (s.pix_begin == nullptr || s.pix_end == nullptr)
        {
            CC_LOG_WARNING("SC_SG_CAPTURE asks for PIX, but WinPixGpuCapturer.dll could not be loaded; nothing "
                           "is captured");
            return;
        }
        if (s.file.empty())
            s.file = "sg-capture.wpix";
        s.which = tool::pix;
        return;
#else
        {
            CC_LOG_WARNING("SC_SG_CAPTURE asks for PIX, which exists on Windows alone; nothing is captured");
            return;
        }
#endif
    }
    CC_LOG_WARNING("SC_SG_CAPTURE names '{}', which is neither 'renderdoc' nor 'pix'; nothing is captured", name);
}

void begin(capture_state& s, sg::context const& ctx)
{
    if (s.which == tool::pix && ctx.backend() != sg::backend_kind::dx12)
    {
        s.is_done = true;
        {
            CC_LOG_WARNING("SC_SG_CAPTURE asks for PIX, which captures dx12 alone; nothing is captured");
            return;
        }
    }
    s.owner = &ctx;
    s.is_capturing = true;
    if (s.which == tool::renderdoc)
        s.renderdoc->start_frame_capture(nullptr, nullptr);
#if defined(_WIN32)
    if (s.which == tool::pix)
    {
        auto file = cc::utf8_to_utf16(s.file);
        file.push_back(u'\0');
        auto const parameters = pix_capture_parameters{.file_name = reinterpret_cast<wchar_t const*>(file.data())};
        if (auto const hr = s.pix_begin(&parameters); FAILED(hr))
        {
            s.is_capturing = false;
            s.is_done = true;
            {
                CC_LOG_WARNING("PIX refused to begin a capture: HRESULT {:#x}", u32(hr));
                return;
            }
        }
    }
#endif
}

void end(capture_state& s)
{
    s.is_capturing = false;
    s.is_done = true;
    if (s.which == tool::renderdoc)
    {
        if (s.renderdoc->end_frame_capture(nullptr, nullptr) != 1)
        {
            CC_LOG_WARNING("RenderDoc captured nothing of the epoch asked for");
            return;
        }
        auto const count = s.renderdoc->get_num_captures();
        auto length = u32(0);
        if (count == 0 || s.renderdoc->get_capture(count - 1, nullptr, &length, nullptr) != 1)
        {
            CC_LOG_INFO("RenderDoc captured the epoch");
            return;
        }
        auto path = cc::string::create_filled(isize(length), '\0');
        (void)s.renderdoc->get_capture(count - 1, path.data(), &length, nullptr);
        // the length counts the terminating zero
        path.resize_down_to(cc::max(isize(0), isize(length) - 1));
        {
            CC_LOG_INFO("RenderDoc captured the epoch into {}", path);
            return;
        }
    }
#if defined(_WIN32)
    if (s.which == tool::pix)
    {
        if (auto const hr = s.pix_end(); FAILED(hr))
        {
            CC_LOG_WARNING("PIX refused to end its capture: HRESULT {:#x}", u32(hr));
            return;
        }
        CC_LOG_INFO("PIX captured the epoch into {}", s.file);
    }
#endif
}
} // namespace
} // namespace sg::impl

void sg::impl::attach_gpu_capture()
{
    static auto const once = []
    {
        state().lock([](capture_state& s) { attach(s); });
        return true;
    }();
    (void)once;
}

void sg::impl::gpu_capture_on_epoch_opened(context const& ctx, epoch opened)
{
    state().lock(
        [&](capture_state& s)
        {
            if (s.which == tool::none || s.is_done || (s.owner != nullptr && s.owner != &ctx))
                return;
            if (s.is_capturing)
                return end(s);
            if (u64(opened) == u64(epoch::first) + s.advances)
                begin(s, ctx);
        });
}

void sg::impl::gpu_capture_on_shutdown(context const& ctx)
{
    state().lock(
        [&](capture_state& s)
        {
            if (s.is_capturing && s.owner == &ctx)
                end(s);
        });
}
