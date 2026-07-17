#pragma once

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-shader-library/shader_compiler.hh>

/// A compiler that pretends. It resolves `#include "x"` lines itself, then turns the flattened text into
/// an sg::compiled_shader whose "bytecode" is that text.
///
/// This is what lets slib's own tests cover the mechanism — packages, lazy per-format compiles, reload,
/// dependency tracking — on every platform, rather than only where DXC exists. The real ssc::dxc adapter
/// is exercised separately.
namespace slib_test
{
/// Text that makes the fake compiler fail, standing in for a shader that does not build.
inline constexpr cc::string_view k_broken_source = "!!broken!!";

class fake_compiler final : public slib::shader_compiler
{
public:
    fake_compiler(slib::shader_language language, sg::shader_format format) : _language(language), _format(format) {}

    [[nodiscard]] slib::shader_language source_language() const override { return _language; }
    [[nodiscard]] sg::shader_format target_format() const override { return _format; }

    /// Inlines every `#include "path"` line through `resolve`, one level deep per pass, until none are
    /// left. Enough to exercise dependency tracking without pulling in a real preprocessor.
    [[nodiscard]] cc::result<cc::string> preprocess(slib::shader_source_description const& desc,
                                                    slib::include_resolver resolve) const override
    {
        _preprocess_count.fetch_add(1);

        cc::string source = desc.source;
        for (int pass = 0; pass < 16; ++pass)
        {
            auto const at = source.find(cc::string_view("#include \""));
            if (at < 0)
                return source;

            auto const path_begin = at + cc::isize(cc::string_view("#include \"").size());
            auto const path_end = source.subview(path_begin).find('"');
            if (path_end < 0)
                return cc::error("unterminated #include");

            auto const path = source.subview({.offset = path_begin, .size = path_end});
            auto const included = resolve(path);
            if (!included.has_value())
                return cc::error(cc::format("include not found: '{}'", path));

            // Replace the whole directive (through the closing quote) with the included text.
            source.replace({.start = at, .end = path_begin + path_end + 1}, included.value());
        }
        return cc::error("too many include passes");
    }

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        _compile_count.fetch_add(1);

        if (desc.source.contains(k_broken_source))
            return cc::make_async_from_error<sg::compiled_shader>(
                cc::async_error::make_error(cc::any_error(cc::format("fake compile error in '{}'", desc.entry_point))));

        sg::compiled_shader shader;
        shader.stage = desc.stage;
        shader.format = _format;
        shader.entry_point = desc.entry_point;
        shader.bytecode
            = cc::pinned_data<cc::byte const>(cc::pinned_data<cc::byte>::create_copy_of(desc.source.as_bytes()));
        shader.compiler = sg::compiler_info{.name = "fake", .version = "1", .signature = desc.entry_point};
        return cc::make_async_from_value(cc::move(shader));
    }

    /// The compiled "bytecode" read back as text — the flattened source that reached compile().
    [[nodiscard]] static cc::string_view source_of(sg::compiled_shader const& shader)
    {
        return cc::string_view(reinterpret_cast<char const*>(shader.bytecode.data()), shader.bytecode.size());
    }

    [[nodiscard]] cc::i64 compile_count() const { return _compile_count.load(); }
    [[nodiscard]] cc::i64 preprocess_count() const { return _preprocess_count.load(); }

private:
    slib::shader_language _language;
    sg::shader_format _format;

    // Mutable so the const compile path can count; the tests read these to prove laziness and caching.
    mutable cc::atomic<cc::i64> _compile_count{0};
    mutable cc::atomic<cc::i64> _preprocess_count{0};
};
} // namespace slib_test
