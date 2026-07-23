#pragma once

#include <clean-core/fwd.hh>

/// Aggregate forward declarations for babel-serializer.
/// Each format lives in its own sub-namespace (babel::json, babel::obj) and owns its own header;
/// include that header directly when it is all you need.

namespace babel
{
// Pull in the shaped-core vocabulary types (i32, u8, isize, ...) so we write them bare inside babel
// without leaking them into the global namespace.
using namespace cc::primitive_defines;
} // namespace babel

namespace babel::json
{
enum class node_kind : cc::u8;
struct node;
class document;
struct ref;
} // namespace babel::json

namespace babel::obj
{
struct corner;
struct face;
struct group;
struct data;
} // namespace babel::obj
