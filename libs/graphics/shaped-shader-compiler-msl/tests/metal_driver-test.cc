#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-msl/impl/metal_driver.hh>

// The child-process helper, which is the only part of this library that touches the OS.
// A host without Apple's Metal toolchain skips the arms that need one, since an absent component is ordinary
// rather than a failure — see impl/metal_driver.hh.

namespace
{
using namespace cc::primitive_defines;

/// A kernel small enough to read, with one binding so the reflection tests have something to find.
constexpr char const* k_double_kernel = R"(
#include <metal_stdlib>
using namespace metal;
kernel void double_it(device float* d [[buffer(0)]], uint i [[thread_position_in_grid]]) { d[i] *= 2.0f; }
)";
} // namespace

TEST("ssc::msl driver - a metallib comes back on stdout, and it is a metallib")
{
    auto const driver = ssc::msl::impl::resolve_metal_driver();
    if (driver.empty())
        return; // no Metal toolchain on this host, which is a supported configuration

    auto const args = cc::vector<cc::string>{"-x", "metal", "-O2", "-o", "-", "-"};
    auto r = ssc::msl::impl::run_process(driver, args, k_double_kernel);
    REQUIRE(r.has_value());
    CHECK(r.value().exit_code == 0);

    // A metallib is AIR in a container, and the container says so in its first four bytes.
    REQUIRE(r.value().out.size() > 4);
    CHECK(char(r.value().out[0]) == 'M');
    CHECK(char(r.value().out[1]) == 'T');
    CHECK(char(r.value().out[2]) == 'L');
    CHECK(char(r.value().out[3]) == 'B');
}

TEST("ssc::msl driver - a broken shader fails with its diagnostics on stderr")
{
    auto const driver = ssc::msl::impl::resolve_metal_driver();
    if (driver.empty())
        return;

    auto const args = cc::vector<cc::string>{"-x", "metal", "-o", "-", "-"};
    auto r = ssc::msl::impl::run_process(driver, args, "kernel void broken() { this_is_not_a_function(); }");
    REQUIRE(r.has_value());
    CHECK(r.value().exit_code != 0);
    CHECK(cc::string_view(r.value().err).contains("error"));
}

TEST("ssc::msl driver - an input larger than a pipe buffer does not deadlock")
{
    auto const driver = ssc::msl::impl::resolve_metal_driver();
    if (driver.empty())
        return;

    // A pipe holds 64 KiB on macOS, so a source past that is what a write-then-read implementation hangs on.
    auto source = cc::string(k_double_kernel);
    for (auto i = 0; i < 4000; ++i)
        source += cc::format("static constant float pad_{} = {}.0f;\n", i, i);
    CHECK(source.size() > 64 * 1024);

    auto const args = cc::vector<cc::string>{"-x", "metal", "-O2", "-o", "-", "-"};
    auto r = ssc::msl::impl::run_process(driver, args, source);
    REQUIRE(r.has_value());
    CHECK(r.value().exit_code == 0);
    CHECK(r.value().out.size() > 4);
}

TEST("ssc::msl driver - the version is what a persistent cache key would carry")
{
    auto const driver = ssc::msl::impl::resolve_metal_driver();
    if (driver.empty())
        return;

    auto const version = ssc::msl::impl::query_driver_version(driver);
    CHECK(!version.empty());
    // "32023.883" and its neighbours: digits and dots, nothing else, so it is comparable and hashable as it stands.
    for (auto i = isize(0); i < version.size(); ++i)
    {
        auto const c = version[i];
        auto const is_version_char = (c >= '0' && c <= '9') || c == '.';
        CHECK(is_version_char);
    }
}
