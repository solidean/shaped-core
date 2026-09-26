#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
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
    /// A plain value in a block: the `@inline` one, or the constant buffer a group owns; at an offset every target agrees on.
    constant,
    /// A `buffer[T]`, which the host binds as a resource of its own.
    buffer,
    /// A sampled texture, `texture_2d[float4]` or a depth texture.
    texture,
    /// A storage texture, `out image_2d[.rgba8_unorm]`.
    image,
    /// A sampler: one the host binds, or a static one of the group, which carries `sampler_state`.
    sampler,
};

/// A static sampler's settings, named as `sg::sampler`'s fields and enum values name them.
struct sgl::described_sampler
{
    cc::string min_filter;
    cc::string mag_filter;
    cc::string mip_filter;
    cc::string address_u;
    cc::string address_v;
    cc::string address_w;
    /// Empty for a sampler that compares nothing.
    cc::string compare;
    i32 max_anisotropy = 1;
    f32 min_lod = 0.0f;
    f32 max_lod = 0.0f;
    f32 mip_lod_bias = 0.0f;
};

struct sgl::described_binding_member
{
    cc::string name;
    described_member_kind kind = described_member_kind::constant;
    /// A constant's type, a buffer's element, and any other resource's whole spelling: `out image_2d[.rgba8_unorm]`.
    cc::string type;
    /// A constant's byte offset in its block; -1 for a resource.
    i32 offset = -1;
    /// A constant's size in bytes; 0 for a resource.
    i32 size = 0;
    /// A resource's position among its binding's resources; -1 for a constant.
    i32 slot = -1;
    /// What the host binds a resource by, `binding.member`; empty for a constant.
    /// slib renames the compiled shader's reflected binding to it, so it is the name sg sees.
    cc::string host_name;

    // What an `sg::binding` states beyond its kind, each spelled as the sg enum value it is; empty where it does not apply.
    /// A texture's or an image's `sg::texture_view_dimension`: `tex_2d`.
    cc::string texture_dimension;
    /// A texture's `sg::texture_sample_type`: `filterable_float`, `depth`, ….
    cc::string sample_type;
    /// An image's `sg::pixel_format`: `rgba8_unorm`.
    cc::string image_format;
    /// Every resource's `sg::access_mode`: `read`, `write` or `read_write`.
    cc::string access;
    /// A sampler's `sg::sampler_binding_type`: `filtering`, `non_filtering` or `comparison`.
    cc::string sampler_type;
    /// A static sampler of the group; absent for one the host binds.
    cc::optional<described_sampler> static_sampler;
};

struct sgl::described_binding
{
    cc::string name;
    /// `@inline`: every member is a constant, and the block rides as inline constants rather than as a group.
    bool is_inline = false;
    cc::vector<described_binding_member> members;
    /// Where the last constant ends; 0 without one.
    i32 block_size = 0;
    /// A group's constant block: its slot, which is 0, and the name the host binds it by, which is the binding's own.
    /// -1 and empty for an `@inline` binding and for a group without a plain member.
    i32 block_slot = -1;
    cc::string block_host_name;
    /// The members' structural hash (`check::structural_hash`), as 32 hex digits: what a hot reload compares.
    cc::string shape;
};

struct sgl::described_struct_member
{
    cc::string name;
    cc::string type;
    /// Vertex attribute i of a `@vertex struct`, render target i of a `@pixel struct`.
    i32 location = -1;
    /// The buffer a vertex input member is read from, and whether it steps per instance; empty on a `@pixel struct`.
    cc::string stream;
    bool is_per_instance = false;
};

/// A `@vertex struct` or a `@pixel struct`.
struct sgl::described_struct
{
    cc::string name;
    /// `vertex` or `pixel`.
    check::stage edge = check::stage::none;
    cc::vector<described_struct_member> members;
    /// The members' structural hash (`check::structural_hash`), as 32 hex digits: what a hot reload compares.
    cc::string shape;
};

struct sgl::described_entry_point
{
    cc::string name;
    check::stage stage = check::stage::none;
    /// A compute entry point's grid; `{1, 1, 1}` for every other stage.
    i32 workgroup[3] = {1, 1, 1};
    /// The binding list in the order written, which is the order of the pipeline layout's groups with any `@inline` one last.
    cc::vector<cc::string> bindings;
    /// The `sg::feature`s a device needs to run it, by name, in the enum's order.
    cc::vector<cc::string> features;
};

/// One field of a pipeline's description, as the check pass resolved it.
struct sgl::described_pipeline_setting
{
    /// From the description down, with a target's member name where sg has an index: `color_targets.albedo.format`.
    cc::string path;
    check::setting_kind kind = check::setting_kind::boolean;
    /// 0 or 1 for a boolean, the value of an integer.
    i64 integer = 0;
    f64 real = 0;
    /// A case of `enum_name`, the enum of the same name in sg.
    cc::string enum_case;
    cc::string enum_name;
};

/// A `pipeline` declaration: its stages, its layout, and its settings over sg's defaults.
struct sgl::described_pipeline
{
    cc::string name;
    /// Entry point names; `pixel` is empty for a pipeline that writes depth alone.
    cc::string vertex;
    cc::string pixel;
    /// The binding layout, in group order, and its one `@inline` binding or empty.
    cc::vector<cc::string> layout;
    cc::string inline_constants;
    /// The `@vertex struct` it reads and the `@pixel struct` it writes; `target_set` is empty without a pixel stage.
    cc::string vertex_input;
    cc::string target_set;
    /// The members of `target_set`, in location order.
    cc::vector<cc::string> targets;
    /// What its stages need of a device together, as `described_entry_point::features`.
    cc::vector<cc::string> features;
    /// In the order they apply, each over the ones before it.
    cc::vector<described_pipeline_setting> settings;
    /// The paths the host states at acquire, whose last setting is `.host`, in the order first set so.
    cc::vector<cc::string> open;
    /// What the host's generated code is built against, one `key = value` line each, in a fixed order:
    /// the layout, the inline constants, the vertex input and the target set, each as `name@shape`, then the last
    /// setting of every format and of the sample count.
    /// A build bakes these, and a hot reload that finds any of them changed keeps what it had.
    cc::vector<cc::string> frozen;
};

struct sgl::module_description
{
    /// In source order.
    cc::vector<described_binding> bindings;
    cc::vector<described_struct> structs;
    cc::vector<described_entry_point> entry_points;
    cc::vector<described_pipeline> pipelines;
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
