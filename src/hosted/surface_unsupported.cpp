// The display surface where the family has none (every family but
// FreeBSD's): open fails with unsupported, so nothing else is reached.

#include "core/host_failure.hpp"
#include "hosted/surface_backend.hpp"

namespace stxlib::surface_backend {

Result<detail::SurfaceState*> open() noexcept {
    Result<detail::SurfaceState*> result = Result<detail::SurfaceState*>::failure(
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

Result<SurfacePresent> present(detail::SurfaceState* const state, const SurfaceRect* const rects,
                               const usize count) noexcept {
    (void)state;
    (void)rects;
    (void)count;
    Result<SurfacePresent> result = Result<SurfacePresent>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.surface.present", HostCode::none));
    return result;
}

Result<usize> read_events(detail::SurfaceState* const state, SurfaceEvent* const events, const usize capacity,
                          const i64 wait_ms) noexcept {
    (void)state;
    (void)events;
    (void)capacity;
    (void)wait_ms;
    Result<usize> result = Result<usize>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.surface.read_events", HostCode::none));
    return result;
}

} // namespace stxlib::surface_backend
