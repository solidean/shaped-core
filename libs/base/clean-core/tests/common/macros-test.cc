#include <clean-core/common/macros.hh>
#include <nexus/test.hh>

#include <cstring>


// =========================================================================================================
// Preprocessor-level compile-time checks
// =========================================================================================================

// Test: Exactly-one macro is selected per detection axis (compiler / arch / OS / ABI / platform).
#if defined(CC_COMPILER_MSVC) + defined(CC_COMPILER_CLANG) + defined(CC_COMPILER_GCC) != 1
#error "Expected exactly one compiler macro to be defined"
#endif

#if defined(CC_ARCH_X64) + defined(CC_ARCH_X86) + defined(CC_ARCH_ARM64) + defined(CC_ARCH_ARM32) \
        + defined(CC_ARCH_WASM32)                                                                 \
    != 1
#error "Expected exactly one architecture macro to be defined"
#endif

#if defined(CC_OS_WINDOWS) + defined(CC_OS_LINUX) + defined(CC_OS_MACOS) + defined(CC_OS_IOS) + defined(CC_OS_TVOS) \
        + defined(CC_OS_ANDROID) + defined(CC_OS_EMSCRIPTEN) + defined(CC_OS_WASI)                                  \
    != 1
#error "Expected exactly one OS macro to be defined"
#endif

#if defined(CC_ABI_MSVC) + defined(CC_ABI_SYSV) + defined(CC_ABI_ANDROID) + defined(CC_ABI_DARWIN) \
        + defined(CC_ABI_WASM)                                                                     \
    != 1
#error "Expected exactly one ABI macro to be defined"
#endif

#if defined(CC_PLATFORM_DESKTOP) + defined(CC_PLATFORM_MOBILE) + defined(CC_PLATFORM_WEB) + defined(CC_PLATFORM_CONSOLE) \
    != 1
#error "Expected exactly one platform macro to be defined"
#endif

// Test: Exactly-one build configuration is selected
#if defined(CC_DEBUG) + defined(CC_RELEASE) + defined(CC_RELWITHDEBINFO) != 1
#error "Expected exactly one build configuration macro to be defined"
#endif

// Test: ABI follows from the OS (the axes are derived, so they must agree).
#if defined(CC_ABI_MSVC) != defined(CC_OS_WINDOWS)
#error "CC_ABI_MSVC must match CC_OS_WINDOWS"
#endif
#if defined(CC_ABI_DARWIN) != (defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS))
#error "CC_ABI_DARWIN must match an Apple OS"
#endif
#if defined(CC_ABI_ANDROID) != defined(CC_OS_ANDROID)
#error "CC_ABI_ANDROID must match CC_OS_ANDROID"
#endif
#if defined(CC_ABI_WASM) != (defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_WASI))
#error "CC_ABI_WASM must match a wasm OS"
#endif

// Test: CC_PLATFORM_WEB / MOBILE follow from the OS.
#if defined(CC_PLATFORM_WEB) != (defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_WASI))
#error "CC_PLATFORM_WEB must match a wasm OS"
#endif
#if defined(CC_PLATFORM_MOBILE) && !defined(CC_OS_IOS) && !defined(CC_OS_TVOS) && !defined(CC_OS_ANDROID)
#error "CC_PLATFORM_MOBILE defined but no mobile OS detected"
#endif
#if (defined(CC_OS_IOS) || defined(CC_OS_TVOS) || defined(CC_OS_ANDROID)) && !defined(CC_PLATFORM_MOBILE)
#error "Mobile OS detected but CC_PLATFORM_MOBILE not defined"
#endif

// Test: a wasm OS implies the wasm32 architecture.
#if (defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_WASI)) && !defined(CC_ARCH_WASM32)
#error "wasm OS detected but CC_ARCH_WASM32 not defined"
#endif

// =========================================================================================================
// Runtime tests
// =========================================================================================================

TEST("macros - compiler detection")
{
    // Verify that exactly one compiler is selected (already checked at compile-time)
    int compiler_count = 0;
#ifdef CC_COMPILER_MSVC
    compiler_count++;
#endif
#ifdef CC_COMPILER_CLANG
    compiler_count++;
#endif
#ifdef CC_COMPILER_GCC
    compiler_count++;
#endif

    CHECK(compiler_count == 1);
}

TEST("macros - architecture detection")
{
    // Verify that exactly one architecture is selected (already checked at compile-time)
    int arch_count = 0;
#ifdef CC_ARCH_X64
    arch_count++;
#endif
#ifdef CC_ARCH_X86
    arch_count++;
#endif
#ifdef CC_ARCH_ARM64
    arch_count++;
#endif
#ifdef CC_ARCH_ARM32
    arch_count++;
#endif
#ifdef CC_ARCH_WASM32
    arch_count++;
#endif

    CHECK(arch_count == 1);
}

TEST("macros - OS detection")
{
    // Verify that exactly one OS is selected (already checked at compile-time)
    int os_count = 0;
#ifdef CC_OS_WINDOWS
    os_count++;
#endif
#ifdef CC_OS_LINUX
    os_count++;
#endif
#ifdef CC_OS_MACOS
    os_count++;
#endif
#ifdef CC_OS_IOS
    os_count++;
#endif
#ifdef CC_OS_TVOS
    os_count++;
#endif
#ifdef CC_OS_ANDROID
    os_count++;
#endif
#ifdef CC_OS_EMSCRIPTEN
    os_count++;
#endif
#ifdef CC_OS_WASI
    os_count++;
#endif

    CHECK(os_count == 1);
}

TEST("macros - ABI and platform detection")
{
    int abi_count = 0;
#ifdef CC_ABI_MSVC
    abi_count++;
#endif
#ifdef CC_ABI_SYSV
    abi_count++;
#endif
#ifdef CC_ABI_ANDROID
    abi_count++;
#endif
#ifdef CC_ABI_DARWIN
    abi_count++;
#endif
#ifdef CC_ABI_WASM
    abi_count++;
#endif

    CHECK(abi_count == 1);

    int platform_count = 0;
#ifdef CC_PLATFORM_DESKTOP
    platform_count++;
#endif
#ifdef CC_PLATFORM_MOBILE
    platform_count++;
#endif
#ifdef CC_PLATFORM_WEB
    platform_count++;
#endif
#ifdef CC_PLATFORM_CONSOLE
    platform_count++;
#endif

    CHECK(platform_count == 1);
}

TEST("macros - build configuration detection")
{
    // Verify that exactly one build configuration is selected (already checked at compile-time)
    int config_count = 0;
#ifdef CC_DEBUG
    config_count++;
#endif
#ifdef CC_RELEASE
    config_count++;
#endif
#ifdef CC_RELWITHDEBINFO
    config_count++;
#endif

    CHECK(config_count == 1);
}

TEST("macros - CC_ARRAY_COUNT_OF")
{
    SECTION("Basic array counting")
    {
        int arr[10];
        CHECK(CC_ARRAY_COUNT_OF(arr) == 10);

        int arr2[1];
        CHECK(CC_ARRAY_COUNT_OF(arr2) == 1);

        int arr3[100];
        CHECK(CC_ARRAY_COUNT_OF(arr3) == 100);
    }

    SECTION("Non-int element types")
    {
        double doubles[5];
        CHECK(CC_ARRAY_COUNT_OF(doubles) == 5);

        char chars[20];
        CHECK(CC_ARRAY_COUNT_OF(chars) == 20);

        struct TestStruct
        {
            int a, b;
        };
        TestStruct structs[7];
        CHECK(CC_ARRAY_COUNT_OF(structs) == 7);
    }

    SECTION("Multidimensional arrays")
    {
        // Should return count of first dimension
        int arr2d[3][5];
        CHECK(CC_ARRAY_COUNT_OF(arr2d) == 3);

        int arr3d[4][2][8];
        CHECK(CC_ARRAY_COUNT_OF(arr3d) == 4);
    }
}

TEST("macros - CC_MACRO_JOIN")
{
    SECTION("Direct token joining")
    {
        int foo_bar = 42;
        int result = CC_MACRO_JOIN(foo_, bar);
        CHECK(result == 42);
    }

    SECTION("Join with macro expansion")
    {
#define TEST_PREFIX foo
        int foo_suffix = 123;
        int result = CC_MACRO_JOIN(TEST_PREFIX, _suffix);
        CHECK(result == 123);
#undef TEST_PREFIX
    }

    SECTION("Join with numeric suffix")
    {
#define VERSION 2
        int var2 = 999;
        int result = CC_MACRO_JOIN(var, VERSION);
        CHECK(result == 999);
#undef VERSION
    }
}

TEST("macros - CC_STRINGIFY_EXPR")
{
    SECTION("Stringify simple value")
    {
#define MY_VALUE 42
        char const* str = CC_STRINGIFY_EXPR(MY_VALUE);
        CHECK(std::strcmp(str, "42") == 0);
#undef MY_VALUE
    }

    SECTION("Stringify identifier")
    {
#define MY_IDENT foo
        char const* str = CC_STRINGIFY_EXPR(MY_IDENT);
        CHECK(std::strcmp(str, "foo") == 0);
#undef MY_IDENT
    }

    SECTION("Stringify expression")
    {
#define MY_EXPR (1 + 2)
        char const* str = CC_STRINGIFY_EXPR(MY_EXPR);
        CHECK(std::strcmp(str, "(1 + 2)") == 0);
#undef MY_EXPR
    }
}

TEST("macros - CC_UNUSED does not evaluate")
{
    SECTION("No side effects on increment")
    {
        int x = 0;
        CC_UNUSED(++x);
        CHECK(x == 0);
    }

    SECTION("No side effects on decrement")
    {
        int y = 10;
        CC_UNUSED(--y);
        CHECK(y == 10);
    }

    SECTION("No side effects on function call")
    {
        int counter = 0;
        auto increment = [&counter]() -> int { return ++counter; };
        CC_UNUSED(increment());
        CHECK(counter == 0);
    }

    SECTION("Works with complex expressions")
    {
        int a = 5;
        int b = 3;
        CC_UNUSED(a * b + (a << 2));
        CHECK(a == 5);
        CHECK(b == 3);
    }
}

// Helper functions for testing CC_CONDITION_LIKELY/UNLIKELY and CC_PRETTY_FUNC
namespace
{
char const* get_pretty_func_name()
{
    return CC_PRETTY_FUNC;
}

struct TestClass
{
    char const* get_method_func_name() { return CC_PRETTY_FUNC; }
};

int test_unreachable_switch(int value)
{
    switch (value)
    {
    case 1:
        return 10;
    case 2:
        return 20;
    default:
        CC_BUILTIN_UNREACHABLE;
    }
}

void test_assume_statement(int* ptr)
{
    CC_ASSUME(ptr != nullptr);
    *ptr = 42;
}
} // namespace

TEST("macros - statement syntax")
{
    SECTION("CC_ASSUME compiles as statement")
    {
        int value = 0;
        test_assume_statement(&value);
        CHECK(value == 42);
    }

    SECTION("CC_BUILTIN_UNREACHABLE compiles in switch")
    {
        // Just verify it compiles; we don't actually call with default case
        CHECK(test_unreachable_switch(1) == 10);
        CHECK(test_unreachable_switch(2) == 20);
    }
}

TEST("macros - CC_PRETTY_FUNC")
{
    SECTION("Contains function name")
    {
        char const* func_name = get_pretty_func_name();
        CHECK(std::strstr(func_name, "get_pretty_func_name") != nullptr);
    }

    SECTION("Works in method")
    {
        TestClass obj;
        char const* method_name = obj.get_method_func_name();
        CHECK(std::strstr(method_name, "get_method_func_name") != nullptr);
    }

    SECTION("Works in lambda")
    {
        auto lambda = []() -> char const* { return CC_PRETTY_FUNC; };
        char const* lambda_name = lambda();
        // Lambda name format varies by compiler, just check it's not empty
        CHECK(lambda_name != nullptr);
        CHECK(std::strlen(lambda_name) > 0);
    }
}
