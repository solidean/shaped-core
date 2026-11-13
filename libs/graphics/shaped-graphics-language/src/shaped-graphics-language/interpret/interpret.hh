#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>
#include <shaped-graphics-language/interpret/scalar.hh>

/// The abstract machine of the flat tree, run directly: what a tree MEANS, in either form.
///
/// Operands and arguments are evaluated left to right, each exactly once, and a `print` is the one effect besides the result.
/// It exists to compare two trees, so it is exact about order and indifferent to speed.
/// Its arithmetic is `f32` and wrapping `i32`; a target's own rounding is not modelled, and `normalize` takes its root by iteration.

/// A value of any type as its scalars in field order, nested structs included; a `mat4` is 16 of them, column by column.
struct sgl::check::value
{
    type_id type = type_id::none;
    cc::vector<scalar> leaves;

    [[nodiscard]] bool operator==(value const& rhs) const
    {
        return type == rhs.type && ast::impl::is_equal(leaves, rhs.leaves);
    }
};

/// One `buffer[T]` member of a binding: its elements' scalars, element after element.
struct sgl::check::buffer_contents
{
    symbol_id binding = symbol_id::none;
    /// A position in the binding's `members`.
    i32 member = -1;
    /// The element of a binding array; 0 for a member that is no array.
    i32 element = 0;
    cc::vector<scalar> leaves;

    [[nodiscard]] bool operator==(buffer_contents const& rhs) const
    {
        return binding == rhs.binding && member == rhs.member && element == rhs.element
            && ast::impl::is_equal(leaves, rhs.leaves);
    }
};

/// One texture or image of a binding, or one element of a binding array of them, as a run holds it (EVAL-120).
/// Its texels are the values the shader reads, never a format's encoding: a `float4` texel is four floats.
/// Level after level, each layer after layer, slice after slice, row after row, texel after texel, and each texel's
/// samples after one another; a cube's layers are its faces, +x, -x, +y, -y, +z, -z, cube after cube.
struct sgl::check::texture_contents
{
    symbol_id binding = symbol_id::none;
    /// A position in the binding's `members`.
    i32 member = -1;
    /// The element of a binding array; 0 for a member that is no array.
    i32 element = 0;
    /// Level 0's extent; each level halves each axis, down to 1.
    i32 width = 0;
    i32 height = 1;
    /// A 3D texture's slices at level 0, halving as width and height do; 1 for every other shape.
    i32 depth = 1;
    /// An array's layers, a cube's 6 or a cube array's 6 per cube; 1 for every other shape.
    i32 layers = 1;
    i32 levels = 1;
    /// A multisampled texture's samples per texel; 1 for every other shape.
    i32 samples = 1;
    /// How many scalars a texel has.
    i32 channels = 4;
    cc::vector<scalar> texels;

    [[nodiscard]] bool operator==(texture_contents const& rhs) const
    {
        return binding == rhs.binding && member == rhs.member && element == rhs.element && width == rhs.width
            && height == rhs.height && depth == rhs.depth && layers == rhs.layers && levels == rhs.levels
            && samples == rhs.samples && channels == rhs.channels && ast::impl::is_equal(texels, rhs.texels);
    }
};

/// The state a driver gives a dynamic sampler member, which the source cannot know (EVAL-123).
struct sgl::check::sampler_binding
{
    symbol_id binding = symbol_id::none;
    i32 member = -1;
    i32 element = 0;
    sampler_state state;
};

enum class sgl::check::run_status : sgl::u8
{
    ok,
    /// The run took more steps than `run_limits::fuel`.
    out_of_fuel,
    /// The end of the function, or of a block expression, was reached without a value.
    fell_off_the_end,
    /// The tree is not well formed or not well typed where the run came through; `outcome::detail` says how.
    type_error,
    /// A `var` was read before anything was assigned to it.
    uninitialized_read,
    /// A `discard` ended the run: the invocation has no result and no effect after it.
    discarded,
    /// An operation met a value no target defines it for, such as an integer divisor of zero; `outcome::detail` says which.
    program_error,
    /// An `assert` was false; the run stopped there (EVAL-76).
    assertion_failed,
    /// The caller raised `run_limits::stop`; what the run had found so far means nothing.
    stopped,
};

struct sgl::check::run_inputs
{
    /// The value of the stage struct, `locals[0]`; unread for an entry point without one.
    value parameter;
    /// Each lane's own stage struct in place of `parameter`, as a pixel quad's four pixels each have theirs (EVAL-148).
    /// A lane past its end takes `parameter`.
    cc::vector<value> lane_parameters;
    /// Parallel to `flat_entry_point::stage_inputs`; a missing one is zero.
    cc::vector<value> stage_inputs;
    /// Parallel to `flat_entry_point::bindings`: the members of each binding as one value, in member order.
    /// A buffer member has no scalars there; its contents are `buffers`.
    cc::vector<value> bindings;
    /// What every buffer the tree reads or writes holds when the run starts; one it names and this lacks is a type error.
    cc::vector<buffer_contents> buffers;
    /// Every texture and image the run binds; one the tree names and this lacks is empty, 0 by 0 with no level.
    cc::vector<texture_contents> textures;
    /// The state of each dynamic sampler member the driver set; one it did not is `sampler_state`'s default.
    cc::vector<sampler_binding> samplers;
    /// sg's acceleration pool, units of 16 bytes, which `acceleration_pool_load` reads (EVAL-95).
    /// Empty reads as zero at every unit, which is the empty TLAS wherever a trace starts.
    cc::pinned_data<byte const> acceleration_pool;
    /// The root unit of each acceleration member of the entry point, counted across its binding list in order; a missing one is 0.
    cc::vector<u32> acceleration_roots;
    /// How many lanes the run is, one invocation each of one subgroup of that size, which run one after another (EVAL-140).
    /// Its subgroup operations and derivatives exchange values between them; one lane has derivatives of 0.
    i32 lane_count = 1;
};

/// The texels a driver binds to a texture or an image, laid out as `texture_contents` holds them.
/// Each scalar takes 4 bytes, as a value's do; the extent, the layers, the levels and the samples say how many.
struct sgl::check::texture_data
{
    i32 width = 0;
    i32 height = 1;
    i32 depth = 1;
    i32 layers = 1;
    i32 levels = 1;
    i32 samples = 1;
    cc::pinned_data<byte const> texels;
    /// For an image, in place of `texels`: what the run stored is written back into it (`write_back`).
    cc::pinned_data<byte> mutable_texels;
};

/// What a driver binds to one member of a binding, found by the member's name.
/// A value is its scalars in field order, 4 bytes each and never padded: a `float3` is 12 bytes, a `float4x4` 64 of them, column by column.
/// A 16-bit scalar takes 4 bytes too, its bits in the low two.
/// A buffer is its elements one after another, each laid out so, and never as a target lays it out; a `bool` is a 32-bit 0 or 1.
struct sgl::check::member_data
{
    cc::string name;
    cc::pinned_data<byte const> bytes;
    /// For a `mut buffer[T]`, in place of `bytes`: what the run stored is written back into it (`write_back`).
    cc::pinned_data<byte> mutable_bytes;
    /// For an acceleration member: the pool unit of its TLAS, where 0 is the empty one.
    u32 acceleration_root = 0;
    /// For a texture or an image: one, or one per element of a binding array of them, in order.
    cc::vector<texture_data> textures;
    /// For a dynamic sampler member: its state, or one per element of a binding array of them.
    cc::vector<sampler_state> samplers;
};

/// What a driver binds to one `binding` of the module, found by the binding's name.
struct sgl::check::bound_group
{
    cc::string name;
    cc::vector<member_data> members;
};

/// Everything a driver binds for runs, by name, which `resolve_inputs` makes the inputs of one run (EVAL-94).
struct sgl::check::driver_bindings
{
    /// A group no entry point lists is never read, so one set serves every test of a module.
    cc::vector<bound_group> groups;
    /// sg's acceleration pool, as the internal doc libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md lays it out.
    cc::pinned_data<byte const> acceleration_pool;
};

struct sgl::check::run_limits
{
    /// One unit per statement, per expression node and per iteration.
    i64 fuel = 1'000'000;
    /// Checks and asserts run, as the structured form means (EVAL-75).
    /// False skips each with its body, as the core form does, which is what a comparison of the two forms wants.
    bool run_checks = true;
    /// A weak compare-exchange that meets `expected` fails spuriously every other time, the first time first (EVAL-100).
    /// So a test of one that forgets its retry loop fails, as it may on a GPU; false makes every one strong.
    bool fails_weak_exchanges = true;
    /// Failures past this many are counted in `outcome::failures_dropped` and not kept.
    i32 max_failures = 8;
    /// Read every few thousand steps when set, and a raised flag ends the run as `stopped`.
    /// It is how an editor abandons a test whose document changed, and raising it from another thread is enough.
    cc::atomic<bool> const* stop = nullptr;
};

/// One check or `assert` that was false where it ran.
struct sgl::check::check_failure
{
    /// A position in the tree's `check_sites`.
    i32 site = -1;
    /// Parallel to the site's nodes: the value each node's `var` held, and whether the node ran at all.
    cc::vector<value> values;
    cc::vector<bool> is_evaluated;
    /// Parallel to the site's `loop_variables`.
    cc::vector<value> loop_values;
    /// Parallel to the site's `message_values`: what an `assert`'s message interpolates.
    cc::vector<value> message_values;
    /// The lane whose run it was false in (EVAL-140).
    i32 lane = 0;

    [[nodiscard]] bool operator==(check_failure const& rhs) const
    {
        return site == rhs.site && lane == rhs.lane && ast::impl::is_equal(values, rhs.values)
            && ast::impl::is_equal(is_evaluated, rhs.is_evaluated) && ast::impl::is_equal(loop_values, rhs.loop_values)
            && ast::impl::is_equal(message_values, rhs.message_values);
    }
};

/// One `print` that ran: its message, a position in the tree's `messages` or -1, and the values it interpolated.
struct sgl::check::printed_message
{
    i32 message = -1;
    cc::vector<value> values;

    [[nodiscard]] bool operator==(printed_message const& rhs) const
    {
        return message == rhs.message && ast::impl::is_equal(values, rhs.values);
    }
};

/// How often one check or `assert` held and how often it did not, over one run.
/// Both zero is a site the run never reached.
struct sgl::check::site_tally
{
    i32 passed = 0;
    i32 failed = 0;

    constexpr bool operator==(site_tally const&) const = default;
};

struct sgl::check::outcome
{
    run_status status = run_status::ok;
    /// What the entry point returned in lane 0; empty unless `status` is `ok`.
    value result;
    /// What it returned in each lane, in lane order, lane 0's included (EVAL-148).
    cc::vector<value> lane_results;
    /// Every printed value, in the order the prints ran; kept up to the point a failed run stopped.
    cc::vector<value> trace;
    /// Every `print` that ran, in order; its values are in `trace` too.
    cc::vector<printed_message> printed;
    /// Every buffer the run stored to, as the run left it: a store is an effect, so two runs are compared by these too.
    cc::vector<buffer_contents> buffers;
    /// Every image the run stored to, as the run left it, compared as the buffers are.
    cc::vector<texture_contents> images;
    /// For a reader, and no part of what two runs are compared by.
    cc::string detail;
    /// Every check and `assert` that was false, up to `run_limits::max_failures`, in the order they ran.
    cc::vector<check_failure> failures;
    /// How many checks ran, whatever they found, `assert`s included (EVAL-76); a test whose run ran none has checked nothing.
    i32 checks_run = 0;
    /// How many of `checks_run` were `assert`s.
    i32 asserts_run = 0;
    /// How many failures `max_failures` left out.
    i32 failures_dropped = 0;
    /// Parallel to the tree's `check_sites`, and counted past `max_failures`, since it holds no values.
    cc::vector<site_tally> sites;

    /// Same status, same result in every lane, same trace, same buffers.
    [[nodiscard]] bool operator==(outcome const& rhs) const
    {
        return status == rhs.status && result == rhs.result && ast::impl::is_equal(lane_results, rhs.lane_results)
            && ast::impl::is_equal(trace, rhs.trace) && ast::impl::is_equal(buffers, rhs.buffers)
            && ast::impl::is_equal(images, rhs.images);
    }
};

namespace sgl::check
{
/// The status in kebab case, as `dump` writes it: `out-of-fuel`, `assertion-failed`.
[[nodiscard]] cc::string_view to_string(run_status s);

/// How many scalars a value of `type` has; 0 for a type that has no value here.
[[nodiscard]] isize leaf_count_of(checked_module const& m, type_id type);

/// A value of `type` with every scalar zero, of the kind its place in the type says.
[[nodiscard]] value zero_value(checked_module const& m, type_id type);

/// Runs `e`, structured or core, on the abstract machine.
/// Total: a malformed tree is a `type_error`, a run without end is `out_of_fuel`, and nothing asserts.
/// Deterministic: equal arguments give equal outcomes, unless `stop` is raised.
[[nodiscard]] outcome interpret(checked_module const& m,
                                flat_entry_point const& e,
                                run_inputs const& inputs,
                                run_limits const& limits = {});

/// The inputs of a run of `e` from what `bound` binds by name, placed through the module's bindings (EVAL-94).
/// A member the driver leaves out is zero, a buffer empty and an acceleration root 0.
/// A member name its binding lacks, or bytes that are no whole value or no whole number of elements, is an error.
[[nodiscard]] cc::result<run_inputs> resolve_inputs(checked_module const& m,
                                                    flat_entry_point const& e,
                                                    driver_bindings const& bound);

/// Writes every buffer `o` stored to back into the `mutable_bytes` `bound` gave it, laid out as `resolve_inputs` read it,
/// and every image it stored to into its `mutable_texels`.
/// A buffer bound through `bytes` alone keeps what it held.
void write_back(checked_module const& m, outcome const& o, driver_bindings const& bound);

/// True where `id` is a constant of `e`: a literal, an enum value, or a construction, a member, a logical operator or
/// a call of a `@pure` builtin whose operands are all constants.
/// It is what WGSL folds when it creates the shader, so what the check pass judges of constants (CHK-310).
[[nodiscard]] bool is_constant(checked_module const& m, flat_entry_point const& e, flat_expr_id id);

/// The value of constant `id` on the abstract machine: `ok` with it as the result, or `program_error` where some call
/// under it has none (EVAL-85); a `type_error` for an `id` that is no constant.
[[nodiscard]] outcome evaluate_constant(checked_module const& m, flat_entry_point const& e, flat_expr_id id);

/// `ok 1.5` with one ` | print …` per printed value, one ` | buffer …` per buffer, then one ` | image …` per image.
[[nodiscard]] cc::string dump(outcome const& o);
} // namespace sgl::check
