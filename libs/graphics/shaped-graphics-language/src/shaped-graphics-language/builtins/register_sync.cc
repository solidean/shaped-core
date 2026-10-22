#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::builtins;

// The barriers of a compute workgroup, each an execution barrier and a memory barrier of one kind of memory, and the
// atomics every invocation updates memory with in one step.
// A memory-only barrier is HLSL's and no other target's, so SGL has none; an atomic is relaxed, the one ordering WGSL has.

namespace
{
using check::scalar;
using check::value_kind;

/// A test runs one invocation, which has nobody to wait for.
void nothing(cc::span<check::scalar const>, cc::vector<check::scalar>&)
{
}

struct barrier
{
    cc::string_view name;
    cc::string_view hlsl;
    cc::string_view wgsl;
    /// The `mem_flags` of Metal's one barrier.
    cc::string_view msl_memory;
    cc::string_view doc;
};

constexpr barrier k_barriers[] = {
    {"workgroup_barrier", "GroupMemoryBarrierWithGroupSync", "workgroupBarrier", "mem_threadgroup",
     "/// Waits for every thread of the workgroup, after which each sees what the others wrote to workgroup memory."},
    {"storage_barrier", "DeviceMemoryBarrierWithGroupSync", "storageBarrier", "mem_device",
     "/// Waits for every thread of the workgroup, after which each sees what the others wrote to buffers."},
    {"texture_barrier", "DeviceMemoryBarrierWithGroupSync", "textureBarrier", "mem_texture",
     "/// Waits for every thread of the workgroup, after which each sees what the others stored to images."},
};

constexpr cc::string_view k_barriers_hlsl[] = {"GroupMemoryBarrierWithGroupSync", "DeviceMemoryBarrierWithGroupSync"};
constexpr cc::string_view k_barriers_wgsl[] = {"workgroupBarrier", "storageBarrier", "textureBarrier"};
constexpr cc::string_view k_barriers_msl[]
    = {"threadgroup_barrier", "mem_flags", "atomic_thread_fence", "memory_order_seq_cst", "thread_scope_device"};

/// EMIT-150: Metal's barrier orders memory within the threadgroup alone, so where a `@coherent` member's writes must
/// reach the whole dispatch, a device-scoped fence follows it.
written write_barrier(call_context const& ctx)
{
    auto const& b = k_barriers[ctx.data];
    switch (ctx.target)
    {
    case language::hlsl:
        return {.text = cc::format("{}()", b.hlsl)};
    case language::wgsl:
        return {.text = cc::format("{}()", b.wgsl)};
    case language::msl:
    {
        auto const barrier = cc::format("threadgroup_barrier(mem_flags::{})", b.msl_memory);
        if (!ctx.is_device_coherent || b.msl_memory == "mem_threadgroup")
            return {.text = barrier};
        auto result = written{.text = cc::format("atomic_thread_fence(mem_flags::{}, memory_order_seq_cst, "
                                                 "thread_scope_device)",
                                                 b.msl_memory)};
        result.lines.push_back(cc::format("{};", barrier));
        return result;
    }
    }
    return {};
}
// ---- the uniform load ----------------------------------------------------------------------------------------------

constexpr cc::string_view k_uniform_load_wgsl[] = {"workgroupUniformLoad"};

/// EMIT-149: WGSL's own; elsewhere the workgroup barrier, the read into a local, and the barrier again, so no thread
/// stores to the memory before every one has read it.
written write_uniform_load(call_context const& ctx)
{
    auto const& m = ctx.arguments[0].text;
    if (ctx.target == language::wgsl)
        return {.text = cc::format("workgroupUniformLoad(&{})", m)};
    auto const barrier = ctx.target == language::hlsl
                           ? cc::string_view("GroupMemoryBarrierWithGroupSync();")
                           : cc::string_view("threadgroup_barrier(mem_flags::mem_threadgroup);");
    auto const loaded = ctx.mint.is_valid() ? ctx.mint("uniform_load") : cc::string("uniform_load");
    auto result = written{.text = loaded};
    result.lines.push_back(cc::string(barrier));
    result.lines.push_back(cc::format("{} {} = {};", ctx.result_type, loaded, m));
    result.lines.push_back(cc::string(barrier));
    return result;
}

/// A test runs one invocation, whose load is a plain read.
void loaded(cc::span<check::scalar const> in, cc::vector<check::scalar>& out)
{
    out.push_back_range(in);
}
// ---- atomics ------------------------------------------------------------------------------------------------------

enum class atomic_op : u8
{
    add,
    subtract,
    min,
    max,
    bit_and,
    bit_or,
    bit_xor,
    exchange,
    load,
    store,
    compare_exchange_strong,
    compare_exchange_weak,
};

struct atomic_info
{
    atomic_op op;
    cc::string_view name;
    /// HLSL's `Interlocked*`, WGSL's `atomic*` and MSL's `atomic_*_explicit`, each without its prefix.
    cc::string_view hlsl;
    cc::string_view wgsl;
    cc::string_view msl;
    cc::string_view doc;
};

constexpr atomic_info k_atomics[] = {
    {atomic_op::add, "add", "Add", "Add", "fetch_add", "/// Adds `v`, wrapping, and gives the value before."},
    {atomic_op::subtract, "subtract", "Add", "Sub", "fetch_sub",
     "/// Subtracts `v`, wrapping, and gives the value before."},
    {atomic_op::min, "min", "Min", "Min", "fetch_min",
     "/// Keeps the smaller of the value and `v`, and gives the value before."},
    {atomic_op::max, "max", "Max", "Max", "fetch_max",
     "/// Keeps the larger of the value and `v`, and gives the value before."},
    {atomic_op::bit_and, "bit_and", "And", "And", "fetch_and",
     "/// Keeps the bits `v` shares, and gives the value before."},
    {atomic_op::bit_or, "bit_or", "Or", "Or", "fetch_or", "/// Sets the bits of `v`, and gives the value before."},
    {atomic_op::bit_xor, "bit_xor", "Xor", "Xor", "fetch_xor", "/// Flips the bits of `v`, and gives the value before."},
    {atomic_op::exchange, "exchange", "Exchange", "Exchange", "exchange", "/// Stores `v`, and gives the value before."},
    {atomic_op::load, "load", "Or", "Load", "load", "/// The value, read in one step."},
    {atomic_op::store, "store", "Exchange", "Store", "store", "/// Stores `v` in one step."},
    {atomic_op::compare_exchange_strong, "compare_exchange_strong", "CompareExchange", "CompareExchangeWeak",
     "compare_exchange_weak",
     "/// Stores `desired` where the value is `expected`, and gives the value before.\n"
     "/// It never fails spuriously: it stored exactly where what it gives is `expected`."},
    {atomic_op::compare_exchange_weak, "compare_exchange_weak", "CompareExchange", "CompareExchangeWeak",
     "compare_exchange_weak",
     "/// Stores `desired` where the value is `expected`, and gives the value before and whether it stored.\n"
     "/// It may fail although the value was `expected`, so only `exchanged` says it stored: retry in a loop."},
};

[[nodiscard]] bool is_compare_exchange(atomic_op op)
{
    return op == atomic_op::compare_exchange_strong || op == atomic_op::compare_exchange_weak;
}

/// EMIT-159: a pixel stage's strong loop must keep a helper invocation out, which needs the target to tell one.
[[nodiscard]] check::feature_set strong_pixel_features(atomic_op op)
{
    return op == atomic_op::compare_exchange_strong ? check::feature_set(check::feature::helper_lane_query)
                                                    : check::feature_set();
}

constexpr cc::string_view k_atomics_hlsl[] = {
    "InterlockedAdd", "InterlockedMin", "InterlockedMax",      "InterlockedAnd",
    "InterlockedOr",  "InterlockedXor", "InterlockedExchange", "InterlockedCompareExchange",
};
constexpr cc::string_view k_atomics_wgsl[] = {
    "atomicAdd",
    "atomicSub",
    "atomicMin",
    "atomicMax",
    "atomicAnd",
    "atomicOr",
    "atomicXor",
    "atomicExchange",
    "atomicLoad",
    "atomicStore",
    "atomicCompareExchangeWeak",
};
constexpr cc::string_view k_atomics_msl[] = {
    "atomic_fetch_add_explicit", "atomic_fetch_sub_explicit", "atomic_fetch_min_explicit",
    "atomic_fetch_max_explicit", "atomic_fetch_and_explicit", "atomic_fetch_or_explicit",
    "atomic_fetch_xor_explicit", "atomic_exchange_explicit",  "atomic_load_explicit",
    "atomic_store_explicit",     "memory_order_relaxed",      "atomic_compare_exchange_weak_explicit",
    "simd_is_helper_thread",
};

/// CHK-296: every stage that writes memory once per invocation the program can count.
/// A vertex stage has no writable storage in WebGPU, and any-hit and intersection stages run as often as traversal decides.
constexpr cc::string_view k_atomic_stages = "@stages(.pixel, .compute, .raygen, .miss, .closest_hit, .callable)";

/// A compare-exchange writes statements in every target (EMIT-157).
bool every_language(language)
{
    return true;
}

/// The data word: the operation, and whether the atomic holds an `int`.
u32 atomic_data(atomic_op op, bool is_signed)
{
    return u32(op) | (is_signed ? 0x100u : 0u);
}

/// A fresh name of the text, or `desired` itself where no text is being written.
cc::string minted(call_context const& ctx, cc::string_view desired)
{
    return ctx.mint.is_valid() ? ctx.mint(desired) : cc::string(desired);
}

/// Where a compare-exchange outside HLSL updates: a buffer's or workgroup memory's atomic, or in MSL a texel.
struct exchange_place
{
    /// The atomic as `&atomic` reaches it; for a texel, the texture.
    cc::string_view atomic;
    bool is_texel = false;
    /// A texel's coordinate as MSL's texture atomics take it, layer included, each a local already declared.
    cc::string texel;
};

/// EMIT-157 and EMIT-158: WGSL's and MSL's compare-exchange, whose own is weak.
/// The weak one is that call, once; the strong one retries it in a loop while it failed having read `expected`.
/// The place, `expected` and `desired` are each evaluated once, in that order, ahead of any loop.
written write_exchange(call_context const& ctx,
                       exchange_place const& place,
                       cc::string_view expected,
                       cc::string_view desired,
                       written result)
{
    auto const& info = k_atomics[ctx.data & 0xffu];
    auto const is_strong = info.op == atomic_op::compare_exchange_strong;
    auto const is_signed = (ctx.data & 0x100u) != 0;
    if (ctx.target == language::wgsl)
    {
        auto const t = is_signed ? "i32" : "u32";
        if (!is_strong)
        {
            // WGSL's own result is a struct no program names, so its two fields fill the prelude's
            auto const exchanged = minted(ctx, "exchange_outcome");
            result.lines.push_back(cc::format("let {} = atomicCompareExchangeWeak(&{}, {}, {});", exchanged,
                                              place.atomic, expected, desired));
            result.text = cc::format("{}({}.old_value, {}.exchanged)", ctx.result_type, exchanged, exchanged);
            return result;
        }
        auto const before = minted(ctx, "exchange_before");
        auto const expected_name = minted(ctx, "exchange_expected");
        auto const desired_name = minted(ctx, "exchange_desired");
        auto const place_name = minted(ctx, "exchange_place");
        auto const exchanged = minted(ctx, "exchange_outcome");
        result.text = before;
        result.lines.push_back(cc::format("let {} = &{};", place_name, place.atomic));
        result.lines.push_back(cc::format("let {}: {} = {};", expected_name, t, expected));
        result.lines.push_back(cc::format("let {}: {} = {};", desired_name, t, desired));
        result.lines.push_back(cc::format("var {}: {};", before, t));
        // a pixel stage's needs helper_lane_query, which WGSL lacks (EMIT-159), since it cannot keep a helper out of this loop
        result.lines.push_back("loop {");
        result.lines.push_back(cc::format("    let {} = atomicCompareExchangeWeak({}, {}, {});", exchanged, place_name,
                                          expected_name, desired_name));
        result.lines.push_back(cc::format("    {} = {}.old_value;", before, exchanged));
        result.lines.push_back(cc::format("    if {}.exchanged || {} != {} {{", exchanged, before, expected_name));
        result.lines.push_back("        break;");
        result.lines.push_back("    }");
        result.lines.push_back("}");
        return result;
    }

    // MSL hands the value it read back through `expected`, which a failure that read the expected value leaves alone
    auto const t = is_signed ? "int" : "uint";
    auto const vector = is_signed ? "int4" : "uint4";
    auto const before = minted(ctx, "exchange_before");
    auto const read = place.is_texel ? cc::format("{}.x", before) : cc::string(before);
    auto const attempt = [&](cc::string_view target, cc::string_view value)
    {
        if (place.is_texel)
            return cc::format("{}.atomic_compare_exchange_weak({}, &{}, {}({}))", place.atomic, place.texel, before,
                              vector, value);
        return cc::format("atomic_compare_exchange_weak_explicit({}, &{}, {}, memory_order_relaxed, "
                          "memory_order_relaxed)",
                          target, before, value);
    };
    if (!is_strong)
    {
        auto const stored = minted(ctx, "exchange_stored");
        result.lines.push_back(place.is_texel ? cc::format("{} {} = {}({});", vector, before, vector, expected)
                                              : cc::format("{} {} = {};", t, before, expected));
        result.lines.push_back(
            cc::format("const bool {} = {};", stored, attempt(cc::format("&{}", place.atomic), desired)));
        result.text = cc::format("{}{{{}, {}}}", ctx.result_type, read, stored);
        return result;
    }
    auto const expected_name = minted(ctx, "exchange_expected");
    auto const desired_name = minted(ctx, "exchange_desired");
    auto target = cc::string();
    if (!place.is_texel)
    {
        target = minted(ctx, "exchange_place");
        result.lines.push_back(cc::format("const auto {} = &{};", target, place.atomic));
    }
    result.lines.push_back(cc::format("const {} {} = {};", t, expected_name, expected));
    result.lines.push_back(cc::format("const {} {} = {};", t, desired_name, desired));
    result.lines.push_back(place.is_texel ? cc::format("{} {} = {}({});", vector, before, vector, expected_name)
                                          : cc::format("{} {} = {};", t, before, expected_name));
    // a helper thread's atomics store nothing and read what they please, so it never enters the loop (EMIT-158)
    auto const helper = ctx.is_pixel_stage ? cc::string_view("!simd_is_helper_thread() && ") : cc::string_view();
    result.lines.push_back(
        cc::format("while ({}!{} && {} == {})", helper, attempt(target, desired_name), read, expected_name));
    result.lines.push_back("{");
    result.lines.push_back("}");
    result.text = read;
    return result;
}

/// EMIT-157: HLSL's one compare-exchange, which is strong; a weak one fills the prelude's struct from it.
written write_hlsl_exchange(call_context const& ctx, cc::string_view place, written result)
{
    auto const& info = k_atomics[ctx.data & 0xffu];
    auto const t = (ctx.data & 0x100u) != 0 ? "int" : "uint";
    auto const before = minted(ctx, "atomic_before");
    auto const desired = wrapped(ctx.arguments[2], precedence::primary);
    if (info.op == atomic_op::compare_exchange_strong)
    {
        result.lines.push_back(cc::format("{} {};", t, before));
        result.lines.push_back(cc::format("InterlockedCompareExchange({}, {}, {}, {});", place,
                                          wrapped(ctx.arguments[1], precedence::primary), desired, before));
        result.text = before;
        return result;
    }
    auto const expected = minted(ctx, "exchange_expected");
    auto const exchanged = minted(ctx, "exchange_outcome");
    result.lines.push_back(cc::format("const {} {} = {};", t, expected, ctx.arguments[1].text));
    result.lines.push_back(cc::format("{} {};", t, before));
    result.lines.push_back(cc::format("InterlockedCompareExchange({}, {}, {}, {});", place, expected, desired, before));
    result.lines.push_back(cc::format("{} {};", ctx.result_type, exchanged));
    result.lines.push_back(cc::format("{}.old = {};", exchanged, before));
    result.lines.push_back(cc::format("{}.exchanged = {} == {};", exchanged, before, expected));
    result.text = exchanged;
    return result;
}

written write_atomic(call_context const& ctx)
{
    auto const& info = k_atomics[ctx.data & 0xffu];
    auto const is_signed = (ctx.data & 0x100u) != 0;
    auto const& a = ctx.arguments[0].text;
    auto const v = ctx.arguments.size() > 1 ? wrapped(ctx.arguments[1], precedence::primary) : cc::string();
    if (is_compare_exchange(info.op))
        return ctx.target == language::hlsl
                 ? write_hlsl_exchange(ctx, a, {})
                 : write_exchange(ctx, {.atomic = a}, ctx.arguments[1].text, ctx.arguments[2].text, {});
    switch (ctx.target)
    {
    case language::hlsl:
    {
        // `Interlocked*` gives the value before through an out parameter, so a local declared ahead holds it
        auto const before = minted(ctx, "atomic_before");
        auto const operand = info.op == atomic_op::subtract ? cc::format("-{}", v)
                           : info.op == atomic_op::load     ? cc::string(is_signed ? "0" : "0u")
                                                            : v;
        auto result = written{.text = before};
        result.lines.push_back(cc::format("{} {};", is_signed ? "int" : "uint", before));
        result.lines.push_back(cc::format("Interlocked{}({}, {}, {});", info.hlsl, a, operand, before));
        if (info.op == atomic_op::store)
            result.text = {};
        return result;
    }
    case language::wgsl:
        if (info.op == atomic_op::load)
            return {.text = cc::format("atomicLoad(&{})", a)};
        return {.text = cc::format("atomic{}(&{}, {})", info.wgsl, a, v)};
    case language::msl:
        if (info.op == atomic_op::load)
            return {.text = cc::format("atomic_load_explicit(&{}, memory_order_relaxed)", a)};
        return {.text = cc::format("atomic_{}_explicit(&{}, {}, memory_order_relaxed)", info.msl, a, v)};
    }
    return {};
}

template <bool IsSigned>
scalar updated(atomic_op op, scalar old, scalar v)
{
    auto const kind = IsSigned ? value_kind::scalar_int : value_kind::scalar_uint;
    auto const a = old.bits;
    auto const b = v.bits;
    auto const less = IsSigned ? i32(a) < i32(b) : a < b;
    switch (op)
    {
    case atomic_op::add:
        return {.kind = kind, .bits = a + b};
    case atomic_op::subtract:
        return {.kind = kind, .bits = a - b};
    case atomic_op::min:
        return {.kind = kind, .bits = less ? a : b};
    case atomic_op::max:
        return {.kind = kind, .bits = less ? b : a};
    case atomic_op::bit_and:
        return {.kind = kind, .bits = a & b};
    case atomic_op::bit_or:
        return {.kind = kind, .bits = a | b};
    case atomic_op::bit_xor:
        return {.kind = kind, .bits = a ^ b};
    case atomic_op::exchange:
    case atomic_op::store:
        return {.kind = kind, .bits = b};
    case atomic_op::load:
        return old;
    case atomic_op::compare_exchange_strong:
    case atomic_op::compare_exchange_weak:
        break;
    }
    return old;
}

/// The atomic's value first, then the operand; gives what the atomic holds after (EVAL-93).
template <atomic_op Op, bool IsSigned>
void atomic_update(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(updated<IsSigned>(Op, in[0], in.size() > 1 ? in[1] : in[0]));
}

/// The atomic's value, `expected` and `desired`: a run is one invocation, so no other update makes it fail (EVAL-93).
/// The interpreter decides a weak one's spurious failures itself, and gives the value read and whether it stored.
void exchanged(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(in[0].bits == in[1].bits ? in[2] : in[0]);
}

template <bool IsSigned>
evaluator evaluator_of(atomic_op op)
{
    switch (op)
    {
    case atomic_op::compare_exchange_strong:
    case atomic_op::compare_exchange_weak:
        return exchanged;
    case atomic_op::add:
        return atomic_update<atomic_op::add, IsSigned>;
    case atomic_op::subtract:
        return atomic_update<atomic_op::subtract, IsSigned>;
    case atomic_op::min:
        return atomic_update<atomic_op::min, IsSigned>;
    case atomic_op::max:
        return atomic_update<atomic_op::max, IsSigned>;
    case atomic_op::bit_and:
        return atomic_update<atomic_op::bit_and, IsSigned>;
    case atomic_op::bit_or:
        return atomic_update<atomic_op::bit_or, IsSigned>;
    case atomic_op::bit_xor:
        return atomic_update<atomic_op::bit_xor, IsSigned>;
    case atomic_op::exchange:
        return atomic_update<atomic_op::exchange, IsSigned>;
    case atomic_op::load:
        return atomic_update<atomic_op::load, IsSigned>;
    case atomic_op::store:
        return atomic_update<atomic_op::store, IsSigned>;
    }
    return nullptr;
}
// ---- the texels of an `@atomic` image -------------------------------------------------------------------------

using check::texture_shape;

/// The data word of a texel's atomic: the atomic's own, and the image's shape above it.
u32 texel_atomic_data(atomic_op op, bool is_signed, texture_shape shape)
{
    return atomic_data(op, is_signed) | u32(shape) << 12;
}

/// The image, the coordinate, an array's layer, and the operand: EMIT-151's spelling of a buffer atomic's update.
written write_texel_atomic(call_context const& ctx)
{
    auto const& info = k_atomics[ctx.data & 0xffu];
    auto const is_signed = (ctx.data & 0x100u) != 0;
    auto const shape = texture_shape((ctx.data >> 12) & 15u);
    auto const is_array = shape == texture_shape::d1_array || shape == texture_shape::d2_array;
    auto const dim = shape == texture_shape::d1 || shape == texture_shape::d1_array ? 1
                   : shape == texture_shape::d3                                     ? 3
                                                                                    : 2;
    auto const& image = ctx.arguments[0].text;
    auto const& xy = ctx.arguments[1].text;
    auto const operand = is_array ? 3 : 2;
    auto const has_operand = ctx.arguments.size() > operand;
    switch (ctx.target)
    {
    case language::hlsl:
    {
        // the texel is a place `Interlocked*` takes as a buffer's element, so the buffer atomic writes the rest
        auto const index = is_array ? cc::format("int{}({}, {})", dim + 1, xy, ctx.arguments[2].text) : cc::string(xy);
        auto arguments = cc::vector<written>();
        arguments.push_back({.text = cc::format("{}[{}]", image, index)});
        for (auto i = operand; i < ctx.arguments.size(); ++i)
            arguments.push_back(ctx.arguments[i]);
        return write_atomic({.target = ctx.target,
                             .arguments = arguments,
                             .builtins = ctx.builtins,
                             .data = ctx.data & 0x1ffu,
                             .mint = ctx.mint,
                             .result_type = ctx.result_type,
                             .is_pixel_stage = ctx.is_pixel_stage});
    }
    case language::msl:
    {
        // a texture's atomics take a four-wide value to store and give a four-wide one back, of which x is the texel
        auto coordinate = dim == 1 ? cc::format("uint({})", xy) : cc::format("uint{}({})", dim, xy);
        if (is_array)
            coordinate.appendf(", uint({})", ctx.arguments[2].text);
        auto const vector = is_signed ? "int4" : "uint4";
        auto const v = has_operand ? cc::string(ctx.arguments[operand].text) : cc::string();
        switch (info.op)
        {
        case atomic_op::compare_exchange_strong:
        case atomic_op::compare_exchange_weak:
        {
            // the coordinate and layer are evaluated once, ahead of the expected and desired values
            auto result = written();
            auto const texel_type = dim == 1 ? cc::string("uint") : cc::format("uint{}", dim);
            auto place = exchange_place{.atomic = image, .is_texel = true, .texel = minted(ctx, "exchange_texel")};
            result.lines.push_back(cc::format("const {} {} = {}({});", texel_type, place.texel, texel_type, xy));
            if (is_array)
            {
                auto const layer = minted(ctx, "exchange_layer");
                result.lines.push_back(cc::format("const uint {} = uint({});", layer, ctx.arguments[2].text));
                place.texel.appendf(", {}", layer);
            }
            return write_exchange(ctx, place, ctx.arguments[operand].text, ctx.arguments[operand + 1].text,
                                  cc::move(result));
        }
        case atomic_op::load:
            return {.text = cc::format("{}.atomic_load({}).x", image, coordinate)};
        case atomic_op::store:
            return {.text = cc::format("{}.atomic_store({}, {}({}))", image, coordinate, vector, v)};
        case atomic_op::exchange:
            return {.text = cc::format("{}.atomic_exchange({}, {}({})).x", image, coordinate, vector, v)};
        default:
            return {.text = cc::format("{}.atomic_{}({}, {}).x", image, info.msl, coordinate, v)};
        }
    }
    case language::wgsl:
        // WGSL lacks `image_atomics` (EMIT-109), so no entry point that reaches one is written for it
        return {};
    }
    return {};
}

/// A test's image has no texels, so an update of one finds zero, as a load of it does.
template <bool IsSigned>
void texel_zero(cc::span<scalar const>, cc::vector<scalar>& out)
{
    out.push_back({.kind = IsSigned ? value_kind::scalar_int : value_kind::scalar_uint, .bits = 0});
}

/// A weak compare-exchange of a texel finds zero too, and stores where zero is `expected`; it never fails spuriously.
void texel_exchanged_weak(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    auto const& expected = in[in.size() - 2];
    out.push_back({.kind = expected.kind, .bits = 0});
    out.push_back({.kind = value_kind::boolean, .bits = expected.bits == 0 ? 1u : 0u});
}

// ---- a geometry stage's streams ---------------------------------------------------------------------------------

/// The data word: 0 for `emit`, 1 for `end_strip`.
written write_stream(call_context const& ctx)
{
    auto const& s = ctx.arguments[0].text;
    if (ctx.data == 1)
        return {.text = cc::format("{}.RestartStrip()", s)};
    return {.text = cc::format("{}.Append({})", s, ctx.arguments[1].text)};
}
} // namespace

void sgl::builtins::register_sync(registry& r)
{
    r.add_comment("// A geometry stage's stream, called as its methods: `stream.emit(v)` appends the vertex `v`, which "
                  "the\n"
                  "// check pass takes as the stream's own type, and `stream.end_strip()` ends the strip (CHK-303).");
    for (auto const shape : {"point_stream", "line_stream", "triangle_stream"})
    {
        r.add(function_record{
            .signature = cc::format("@stages(.geometry) fun emit(s: mut {})", shape),
            .doc = "/// Appends a vertex, handed as a second argument of the stream's own type.",
            .evaluate = nothing,
            .write = {.kind = spelling_kind::custom, .custom = write_stream, .data = 0},
            .takes_element = true,
        });
        r.add(function_record{
            .signature = cc::format("@stages(.geometry) fun end_strip(s: mut {})", shape),
            .doc = "/// Ends the strip being appended, so the next vertex starts a new one.",
            .evaluate = nothing,
            .write = {.kind = spelling_kind::custom, .custom = write_stream, .data = 1},
        });
    }

    r.add_comment("// Barriers, which a compute shader reaches in control flow every thread of its workgroup takes "
                  "(CHK-282).");
    for (auto i = u32(0); i < u32(sizeof(k_barriers) / sizeof(k_barriers[0])); ++i)
        r.add(function_record{
            .signature = cc::format("@stages(.compute) fun {}()", k_barriers[i].name),
            .doc = k_barriers[i].doc,
            .evaluate = nothing,
            .write = {.kind = spelling_kind::custom,
                      .custom = write_barrier,
                      .writes_lines = is_msl,
                      .data = i,
                      .hlsl_names = k_barriers_hlsl,
                      .wgsl_names = k_barriers_wgsl,
                      .msl_names = k_barriers_msl},
            .is_barrier = true,
        });

    r.add_comment("// The uniform load, a barrier that then reads workgroup memory and gives every thread the same "
                  "value "
                  "(CHK-374).");
    for (auto const type : impl::k_selectable)
        r.add(function_record{
            .signature = cc::format("@stages(.compute) fun workgroup_uniform_load(m: {0}) -> {0}", type),
            .doc = "/// `m`, a member of workgroup memory, read once every thread of the workgroup has arrived.",
            .evaluate = loaded,
            .write = {.kind = spelling_kind::custom,
                      .custom = write_uniform_load,
                      .writes_lines = is_not_wgsl,
                      .hlsl_names = k_barriers_hlsl,
                      .wgsl_names = k_uniform_load_wgsl,
                      .msl_names = k_barriers_msl},
            .is_barrier = true,
            .is_uniform_load = true,
        });

    r.add_comment("// What a weak compare-exchange gives, a struct of fields every target declares as a program's "
                  "own.\n"
                  "// The weak one may fail although the atomic held `expected`, so `old` alone cannot say whether it "
                  "stored (EMIT-157).");
    r.add_declaration("/// What a weak compare-exchange read, and whether it stored `desired`.\n"
                      "struct exchange_result[T]:\n"
                      "    old: T\n"
                      "    exchanged: bool");

    r.add_comment("// Atomics, called as methods of the atomic they update: `stats.hits[0].add(1)`.\n"
                  "// Each is relaxed, and each but a store gives the value before; a vertex, an any-hit and an "
                  "intersection stage have none (CHK-296).");
    for (auto const is_signed : {false, true})
    {
        auto const t = is_signed ? "int" : "uint";
        for (auto const& info : k_atomics)
        {
            auto const signature
                = info.op == atomic_op::load  ? cc::format("fun load(a: atomic[{0}]) -> {0}", t)
                : info.op == atomic_op::store ? cc::format("fun store(a: out atomic[{0}], v: {0})", t)
                : info.op == atomic_op::compare_exchange_strong
                    ? cc::format("fun {1}(a: mut atomic[{0}], expected: {0}, desired: {0}) -> {0}", t, info.name)
                : info.op == atomic_op::compare_exchange_weak
                    ? cc::format("fun {1}(a: mut atomic[{0}], expected: {0}, desired: {0}) -> exchange_result[{0}]", t,
                                 info.name)
                    : cc::format("fun {1}(a: mut atomic[{0}], v: {0}) -> {0}", t, info.name);
            r.add(function_record{
                .signature = cc::format("{} {}", k_atomic_stages, signature),
                .doc = info.doc,
                .evaluate = is_signed ? evaluator_of<true>(info.op) : evaluator_of<false>(info.op),
                .write = {.kind = spelling_kind::custom,
                          .custom = write_atomic,
                          .writes_lines = is_compare_exchange(info.op) ? every_language : is_hlsl,
                          .data = atomic_data(info.op, is_signed),
                          .hlsl_names = k_atomics_hlsl,
                          .wgsl_names = k_atomics_wgsl,
                          .msl_names = k_atomics_msl},
                .is_atomic = true,
                .gives_prelude_struct = info.op == atomic_op::compare_exchange_weak,
                .pixel_features = strong_pixel_features(info.op),
            });
        }
    }

    r.add_comment("// The atomics of an `@atomic` image's texels, which the flat tree calls for `img[xy].max(v)` "
                  "(CHK-373).\n"
                  "// Each takes the image, the coordinate and an array's layer where the buffer atomic takes the "
                  "atomic.");
    for (auto const& entry : check::k_shapes)
    {
        if (entry.image.empty())
            continue;
        auto const is_array = entry.shape == texture_shape::d1_array || entry.shape == texture_shape::d2_array;
        auto const coordinate = entry.shape == texture_shape::d1 || entry.shape == texture_shape::d1_array ? "int"
                              : entry.shape == texture_shape::d3                                           ? "int3"
                                                                                                           : "int2";
        for (auto const is_signed : {false, true})
        {
            auto const t = is_signed ? "int" : "uint";
            auto const texel
                = cc::format("i: mut {}[{}], xy: {}{}", entry.image, t, coordinate, is_array ? ", layer: int" : "");
            for (auto const& info : k_atomics)
            {
                auto const signature
                    = info.op == atomic_op::load  ? cc::format("fun texel_load({}) -> {}", texel, t)
                    : info.op == atomic_op::store ? cc::format("fun texel_store({}, v: {})", texel, t)
                    : info.op == atomic_op::compare_exchange_strong
                        ? cc::format("fun texel_{0}({1}, expected: {2}, desired: {2}) -> {2}", info.name, texel, t)
                    : info.op == atomic_op::compare_exchange_weak
                        ? cc::format("fun texel_{0}({1}, expected: {2}, desired: {2}) -> exchange_result[{2}]",
                                     info.name, texel, t)
                        : cc::format("fun texel_{}({}, v: {}) -> {}", info.name, texel, t, t);
                r.add(function_record{
                    .signature = cc::format("@internal {} {}", k_atomic_stages, signature),
                    .doc = info.doc,
                    .evaluate = info.op == atomic_op::store                 ? nothing
                              : info.op == atomic_op::compare_exchange_weak ? texel_exchanged_weak
                              : is_signed                                   ? texel_zero<true>
                                                                            : texel_zero<false>,
                    .write = {.kind = spelling_kind::custom,
                              .custom = write_texel_atomic,
                              .writes_lines = is_compare_exchange(info.op) ? is_not_wgsl : is_hlsl,
                              .data = texel_atomic_data(info.op, is_signed, entry.shape),
                              .hlsl_names = k_atomics_hlsl},
                    .is_atomic = true,
                    .gives_prelude_struct = info.op == atomic_op::compare_exchange_weak,
                    .pixel_features = strong_pixel_features(info.op),
                });
            }
        }
    }
}
