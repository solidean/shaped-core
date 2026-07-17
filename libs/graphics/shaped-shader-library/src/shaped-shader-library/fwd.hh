#pragma once

#include <clean-core/fwd.hh>

#include <memory>

/// Forward declarations and handle typedefs for shaped-shader-library. Include when a forward decl is
/// all you need.

namespace slib
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside slib, not leaked globally.
using namespace cc::primitive_defines;

// virtual filesystem
enum class file_revision : u64;
struct embedded_file;
class filesystem;
class memory_filesystem;
class embedded_filesystem;
class real_filesystem;
class mount_table;

/// A shared filesystem. std::shared_ptr because the handle is polymorphic: cc::shared_ptr's default
/// traits place the refcount at an offset derived from sizeof(T), so it cannot hold a derived object
/// through a base handle. Matches shaped-graphics, which uses std::shared_ptr for the same reason.
using filesystem_handle = std::shared_ptr<filesystem>;
} // namespace slib
