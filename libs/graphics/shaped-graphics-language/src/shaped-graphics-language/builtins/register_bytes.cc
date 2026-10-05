#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>

using namespace sgl;
using namespace sgl::builtins;

// Raw memory: `bytes` loaded and stored a 32-bit word at a time, at a byte offset that must be a multiple of 4.
// HLSL's ByteAddressBuffer is this exactly; WGSL and MSL see an array of `u32` / `uint`, indexed by the offset over 4.

namespace
{
using check::scalar;

constexpr cc::string_view k_bytes_hlsl[] = {"Load", "Load2", "Load3", "Load4", "Store", "Store2", "Store3", "Store4"};

/// The data word: how many words, and whether the call stores.
[[nodiscard]] u32 bytes_data(int words, bool is_store)
{
    return u32(words) | (is_store ? 0x100u : 0u);
}

[[nodiscard]] bool is_not_hlsl(language l)
{
    return l != language::hlsl;
}

constexpr char k_lanes[] = {'x', 'y', 'z', 'w'};

/// A local the text binds `value` to, so an expression the call reads more than once is evaluated once.
[[nodiscard]] cc::string bound(call_context const& ctx,
                               written& result,
                               cc::string_view stem,
                               cc::string_view type,
                               cc::string value)
{
    auto const name = ctx.mint.is_valid() ? ctx.mint(stem) : cc::string(stem);
    result.lines.push_back(ctx.target == language::wgsl ? cc::format("let {} = {};", name, value)
                                                        : cc::format("{} {} = {};", type, name, value));
    return name;
}

written write_bytes(call_context const& ctx)
{
    auto const words = int(ctx.data & 0xffu);
    auto const is_store = (ctx.data & 0x100u) != 0;
    auto const b = wrapped(ctx.arguments[0], precedence::primary);
    auto const offset = ctx.arguments[1];

    if (ctx.target == language::hlsl)
    {
        auto const suffix = words == 1 ? cc::string() : cc::format("{}", words);
        if (is_store)
            return {.text = cc::format("{}.Store{}({}, {})", b, suffix, offset.text, ctx.arguments[2].text)};
        return {.text = cc::format("{}.Load{}({})", b, suffix, offset.text)};
    }

    // WGSL and MSL index words: the offset over 4, which an index of one word writes in place
    auto const is_wgsl = ctx.target == language::wgsl;
    auto const index_of = cc::format("{} / 4u", wrapped(offset, precedence::multiplicative));
    if (words == 1)
    {
        if (is_store)
            return {.text = cc::format("{}[{}] = {}", b, index_of, ctx.arguments[2].text)};
        return {.text = cc::format("{}[{}]", b, index_of)};
    }

    auto result = written{};
    auto const index = bound(ctx, result, "word", "uint", index_of);
    auto const at
        = [&](int k) { return k == 0 ? cc::format("{}[{}]", b, index) : cc::format("{}[{} + {}u]", b, index, k); };
    if (is_store)
    {
        auto const value = bound(ctx, result, "stored", cc::format("uint{}", words), ctx.arguments[2].text);
        for (auto k = 0; k + 1 < words; ++k)
            result.lines.push_back(cc::format("{} = {}.{};", at(k), value, k_lanes[k]));
        result.text = cc::format("{} = {}.{}", at(words - 1), value, k_lanes[words - 1]);
        return result;
    }
    auto loaded = is_wgsl ? cc::format("vec{}<u32>(", words) : cc::format("uint{}(", words);
    for (auto k = 0; k < words; ++k)
        loaded += cc::format("{}{}", k == 0 ? "" : ", ", at(k));
    result.text = loaded + ")";
    return result;
}

/// The interpreter reads and writes the memory itself (EVAL-97); these stand for a call it never evaluates.
void no_value(cc::span<scalar const>, cc::vector<scalar>&)
{
}
} // namespace

void sgl::builtins::register_bytes(registry& r)
{
    r.add_comment("// raw memory: 32-bit words of `bytes` at a byte offset, which must be a multiple of 4");
    for (auto words = 1; words <= 4; ++words)
    {
        auto const type = words == 1 ? cc::string("uint") : cc::format("uint{}", words);
        auto const name = words == 1 ? cc::string("load") : cc::format("load{}", words);
        auto const spell = [&](bool is_store)
        {
            return spelling{.kind = spelling_kind::custom,
                            .custom = write_bytes,
                            .writes_lines = words > 1 ? is_not_hlsl : nullptr,
                            .data = bytes_data(words, is_store),
                            .hlsl_names = k_bytes_hlsl};
        };
        // Bytes another invocation may write are read where the call stands, so a load is not @pure.
        r.add(function_record{
            .signature = cc::format("fun {}(b: bytes, offset: uint) -> {}", name, type),
            .doc = words == 1 ? cc::string("/// The word at byte `offset`, a multiple of 4.")
                              : cc::format("/// The {} words from byte `offset`, a multiple of 4.", words),
            .evaluate = no_value,
            .write = spell(false),
            .bytes_words = words,
        });
        // As an image store: no writable storage in a vertex stage on WebGPU, and none where a ray stage may run any
        // number of times per ray.
        r.add(function_record{
            .signature = cc::format("@stages(.pixel, .compute, .raygen, .closest_hit, .miss, .callable) fun "
                                    "store(b: mut bytes, offset: uint, value: {})",
                                    type),
            .doc = words == 1
                     ? cc::string("/// Stores `value` as the word at byte `offset`, a multiple of 4.")
                     : cc::format("/// Stores `value` as the {} words from byte `offset`, a multiple of 4.", words),
            .evaluate = no_value,
            .write = spell(true),
            .bytes_words = words,
            .is_bytes_store = true,
        });
    }
}
