#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>

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
        return {.text = cc::format("threadgroup_barrier(mem_flags::{})", b.msl_memory)};
    }
    return {};
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
};

/// The data word: the operation, and whether the atomic holds an `int`.
u32 atomic_data(atomic_op op, bool is_signed)
{
    return u32(op) | (is_signed ? 0x100u : 0u);
}

written write_atomic(call_context const& ctx)
{
    auto const& info = k_atomics[ctx.data & 0xffu];
    auto const is_signed = (ctx.data & 0x100u) != 0;
    auto const& a = ctx.arguments[0].text;
    auto const v = ctx.arguments.size() > 1 ? wrapped(ctx.arguments[1], precedence::primary) : cc::string();
    switch (ctx.target)
    {
    case language::hlsl:
    {
        // `Interlocked*` gives the value before through an out parameter, so a local declared ahead holds it
        auto const before = ctx.mint.is_valid() ? ctx.mint("atomic_before") : cc::string("atomic_before");
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
    }
    return old;
}

/// The atomic's value first, then the operand; gives what the atomic holds after (EVAL-93).
template <atomic_op Op, bool IsSigned>
void atomic_update(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    out.push_back(updated<IsSigned>(Op, in[0], in.size() > 1 ? in[1] : in[0]));
}

template <bool IsSigned>
evaluator evaluator_of(atomic_op op)
{
    switch (op)
    {
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
} // namespace

void sgl::builtins::register_sync(registry& r)
{
    r.add_comment("// Barriers, which a compute shader reaches in control flow every thread of its workgroup takes "
                  "(CHK-282).");
    for (auto i = u32(0); i < u32(sizeof(k_barriers) / sizeof(k_barriers[0])); ++i)
        r.add(function_record{
            .signature = cc::format("@stages(.compute) fun {}()", k_barriers[i].name),
            .doc = k_barriers[i].doc,
            .evaluate = nothing,
            .write = {.kind = spelling_kind::custom, .custom = write_barrier, .data = i},
            .is_barrier = true,
        });

    r.add_comment("// Atomics, called as methods of the atomic they update: `stats.hits[0].add(1)`.\n"
                  "// Each is relaxed, and each but a store gives the value before; a vertex stage has none "
                  "(CHK-296).");
    for (auto const is_signed : {false, true})
    {
        auto const t = is_signed ? "int" : "uint";
        for (auto const& info : k_atomics)
        {
            auto const signature = info.op == atomic_op::load ? cc::format("fun load(a: atomic[{0}]) -> {0}", t)
                                 : info.op == atomic_op::store
                                     ? cc::format("fun store(a: out atomic[{0}], v: {0})", t)
                                     : cc::format("fun {1}(a: mut atomic[{0}], v: {0}) -> {0}", t, info.name);
            r.add(function_record{
                .signature = cc::format("@stages(.pixel, .compute) {}", signature),
                .doc = info.doc,
                .evaluate = is_signed ? evaluator_of<true>(info.op) : evaluator_of<false>(info.op),
                .write = {.kind = spelling_kind::custom, .custom = write_atomic, .data = atomic_data(info.op, is_signed)},
                .is_atomic = true,
            });
        }
    }
}
