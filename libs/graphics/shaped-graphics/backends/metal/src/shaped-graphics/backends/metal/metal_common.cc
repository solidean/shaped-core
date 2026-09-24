#include "metal_common.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/compiled_shader.hh> // library_from_shader reads a shader's format and bytes

#include <cstdlib> // setenv, the only way to configure Metal's validation layer
#include <mutex>

namespace sg::backend::metal
{
std::mutex& pipeline_compilation_lock()
{
    static std::mutex lock;
    return lock;
}

void arm_validation_layer()
{
    // `assert` rather than `abort`: Metal parses this value itself and asserts on one it does not know, so `abort`
    // takes the process down while parsing and never reaches a violation.
    // The accepted spellings are `ignore`, `nslog` and `assert`.
    setenv("MTL_DEBUG_LAYER", "1", 0);
    setenv("MTL_DEBUG_LAYER_ERROR_MODE", "assert", 0);
}

NS::String* ns_string(cc::string_view text)
{
    // NSString's UTF-8 initializer wants a NUL-terminated pointer, and a string_view carries no terminator of its own.
    // A cc::string materializes one; the bytes are copied into the NSString before it goes out of scope.
    auto owned = cc::string(text);
    return NS::String::string(owned.c_str_materialize(), NS::UTF8StringEncoding);
}

cc::string to_string(NS::String const* string)
{
    if (string == nullptr)
        return {};

    auto const* const utf8 = const_cast<NS::String*>(string)->utf8String();
    return utf8 == nullptr ? cc::string() : cc::string(utf8);
}

cc::string describe_error(NS::Error const* error, cc::string_view what)
{
    if (error == nullptr)
        return cc::format("{} (no NSError reported)", what);

    auto* const mutable_error = const_cast<NS::Error*>(error);
    return cc::format("{} ({}, code {})", what, to_string(mutable_error->localizedDescription()),
                      i64(mutable_error->code()));
}
} // namespace sg::backend::metal

cc::result<MTL::Library*> sg::backend::metal::library_from_shader(MTL::Device* device,
                                                                  sg::compiled_shader const& shader,
                                                                  cc::string_view what)
{
    if (shader.bytecode.empty())
        return cc::error(cc::format("{}: the shader carries nothing to build a library from", what));

    NS::Error* error = nullptr;
    auto* library = static_cast<MTL::Library*>(nullptr);

    if (shader.format == sg::shader_format::metal_lib)
    {
        // DISPATCH_DATA_DESTRUCTOR_DEFAULT copies, so the pinned bytes need not outlive this call.
        auto* const blob = dispatch_data_create(shader.bytecode.data(), size_t(shader.bytecode.size()), nullptr,
                                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        library = device->newLibrary(blob, &error);
        dispatch_release(blob);
    }
    else if (shader.format == sg::shader_format::msl)
    {
        // The blob is UTF-8 source rather than bytecode, and NS::String wants a terminator the blob does not carry.
        auto source = cc::string::create_filled(shader.bytecode.size(), char(0));
        for (auto i = isize(0); i < shader.bytecode.size(); ++i)
            source[i] = char(shader.bytecode[i]);

        auto* const options = MTL::CompileOptions::alloc()->init();
        library = device->newLibrary(ns_string(source), options, &error);
        options->release();
    }
    else
    {
        return cc::error(cc::format("{}: metal takes a metal_lib or msl shader, got format {}", what, int(shader.format)));
    }

    if (library == nullptr)
        return metal_error(error, cc::format("{}: the shader library could not be built", what));

    return library;
}
