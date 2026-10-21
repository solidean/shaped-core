#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>

namespace slib::impl
{
/// What a bundle files a compile under: every input that decides the finished shader, read before any compiler runs.
/// Text is hashed without its carriage returns, since a recording build reads a checkout's files and a frozen one the embedded copy of them.
/// `modules` is what an SGL source may `use`, in any order; other languages pass none, and their `source` is the flattened text.
[[nodiscard]] cc::hash128 bundle_key(shader_language language,
                                     sg::shader_format format,
                                     sg::shader_stage stage,
                                     cc::string_view entry_point,
                                     cc::span<shader_option const> options,
                                     cc::string_view host_namespace,
                                     cc::string_view source,
                                     cc::span<module_source const> modules);

/// The shader a registered bundle holds under `key`, or nullopt.
[[nodiscard]] cc::optional<sg::compiled_shader> find_bundled_shader(cc::hash128 key);

/// Whether this process records what it compiles, which `SC_SHADER_RECORD` decides once.
[[nodiscard]] bool is_recording_shaders();

/// Writes `shader` to the recording under `key`, if this process records.
/// A bundled shader is written too, so a frozen build recording itself keeps every entry it still uses and drops the rest.
void record_shader(cc::hash128 key, sg::compiled_shader const& shader);

/// `built`, which writes its shader to the recording under `key` once it settles; a failed compile is not written.
[[nodiscard]] sg::async_compiled_shader recorded_shader(cc::hash128 key, sg::async_compiled_shader built);

/// The bytes a bundle holding just `shader` under `key` consists of, as a recording would write them.
[[nodiscard]] cc::vector<byte> make_bundle(cc::hash128 key, sg::compiled_shader const& shader);
} // namespace slib::impl
