// The display surface (surface.hpp): the one-per-process rule and the
// argument checks; the family's backend does the rest.
//
// Early exits (decision 2026-11-15, every foreign call a possible early
// exit): the only foreign calls are the backend's (kernel interfaces).
// open() claims the process's surface before the backend runs and gives the
// claim back if the backend fails, so the claim is whole either way; a
// Surface's state pointer is set only from a completed open and cleared
// before the backend's close runs, so an exit during close leaves a closed
// Surface (whose terminal the kernel restores when the process ends).

#include "core/host_failure.hpp"
#include "core/surface_blit.hpp"
#include "hosted/surface_backend.hpp"

#include <atomic>

namespace stxlib {

namespace {

std::atomic<bool> surface_claimed{false};

} // namespace

Surface::Surface(detail::SurfaceState* const state) noexcept : state_(state) {}

Surface::Surface(Surface&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

Surface& Surface::operator=(Surface&& other) noexcept {
    if (this != &other) {
        close();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

Surface::~Surface() { close(); }

u32 Surface::width() const noexcept { return state_ == nullptr ? 0U : surface_backend::width(state_); }

u32 Surface::height() const noexcept { return state_ == nullptr ? 0U : surface_backend::height(state_); }

SurfacePixels Surface::pixels() const noexcept {
    if (state_ == nullptr) {
        const SurfacePixels none{nullptr, 0U, 0U, 0U};
        return none;
    }
    return surface_backend::pixels(state_);
}

OperationResult<SurfacePresent> Surface::present(const SurfaceRect* const rects, const usize count) noexcept {
    if (state_ == nullptr) {
        return OperationResult<SurfacePresent>::failure(
            host_failure::record(ErrorKind::disposed, "stxlib.surface.present", HostCode::closed_handle));
    }
    if (count != 0U && rects == nullptr) {
        return OperationResult<SurfacePresent>::failure(
            host_failure::record(ErrorKind::invalid_argument, "stxlib.surface.present", HostCode::empty_request));
    }
    const u32 surface_width = surface_backend::width(state_);
    const u32 surface_height = surface_backend::height(state_);
    for (usize index = 0U; index < count; ++index) {
        if (!surface_blit::rect_within(rects[index], surface_width, surface_height)) {
            return OperationResult<SurfacePresent>::failure(
                host_failure::record(ErrorKind::invalid_argument, "stxlib.surface.present", HostCode::invalid_value));
        }
    }
    if (count == 0U) return OperationResult<SurfacePresent>::success(SurfacePresent::presented);
    return surface_backend::present(state_, rects, count);
}

OperationResult<usize> Surface::read_events(SurfaceEvent* const events, const usize capacity,
                                            const i64 wait_ms) noexcept {
    if (state_ == nullptr) {
        return OperationResult<usize>::failure(
            host_failure::record(ErrorKind::disposed, "stxlib.surface.read_events", HostCode::closed_handle));
    }
    if (events == nullptr || capacity == 0U || wait_ms < -1) {
        return OperationResult<usize>::failure(
            host_failure::record(ErrorKind::invalid_argument, "stxlib.surface.read_events", HostCode::invalid_value));
    }
    return surface_backend::read_events(state_, events, capacity, wait_ms);
}

void Surface::close() noexcept {
    detail::SurfaceState* const state = state_;
    if (state == nullptr) return;
    state_ = nullptr;
    surface_backend::close(state);
    surface_claimed.store(false, std::memory_order_release);
}

OperationResult<Surface> open_surface() noexcept {
    bool expected = false;
    if (!surface_claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return OperationResult<Surface>::failure(
            host_failure::record(ErrorKind::limit_exceeded, "stxlib.surface.open", HostCode::busy));
    }
    OperationResult<detail::SurfaceState*> opened = surface_backend::open();
    if (opened.failed()) {
        surface_claimed.store(false, std::memory_order_release);
        return OperationResult<Surface>::failure(*opened.error());
    }
    Surface surface(*opened.value());
    return OperationResult<Surface>::success(static_cast<Surface&&>(surface));
}

} // namespace stxlib
