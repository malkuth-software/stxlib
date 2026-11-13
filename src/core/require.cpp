// The requirement failure report (include/stxlib/require.hpp).

#include "stxlib/require.hpp"

#include "core/report.hpp"
#include "hosted/host.hpp"

namespace stxlib::detail {

// Bounds for foreign strings in the report: a message is ours, a file name
// is the compiler's; neither is trusted to be short.
constexpr usize message_limit = 1024;
constexpr usize file_limit = 4096;

void requirement_failed(const char* const message, const char* const file, const int line) noexcept {
    report::text("stxlib: requirement failed: ");
    report::c_text(message, message_limit);
    report::text(" (");
    report::c_text(file, file_limit);
    report::text(":");
    report::signed_number(static_cast<i64>(line));
    report::text(")\n");
    host::end_process_abnormally();
}

} // namespace stxlib::detail
