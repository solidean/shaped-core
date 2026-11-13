#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

namespace sxr
{
using namespace cc::primitive_defines;

enum class graphics_api : u8; // the binding a session renders through: vulkan or dx12
struct system_description;    // what a system is created for
struct session_description;   // what a session is created for
enum class capability : u8;   // what an application can ask of a runtime beyond rendering
enum class availability : u8; // whether a session may use a capability now
enum class blend_mode : u8;   // how a frame meets the room behind it
enum class action_kind : u8;  // boolean, scalar, vector2, pose or vibration
enum class hand : u8;         // left or right
struct action_binding;        // an interaction profile's path an action is suggested at
struct action_description;    // an action by name, its kind and its bindings
struct runtime_feature;       // one thing a runtime says about its headset
struct runtime_report;        // what the active runtime offers
struct controller_pose;       // one hand as the controller routine draws it
class controller_routine;     // draws the hands as plain boxes
struct view;                  // one eye's pose and projection in tg's frame
struct frame;                 // one frame's views and the images each is rendered into
class system;                 // the runtime and the headset it found
class session;                // the runtime's frame loop over an sg context

/// The domain every recording site in shaped-xr is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sxr
