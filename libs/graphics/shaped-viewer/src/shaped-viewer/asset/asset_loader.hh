#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/asset/asset_data.hh>
#include <shaped-viewer/asset/uri_resolver.hh>
#include <typed-geometry/scalar/angle.hh>

/// Loading meshes and materials out of a file, into the CPU-side types the scene API already takes.
///
/// babel is what reads the formats; sv is what turns a parsed document into things a view can draw, because materials,
/// textures, instancing and tangent frames are sv's vocabulary and babel must not grow one of its own.
/// See libs/graphics/shaped-viewer/docs/asset-loading.md for the design.
///
///     auto const loader = sv::asset_loader();
///     auto const car = loader.load("car.glb").value();
///     for (auto frame : viewer.frames())
///         for (auto const& m : car.meshes)
///             frame.add_scene().add_mesh(m);
///
/// The loader holds NO DEVICE, which is what buys the rest: loading on a worker thread, loading before a viewer exists,
/// CPU-side mesh processing after the load, and testing the whole importer with no device at all.

/// How a mesh's tangent frames are obtained.
///
/// `none` is the default and generates nothing: the hit shader computes the geometric frame from the triangle it
/// already has, so importing per-triangle normals would spend memory and bandwidth to store what is free.
/// Smoothing is a guess about authoring intent, which is why it is asked for rather than assumed.
enum class sv::frame_generation : sv::u8
{
    /// generate nothing, and let the geometric fallback answer where the file supplied no frame
    none,
    /// per_vertex after a position weld; cannot represent a hard edge
    smooth,
    /// per_corner, welded then split by `crease_angle` — smooth surfaces WITH hard edges
    crease,
};

/// How the importer fills `tangent_frame` / `tangent_handedness`.
///
/// The renderer wants a tangent FRAME rather than a normal: `tangent_frame` is a rotation taking tangent space to object
/// space, plus the mirror bit no rotation carries.
/// That is half the memory of a normal plus a tangent as vectors, and `SV_ATTR_SUPPLIED_tangent_frame` is what makes an
/// unsupplied one fall back to the geometric frame rather than to the identity rotation.
struct sv::tangent_frame_options
{
    /// import what the asset carries, which is the whole of phase 3
    bool prefer_file = true;

    sv::frame_generation generate = frame_generation::none;

    /// the angle above which `crease` splits a welded vertex into separate corners
    tg::angle_f crease_angle = tg::angle_f::make_from_degree(40.0f);

    /// how close two positions must be to weld; 0 welds exact duplicates only
    float weld_epsilon = 0.0f;
};

/// The formats `asset_loader` reads.
/// Every source is an overload rather than a mode flag, so this only names the container when a caller hands over
/// bytes with no uri to infer it from.
enum class sv::asset_format : sv::u8
{
    /// glTF 2.0, either container — `read` auto-detects `.gltf` JSON from `.glb`
    gltf,
    /// Wavefront OBJ
    obj,
    /// STL, either container — `read` decides by size, not by the leading `solid`
    stl,
};

/// What a loader does to every file it reads.
///
/// These carry ONLY what must happen during the parse, because it saves work or cannot be undone afterwards.
/// Everything else — picking meshes out, renaming, replacing a material after the fact — is ordinary code over
/// `asset_data`, which is why there is so little here.
///
/// The two hooks own their callables, since a loader outlives the expression that built it.
struct sv::asset_loader_config
{
    /// where imported materials are minted; null means the process-wide `sv::acquire_material_library`
    material_library* materials = nullptr;

    /// how a uri becomes bytes; unset means the process-wide `sv::resolve_uri`
    uri_resolver_provider resolve;

    /// Called per candidate mesh, with the name it would carry.
    /// Returning false skips it before its payloads are read, which is the point of doing this during the parse.
    /// Unset means every mesh is imported.
    cc::unique_function<bool(cc::string_view name)> include_mesh;

    /// false leaves every mesh at `material_id::invalid`, so they draw with `sv::default_material`
    bool import_materials = true;

    /// false imports the factors but none of the maps, which is what a caller wanting a fast structural look wants
    bool import_textures = true;

    /// Called per material the file names, before one is built for it.
    /// Return a valid id to use that material instead, or `material_id::invalid` to let the import proceed — which is
    /// what saves building and acquiring a material a caller was going to replace anyway.
    cc::unique_function<material_id(cc::string_view name)> material_override;

    /// true places every mesh in world space and leaves `asset_data::nodes` as a record.
    ///
    /// Instancing is already free here — geometry is content-hashed, so ten nodes referencing one glTF mesh produce ten
    /// meshes with ten transforms over a single upload.
    /// false leaves each mesh at its node's LOCAL transform, for a caller composing the tree themselves.
    bool flatten_hierarchy = true;

    sv::tangent_frame_options frames = {};
};

namespace sv
{
/// The format `uri`'s extension names, or empty when it names none this loader reads.
[[nodiscard]] cc::optional<asset_format> asset_format_of_uri(cc::string_view uri);
} // namespace sv

/// Reads assets into `sv::asset_data`, holding the options every load shares.
///
/// Its own type rather than more methods on `gpu_resource_manager` for two reasons.
/// It keeps babel out of the manager entirely, so the dependency stays confined to this corner.
/// And load options are almost always shared across many loads, so a loader holding them beats repeating an options
/// struct per call.
///
/// Move-only, because its config owns its hooks.
class sv::asset_loader
{
public:
    explicit asset_loader(asset_loader_config cfg = {}) : _config(cc::move(cfg)) {}

    asset_loader(asset_loader&&) noexcept = default;
    asset_loader& operator=(asset_loader&&) noexcept = default;
    asset_loader(asset_loader const&) = delete;
    asset_loader& operator=(asset_loader const&) = delete;
    ~asset_loader() = default;

    /// Loads `uri` through the resolver — never through the filesystem directly.
    /// The format comes from the extension, and a uri naming none this loader reads is an error rather than a guess.
    /// Relative uris inside the document (a glTF's external buffers and images) are joined against `uri`'s directory.
    [[nodiscard]] cc::result<asset_data> load(cc::string_view uri) const;

    /// The same from bytes already in hand, for a caller who fetched them their own way.
    ///
    /// `name` is what the asset and its material namespace are called; empty falls back to the format's own name.
    /// `base_uri` is what the document's own relative uris are joined against — pass the location the bytes came from
    /// when the document may reference files beside it, and nothing when it cannot.
    [[nodiscard]] cc::result<asset_data> load(cc::pinned_data<byte const> bytes,
                                              asset_format format,
                                              cc::string_view name = {},
                                              cc::string_view base_uri = {}) const;

    /// Imports an ALREADY-PARSED document.
    ///
    /// This is what lets a caller who read a file for their own reasons get meshes out of it without re-reading, and it
    /// is what keeps the babel document as the importer's actual input rather than an internal detail.
    [[nodiscard]] cc::result<asset_data> load(babel::gltf::data const& doc, cc::string_view name = {}) const;
    [[nodiscard]] cc::result<asset_data> load(babel::obj::data const& doc, cc::string_view name = {}) const;
    [[nodiscard]] cc::result<asset_data> load(babel::stl::data const& doc, cc::string_view name = {}) const;

    [[nodiscard]] asset_loader_config const& config() const { return _config; }
    [[nodiscard]] asset_loader_config& config() { return _config; }

private:
    /// The library imported materials are minted into, or an error when none could be reached.
    [[nodiscard]] cc::result<material_library*> _library() const;

    asset_loader_config _config;
};
