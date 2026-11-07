# MSVC crashes on a co_await inside an aggregate's initializer

**Status:** not filed, since the 19.52 preview compiles every case, and worked around in shaped-core.
**Affects:** `cl` 19.29, 19.44 and 19.51 (toolsets 14.29, 14.44, 14.51), x64, and 14.44 targeting arm64, at every optimization level.
The 19.52 preview compiles it, and clang does too.
**Found by:** `libs/graphics/shaped-graphics/tests/raster/mixed-route-test.cc`, which stopped all three MSVC CI jobs.

## What happens

An aggregate or array is being initialized, an element with a destructor is already built, and a later element's initializer is a `co_await`:

```cpp
pair p = {seed(), co_await awaitable{}};
```

`cl` dies with `fatal error C1001: Internal compiler error`, at `Utc\src\p2\main.cpp` line 258, or 262 on 14.51.
It names the coroutine's opening brace or the statement, depending on the code around it, and it says nothing about why.
The code is valid C++.

```
case          flags       14.29       14.44       14.51       14.52       isolates
CRASH         /EHsc       CRASH       CRASH       CRASH       ok          a built member with a destructor, across a co_await
CRASH         /EHsc /Od   CRASH       CRASH       CRASH       ok          the same, with the optimizer off
CRASH                     ok          ok          ok          ok          the same, without exception handling
ARRAY         /EHsc       CRASH       CRASH       CRASH       ok          the same in an array
AWAIT_FIRST   /EHsc       ok          ok          ok          ok          nothing built yet when it suspends
NAMED         /EHsc       ok          ok          ok          ok          the workaround: await into a local first
INT_FIRST     /EHsc       ok          ok          ok          ok          what is built has no destructor
CONSTRUCTOR   /EHsc       ok          ok          ok          ok          a constructor's arguments, not an aggregate
CALL          /EHsc       ok          ok          ok          ok          a function's arguments
```

## Why it is the compiler and not us

An internal compiler error is the compiler's bug whatever the source says.
`repro.cc` includes only `<coroutine>`, and clang compiles it.

## What triggers it

Each of these is necessary, and `repro.cc` keeps every one:

- **A built element with a destructor.** `AWAIT_FIRST` has nothing built yet, and `INT_FIRST` has nothing to destroy.
- **An object under construction.** `CONSTRUCTOR` and `CALL` hold the same values as temporaries, with no object begun, and both compile.
- **Exception handling.** Without `/EHsc` it compiles.

Those three point at one state: a partly built object whose built elements need cleanup if the coroutine does not resume, carried across a suspension.
That reading is inferred from the matrix, since the compiler's source is not available.
`/Od` crashes too, so this is not an optimization, and `#pragma optimize` cannot help.

## Reproducing it

```
uv run run.py                   # every toolset it finds, targeting x64
uv run run.py --arch x64_arm64  # the same, targeting arm64
```

It only compiles, because the crash comes before any link, and it exits 1 when any toolset crashes.
The `clang++` line is the control.

## What we do about it

Nothing in shaped-core may `co_await` inside the initializer of an aggregate or array with a destructor.
Await into a named local first, as `NAMED` does.
`mixed-route-test.cc` does that and names this directory, at each place it needs to.
The `co-await-in-initializer` rule of [shaped-linter](../../../tools/shaped-linter/readme.md) holds the line, `#define` bodies included, since this came back three times in new code.

`raster-test.cc` crashed the same way on CI's arm64-hosted 14.44, at a `create_texture_2d` call with a designated-initializer argument and no `co_await` inside it.
Moving that call out of the coroutine stopped it.
This repro does not explain that crash, so it may be another bug.

## What to re-check on an update

Run `run.py` when CI's toolsets move.
19.52 was a preview when this was written, and "compiles" is all that was measured: nobody has run the code it generates.
Retire the lint rule and the test's comments once every MSVC we build with reports `ok`.
