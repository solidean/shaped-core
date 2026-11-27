#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>

namespace sgl::driver::impl
{
/// The back half of `compile_to_text`: one entry point of a checked module, written for one target.
/// `numbering` is a link group's (LINK-6), null for the module's own; `compile_to_text` and `sgl::link` share it, so a
/// linked entry whose numbers are its file's is the same output byte for byte.
[[nodiscard]] cc::result<emitted_source, cc::string> emit_text(front_end const& front,
                                                               check::flat_entry_point const& e,
                                                               emit::target target,
                                                               bool line_directives,
                                                               bool diagnostics,
                                                               check::undefined_guard guards_undefined,
                                                               bool asks_software_trace,
                                                               bool weakens_float,
                                                               check::float_level float_floor,
                                                               check::feature_set device_features,
                                                               cc::span<check::feature_fill const> fills,
                                                               check::core_fills core,
                                                               emit::native_route native = emit::native_route::text,
                                                               emit::debug_info native_debug_info = emit::debug_info::none,
                                                               emit::mesh_form mesh = emit::mesh_form::native,
                                                               check::flat_entry_point const* polyfill_vertex = nullptr,
                                                               check::flat_entry_point const* polyfill_task = nullptr,
                                                               check::flat_entry_point const* polyfill_control = nullptr,
                                                               emit::numbering const* numbering = nullptr);

/// What `front` says of the text of entry point `e`: the library files the source reached, and the warnings of the
/// program and of `e` (CHK-402).
void add_source_facts(cc::result<emitted_source, cc::string>& text,
                      front_end const& front,
                      check::flat_entry_point const& e);

/// The warnings a compile of `e` adds once it is written: CHK-436's dropped under a float floor, LEGAL-69's added.
void finish_warnings(cc::result<emitted_source, cc::string>& result,
                     cc::string_view source_name,
                     check::flat_entry_point const& e);
} // namespace sgl::driver::impl
