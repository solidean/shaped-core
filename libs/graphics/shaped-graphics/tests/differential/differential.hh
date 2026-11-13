#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics-language/interpret/interpret.hh>
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
struct interpreter_half;
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
    /// Compared byte for byte whatever the file's tolerance, as an integer output must be, and committed as a hash.
    bool is_exact = false;
};

/// What a setup does with a corpus entry point: build its pipeline over the setup's module, fill the inputs, run it, read back.
struct sg_diff::setup
{
    cc::string name;
    /// The stage of the entry points it runs; a corpus file's entry points of another stage are not its.
    sg::shader_stage stage = sg::shader_stage::compute;
    /// What a context must support to run it at all; a run on one without is skipped, not failed.
    cc::optional<sg::feature> required = {};
    /// Runs on the context's device home, where it has one.
    cc::unique_function<cc::shared_async<cc::vector<output>>(sg::context*, shaders_on_route)> run;
    /// Two routes of one context, unless an entry point's comment block says otherwise.
    tolerance routes;
    /// Two contexts, unless an entry point's comment block says otherwise.
    tolerance across;
    /// The same run for SGL's interpreter, the same inputs bound by name; null for a setup it cannot run yet.
    /// It may build what the inputs need on the context it is given, which must be on its device home.
    interpreter_half (*interpreted)(sg::context& ctx) = nullptr;
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

/// What a setup binds for SGL's interpreter, which runs a dispatch one invocation after another.
/// Every value is laid out as the interpreter reads it, unpadded, and a texture's texels are the floats a shader reads.
struct sg_diff::interpreter_half
{
    /// One output the runs store into, which `bindings` holds too, through `mutable_bytes`.
    struct output
    {
        cc::string name;
        cc::pinned_data<byte> bytes;
    };

    /// The groups the setup's module declares, by the names the module gives them.
    sgl::check::driver_bindings bindings;
    /// For a compute stage; a pixel stage's one output is named by the first, which has no bytes.
    cc::vector<output> outputs;
    /// Set where the half cannot give this context's inputs, which skips the run.
    cc::optional<cc::string> skipped;
    /// The dispatch's threads along x, each one invocation with its `@thread_id`.
    isize threads = 0;
    /// For a pixel stage, in place of `threads`: a square target this many pixels wide, run as 2 by 2 quads.
    isize extent = 0;
    /// Each pixel's stage struct as the rasterizer interpolates it at the pixel's center `(x, y)`, every scalar a float.
    void (*pixel_varyings)(f32 x, f32 y, cc::span<f32> leaves) = nullptr;
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

/// `w` run on SGL's interpreter: the outputs, `skipped` where it cannot model the run, an error where the run fails.
/// The interpreter takes no target's rounding, so it is a third opinion beside the GPUs and held to them at `across`.
/// Built on `ctx` where the setup's half needs a device; called on its device home.
[[nodiscard]] cc::result<run_result> interpreted(workload const& w, sg::context& ctx);

/// The name of `w`'s file's committed reference, `integer_bits.ref` for `integer_bits.sgl`.
[[nodiscard]] cc::string reference_name(workload const& w);

/// What `r` commits as `w`'s reference, one line each: an output `w` holds exactly across APIs as its size and hash,
/// any other as k_reference_samples of its elements, each its bits and, for a reader, its value.
[[nodiscard]] cc::string reference_lines(workload const& w, run_result const& r);

/// Where `r` strays from the lines `reference` holds for `w`, at `w`'s `across` bound; one line per difference, or one naming an output it lacks.
[[nodiscard]] cc::vector<cc::string> against_reference(cc::string_view reference, workload const& w, run_result const& r);
} // namespace sg_diff
