#pragma once

/// Umbrella include for shaped-graphics' public surface (backends add their own headers). Prefer
/// the specific header you need.

#include <shaped-graphics/allocation_info.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_group.hh>
#include <shaped-graphics/binding_layout.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/command_list.compute.hh>
#include <shaped-graphics/command_list.copy.hh>
#include <shaped-graphics/command_list.download.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.upload.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/compute_pipeline.hh>
#include <shaped-graphics/context.cached.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.persistent.hh>
#include <shaped-graphics/context.transient.hh>
#include <shaped-graphics/context.upload.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory_heap.hh>
#include <shaped-graphics/pipeline_cache.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-graphics/types.hh>
#include <shaped-graphics/views.hh>
