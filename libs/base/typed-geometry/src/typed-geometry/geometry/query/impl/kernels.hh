#pragma once

/// Every kernel tg ships, so that a verb header sees all of them before it defines anything.
/// A concept's answer is cached per translation unit; including kernels after a verb was first asked about a pair
/// would let that pair have two answers in one program.

#include <typed-geometry/geometry/query/impl/kernels/aabb.hh>
#include <typed-geometry/geometry/query/impl/kernels/box.hh>
#include <typed-geometry/geometry/query/impl/kernels/constructive.hh>
#include <typed-geometry/geometry/query/impl/kernels/halfspace.hh>
#include <typed-geometry/geometry/query/impl/kernels/hot_pairs.hh>
#include <typed-geometry/geometry/query/impl/kernels/linear.hh>
#include <typed-geometry/geometry/query/impl/kernels/parameters.hh>
#include <typed-geometry/geometry/query/impl/kernels/plane.hh>
#include <typed-geometry/geometry/query/impl/kernels/pos.hh>
#include <typed-geometry/geometry/query/impl/kernels/round.hh>
#include <typed-geometry/geometry/query/impl/kernels/sphere.hh>
#include <typed-geometry/geometry/query/impl/kernels/triangle.hh>

// the generic floor, after every closed form
#include <typed-geometry/geometry/query/impl/epa.hh>
#include <typed-geometry/geometry/query/impl/gjk.hh>
