#pragma once

// The primitives each family's display surface supplies
// (freebsd/surface_freebsd.cpp; surface_unsupported.cpp elsewhere).
// core/surface.cpp owns the one-per-process rule and checks every argument
// before calling them.

#include "stxlib/result.hpp"
#include "stxlib/surface.hpp"
#include "stxlib/types.hpp"

namespace stxlib::surface_backend {

// The host's surface, opened: the state is the caller's until close.
[[nodiscard]] Result<detail::SurfaceState*> open() noexcept;
// Restores the terminal and releases everything open() took.
void close(detail::SurfaceState* state) noexcept;
[[nodiscard]] u32 width(const detail::SurfaceState* state) noexcept;
[[nodiscard]] u32 height(const detail::SurfaceState* state) noexcept;
[[nodiscard]] SurfacePixels pixels(const detail::SurfaceState* state) noexcept;
// Every rectangle already checked within the surface.
[[nodiscard]] Result<SurfacePresent> present(detail::SurfaceState* state, const SurfaceRect* rects,
                                             usize count) noexcept;
// capacity is at least 1; wait_ms is -1 or at least 0.
[[nodiscard]] Result<usize> read_events(detail::SurfaceState* state, SurfaceEvent* events, usize capacity,
                                        i64 wait_ms) noexcept;

} // namespace stxlib::surface_backend
