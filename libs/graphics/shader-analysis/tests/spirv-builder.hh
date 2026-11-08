#pragma once

#include <babel-data/shader/spirv.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>

#include <initializer_list>

/// SPIR-V modules assembled by hand, since a metric's contract is about shapes a compiler need not write.
namespace shan_test::impl
{
using namespace cc::primitive_defines;

struct spirv_builder
{
    cc::vector<u32> words = {0x07230203u, 0x00010600u, 0, 1000, 0};

    void op(u16 opcode, std::initializer_list<u32> operands)
    {
        words.push_back((u32(operands.size() + 1) << 16) | opcode);
        for (auto const o : operands)
            words.push_back(o);
    }
    /// `OpName %id "name"`, the name padded to whole words.
    void name(u32 id, cc::string_view text)
    {
        auto const count = isize(text.size()) / 4 + 1;
        words.push_back((u32(count + 2) << 16) | 5u);
        words.push_back(id);
        for (auto w = isize(0); w < count; ++w)
        {
            auto word = u32(0);
            for (auto b = isize(0); b < 4; ++b)
                if (w * 4 + b < text.size())
                    word |= u32(u8(text[w * 4 + b])) << (8 * b);
            words.push_back(word);
        }
    }
    [[nodiscard]] babel::spirv::module read() const { return babel::spirv::read(cc::span<u32 const>(words)).value(); }
};

// ids the tests share: types first, then constants, then the function's own
inline constexpr u32 t_void = 1;
inline constexpr u32 t_float = 2;
inline constexpr u32 t_float4 = 3;
inline constexpr u32 t_fn = 4;
inline constexpr u32 t_ptr = 5;
inline constexpr u32 t_bool = 6;
inline constexpr u32 c_one = 7;
inline constexpr u32 g_buffer = 8;

/// The types, a constant 1.0, and a global float a function reads and writes.
inline void declare_basics(spirv_builder& b)
{
    b.op(19, {t_void});
    b.op(22, {t_float, 32});
    b.op(23, {t_float4, t_float, 4});
    b.op(33, {t_fn, t_void});
    b.op(32, {t_ptr, 12, t_float}); // StorageBuffer
    b.op(20, {t_bool});
    b.op(43, {t_float, c_one, 0x3f800000u});
    b.op(59, {t_ptr, g_buffer, 12});
}
} // namespace shan_test::impl
