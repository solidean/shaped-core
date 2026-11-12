#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
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
constexpr cc::string_view k_mesh_output_hlsl[] = {"SetMeshOutputCounts"};
constexpr cc::string_view k_dispatch_mesh_hlsl[] = {"DispatchMesh", "sgl_payload"};
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

// ---- LEGAL-69: the shadow of workgroup memory ----------------------------------------------------------------------

// One bit per scalar of the entry point's workgroup memory, which a robustness profile sets where a scalar is stored
// and tests where it is read.
// The guards write a read and a mark as these calls, and the last step of legalize makes each the atomic on its word
// of the shadow (LEGAL-71), so no target writes one.
// The interpreter never runs these: it reports such a read itself (EVAL-92), so to it every bit is set.

/// Nothing: legalize rewrites every call of the shadow before a target writes one (LEGAL-71).
written never_written(call_context const&)
{
    return {};
}

void every_bit_written(cc::span<check::scalar const>, cc::vector<check::scalar>& out)
{
    out.push_back(scalar::of_uint(0xffffffffu));
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

/// EMIT-163: a pixel stage's strong loop must keep a helper invocation out, which needs the target to tell one.
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
    "simd_is_helper_thread",     "atomic_min_explicit",       "atomic_max_explicit",
};

/// CHK-296: every stage that writes memory once per invocation the program can count.
/// A vertex stage has no writable storage in WebGPU, and any-hit and intersection stages run as often as traversal decides.
constexpr cc::string_view k_atomic_stages
    = "@stages(.pixel, .compute, .task, .mesh, .raygen, .miss, .closest_hit, .callable)";

/// A compare-exchange writes statements in every target (EMIT-161).
bool every_language(language)
{
    return true;
}

/// The data word: the operation, whether the atomic holds a signed integer, and whether a 64-bit one (CHK-296).
u32 atomic_data(atomic_op op, bool is_signed, bool is_wide = false)
{
    return u32(op) | (is_signed ? 0x100u : 0u) | (is_wide ? 0x200u : 0u);
}

/// The integer an atomic of `data` holds, as `target` spells it; a 64-bit one is never MSL's or WGSL's (EMIT-192).
cc::string_view atomic_scalar(u32 data, language target)
{
    auto const is_signed = (data & 0x100u) != 0;
    if ((data & 0x200u) != 0)
        return is_signed ? "int64_t" : "uint64_t";
    if (target == language::wgsl)
        return is_signed ? "i32" : "u32";
    return is_signed ? "int" : "uint";
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

/// EMIT-161 and EMIT-162: WGSL's and MSL's compare-exchange, whose own is weak.
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
        // a pixel stage's needs helper_lane_query, which WGSL lacks (EMIT-163), since it cannot keep a helper out of this loop
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
    // a helper thread's atomics store nothing and read what they please, so it never enters the loop (EMIT-162)
    auto const helper = ctx.is_pixel_stage ? cc::string_view("!simd_is_helper_thread() && ") : cc::string_view();
    result.lines.push_back(
        cc::format("while ({}!{} && {} == {})", helper, attempt(target, desired_name), read, expected_name));
    result.lines.push_back("{");
    result.lines.push_back("}");
    result.text = read;
    return result;
}

/// EMIT-161: HLSL's one compare-exchange, which is strong; a weak one fills the prelude's struct from it.
written write_hlsl_exchange(call_context const& ctx, cc::string_view place, written result)
{
    auto const& info = k_atomics[ctx.data & 0xffu];
    auto const t = atomic_scalar(ctx.data, language::hlsl);
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
        auto const is_wide = (ctx.data & 0x200u) != 0;
        auto const zero = is_wide ? (is_signed ? "0LL" : "0ULL") : (is_signed ? "0" : "0u");
        auto const operand = info.op == atomic_op::subtract ? cc::format("-{}", v)
                           : info.op == atomic_op::load     ? cc::string(zero)
                                                            : v;
        auto result = written{.text = before};
        result.lines.push_back(cc::format("{} {};", atomic_scalar(ctx.data, language::hlsl), before));
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
        // EMIT-218: MSL's ulong min and max give nothing back, so they stand as statements alone
        if ((ctx.data & 0x200u) != 0 && (info.op == atomic_op::min || info.op == atomic_op::max))
        {
            auto result = written{};
            result.lines.push_back(cc::format("atomic_{}_explicit(&{}, {}, memory_order_relaxed);",
                                              info.op == atomic_op::min ? "min" : "max", a, v));
            return result;
        }
        if (info.op == atomic_op::load)
            return {.text = cc::format("atomic_load_explicit(&{}, memory_order_relaxed)", a)};
        return {.text = cc::format("atomic_{}_explicit(&{}, {}, memory_order_relaxed)", info.msl, a, v)};
    }
    return {};
}

/// An update of the integer of `Kind`, 32 or 64 bits, wrapping in either.
template <value_kind Kind>
scalar updated(atomic_op op, scalar old, scalar v)
{
    constexpr auto is_wide = check::is_64_bit(Kind);
    constexpr auto is_signed = Kind == value_kind::scalar_int || Kind == value_kind::scalar_long;
    auto const a = old.wide_bits();
    auto const b = v.wide_bits();
    auto const less = !is_signed ? a < b : is_wide ? i64(a) < i64(b) : i32(u32(a)) < i32(u32(b));
    auto const of = [](u64 bits)
    { return check::is_64_bit(Kind) ? scalar::of_wide(Kind, bits) : scalar{.kind = Kind, .bits = u32(bits)}; };
    switch (op)
    {
    case atomic_op::add:
        return of(a + b);
    case atomic_op::subtract:
        return of(a - b);
    case atomic_op::min:
        return of(less ? a : b);
    case atomic_op::max:
        return of(less ? b : a);
    case atomic_op::bit_and:
        return of(a & b);
    case atomic_op::bit_or:
        return of(a | b);
    case atomic_op::bit_xor:
        return of(a ^ b);
    case atomic_op::exchange:
    case atomic_op::store:
        return of(b);
    case atomic_op::load:
        return old;
    case atomic_op::compare_exchange_strong:
    case atomic_op::compare_exchange_weak:
        break;
    }
    return old;
}

/// The atomic's value first, then the operand; gives what the atomic holds after (EVAL-93).
template <atomic_op Op, value_kind Kind>
void atomic_update(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(updated<Kind>(Op, in[0], in.size() > 1 ? in[1] : in[0]));
}

/// The atomic's value, `expected` and `desired`: a run is one invocation, so no other update makes it fail (EVAL-93).
/// The interpreter decides a weak one's spurious failures itself, and gives the value read and whether it stored.
void exchanged(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(in[0].wide_bits() == in[1].wide_bits() ? in[2] : in[0]);
}

template <value_kind Kind>
evaluator evaluator_of(atomic_op op)
{
    switch (op)
    {
    case atomic_op::compare_exchange_strong:
    case atomic_op::compare_exchange_weak:
        return exchanged;
    case atomic_op::add:
        return atomic_update<atomic_op::add, Kind>;
    case atomic_op::subtract:
        return atomic_update<atomic_op::subtract, Kind>;
    case atomic_op::min:
        return atomic_update<atomic_op::min, Kind>;
    case atomic_op::max:
        return atomic_update<atomic_op::max, Kind>;
    case atomic_op::bit_and:
        return atomic_update<atomic_op::bit_and, Kind>;
    case atomic_op::bit_or:
        return atomic_update<atomic_op::bit_or, Kind>;
    case atomic_op::bit_xor:
        return atomic_update<atomic_op::bit_xor, Kind>;
    case atomic_op::exchange:
        return atomic_update<atomic_op::exchange, Kind>;
    case atomic_op::load:
        return atomic_update<atomic_op::load, Kind>;
    case atomic_op::store:
        return atomic_update<atomic_op::store, Kind>;
    }
    return nullptr;
}
// ---- a float's atomics -----------------------------------------------------------------------------------------
//
// Every target holds an `atomic[float]` as the bits of a `uint` atomic, so the host sees the float it wrote.
// A load, a store and an exchange carry the bits across; an add retries a compare-exchange of them until no other
// update came between its read and its write.

constexpr cc::string_view k_float_atomics_hlsl[] = {
    "InterlockedOr",
    "InterlockedExchange",
    "InterlockedCompareExchange",
    "asfloat",
    "asuint",
    "sgl_atomic_add_float",
    "sgl_atomic_load_float",
    "sgl_atomic_store_float",
    "sgl_atomic_exchange_float",
};
constexpr cc::string_view k_float_atomics_wgsl[] = {
    "atomicLoad", "atomicStore", "atomicExchange", "atomicCompareExchangeWeak", "bitcast",
};
constexpr cc::string_view k_float_atomics_msl[] = {
    "atomic_load_explicit",      "atomic_store_explicit",
    "atomic_exchange_explicit",  "atomic_compare_exchange_weak_explicit",
    "memory_order_relaxed",      "as_type",
    "atomic_fetch_add_explicit",
};

/// Vulkan's native float atomics, which HLSL has no word for: SPIR-V's own instructions, declared once ahead of the entry
/// point of a text whose `atomic[float]` is a `float`.
/// The scope is Device (1) and the semantics Relaxed (0), the ordering every SGL atomic has.
cc::string float_atomic_helper(helper_context const& c)
{
    if (c.target != language::hlsl || c.argument_types.empty() || c.argument_types[0] != "float")
        return {};
    return "[[vk::ext_extension(\"SPV_EXT_shader_atomic_float_add\")]]\n"
           "[[vk::ext_capability(6033)]]\n"
           "[[vk::ext_instruction(6035)]]\n"
           "float sgl_atomic_add_float([[vk::ext_reference]] float place, uint scope, uint semantics, float value);\n"
           "[[vk::ext_instruction(227)]]\n"
           "float sgl_atomic_load_float([[vk::ext_reference]] float place, uint scope, uint semantics);\n"
           "[[vk::ext_instruction(228)]]\n"
           "void sgl_atomic_store_float([[vk::ext_reference]] float place, uint scope, uint semantics, float value);\n"
           "[[vk::ext_instruction(229)]]\n"
           "float sgl_atomic_exchange_float([[vk::ext_reference]] float place, uint scope, uint semantics, float "
           "value);\n";
}

/// An add stands in a loop, which a helper invocation, whose atomics have no effect, would never leave.
constexpr cc::string_view k_float_add_stages = "@stages(.compute, .raygen, .miss, .closest_hit, .callable)";

written write_float_atomic(call_context const& ctx)
{
    auto const op = atomic_op(ctx.data);
    auto const& a = ctx.arguments[0].text;
    auto const v = ctx.arguments.size() > 1 ? wrapped(ctx.arguments[1], precedence::primary) : cc::string();
    auto result = written();
    // the target's own float atomic, one instruction each
    if (ctx.native_float_atomics && ctx.target == language::hlsl)
    {
        if (op == atomic_op::load)
            return {.text = cc::format("sgl_atomic_load_float({}, 1, 0)", a)};
        auto const name = op == atomic_op::store ? "store" : op == atomic_op::exchange ? "exchange" : "add";
        return {.text = cc::format("sgl_atomic_{}_float({}, 1, 0, {})", name, a, v)};
    }
    if (ctx.native_float_atomics && ctx.target == language::msl)
    {
        if (op == atomic_op::load)
            return {.text = cc::format("atomic_load_explicit(&{}, memory_order_relaxed)", a)};
        auto const name = op == atomic_op::store ? "store" : op == atomic_op::exchange ? "exchange" : "fetch_add";
        return {.text = cc::format("atomic_{}_explicit(&{}, {}, memory_order_relaxed)", name, a, v)};
    }
    switch (ctx.target)
    {
    case language::hlsl:
    {
        auto const before = minted(ctx, "atomic_before");
        if (op == atomic_op::add)
        {
            // the plain read is only a first guess, which the compare-exchange checks
            auto const operand = minted(ctx, "atomic_operand");
            auto const expected = minted(ctx, "atomic_expected");
            result.lines.push_back(cc::format("const float {} = {};", operand, v));
            result.lines.push_back(cc::format("uint {} = {};", expected, a));
            result.lines.push_back(cc::format("uint {};", before));
            result.lines.push_back("[allow_uav_condition] for (;;)");
            result.lines.push_back("{");
            result.lines.push_back(cc::format("    InterlockedCompareExchange({}, {}, asuint(asfloat({}) + {}), {});",
                                              a, expected, expected, operand, before));
            result.lines.push_back(cc::format("    if ({} == {})", before, expected));
            result.lines.push_back("        break;");
            result.lines.push_back(cc::format("    {} = {};", expected, before));
            result.lines.push_back("}");
            result.text = cc::format("asfloat({})", before);
            return result;
        }
        result.lines.push_back(cc::format("uint {};", before));
        if (op == atomic_op::load)
            result.lines.push_back(cc::format("InterlockedOr({}, 0u, {});", a, before));
        else
            result.lines.push_back(cc::format("InterlockedExchange({}, asuint({}), {});", a, v, before));
        result.text = op == atomic_op::store ? cc::string() : cc::format("asfloat({})", before);
        return result;
    }
    case language::wgsl:
    {
        if (op == atomic_op::load)
            return {.text = cc::format("bitcast<f32>(atomicLoad(&{}))", a)};
        if (op == atomic_op::store)
            return {.text = cc::format("atomicStore(&{}, bitcast<u32>({}))", a, v)};
        if (op == atomic_op::exchange)
            return {.text = cc::format("bitcast<f32>(atomicExchange(&{}, bitcast<u32>({})))", a, v)};
        auto const place = minted(ctx, "atomic_place");
        auto const operand = minted(ctx, "atomic_operand");
        auto const before = minted(ctx, "atomic_before");
        auto const outcome = minted(ctx, "exchange_outcome");
        result.lines.push_back(cc::format("let {} = &{};", place, a));
        result.lines.push_back(cc::format("let {}: f32 = {};", operand, v));
        result.lines.push_back(cc::format("var {}: u32 = atomicLoad({});", before, place));
        result.lines.push_back("loop {");
        result.lines.push_back(cc::format("    let {} = atomicCompareExchangeWeak({}, {}, "
                                          "bitcast<u32>(bitcast<f32>({}) + {}));",
                                          outcome, place, before, before, operand));
        result.lines.push_back(cc::format("    if {}.exchanged {{", outcome));
        result.lines.push_back("        break;");
        result.lines.push_back("    }");
        result.lines.push_back(cc::format("    {} = {}.old_value;", before, outcome));
        result.lines.push_back("}");
        result.text = cc::format("bitcast<f32>({})", before);
        return result;
    }
    case language::msl:
    {
        if (op == atomic_op::load)
            return {.text = cc::format("as_type<float>(atomic_load_explicit(&{}, memory_order_relaxed))", a)};
        if (op == atomic_op::store)
            return {.text = cc::format("atomic_store_explicit(&{}, as_type<uint>({}), memory_order_relaxed)", a, v)};
        if (op == atomic_op::exchange)
            return {.text = cc::format(
                        "as_type<float>(atomic_exchange_explicit(&{}, as_type<uint>({}), memory_order_relaxed))", a, v)};
        // a failed attempt leaves what it read in `before`, which the next attempt adds to
        auto const operand = minted(ctx, "atomic_operand");
        auto const before = minted(ctx, "atomic_before");
        result.lines.push_back(cc::format("const float {} = {};", operand, v));
        result.lines.push_back(cc::format("uint {} = atomic_load_explicit(&{}, memory_order_relaxed);", before, a));
        result.lines.push_back(cc::format("while (!atomic_compare_exchange_weak_explicit(&{}, &{}, "
                                          "as_type<uint>(as_type<float>({}) + "
                                          "{}), memory_order_relaxed, memory_order_relaxed)) {{}}",
                                          a, before, before, operand));
        result.text = cc::format("as_type<float>({})", before);
        return result;
    }
    }
    return {};
}

/// The atomic's float first, then the operand; gives what the atomic holds after (EVAL-93).
template <atomic_op Op>
void float_atomic_update(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    if constexpr (Op == atomic_op::add)
        out.push_back(scalar::of(in[0].as_float() + in[1].as_float()));
    else if constexpr (Op == atomic_op::load)
        out.push_back(in[0]);
    else
        out.push_back(in[1]);
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
        // a 1D image is 2D one texel high on metal (EMIT-100)
        auto coordinate = dim == 1 ? cc::format("uint2(uint({}), 0u)", xy) : cc::format("uint{}({})", dim, xy);
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
            auto const texel_type = dim == 1 ? cc::string("uint2") : cc::format("uint{}", dim);
            auto place = exchange_place{.atomic = image, .is_texel = true, .texel = minted(ctx, "exchange_texel")};
            auto const texel = dim == 1 ? cc::format("uint2(uint({}), 0u)", xy) : cc::format("{}({})", texel_type, xy);
            result.lines.push_back(cc::format("const {} {} = {};", texel_type, place.texel, texel));
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
            // the value first, as `write` takes it, where every other texel atomic takes the coordinate first
            return {.text = cc::format("{}.atomic_store({}({}), {})", image, vector, v, coordinate)};
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


// ---- a geometry stage's streams ---------------------------------------------------------------------------------

/// The data word: 0 for `emit`, 1 for `end_strip`.
written write_stream(call_context const& ctx)
{
    auto const& s = ctx.arguments[0].text;
    if (ctx.data == 1)
        return {.text = cc::format("{}.RestartStrip()", s)};
    return {.text = cc::format("{}.Append({})", s, ctx.arguments[1].text)};
}

// ---- a mesh stage's output -------------------------------------------------------------------------------------------

/// The data word: 0 for `set_counts`, 1 for `vertex`, 2 for `triangle` (EMIT-205).
/// HLSL names the output's two arrays after the parameter, as the entry point declares them.
written write_mesh_output(call_context const& ctx)
{
    auto const& o = ctx.arguments[0].text;
    // EMIT-206: Metal's mesh object takes the primitives' count alone, and a primitive's corners index by index
    if (ctx.target == language::msl)
    {
        if (ctx.data == 0)
            return {.text = cc::format("{}.set_primitive_count({})", o, ctx.arguments[2].text)};
        if (ctx.data == 1)
            return {.text = cc::format("{}.set_vertex({}, {})", o, ctx.arguments[1].text, ctx.arguments[2].text)};
        if (ctx.data == 3)
            return {.text = cc::format("{}.set_primitive({}, {})", o, ctx.arguments[1].text, ctx.arguments[2].text)};
        auto const corners = ctx.data == 2 ? 3 : 2;
        auto const mint = [&](cc::string_view name) { return ctx.mint.is_valid() ? ctx.mint(name) : cc::string(name); };
        auto const first = mint("sgl_first");
        auto const indices = mint("sgl_indices");
        auto result = written();
        result.lines.push_back(cc::format("const uint {} = ({}) * {}u;", first, ctx.arguments[1].text, corners));
        result.lines.push_back(cc::format("const uint{} {} = {};", corners, indices, ctx.arguments[2].text));
        constexpr char lanes[] = {'x', 'y', 'z'};
        for (auto c = 0; c < corners; ++c)
            result.lines.push_back(cc::format("{}.set_index({} + {}u, {}.{});", o, first, c, indices, lanes[c]));
        return result;
    }
    if (ctx.data == 0)
        return {.text = cc::format("SetMeshOutputCounts({}, {})", ctx.arguments[1].text, ctx.arguments[2].text)};
    auto const array = ctx.data == 1 ? "vertices"
                     : ctx.data == 2 ? "triangles"
                     : ctx.data == 4 ? "lines"
                                     : "primitives";
    return {.text = cc::format("{}_{}[{}] = {}", o, array, ctx.arguments[1].text, ctx.arguments[2].text)};
}

/// EMIT-207: HLSL hands a payload over from groupshared memory alone, so the call stores it there first.
written write_dispatch_mesh(call_context const& ctx)
{
    // EMIT-207: Metal's object stage writes its payload and its grid, once, from its first invocation
    if (ctx.target == language::msl)
    {
        auto result = written();
        result.lines.push_back("if (sgl_thread == 0u)");
        result.lines.push_back("{");
        result.lines.push_back(cc::format("    sgl_payload = {};", ctx.arguments[3].text));
        result.lines.push_back(cc::format("    sgl_grid.set_threadgroups_per_grid(uint3({}, {}, {}));",
                                          ctx.arguments[0].text, ctx.arguments[1].text, ctx.arguments[2].text));
        result.lines.push_back("}");
        return result;
    }
    auto result = written{.text = cc::format("DispatchMesh({}, {}, {}, sgl_payload)", ctx.arguments[0].text,
                                             ctx.arguments[1].text, ctx.arguments[2].text)};
    // no WGSL text writes a task stage, which the feature refuses
    if (ctx.target == language::hlsl)
        result.lines.push_back(cc::format("sgl_payload = {};", ctx.arguments[3].text));
    return result;
}

/// The groupshared payload the call stores into, of the payload's type.
cc::string dispatch_mesh_helper(helper_context const& h)
{
    if (h.target != language::hlsl || h.argument_types.size() < 4)
        return {};
    return cc::format("groupshared {} sgl_payload;\n", h.argument_types[3]);
}

bool is_hlsl_text(language l)
{
    return l == language::hlsl;
}

bool is_hlsl_or_msl_text(language l)
{
    return l == language::hlsl || l == language::msl;
}

bool is_msl_text(language l)
{
    return l == language::msl;
}
} // namespace

void sgl::builtins::register_sync(registry& r)
{
    r.add_comment("// A task stage's launch (CHK-485): `dispatch_mesh(x, y, z, payload)` starts x * y * z mesh "
                  "workgroups, "
                  "each\n"
                  "// handed the struct `payload`, once, as the entry point's last statement.");
    r.add(function_record{
        .signature = "@stages(.task) fun dispatch_mesh(x: uint, y: uint, z: uint)",
        .doc = "/// Launches the mesh workgroups, handing each the payload passed as a fourth argument.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom,
                  .custom = write_dispatch_mesh,
                  .helper = dispatch_mesh_helper,
                  .writes_lines = is_hlsl_or_msl_text,
                  .hlsl_names = k_dispatch_mesh_hlsl,
                  .wgsl_names = k_dispatch_mesh_hlsl,
                  .msl_names = k_dispatch_mesh_hlsl},
        .is_barrier = true,
        .takes_element = true,
    });
    r.add_comment("// A mesh stage's output, called as its methods (CHK-482): `out.set_counts(v, t)` says how many\n"
                  "// vertices and triangles it writes, once, in control flow every invocation of the workgroup "
                  "takes.\n"
                  "// `out.vertex(i, v)` writes the vertex at `i`, of the output's own type, and `out.triangle(i, t)`\n"
                  "// the triangle of the three vertices `t` indexes.");
    r.add(function_record{
        .signature = "@stages(.mesh) fun set_counts(o: mut mesh_output, vertices: uint, triangles: uint)",
        .doc = "/// How many vertices and triangles the workgroup writes, at most the entry point's bounds.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom,
                  .custom = write_mesh_output,
                  .data = 0,
                  // no WGSL text writes a mesh stage, and the name stays the target's alike
                  .hlsl_names = k_mesh_output_hlsl,
                  .wgsl_names = k_mesh_output_hlsl,
                  .msl_names = k_mesh_output_hlsl},
        .is_barrier = true,
    });
    r.add(function_record{
        .signature = "@stages(.mesh) fun vertex(o: mut mesh_output, i: uint)",
        .doc = "/// Writes the vertex at `i`, handed as a third argument of the output's own type.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom, .custom = write_mesh_output, .data = 1},
        .takes_element = true,
    });
    r.add(function_record{
        .signature = "@stages(.mesh) fun primitive(o: mut mesh_output, i: uint)",
        .doc = "/// Writes the values of the triangle at `i`, handed as a third argument of the output's second "
               "struct.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom, .custom = write_mesh_output, .data = 3},
        .takes_element = true,
    });
    r.add(function_record{
        .signature = "@stages(.mesh) fun triangle(o: mut mesh_output, i: uint, t: uint3)",
        .doc = "/// Writes the triangle at `i`, of the three vertices `t` indexes.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom, .custom = write_mesh_output, .writes_lines = is_msl_text, .data = 2},
    });
    r.add(function_record{
        .signature = "@stages(.mesh) fun line(o: mut mesh_output, i: uint, l: uint2)",
        .doc = "/// Writes the line at `i`, of the two vertices `l` indexes, in a mesh stage of `lines = M`.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom, .custom = write_mesh_output, .writes_lines = is_msl_text, .data = 4},
    });

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
            .signature = cc::format("@stages(.compute, .task, .mesh) fun {}()", k_barriers[i].name),
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

    r.add_comment("// The shadow of workgroup memory a robustness profile tests a read by (LEGAL-69): a bit per "
                  "scalar,\n"
                  "// set where it is stored; the legalizer alone calls these.");
    r.add(function_record{
        .signature = "@internal @stages(.compute) fun workgroup_written_word(word: uint) -> uint",
        .doc = "/// Word `word` of the shadow, one bit per scalar some invocation stored.",
        .evaluate = every_bit_written,
        .write = {.kind = spelling_kind::custom, .custom = never_written},
        .reads_only = true,
    });
    r.add(function_record{
        .signature = "@internal @stages(.compute) fun workgroup_mark_written(word: uint, bits: uint)",
        .doc = "/// Sets `bits` of word `word` of the shadow, in one step.",
        .evaluate = nothing,
        .write = {.kind = spelling_kind::custom, .custom = never_written},
    });

    r.add_comment("// What a weak compare-exchange gives, a struct of fields every target declares as a program's "
                  "own.\n"
                  "// The weak one may fail although the atomic held `expected`, so `old` alone cannot say whether it "
                  "stored (EMIT-161).");
    r.add_comment("// What sg binds itself: an entry of `check::k_reserved_bindings`, which legalize alone reaches.");
    for (auto const& b : check::k_reserved_bindings)
        if (!b.declaration.empty())
            r.add_declaration(b.declaration);
    r.add_declaration("/// What a weak compare-exchange read, and whether it stored `desired`.\n"
                      "struct exchange_result[T]:\n"
                      "    old: T\n"
                      "    exchanged: bool");

    r.add_comment("// Atomics, called as methods of the atomic they update: `stats.hits[0].add(1)`.\n"
                  "// Each is relaxed, and each but a store gives the value before; only the stages its `@stages` "
                  "names take one (CHK-296).");
    struct atomic_type
    {
        cc::string_view name;
        value_kind kind;
    };
    // CHK-296: a 64-bit atomic needs shader_int64_atomics, but a ulong's min and max, which need less (CHK-511)
    for (auto const [t, kind] : {atomic_type{"uint", value_kind::scalar_uint},
                                 {"int", value_kind::scalar_int},
                                 {"ulong", value_kind::scalar_ulong},
                                 {"long", value_kind::scalar_long}})
    {
        auto const is_signed = kind == value_kind::scalar_int || kind == value_kind::scalar_long;
        auto const is_wide = check::is_64_bit(kind);
        auto const evaluate = kind == value_kind::scalar_uint  ? evaluator_of<value_kind::scalar_uint>
                            : kind == value_kind::scalar_int   ? evaluator_of<value_kind::scalar_int>
                            : kind == value_kind::scalar_ulong ? evaluator_of<value_kind::scalar_ulong>
                                                               : evaluator_of<value_kind::scalar_long>;
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
                .evaluate = evaluate(info.op),
                .write = {.kind = spelling_kind::custom,
                          .custom = write_atomic,
                          // MSL's 64-bit min and max stand as statements (EMIT-218)
                          .writes_lines = is_compare_exchange(info.op)
                                               || (is_wide && (info.op == atomic_op::min || info.op == atomic_op::max))
                                            ? every_language
                                            : is_hlsl,
                          .data = atomic_data(info.op, is_signed, is_wide),
                          .hlsl_names = k_atomics_hlsl,
                          .wgsl_names = k_atomics_wgsl,
                          .msl_names = k_atomics_msl},
                .is_atomic = true,
                .gives_prelude_struct = info.op == atomic_op::compare_exchange_weak,
                .features = !is_wide ? check::feature_set()
                          : kind == value_kind::scalar_ulong && (info.op == atomic_op::min || info.op == atomic_op::max)
                              ? check::feature_set(check::feature::shader_int64_min_max_atomics)
                              : check::feature_set(check::feature::shader_int64_atomics),
                .pixel_features = strong_pixel_features(info.op),
            });
        }
    }

    r.add_comment("// A float's atomics, which every target holds as the bits of a `uint` atomic.\n"
                  "// Its add retries a compare-exchange of those bits, so a stage with helper invocations takes "
                  "none.");
    struct float_atomic
    {
        atomic_op op;
        cc::string_view signature;
        cc::string_view doc;
        evaluator evaluate;
    };
    float_atomic const float_atomics[] = {
        {atomic_op::load, "fun load(a: atomic[float]) -> float", "/// The value, read in one step.",
         float_atomic_update<atomic_op::load>},
        {atomic_op::store, "fun store(a: out atomic[float], v: float)", "/// Stores `v` in one step.",
         float_atomic_update<atomic_op::store>},
        {atomic_op::exchange, "fun exchange(a: mut atomic[float], v: float) -> float",
         "/// Stores `v`, and gives the value before.", float_atomic_update<atomic_op::exchange>},
        {atomic_op::add, "fun add(a: mut atomic[float], v: float) -> float",
         "/// Adds `v`, rounded once, and gives the value before.\n"
         "/// Where the device has no float add it retries a compare-exchange until no other update came between, "
         "which a\n"
         "/// pixel stage's helper invocations would never leave.",
         float_atomic_update<atomic_op::add>},
    };
    for (auto const& f : float_atomics)
        r.add(function_record{
            .signature = cc::format("{} {}", f.op == atomic_op::add ? k_float_add_stages : k_atomic_stages, f.signature),
            .doc = cc::string(f.doc),
            .evaluate = f.evaluate,
            .write = {.kind = spelling_kind::custom,
                      .custom = write_float_atomic,
                      .helper = float_atomic_helper,
                      .writes_lines = f.op == atomic_op::add ? every_language : is_hlsl,
                      .data = u32(f.op),
                      .hlsl_names = k_float_atomics_hlsl,
                      .wgsl_names = k_float_atomics_wgsl,
                      .msl_names = k_float_atomics_msl},
            .is_atomic = true,
        });

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
                    // the interpreter updates the texel as it does a buffer's element (EVAL-100)
                    .evaluate = is_signed ? evaluator_of<value_kind::scalar_int>(info.op)
                                          : evaluator_of<value_kind::scalar_uint>(info.op),
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
