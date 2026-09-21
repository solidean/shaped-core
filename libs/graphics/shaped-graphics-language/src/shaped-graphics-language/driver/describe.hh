#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/symbols.hh>

/// What the host side of one SGL source is generated from: its binding groups, its pipeline-edge structs and its entry points.
///
/// This is the seam between the compiler and a build that writes C++, and it knows nothing about that C++.
/// A type is named as SGL spells it, `float3` or `mat4`, and mapping it to a host type is the reader's job.
/// It holds only what the file itself declares: a module it `use`s describes its own declarations.

/// What a binding member is to the host.
enum class sgl::described_member_kind : sgl::u8
{
    /// A plain value in an `@inline` block, at a byte offset every target agrees on.
    constant,
    /// A `buffer[T]`, which the host binds as a resource of its own.
    buffer,
};

struct sgl::described_binding_member
{
    cc::string name;
    described_member_kind kind = described_member_kind::constant;
    /// The value's type; for a buffer, its element.
    cc::string type;
    /// A buffer the shader may write: `mut buffer[T]`.
    bool is_mut = false;
    /// A constant's byte offset in its block; -1 for a buffer.
    i32 offset = -1;
    /// A constant's size in bytes; 0 for a buffer.
    i32 size = 0;
    /// A buffer's position among its binding's resources; -1 for a constant.
    i32 slot = -1;
    /// The name a compiled shader reflects a buffer under, `<binding>_<member>`; empty for a constant.
    cc::string reflected_name;
};

struct sgl::described_binding
{
    cc::string name;
    /// `@inline`: every member is a constant, and the block rides as inline constants rather than as a group.
    bool is_inline = false;
    cc::vector<described_binding_member> members;
    /// Where the last constant ends; 0 without one.
    i32 block_size = 0;
};

struct sgl::described_struct_member
{
    cc::string name;
    cc::string type;
    /// Vertex attribute i of a `@vertex struct`, render target i of a `@pixel struct`.
    i32 location = -1;
};

/// A `@vertex struct` or a `@pixel struct`.
struct sgl::described_struct
{
    cc::string name;
    /// `vertex` or `pixel`.
    check::stage edge = check::stage::none;
    cc::vector<described_struct_member> members;
};

struct sgl::described_entry_point
{
    cc::string name;
    check::stage stage = check::stage::none;
    /// A compute entry point's grid; `{1, 1, 1}` for every other stage.
    i32 workgroup[3] = {1, 1, 1};
    /// The binding list in the order written, which is the order of the pipeline layout's groups with any `@inline` one last.
    cc::vector<cc::string> bindings;
};

struct sgl::module_description
{
    /// In source order.
    cc::vector<described_binding> bindings;
    cc::vector<described_struct> structs;
    cc::vector<described_entry_point> entry_points;
};

struct sgl::describe_request
{
    cc::string_view source;
    /// What a diagnostic calls the source; it is never opened.
    cc::string_view source_name = "<sgl>";
};

namespace sgl
{
/// Everything the host side of `request.source` is generated from, or why it cannot be.
///
/// It holds exactly what compiles: a declaration an emitter would refuse is refused here too, in the words `compile_to_text` uses.
/// That covers every binding and every edge struct the file declares, listed by an entry point or not, and every entry point.
/// So a source that describes also emits, and a generated C++ type never stands for a shader that cannot be built.
///
/// Deterministic, and it reads nothing but its arguments.
[[nodiscard]] cc::result<module_description, cc::string> describe(describe_request const& request);
} // namespace sgl
