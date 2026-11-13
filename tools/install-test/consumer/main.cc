#include <clean-core/common/macros.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

// The configuration reached this TU through build_config.hh alone: the consumer's CMakeLists defines nothing.
#if !defined(CC_DEBUG) && !defined(CC_RELEASE) && !defined(CC_RELWITHDEBINFO) && !defined(CC_MINSIZEREL)
#error "clean-core/build_config.hh did not name the configuration shaped-core was built in"
#endif

int main()
{
    auto const source = "binding work:\n"
                        "    values: mut buffer[float]\n"
                        "\n"
                        "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                        "    work.values[id.x] = 1.0\n";
    auto const emitted = sgl::compile_to_text({.source = source, .entry_point = "cs", .target = sgl::emit::target::hlsl_dx12});
    if (!emitted.has_value())
    {
        cc::println("compile_to_text failed: {}", emitted.error());
        return 1;
    }
    if (!emitted.value().text.contains("[numthreads(64, 1, 1)]"))
    {
        cc::println("compile_to_text wrote no 64-wide compute entry point:\n{}", emitted.value().text);
        return 1;
    }
    cc::println("shaped-core install test: SGL compiled a compute entry point to HLSL ({} bytes)", emitted.value().text.size());
    return 0;
}
