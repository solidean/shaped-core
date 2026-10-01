#pragma once

/// Every geometric query verb, for every object type.
/// The verbs are members (`a.distance_to(b)`); including a verb's header is what makes it callable.

#include <typed-geometry/geometry/query/closest_points.hh>
#include <typed-geometry/geometry/query/contains.hh>
#include <typed-geometry/geometry/query/distance.hh>
#include <typed-geometry/geometry/query/intersects.hh>
#include <typed-geometry/geometry/query/project.hh>
#include <typed-geometry/geometry/query/separation.hh>
