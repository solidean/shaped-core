#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/shader_stage.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>

// Differential testing: one shader run every way this machine can run it, every result held to every other.
// A corpus entry point is an SGL file under tests/differential/corpus naming a setup in its comment block.
// A setup is C++ under tests/differential/setups: it fills the bindings its SGL module declares, dispatches, and reads back.
// ../../docs/differential-testing.md is the design.

namespace sg_diff
{
using namespace cc::primitive_defines;

struct tolerance;
struct output;
struct setup;
struct workload;
struct corpus;
struct route;
struct shaders_on_route;
struct run_result;
} // namespace sg_diff

/// How far one output of two runs may stray apart, element by element.
/// `ulps` and `relative` read the output as 32-bit floats, and any NaN matches any NaN.
struct sg_diff::tolerance
{
    enum class measure
    {
        exact,    ///< byte for byte
        ulps,     ///< at most `amount` units in the last place
        relative, ///< at most `amount` times the larger magnitude, or `amount` itself below a magnitude of 1
    };
    measure kind = measure::exact;
    double amount = 0;

    /// `exact`, `ulps 4` or `relative 0.00001`; nullopt for anything else.
    [[nodiscard]] static cc::optional<tolerance> parse(cc::string_view text);
    [[nodiscard]] cc::string to_string() const;
};

/// One resource a run read back.
struct sg_diff::output
{
    cc::string name;
    cc::vector<byte> bytes;
};

/// What a setup does with a corpus entry point: build its pipeline over the setup's module, fill the inputs, run it, read back.
struct sg_diff::setup
{
    cc::string name;
    /// The stage of the entry points it runs; a corpus file's entry points of another stage are not its.
    sg::shader_stage stage = sg::shader_stage::compute;
    /// Runs on the context's device home, where it has one.
    cc::unique_function<cc::shared_async<cc::vector<output>>(sg::context*, shaders_on_route)> run;
    /// Two routes of one context, unless an entry point's comment block says otherwise.
    tolerance routes;
    /// Two contexts, unless an entry point's comment block says otherwise.
    tolerance across;
};

/// One corpus entry point, under the setup and tolerances its file's comment block states.
struct sg_diff::workload
{
    cc::string file;
    cc::string entry;
    slib::shader_asset_handle asset;
    sg_diff::setup const* setup = nullptr;
    tolerance routes;
    tolerance across;
};

/// Every compute entry point of the corpus package.
/// A comment block that names no known setup or does not parse is an entry of `problems` instead, naming the file.
struct sg_diff::corpus
{
    cc::vector<workload> workloads;
    cc::vector<cc::string> problems;
};

/// One way a context runs a shader: the format it takes, and whether SGL writes that binary itself or DXC does.
struct sg_diff::route
{
    cc::string name; ///< `dxc`, `native`, `wgsl` or `msl`
    sg::shader_format format;
    bool native = false;
};

/// What a setup is handed for one run: the corpus entry point compiled on the route, and the route, which the setup's own stages compile on too.
struct sg_diff::shaders_on_route
{
    sg::compiled_shader entry;
    sg_diff::route route;
};

/// One workload run on one route: the outputs, or why the route had nothing to compare.
struct sg_diff::run_result
{
    cc::vector<output> outputs;
    /// Set where the native route fell back to DXC, which a comparison with the DXC route would hold to itself.
    cc::optional<cc::string> skipped;
};

namespace sg_diff
{
/// The registered setups; a function-local static, so the order the setup files initialize in does not matter.
cc::vector<setup>& setups();

/// Appends a setup; returns true so it can seed a static-init bool.
bool register_setup(setup s);

[[nodiscard]] corpus read_corpus();

/// The routes `ctx` takes for SGL, in the order it prefers its formats; DXIL and SPIR-V give two routes, the rest one.
[[nodiscard]] cc::vector<route> routes_of(sg::context const& ctx);

/// `asset` compiled for `r`: the context's own options, with the route stated rather than the context's preference.
[[nodiscard]] sg::async_compiled_shader compiled_on(sg::context const& ctx,
                                                    route const& r,
                                                    slib::shader_asset const& asset);

/// Compiles `w` for `r` and has its setup run it; a failed compile is the async's error.
[[nodiscard]] cc::shared_async<run_result> run(sg::context* ctx, workload const* w, route r);

/// Where `b` strays from `a` beyond `t`: one line per output that differs, naming the first elements that do.
[[nodiscard]] cc::vector<cc::string> differences(run_result const& a, run_result const& b, tolerance t);
} // namespace sg_diff
