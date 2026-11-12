#pragma once

#include <shaped-graphics/raytracing/blas_table.hh>

/// The host types of module `accel`, which SGL ships rather than a package exporting it, so slib names them here.
/// A package that binds `buffer[accel.ptlas_write]` binds a view of `sg::gpu_ptlas_write`, opaque on the host: its
/// fields stand in each backend's own order, and only a shader writes one.
namespace sgl_modules::accel
{
using blas_ref = sg::gpu_blas_ref;
using buffer_ref = sg::gpu_buffer_ref;
using clas_triangles = sg::gpu_clas_triangles;
using ptlas_write = sg::gpu_ptlas_write;
using ptlas_update = sg::gpu_ptlas_update;
using ptlas_translation = sg::gpu_ptlas_translation;
} // namespace sgl_modules::accel
