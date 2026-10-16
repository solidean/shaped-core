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
    compare_exchange,
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
    {atomic_op::compare_exchange, "compare_exchange", "CompareExchange", "CompareExchangeWeak", "compare_exchange_weak",
     "/// Stores `desired` where the value is `expected`, and gives the value before.\n"
     "/// So it stored exactly where what it gives is `expected`."},
};

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
};

/// A compare-exchange is a loop in WGSL and MSL, and HLSL's out value (EMIT-157).
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

/// EMIT-157: WGSL's and MSL's compare-exchange is weak, so a loop retries the one that failed on the expected value.
/// The atomic's place, `expected` and `desired` are each evaluated once, in that order, ahead of the loop.
written write_strong_exchange(call_context const& ctx,
                              cc::string_view place,
                              cc::string_view expected,
                              cc::string_view desired)
{
    auto const is_signed = (ctx.data & 0x100u) != 0;
    auto const before = minted(ctx, "exchange_before");
    auto const expected_name = minted(ctx, "exchange_expected");
    auto const desired_name = minted(ctx, "exchange_desired");
    auto const place_name = minted(ctx, "exchange_place");
    auto result = written{.text = before};
    if (ctx.target == language::wgsl)
    {
        auto const t = is_signed ? "i32" : "u32";
        auto const exchanged = minted(ctx, "exchange_result");
        result.lines.push_back(cc::format("let {} = &{};", place_name, place));
        result.lines.push_back(cc::format("let {}: {} = {};", expected_name, t, expected));
        result.lines.push_back(cc::format("let {}: {} = {};", desired_name, t, desired));
        result.lines.push_back(cc::format("var {}: {};", before, t));
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
    result.lines.push_back(cc::format("const auto {} = &{};", place_name, place));
    result.lines.push_back(cc::format("const {} {} = {};", t, expected_name, expected));
    result.lines.push_back(cc::format("const {} {} = {};", t, desired_name, desired));
    result.lines.push_back(cc::format("{} {} = {};", t, before, expected_name));
    auto const weak = cc::format("atomic_compare_exchange_weak_explicit({}, &{}, {}, memory_order_relaxed, "
                                 "memory_order_relaxed)",
                                 place_name, before, desired_name);
    result.lines.push_back(cc::format("while (!{} && {} == {})", weak, before, expected_name));
    result.lines.push_back("{");
    result.lines.push_back("}");
    return result;
}

written write_atomic(call_context const& ctx)
{
    auto const& info = k_atomics[ctx.data & 0xffu];
    auto const is_signed = (ctx.data & 0x100u) != 0;
    auto const& a = ctx.arguments[0].text;
    auto const v = ctx.arguments.size() > 1 ? wrapped(ctx.arguments[1], precedence::primary) : cc::string();
    if (info.op == atomic_op::compare_exchange && ctx.target != language::hlsl)
        return write_strong_exchange(ctx, a, ctx.arguments[1].text, ctx.arguments[2].text);
    switch (ctx.target)
    {
    case language::hlsl:
    {
        // `Interlocked*` gives the value before through an out parameter, so a local declared ahead holds it
        auto const before = minted(ctx, "atomic_before");
        auto const operand = info.op == atomic_op::subtract ? cc::format("-{}", v)
                           : info.op == atomic_op::load     ? cc::string(is_signed ? "0" : "0u")
                           : info.op == atomic_op::compare_exchange
                               ? cc::format("{}, {}", v, wrapped(ctx.arguments[2], precedence::primary))
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
    case atomic_op::compare_exchange:
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
void exchanged(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(in[0].bits == in[1].bits ? in[2] : in[0]);
}

template <bool IsSigned>
evaluator evaluator_of(atomic_op op)
{
    switch (op)
    {
    case atomic_op::compare_exchange:
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
                             .result_type = ctx.result_type});
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
        case atomic_op::compare_exchange:
        {
            // EMIT-157 as a buffer atomic's, with the coordinate in place of the pointer: each evaluated once, first
            auto const t = is_signed ? "int" : "uint";
            auto const at = minted(ctx, "exchange_texel");
            auto const expected = minted(ctx, "exchange_expected");
            auto const desired = minted(ctx, "exchange_desired");
            auto const before = minted(ctx, "exchange_before");
            auto result = written{.text = cc::format("{}.x", before)};
            auto const texel_type = dim == 1 ? cc::string("uint") : cc::format("uint{}", dim);
            result.lines.push_back(cc::format("const {} {} = {}({});", texel_type, at, texel_type, xy));
            auto where = cc::string(at);
            if (is_array)
            {
                auto const layer = minted(ctx, "exchange_layer");
                result.lines.push_back(cc::format("const uint {} = uint({});", layer, ctx.arguments[2].text));
                where.appendf(", {}", layer);
            }
            result.lines.push_back(cc::format("const {} {} = {};", t, expected, ctx.arguments[operand].text));
            result.lines.push_back(cc::format("const {} {} = {};", t, desired, ctx.arguments[operand + 1].text));
            result.lines.push_back(cc::format("{} {} = {}({});", vector, before, vector, expected));
            result.lines.push_back(cc::format("while (!{}.atomic_compare_exchange_weak({}, &{}, {}({})) && {}.x == {})",
                                              image, where, before, vector, desired, before, expected));
            result.lines.push_back("{");
            result.lines.push_back("}");
            return result;
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

    r.add_comment("// Atomics, called as methods of the atomic they update: `stats.hits[0].add(1)`.\n"
                  "// Each is relaxed, and each but a store gives the value before; a vertex stage has none "
                  "(CHK-296).");
    for (auto const is_signed : {false, true})
    {
        auto const t = is_signed ? "int" : "uint";
        for (auto const& info : k_atomics)
        {
            auto const signature
                = info.op == atomic_op::load  ? cc::format("fun load(a: atomic[{0}]) -> {0}", t)
                : info.op == atomic_op::store ? cc::format("fun store(a: out atomic[{0}], v: {0})", t)
                : info.op == atomic_op::compare_exchange
                    ? cc::format("fun compare_exchange(a: mut atomic[{0}], expected: {0}, desired: {0}) -> {0}", t)
                    : cc::format("fun {1}(a: mut atomic[{0}], v: {0}) -> {0}", t, info.name);
            r.add(function_record{
                .signature = cc::format("@stages(.pixel, .compute) {}", signature),
                .doc = info.doc,
                .evaluate = is_signed ? evaluator_of<true>(info.op) : evaluator_of<false>(info.op),
                .write = {.kind = spelling_kind::custom,
                          .custom = write_atomic,
                          .writes_lines = info.op == atomic_op::compare_exchange ? every_language : is_hlsl,
                          .data = atomic_data(info.op, is_signed),
                          .hlsl_names = k_atomics_hlsl,
                          .wgsl_names = k_atomics_wgsl,
                          .msl_names = k_atomics_msl},
                .is_atomic = true,
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
                    : info.op == atomic_op::compare_exchange
                        ? cc::format("fun texel_compare_exchange({0}, expected: {1}, desired: {1}) -> {1}", texel, t)
                        : cc::format("fun texel_{}({}, v: {}) -> {}", info.name, texel, t, t);
                r.add(function_record{
                    .signature = cc::format("@internal @stages(.pixel, .compute) {}", signature),
                    .doc = info.doc,
                    .evaluate = info.op == atomic_op::store ? nothing
                              : is_signed                   ? texel_zero<true>
                                                            : texel_zero<false>,
                    .write = {.kind = spelling_kind::custom,
                              .custom = write_texel_atomic,
                              .writes_lines = info.op == atomic_op::compare_exchange ? is_not_wgsl : is_hlsl,
                              .data = texel_atomic_data(info.op, is_signed, entry.shape),
                              .hlsl_names = k_atomics_hlsl},
                    .is_atomic = true,
                });
            }
        }
    }
}
