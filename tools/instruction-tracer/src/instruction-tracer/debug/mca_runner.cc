#include "mca_runner.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh>
#include <clean-core/string/format.hh>

namespace itrace
{
using namespace cc::primitive_defines;

namespace
{
cc::vector<char16_t> to_utf16_z(cc::string_view s)
{
    auto out = cc::utf8_to_utf16(s);
    out.push_back(u'\0');
    return out;
}

wchar_t const* as_wide(cc::vector<char16_t> const& v)
{
    return reinterpret_cast<wchar_t const*>(v.data());
}

/// An inheritable, delete-on-close temp file. All three llvm-mca streams ride on these: we write the
/// input, hand the handles to the child as its std streams, then read stdout/stderr back afterwards.
/// The child inherits duplicate handles sharing the file pointer, so a rewind before launch suffices.
HANDLE make_temp_file()
{
    wchar_t dir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, dir) == 0)
        return INVALID_HANDLE_VALUE;

    wchar_t path[MAX_PATH] = {};
    if (GetTempFileNameW(dir, L"mca", 0, path) == 0)
        return INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
}

void rewind(HANDLE h)
{
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
}

bool write_all(HANDLE h, cc::string_view s)
{
    isize written_total = 0;
    while (written_total < s.size())
    {
        DWORD written = 0;
        auto const chunk = DWORD(cc::min<isize>(s.size() - written_total, 1 << 20));
        if (!WriteFile(h, s.data() + written_total, chunk, &written, nullptr) || written == 0)
            return false;
        written_total += isize(written);
    }
    return true;
}

cc::string read_all(HANDLE h)
{
    rewind(h);
    cc::string out;
    char buf[1 << 16];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
        out += cc::string_view(buf, isize(n));
    return out;
}

mca_run_result run_once(cc::string_view mca_exe, cc::string_view cpu, cc::string_view input_asm)
{
    mca_run_result result;

    HANDLE h_in = make_temp_file();
    HANDLE h_out = make_temp_file();
    HANDLE h_err = make_temp_file();
    auto cleanup = [&]
    {
        if (h_in != INVALID_HANDLE_VALUE)
            CloseHandle(h_in);
        if (h_out != INVALID_HANDLE_VALUE)
            CloseHandle(h_out);
        if (h_err != INVALID_HANDLE_VALUE)
            CloseHandle(h_err);
    };

    if (h_in == INVALID_HANDLE_VALUE || h_out == INVALID_HANDLE_VALUE || h_err == INVALID_HANDLE_VALUE)
    {
        cleanup();
        return result;
    }

    if (!write_all(h_in, input_asm))
    {
        cleanup();
        return result;
    }
    rewind(h_in); // the child reads stdin from the start

    // Fixed command line (see the tracer readme's timing section for the rationale of each flag).
    cc::string cmd;
    cmd += '"';
    cmd += mca_exe;
    cmd += '"';
    cmd += " -mtriple=x86_64-pc-windows-msvc";
    cmd += cc::format(" -mcpu={}", cpu);
    cmd += " -json -timeline -timeline-max-iterations=1 -timeline-max-cycles=0";
    cmd += " -bottleneck-analysis -skip-unsupported-instructions=parse-failure";

    auto cmd_w = to_utf16_z(cmd);
    auto exe_w = to_utf16_z(mca_exe);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = h_in;
    si.hStdOutput = h_out;
    si.hStdError = h_err;

    PROCESS_INFORMATION pi = {};
    BOOL const ok = CreateProcessW(as_wide(exe_w), reinterpret_cast<wchar_t*>(cmd_w.data()), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok)
    {
        cleanup();
        return result;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.ran = true;
    result.json = read_all(h_out);
    result.stderr_text = read_all(h_err);
    cleanup();
    return result;
}
} // namespace

mca_run_result run_llvm_mca(cc::string_view mca_exe, cc::string_view cpu, cc::string_view input_asm)
{
    cc::vector<cc::string> cpus;
    if (!cpu.empty())
        cpus.push_back(cc::string(cpu));
    else
    {
        cpus.push_back("native"); // host model by default
        cpus.push_back("x86-64"); // graceful fallbacks if native is unavailable
        cpus.push_back("skylake");
    }

    mca_run_result last;
    for (auto const& c : cpus)
    {
        last = run_once(mca_exe, c, input_asm);
        if (last.ran && !last.json.empty())
            return last;
    }
    return last;
}
} // namespace itrace
