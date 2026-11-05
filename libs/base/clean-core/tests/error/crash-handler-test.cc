#include <clean-core/error/crash_handler.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh> // cc::this_thread_yield
#include <nexus/args/ambient.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// The whole file at `path`, or empty when it cannot be read.
cc::vector<byte> read_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return {};
    auto bytes = adapter.value().stream().read_all();
    return bytes.has_error() ? cc::vector<byte>() : cc::move(bytes.value());
}

bool contains(cc::span<byte const> bytes, cc::string_view text)
{
    for (auto i = isize(0); i + text.size() <= bytes.size(); ++i)
    {
        auto match = true;
        for (auto j = isize(0); j < text.size() && match; ++j)
            match = char(bytes[i + j]) == text[j];
        if (match)
            return true;
    }
    return false;
}
} // namespace

TEST("crash handler - a report kept in a file holds what stderr got, and on Windows a minidump beside it", exclusive())
{
    if constexpr (CC_CRASH_HANDLER_SANITIZED != 0)
        SKIP("a sanitized build compiles the report machinery out, so the runtime's own handlers stay in place");

    cc::install_crash_handler();
    cc::set_crash_report_files({.directory = cc::temp_directory_path(), .name = "cc-crash-report-test"});
    // what the test runner had, restored however the test ends
    CC_DEFER
    {
        auto const directory = cc::environment_variable("NX_CRASH_DIR").value_or(cc::string());
        cc::set_crash_report_files({.directory = directory, .name = nx::program_name()});
    };

    // whether this build resolves names at all, asked before any report
    auto const had_symbols = cc::symbolizer().resolve(reinterpret_cast<void const*>(&contains)).has_function();

    // the non-fatal entry: the same report, file and minidump as a fault, without the fault
    cc::report_all_thread_stacks("a report the test asked for");

    auto const stem = cc::string(cc::last_crash_report_path());
    REQUIRE(!stem.empty());
    CHECK(stem.contains("cc-crash-report-test-"));

    auto const report_path = cc::format("{}.txt", stem);
    auto const report = read_file(report_path);
    CHECK(contains(report, "all thread stacks"));
    CHECK(contains(report, "a report the test asked for"));
    // the thread scopes are written by another file's writer, and land in the report all the same
    CHECK(contains(report, "open scopes, per thread"));
    CHECK(cc::remove_file(report_path));

#ifdef CC_OS_WINDOWS
    auto const dump_path = cc::format("{}.dmp", stem);
    auto const dump = read_file(dump_path);
    REQUIRE(dump.size() > 4);
    CHECK(contains(cc::span<byte const>(dump).first_n(4), "MDMP"));
    CHECK(cc::remove_file(dump_path));
#endif

    // a second report gets a name of its own, even within the same second
    cc::report_all_thread_stacks("a second one");
    auto const second = cc::string(cc::last_crash_report_path());
    CHECK(second != stem);
    CHECK(cc::remove_file(cc::format("{}.txt", second)));
#ifdef CC_OS_WINDOWS
    CHECK(cc::remove_file(cc::format("{}.dmp", second)));
#endif

    // the process goes on after a report, and so must the symbols every later profile resolves through
    if (had_symbols)
        CHECK(cc::symbolizer().resolve(reinterpret_cast<void const*>(&read_file)).has_function());
}

namespace
{
/// Recurses until the stack runs out, a frame of real stack each, which the optimizer cannot fold into a loop.
[[gnu::noinline]] int overflow_stack(int depth)
{
    char volatile frame[1024];
    frame[0] = char(depth);
    return overflow_stack(depth + 1) + frame[0];
}
} // namespace

// Ends the process by design, so it never runs in a sweep: run it by its exact name, and read what stderr got.
// The report it should leave is the one any other crash leaves, with the reason `stack overflow`.
TEST("crash handler - a stack overflow is reported", nx::config::manual, nx::config::exclusive())
{
    CHECK(overflow_stack(0) == 0); // never returns
}

// The same, on a thread clean-core started, which takes a signal stack of its own when it starts.
TEST("crash handler - a stack overflow on a clean-core thread is reported", nx::config::manual, nx::config::exclusive())
{
    auto thread = cc::native_thread::start([] { (void)overflow_stack(0); });
    thread.join();
}

#ifdef __APPLE__
namespace
{
cc::atomic<int> g_parked_state = 0; // 1 once the thread is parked, 2 to let it go

/// Spins until told to stop, so a report finds this frame on the thread's stack.
[[gnu::noinline]] void parked_for_the_report()
{
    g_parked_state.store(1);
    while (g_parked_state.load() != 2)
        cc::this_thread_yield();
}
} // namespace

// macOS walks every other thread from outside, suspending it; so a thread parked in a known function shows it by name.
TEST("crash handler - a report names what another thread was doing, on macOS", exclusive())
{
    if constexpr (CC_CRASH_HANDLER_SANITIZED != 0)
        SKIP("a sanitized build compiles the report machinery out, so the runtime's own handlers stay in place");

    cc::install_crash_handler();
    cc::set_crash_report_files({.directory = cc::temp_directory_path(), .name = "cc-crash-threads-test"});
    CC_DEFER
    {
        auto const directory = cc::environment_variable("NX_CRASH_DIR").value_or(cc::string());
        cc::set_crash_report_files({.directory = directory, .name = nx::program_name()});
    };

    g_parked_state.store(0);
    auto thread = cc::native_thread::start([] { parked_for_the_report(); });
    while (g_parked_state.load() != 1)
        cc::this_thread_yield();

    cc::report_all_thread_stacks("a report of a parked thread");
    g_parked_state.store(2);
    thread.join();

    auto const report_path = cc::format("{}.txt", cc::last_crash_report_path());
    auto const report = read_file(report_path);
    CHECK(contains(report, "parked_for_the_report"));
    CHECK(cc::remove_file(report_path));
}
#endif
