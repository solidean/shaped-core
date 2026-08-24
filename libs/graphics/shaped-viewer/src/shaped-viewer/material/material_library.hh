#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_type.hh>

/// Where material types and materials live, and what mints the ids a mesh carries.
///
/// Both halves are content-addressed: registering a type or a material twice from equal inputs hands back the id already resident,
/// so a caller rebuilding its scene every frame re-registers rather than accumulating.
///
/// **Nothing is ever evicted.** A `material_id` is written into GPU memory that outlives the frame it was minted in, so an
/// eviction would invalidate an index a parameter slot already stored.
/// The library is CPU-side and small — a type is a signature plus a string — so keeping everything costs little and is the only
/// thing that keeps those ids meaningful.
/// This is why it is not built on `impl::lru_pool`, which every other manager here is.
///
/// Not thread-safe, like the rest of viewer setup.
class sv::material_library
{
public:
    [[nodiscard]] static material_library create();

    /// The id for `type`, resident from an earlier register (O(1) on its content hash), or newly minted.
    /// A type whose `name` is taken by a DIFFERENT type asserts: `acquire_type(name)` would otherwise have no answer.
    material_type_id register_type(material_type type);

    /// The id of the type named `name`, or nullopt if none is registered.
    [[nodiscard]] cc::optional<material_type_id> acquire_type(cc::string_view name) const;

    [[nodiscard]] material_type const& get_type(material_type_id id) const;
    [[nodiscard]] bool contains_type(material_type_id id) const;

    /// The id for `m`, resident from an earlier acquire (O(1) on its content hash), or newly minted.
    ///
    /// Every binding is validated against the type's signature here, once, rather than on every resolve:
    /// a binding naming an attribute the type does not declare asserts, and so does a constant whose size is not its declaration's.
    /// This is the only place that pairing is known, which is why `material::create` checks neither.
    material_id acquire(material m);

    /// The id of the material named `name`, or nullopt if none is registered.
    /// Names are not unique across materials — the last one registered under a name wins — so this is a convenience for callers
    /// that keep them unique, not an identity.
    [[nodiscard]] cc::optional<material_id> acquire(cc::string_view name) const;

    [[nodiscard]] material const& get(material_id id) const;
    [[nodiscard]] bool contains(material_id id) const;

    [[nodiscard]] isize type_count() const { return _types.size(); }
    [[nodiscard]] isize material_count() const { return _materials.size(); }

private:
    // Stored in maps rather than vectors for the references, not for the lookup: a `resolved_material` borrows pointers into a
    // type's declarations and a material's bindings, and cc::map keeps those valid across every later insert.
    // A vector would invalidate them on a grow, so registering one more material would quietly poison every resolve still in hand.
    cc::map<material_type_id, material_type> _types;
    cc::map<cc::hash128, material_type_id> _type_by_hash;
    cc::map<cc::string, material_type_id> _type_by_name;

    cc::map<material_id, material> _materials;
    cc::map<cc::hash128, material_id> _material_by_hash;
    cc::map<cc::string, material_id> _material_by_name;

    u32 _next_type = 0;
    u32 _next_material = 0;
};

namespace sv
{
/// How a viewer gets hold of a material library.
///
/// A provider only has to *create* one: it is called at most once per process, and `acquire_material_library` is what makes that
/// happen only once — so a provider needs no static and no caching of its own.
using material_library_provider = cc::unique_function<cc::result<material_library*>()>;

/// Sets the hook that decides which material library viewers draw from.
///     sv::set_acquire_material_library([] { return &my_library; });
/// Unset by default, and then `impl::acquire_default_material_library` answers instead, which registers the builtin types.
/// Passing `{}` clears it again, which is how a test hands the default back.
///
/// The library must outlive every viewer using it, which is why this hands out a pointer rather than a value: a library is
/// referenced by id from GPU memory, so it cannot be copied or moved out from under one.
void set_acquire_material_library(material_library_provider provider);

namespace impl
{
/// Creates the library used when no provider was set, registering every builtin type.
/// It creates unconditionally — `acquire_material_library` is what makes that happen only once.
[[nodiscard]] cc::result<material_library*> acquire_default_material_library();
} // namespace impl

/// The material library every viewer draws from: the caller's provider if they set one, otherwise the built-in default.
/// Created on the first call and shared by every caller after.
/// Not thread-safe, like the rest of viewer setup.
[[nodiscard]] cc::result<material_library*> acquire_material_library();

/// Registers the builtin types into `lib` — `sv::builtin_material::pbr` and `unlit`.
/// Public so a caller supplying their own library still gets them without reaching into the default.
void register_builtin_material_types(material_library& lib);
} // namespace sv

/// The names the builtin material types are registered under.
namespace sv::builtin_material
{
inline constexpr cc::string_view pbr = "pbr";
inline constexpr cc::string_view unlit = "unlit";
} // namespace sv::builtin_material
