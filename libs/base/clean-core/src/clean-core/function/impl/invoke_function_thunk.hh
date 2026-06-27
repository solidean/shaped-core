#pragma once

#include <clean-core/common/utility.hh>

namespace cc::impl
{
/// Type-erased call trampoline shared by cc::function_ref and cc::unique_function: casts the erased
/// payload back to Fn and invokes it.
///
/// Deliberately a named function template rather than a captureless lambda converted to a function
/// pointer. The two forms are semantically identical, but on CI's MSVC (cl 19.51 /O2) the reference-return
/// tests for function_ref/unique_function fail — the returned reference comes back with a garbage address —
/// while clang/gcc and older cl pass. We suspect the lambda-to-function-pointer thunk is miscompiled for
/// reference return types and that this named form avoids it; keep it unless that's disproven.
template <class Fn, class R, class... Args>
R invoke_function_thunk(void* p, Args... args)
{
    return cc::invoke(*static_cast<Fn*>(p), cc::forward<Args>(args)...);
}
} // namespace cc::impl
