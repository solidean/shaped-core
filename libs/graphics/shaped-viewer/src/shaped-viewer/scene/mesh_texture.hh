#pragma once

#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>

/// A texture the mesh offers its material, bound to the slot `name` identifies (`"albedo"`, `"normal"`, ...).
///
/// The mesh names *which* texture, the material names *what for* — same contract as the parameters.
/// The id is resolved through the texture_manager at render time; `texture_id::invalid` is a slot that is declared but unfilled.
struct sv::mesh_texture
{
    cc::string name;
    texture_id texture = texture_id::invalid;
};
