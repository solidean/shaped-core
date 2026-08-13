#pragma once

#include <shaped-viewer/fwd.hh>

namespace sv
{
/// Strongly-typed resource handles a scene item references.
///
/// Each is an opaque integer newtype minted by the matching manager (see resource_managers.hh).
/// A scene item names *what* it draws by id, and the renderer resolves the id to the concrete GPU resource through the managers.
/// Being `enum class`, they hash and compare out of the box, so they key a cc::map with no extra boilerplate.
///
/// `invalid` (`u32(-1)`, all bits set) is the reserved null id every manager skips when handing ids out.
/// The managers mint from 0 upward, so 0 is a usable id and only the top of the range is the sentinel.
enum class mesh_id : u32
{
    invalid = u32(-1)
};

enum class material_set_id : u32
{
    invalid = u32(-1)
};

/// Names ONE material definition — how a mesh is drawn — rather than a per-triangle array of them.
///
/// This is the thin handle an `sv::mesh` carries: the definition lives outside the mesh and is shared across many.
/// It is what gives a mesh's attributes, parameters, textures and flags their meaning.
/// No manager mints these yet — the material library is still to come — so a mesh only ever carries `invalid` today.
enum class material_id : u32
{
    invalid = u32(-1)
};

enum class tlas_id : u32
{
    invalid = u32(-1)
};

enum class texture_id : u32
{
    invalid = u32(-1)
};

enum class buffer_id : u32
{
    invalid = u32(-1)
};
} // namespace sv
