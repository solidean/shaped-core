#include "../shaders/shader_fixtures.hh"
#include "sg_backends.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/conversion.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/thorough.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_runtime.hh>

using namespace cc::primitive_defines;

// The tests of the Agility SDK's own features, in a process of their own wherever this one keeps to the system runtime.
//
// On Intel's driver, a process that loaded the Agility SDK compiles every shader anew, on dx12 and vulkan alike.
// docs/bugs-external/intel-agility-process-recompiles-acceleration-structure-kernels has the reproduction.
// So there every process of ours keeps to the system runtime unless asked otherwise (dx12_runtime.hh).
// The launcher below then starts this binary again on the Agility SDK, running only the invocables listed here.
// Everywhere else every driver runs on the Agility SDK already, and the launcher skips.

namespace
{
namespace dx12 = sg::backend::dx12;

// The invocables whose dx12 path needs the Agility SDK: Shader Model 6.9, shader execution reordering or opacity micromaps.
// On the system runtime each skips or takes its fallback, so one missing from here has no Intel run of that path.
constexpr char const* k_agility_tests[] = {
    "sg - a hit object and a regrouped trace through SGL's binary trace what DXC's trace",
    "sg - an opacity micromap's opaque micro-triangles are hit without asking the any-hit decision",
    "sg - an opacity micromap's special index misses a whole triangle the any-hit decision would accept",
    "sg - an SGL raygen that reorders its invocations traces what it would have traced",
    "sg - an SGL trace that reorders by its hit traces what it would have traced",
    "sg - an SGL hit object says what its closest hit is then handed",
};

constexpr char const* k_agility_alias = "sg dx12 agility runtime";
constexpr char const* k_text_driver = "sg dx12 agility backend";
constexpr char const* k_native_driver = "sg dx12 agility native-dxil backend";
constexpr char const* k_process_marker = "SG_TEST_AGILITY_PROCESS";

[[nodiscard]] bool is_agility_process()
{
    auto const value = cc::environment_variable(k_process_marker);
    return value.has_value() && value.value() == "1";
}

[[nodiscard]] cc::vector<wchar_t> wide(cc::string_view s)
{
    auto const utf16 = cc::utf8_to_utf16(s);
    auto out = cc::vector<wchar_t>();
    out.reserve(utf16.size() + 1);
    for (auto const c : utf16)
        out.push_back(wchar_t(c));
    out.push_back(L'\0');
    return out;
}

/// One argument as CommandLineToArgvW reads it back.
[[nodiscard]] cc::string quote_argument(cc::string_view arg)
{
    auto needs_quotes = arg.empty();
    for (auto const c : arg)
        needs_quotes |= c == ' ' || c == '\t' || c == '"';
    if (!needs_quotes)
        return cc::string(arg);
    auto out = cc::string("\"");
    auto backslashes = 0;
    for (auto const c : arg)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }
        for (auto i = 0; i < (c == '"' ? 2 * backslashes + 1 : backslashes); ++i)
            out += '\\';
        backslashes = 0;
        out += c;
    }
    for (auto i = 0; i < 2 * backslashes; ++i)
        out += '\\';
    out += '"';
    return out;
}

/// This process's environment block, with `overrides` (`NAME=value`) replacing any variable of the same name.
[[nodiscard]] cc::vector<wchar_t> environment_with(cc::span<cc::string const> overrides)
{
    auto const names_match = [](wchar_t const* entry, cc::string_view assignment)
    {
        for (auto i = isize(0); i < assignment.size(); ++i)
        {
            auto const a = assignment[i];
            auto e = entry[i];
            if (e == L'\0')
                return false;
            if (e >= L'a' && e <= L'z')
                e = wchar_t(e - L'a' + L'A');
            if (wchar_t(a >= 'a' && a <= 'z' ? a - 'a' + 'A' : a) != e)
                return false;
            if (a == '=')
                return true;
        }
        return false;
    };

    auto block = cc::vector<wchar_t>();
    auto* const strings = GetEnvironmentStringsW();
    for (auto const* entry = strings; entry != nullptr && *entry != L'\0'; entry += wcslen(entry) + 1)
    {
        auto overridden = false;
        for (auto const& o : overrides)
            overridden |= names_match(entry, o);
        if (overridden)
            continue;
        for (auto const* c = entry; *c != L'\0'; ++c)
            block.push_back(*c);
        block.push_back(L'\0');
    }
    if (strings != nullptr)
        FreeEnvironmentStringsW(strings);
    for (auto const& o : overrides)
        for (auto const c : wide(o))
            block.push_back(c);
    block.push_back(L'\0');
    return block;
}

[[nodiscard]] cc::string executable_path()
{
    wchar_t module[MAX_PATH] = {};
    auto const length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    auto utf16 = cc::vector<char16_t>();
    for (auto i = DWORD(0); i < length; ++i)
        utf16.push_back(char16_t(module[i]));
    return cc::utf16_to_utf8(utf16);
}

[[nodiscard]] HANDLE inheritable_file(cc::string const& path)
{
    auto security = SECURITY_ATTRIBUTES{.nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
    return CreateFileW(wide(path).data(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

/// The last `max_bytes` of a text file, or nothing where it cannot be read.
[[nodiscard]] cc::string file_tail(cc::string const& path, isize max_bytes)
{
    auto const file = CreateFileW(wide(path).data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    auto size = LARGE_INTEGER{};
    GetFileSizeEx(file, &size);
    auto const take = size.QuadPart < max_bytes ? isize(size.QuadPart) : max_bytes;
    auto offset = LARGE_INTEGER{.QuadPart = size.QuadPart - take};
    SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
    auto out = cc::string();
    out.resize_to_defaulted(take);
    auto read = DWORD(0);
    ReadFile(file, out.data(), DWORD(take), &read, nullptr);
    CloseHandle(file);
    out.resize_down_to(isize(read));
    return out;
}

struct child_outcome
{
    cc::string error; ///< why it did not start or finish; empty where it exited
    DWORD exit_code = 0;
};

/// Runs `command_line` with `environment`, its output into the two files, and waits for it up to `timeout_ms`.
/// The child is in a job that closes with this process, so a killed test run leaves no child holding the GPU.
[[nodiscard]] child_outcome run_child(cc::string const& command_line,
                                      cc::vector<wchar_t> environment,
                                      cc::string const& stdout_path,
                                      cc::string const& stderr_path,
                                      DWORD timeout_ms)
{
    auto const job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr)
        return {.error = cc::format("CreateJobObject failed ({})", GetLastError())};
    auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    auto const out = inheritable_file(stdout_path);
    auto const err = inheritable_file(stderr_path);
    auto startup = STARTUPINFOW{.cb = sizeof(STARTUPINFOW),
                                .dwFlags = STARTF_USESTDHANDLES,
                                .hStdInput = nullptr,
                                .hStdOutput = out,
                                .hStdError = err};
    auto process = PROCESS_INFORMATION{};
    auto command = wide(command_line);
    auto const started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                        environment.data(), nullptr, &startup, &process);
    auto const start_error = GetLastError();
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE)
        CloseHandle(err);
    if (!started)
    {
        CloseHandle(job);
        return {.error = cc::format("CreateProcess failed ({})", start_error)};
    }

    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    auto outcome = child_outcome{};
    if (WaitForSingleObject(process.hProcess, timeout_ms) != WAIT_OBJECT_0)
    {
        TerminateJobObject(job, 1);
        WaitForSingleObject(process.hProcess, 10'000);
        outcome.error = cc::format("it was still running after {} s, and was killed", timeout_ms / 1000);
    }
    GetExitCodeProcess(process.hProcess, &outcome.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    return outcome;
}

/// The sweep of one Agility driver: the listed invocables on a hardware context of this process's Agility runtime.
cc::shared_async<cc::unit> agility_sweep(cc::string invoke, bool native_dxil)
{
    auto ctx = sg::create_dx12_context({.activate_global_debug_layer = true,
                                        .native_dxil = native_dxil,
                                        .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (ctx.has_error())
        FAIL(cc::format("dx12 hardware device creation failed in the Agility process: {}", ctx.error().to_string()));
    else
    {
        (void)sg_test::shader_fixtures();
        ctx.value()->set_portability_checks(true);
        ctx.value()->set_hit_record_checks(true);
        co_await nx::async_invoke_tests_in_sequence(
            invoke,
            nx::invocation_options{.after_each = sg_test::sweep_after_each(ctx.value()),
                                   .finish_each = sg_test::settle_each(ctx.value())},
            ctx.value());
        auto& dx = static_cast<dx12::dx12_context&>(*ctx.value());
        dx.poll_device_removal();
        CHECK(!dx.is_device_lost())
            .context(cc::format("the device was lost while running this binary's GPU tests: {}", dx.device_loss_reason()));
    }
    co_return;
}
} // namespace

NX_TEST_SETUP(nx::setup& s)
{
    auto const* const text = s.find_test(k_text_driver);
    auto const* const native = s.find_test(k_native_driver);
    auto fragments = cc::vector<nx::alias_fragment>();
    for (auto const* t : s.invocables_with<sg::context_handle>())
        for (auto const* name : k_agility_tests)
            if (cc::string_view(t->name) == name)
            {
                fragments.push_back({.driver = text, .section_path = {"dx12-agility", t->name}});
                fragments.push_back({.driver = native, .section_path = {"dx12-agility-native-dxil", t->name}});
            }
    CC_ASSERT(fragments.size() == 2 * isize(sizeof(k_agility_tests) / sizeof(k_agility_tests[0])),
              "an Agility test listed in dx12-agility-process.cc names no invocable: one was renamed");
    s.define_alias(k_agility_alias, cc::move(fragments));
}

// Both routes, since a test comparing the routes skips where one is the whole of a context.
ASYNC_TEST("sg dx12 agility backend")
{
    if (!is_agility_process())
        SKIP("runs in the process \"sg dx12 - the Agility SDK's features run in a process of their own\" starts");
    else if (!dx12::runs_agility_runtime())
        FAIL("the Agility SDK did not load in the process started for it");
    else
        co_await agility_sweep("dx12-agility", /*native_dxil =*/false);
}

ASYNC_TEST("sg dx12 agility native-dxil backend")
{
    if (!is_agility_process())
        SKIP("runs in the process \"sg dx12 - the Agility SDK's features run in a process of their own\" starts");
    else if (!dx12::runs_agility_runtime())
        FAIL("the Agility SDK did not load in the process started for it");
    else
        co_await agility_sweep("dx12-agility-native-dxil", /*native_dxil =*/true);
}

TEST("sg dx12 - the Agility SDK's features run in a process of their own")
{
    if (!SG_HAS_AGILITY)
        SKIP("the Agility SDK was not fetched (uv run extern/agility/fetch-agility.py --accept-license)");
    if (is_agility_process())
        SKIP("this is that process");
    if (dx12::requested_d3d12_runtime() == dx12::d3d12_runtime::agility)
        SKIP("this process's own dx12 drivers run on the Agility SDK");
    if (!dx12::has_hardware_adapter())
        SKIP("no dx12 hardware adapter");

    auto const exe = executable_path();
    auto const xml = exe + ".agility.results.xml";
    auto const stdout_path = exe + ".agility.stdout.txt";
    auto const stderr_path = exe + ".agility.stderr.txt";
    DeleteFileW(wide(xml).data());

    auto command
        = quote_argument(exe) + " --tests " + quote_argument(k_agility_alias) + " --junit-xml " + quote_argument(xml);
    if (nx::is_thorough())
        command += " --thorough";
    if (nx::is_smoke())
        command += " --smoke";
    auto const overrides = cc::vector<cc::string>{"SC_DX12_RUNTIME=agility", cc::format("{}=1", k_process_marker)};

    auto const outcome = run_child(command, environment_with(overrides), stdout_path, stderr_path, 150'000);
    auto const tail = file_tail(stderr_path, 4000);
    REQUIRE(outcome.error.empty()).context(cc::format("the Agility process: {}\n{}", outcome.error, tail));
    CHECK(outcome.exit_code == 0)
        .context(cc::format("the Agility process exited with {}; its tests are in {}, its output in {}\n{}",
                            outcome.exit_code, xml, stderr_path, tail));
}
