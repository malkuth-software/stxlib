#pragma once

// stxlib::conv's canon for affine, mesh and projective maps (canon.cpp): each destination
// pixel's value is the average of the canon field over the pixel's pulled-back footprint.
// Called by transfer.cpp after it has validated the map, the destination and the region.
// On failure no pixel of the region has been written.

#include "field_state.hpp"

#include "stxlib/conv.hpp"
#include "stxlib/result.hpp"

namespace stxlib::conv::detail {

[[nodiscard]] OperationStatus affine_canon(const FieldState& field, const AffineMap& map, const MutableRgba8Image& destination,
                                           const Region& region, WorkspaceState& workspace) noexcept;
[[nodiscard]] OperationStatus mesh_canon(const FieldState& field, const MeshMap& map, const MutableRgba8Image& destination,
                                         const Region& region, WorkspaceState& workspace) noexcept;
[[nodiscard]] OperationStatus projective_canon(const FieldState& field, const ProjectiveMap& map, const MutableRgba8Image& destination,
                                               const Region& region, WorkspaceState& workspace) noexcept;

} // namespace stxlib::conv::detail
