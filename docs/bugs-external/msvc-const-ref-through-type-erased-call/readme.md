# MSVC 19.51 /O2 returns a dangling reference from a type-erased call

**Status:** open upstream, worked around in shaped-core.
**Filed:** <https://developercommunity.visualstudio.com/t/MSVC-O2-miscompiles-returned-const-loca/11114080>
**Affects:** `cl` 19.51 (toolset 14.51) at `/O2`. Not 19.29, not `/Od`, not clang-cl, not clang, not gcc.
**Found by:** `libs/base/clean-core/tests/function/function_ref-test.cc`, whose `cc::function_ref<int const&()>` case returned garbage.

## What happens

A callable returning `int const&` to an odr-used `const` local is invoked through a type-erasing wrapper — a `void const*` plus a thunk, which is what `cc::function_ref` is.
Under `/O2` the reference that comes back does not point at the local.
Reading through it yields a garbage value from freed stack rather than the local's.

```
cl 14.51  /O2   MISCOMPILED   expected 42, observed 1085542878
cl 14.51  /Od   ok            expected 42, observed 42
cl 14.29  /O2   ok            expected 42, observed 42
clang-cl  -O2   ok            expected 42, observed 42
clang++   -O2   ok            expected 42, observed 42
```

The value differs run to run, which is the tell that it is an address rather than a wrong constant.

## Why it is the compiler and not us

The lifetime of every object involved outlives the read.
`value` is a local of `main` and `main` has not returned; the lambda holds a reference to it, the wrapper holds a pointer to the lambda, and the read happens before any of them go out of scope.
There is no dangling reference in the source for the optimizer to be exploiting.

The same source is correct at `/Od` on the same compiler, and correct at `/O2` on the previous MSVC toolset and on two other compilers.
A source-level defect would not be sensitive to any of those.

The `__declspec(noinline)` on `read_through` is load-bearing: without it the optimizer sees the whole chain, constant-folds it, and produces 42.
The bug needs the thunk to be opaque at the call site, which is exactly the condition a real `function_ref` call site is in.

## Reproducing

```bash
uv run run.py            # every toolset installed here, at /O2 and /Od, plus clang as a control
uv run run.py --keep     # keep the binaries to disassemble
```

Exit code 1 means it reproduced, 0 means it did not.
The script finds Visual Studio through `vswhere` and needs nothing else; there is no shaped-core dependency and no `dev.py`.

By hand, if you prefer:

```bat
cl /O2 /std:c++20 /EHsc repro.cc && repro.exe
```

## The workaround in shaped-core

`function_ref-test.cc` quarantines the runtime value-check behind `_MSC_VER >= 1951`, keeping the `static_assert` on the return type running everywhere.
It carries a `TODO(msvc-19.51)` to drop the guard when the fix ships.

We do not otherwise avoid the pattern: `cc::function_ref` returning a reference is a legitimate shape, and the repo builds with clang-cl by default, where it is correct.
The exposure is a consumer building shaped-core with `cl` 19.51 at `/O2`, which nothing in CI does today.

## What to check when a new MSVC lands

Run `run.py`.
If the newest toolset reports `ok` at `/O2`, drop the `#if` guard in `function_ref-test.cc` and this directory with it.
