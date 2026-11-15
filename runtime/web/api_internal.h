// api_internal.h: helpers api.cpp and api_pages.cpp share (not part of the
// interface; tests do not use it).

#ifndef STXLIB_RUNTIME_WEB_API_INTERNAL_H
#define STXLIB_RUNTIME_WEB_API_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "../supervisor/control/control_protocol.h"
#include "api.h"
#include "json.h"

namespace stxweb::api_detail {

// The HTTP status a supervisor status answers with.
[[nodiscard]] std::uint16_t http_status_of(stxcontrol::Status status) noexcept;
// A slot as "A", "B" or null.
void write_slot(JsonWriter& writer, stxcontrol::SlotCode code) noexcept;
void write_service_record(JsonWriter& writer, const stxcontrol::ServiceRecord& record) noexcept;
extern const char empty_object[];
// A response with nothing decided yet: 500, JSON, the given (empty) body,
// no cookie, no event.
[[nodiscard]] ApiResponse blank_response(std::span<const char> body) noexcept;
// Writes "{}" into body; returns its size.
[[nodiscard]] std::size_t copy_empty_object(std::span<char> body) noexcept;

}  // namespace stxweb::api_detail

#endif  // STXLIB_RUNTIME_WEB_API_INTERNAL_H
