// The display surface where the family has none (every family but
// FreeBSD's): open fails with unsupported, so nothing else is reached.

#include "core/host_failure.hpp"
#include "hosted/surface_backend.hpp"

namespace stxlib::surface_backend {

OperationResult<detail::SurfaceState*> open() noexcept {
    OperationResult<detail::SurfaceState*> result = OperationResult<detail::SurfaceState*>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.surface.open", HostCode::none));
    return result;
}

void close(detail::SurfaceState* const state) noexcept { (void)state; }

u32 width(const detail::SurfaceState* const state) noexcept {
    (void)state;
    return 0U;
}

u32 height(const detail::SurfaceState* const state) noexcept {
    (void)state;
    return 0U;
}

SurfacePixels pixels(const detail::SurfaceState* const state) noexcept {
    (void)state;
    const SurfacePixels none{nullptr, 0U, 0U, 0U};
    return none;
}

OperationResult<SurfacePresent> present(detail::SurfaceState* const state, const SurfaceRect* const rects,
                               const usize count) noexcept {
    (void)state;
    (void)rects;
    (void)count;
    OperationResult<SurfacePresent> result = OperationResult<SurfacePresent>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.surface.present", HostCode::none));
    return result;
}

OperationResult<usize> read_events(detail::SurfaceState* const state, SurfaceEvent* const events, const usize capacity,
                          const i64 wait_ms) noexcept {
    (void)state;
    (void)events;
    (void)capacity;
    (void)wait_ms;
    OperationResult<usize> result = OperationResult<usize>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.surface.read_events", HostCode::none));
    return result;
}

} // namespace stxlib::surface_backend
