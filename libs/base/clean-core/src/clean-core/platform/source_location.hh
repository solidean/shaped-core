#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/fwd.hh>

#if CC_SOURCE_SITES == 2
#include <source_location>
#endif

// Where in the source something happened, kept at the level SC_SOURCE_SITES picks (macros.hh, CC_SOURCE_SITES).
//
// FULL is std::source_location: file, line and function are strings in the binary.
// OPAQUE keeps the code address of the site and nothing else, so a shipped binary names none of its source.
// The build's debug info resolves that address offline, inlined sites included: `llvm-symbolizer --inlining`,
// skipping the frames of this header, names the site's file, line and function.
// OFF keeps nothing.
//
// Below FULL, file_name() and function_name() are "" and line() and column() 0, so code reading them still compiles;
// describe_site is what prints a site at every level.

#if CC_SOURCE_SITES == 2

namespace cc
{
/// std::source_location under the cc name, so clean-core code spells one namespace.
using source_location = std::source_location;

/// The code address a site was captured at: nullptr, since FULL keeps the strings instead.
[[nodiscard]] constexpr void const* site_address(source_location const&) noexcept
{
    return nullptr;
}
} // namespace cc

#else

namespace cc
{
struct source_location;
} // namespace cc

namespace cc::impl
{
#if CC_SOURCE_SITES == 1 && (defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)) && defined(CC_ARCH_X64)
/// The address of this instruction, inlined into the site: one `lea`, as std::source_location's capture is.
CC_FORCE_INLINE void const* current_code_address() noexcept
{
    void const* p;
    asm volatile("leaq 0(%%rip), %0" : "=r"(p));
    return p;
}
#elif CC_SOURCE_SITES == 1 && (defined(CC_COMPILER_CLANG) || defined(CC_COMPILER_GCC)) && defined(CC_ARCH_ARM64)
CC_FORCE_INLINE void const* current_code_address() noexcept
{
    void const* p;
    asm volatile("adr %0, ." : "=r"(p));
    return p;
}
#elif CC_SOURCE_SITES == 1 && defined(CC_COMPILER_MSVC)
/// cl.exe has no inline assembly on x64 or arm64, so a call: its return address lies in the site, one byte before is
/// the call itself.
extern "C" void* _ReturnAddress();
#pragma intrinsic(_ReturnAddress)
__declspec(noinline) inline void const* current_code_address() noexcept
{
    return static_cast<char const*>(_ReturnAddress()) - 1;
}
#else
/// OFF, or a target with no address to take (wasm): the site is unnamed.
CC_FORCE_INLINE void const* current_code_address() noexcept
{
    return nullptr;
}
#endif
} // namespace cc::impl

/// A site as OPAQUE or OFF keeps it: its code address, or nothing.
/// The same interface as std::source_location, with every string empty.
struct cc::source_location
{
    /// The site this is evaluated at, when it is the default argument of a function, as std::source_location's is.
    /// Constant-evaluated it names no site, since a constant has no code address.
    [[nodiscard]] CC_FORCE_INLINE static constexpr source_location current() noexcept
    {
        if consteval
        {
            return {};
        }
        else
        {
            source_location s;
            s._address = impl::current_code_address();
            return s;
        }
    }

    constexpr source_location() noexcept = default;

    [[nodiscard]] constexpr cc::u32 line() const noexcept { return 0; }
    [[nodiscard]] constexpr cc::u32 column() const noexcept { return 0; }
    [[nodiscard]] constexpr char const* file_name() const noexcept { return ""; }
    [[nodiscard]] constexpr char const* function_name() const noexcept { return ""; }

private:
    void const* _address = nullptr;

    friend constexpr void const* site_address(source_location const& s) noexcept;
};

namespace cc
{
/// The code address a site was captured at, which the build's debug info resolves; nullptr at OFF, or where there is
/// none to take.
[[nodiscard]] constexpr void const* site_address(source_location const& s) noexcept
{
    return s._address;
}
} // namespace cc

#endif

namespace cc
{
/// One line naming a site for a human, at every level:
/// `file:line (function)` at FULL, `module+0xoffset` at OPAQUE, which the module's debug info resolves, and "" at OFF.
[[nodiscard]] cc::string describe_site(source_location const& s);
} // namespace cc
