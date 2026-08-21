#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/binding/impl/shader_codec.hh>

namespace sg::impl
{
namespace
{
// The enum ranges this build knows.
// Anything outside them is a blob from a future build, or a corrupt one; either way it decodes to nothing.
constexpr u32 k_shader_stage_count = u32(shader_stage::callable) + 1;
constexpr u32 k_shader_format_count = u32(shader_format::metal_lib) + 1;
constexpr u32 k_binding_type_count = u32(binding_type::acceleration_structure) + 1;

void put_u32(cc::vector<byte>& out, u32 value)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(byte((value >> (8 * i)) & 0xFF));
}

void put_u64(cc::vector<byte>& out, u64 value)
{
    for (auto i = 0; i < 8; ++i)
        out.push_back(byte((value >> (8 * i)) & 0xFF));
}

void put_i64(cc::vector<byte>& out, i64 value)
{
    put_u64(out, u64(value));
}

void put_bool(cc::vector<byte>& out, bool value)
{
    out.push_back(byte(value ? 1 : 0));
}

void put_bytes(cc::vector<byte>& out, cc::span<byte const> bytes)
{
    put_u64(out, u64(bytes.size()));
    for (auto b : bytes)
        out.push_back(b);
}

void put_string(cc::vector<byte>& out, cc::string_view text)
{
    put_u64(out, u64(text.size()));
    for (auto c : text)
        out.push_back(byte(c));
}

void put_optional_u32(cc::vector<byte>& out, cc::optional<u32> const& value)
{
    put_bool(out, value.has_value());
    put_u32(out, value.value_or(0));
}

void put_binding(cc::vector<byte>& out, binding const& b)
{
    put_string(out, b.name);
    put_optional_u32(out, b.group_index);
    put_optional_u32(out, b.space);
    put_u32(out, b.index);
    put_u32(out, b.count);
    put_u32(out, u32(b.type));
    put_bool(out, b.block_size.has_value());
    put_i64(out, b.block_size.has_value() ? i64(b.block_size.value()) : 0);
}

/// A cursor that goes sour on the first bad read and stays that way.
/// Every getter must therefore be safe to call after a failure — it returns a zero rather than reading past the end —
/// so the decode reads straight through and is checked once at the end.
struct reader
{
    cc::span<byte const> data;
    isize pos = 0;
    bool ok = true;

    [[nodiscard]] bool take(isize count)
    {
        if (!ok || count < 0 || pos + count > data.size())
        {
            ok = false;
            return false;
        }
        return true;
    }

    [[nodiscard]] u32 get_u32()
    {
        if (!take(4))
            return 0;
        auto value = u32(0);
        for (auto i = 0; i < 4; ++i)
            value |= u32(data[pos + i]) << (8 * i);
        pos += 4;
        return value;
    }

    [[nodiscard]] u64 get_u64()
    {
        if (!take(8))
            return 0;
        auto value = u64(0);
        for (auto i = 0; i < 8; ++i)
            value |= u64(data[pos + i]) << (8 * i);
        pos += 8;
        return value;
    }

    [[nodiscard]] i64 get_i64() { return i64(get_u64()); }

    [[nodiscard]] bool get_bool()
    {
        if (!take(1))
            return false;
        auto const value = data[pos] != byte(0);
        pos += 1;
        return value;
    }

    /// A length-prefixed run, as a view INTO the input — the caller copies what it wants to keep.
    [[nodiscard]] cc::span<byte const> get_bytes()
    {
        auto const size = isize(get_u64());
        if (!take(size))
            return {};
        auto const view = data.subspan({.offset = pos, .size = size});
        pos += size;
        return view;
    }

    [[nodiscard]] cc::string get_string()
    {
        auto const view = get_bytes();
        if (!ok)
            return {};
        return cc::string(cc::string_view(reinterpret_cast<char const*>(view.data()), view.size()));
    }

    /// A count that must not be trusted before it is spent.
    /// A corrupt blob can claim billions of entries, and reserving on that alone would be an allocation the input chose.
    [[nodiscard]] isize get_count(isize bytes_per_entry)
    {
        auto const count = isize(get_u64());
        if (count < 0 || (bytes_per_entry > 0 && count > (data.size() - pos) / bytes_per_entry))
        {
            ok = false;
            return 0;
        }
        return count;
    }

    [[nodiscard]] cc::optional<u32> get_optional_u32()
    {
        auto const present = get_bool();
        auto const value = get_u32();
        return present ? cc::optional<u32>(value) : cc::optional<u32>();
    }

    [[nodiscard]] binding get_binding()
    {
        auto b = binding();
        b.name = get_string();
        b.group_index = get_optional_u32();
        b.space = get_optional_u32();
        b.index = get_u32();
        b.count = get_u32();

        auto const type = get_u32();
        if (type >= k_binding_type_count)
            ok = false;
        b.type = ok ? binding_type(type) : binding_type::uniform_buffer;

        auto const has_block_size = get_bool();
        auto const block_size = get_i64();
        if (has_block_size)
            b.block_size = isize(block_size);
        return b;
    }
};
} // namespace

cc::vector<byte> encode_compiled_shader(compiled_shader const& shader)
{
    auto out = cc::vector<byte>();

    put_u32(out, k_shader_codec_version);
    put_u32(out, u32(shader.stage));
    put_u32(out, u32(shader.format));
    put_string(out, shader.entry_point);
    put_bytes(out, shader.bytecode.span());

    put_u64(out, u64(shader.bindings.size()));
    for (auto const& b : shader.bindings)
        put_binding(out, b);

    put_bool(out, shader.workgroup_size.has_value());
    auto const workgroup = shader.workgroup_size.has_value() ? shader.workgroup_size.value() : compute_dimensions{};
    put_u32(out, u32(workgroup.x));
    put_u32(out, u32(workgroup.y));
    put_u32(out, u32(workgroup.z));

    put_string(out, shader.compiler.name);
    put_string(out, shader.compiler.version);
    put_string(out, shader.compiler.signature);

    return out;
}

cc::optional<compiled_shader> decode_compiled_shader(cc::span<byte const> bytes)
{
    auto r = reader{.data = bytes};

    if (r.get_u32() != k_shader_codec_version)
        return {};

    auto shader = compiled_shader();

    auto const stage = r.get_u32();
    auto const format = r.get_u32();
    if (stage >= k_shader_stage_count || format >= k_shader_format_count)
        return {};
    shader.stage = shader_stage(stage);
    shader.format = shader_format(format);

    shader.entry_point = r.get_string();

    // Copied out: the decoded shader outlives the buffer it was read from.
    auto const bytecode = r.get_bytes();
    if (r.ok && !bytecode.empty())
    {
        auto owned = cc::pinned_data<byte>::create_uninitialized(bytecode.size());
        cc::memcpy(owned.data(), bytecode.data(), size_t(bytecode.size()));
        shader.bytecode = owned;
    }

    // A binding is at least its five fixed fields plus two length prefixes, so a claimed count far past what the
    // remaining bytes could hold is rejected before anything is allocated for it.
    auto const binding_count = r.get_count(29);
    for (auto i = isize(0); r.ok && i < binding_count; ++i)
        shader.bindings.push_back(r.get_binding());

    auto const has_workgroup = r.get_bool();
    auto const x = i32(r.get_u32());
    auto const y = i32(r.get_u32());
    auto const z = i32(r.get_u32());
    if (has_workgroup)
        shader.workgroup_size = compute_dimensions{.x = x, .y = y, .z = z};

    shader.compiler.name = r.get_string();
    shader.compiler.version = r.get_string();
    shader.compiler.signature = r.get_string();

    // Trailing bytes mean this is not the blob we think it is, so it is refused like any other inconsistency.
    if (!r.ok || r.pos != bytes.size())
        return {};

    return shader;
}
} // namespace sg::impl
