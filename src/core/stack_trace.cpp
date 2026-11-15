// The calling thread's stack (include/stxlib/stack_trace.hpp): the
// interface modules' walk (host.hpp, capture_return_addresses) and a
// writer over describe_address (images.hpp) and the line formatter
// (core/stack_line.hpp). Allocates nothing. A1 platform-rest.

#include "stxlib/capabilities.hpp"
#include "stxlib/images.hpp"
#include "stxlib/stack_trace.hpp"

#include "core/stack_line.hpp"
#include "hosted/host.hpp"

namespace stxlib {

namespace {

// Room for the names one line shows; the line itself cuts them to
// stack_trace_line_capacity.
constexpr usize image_name_room_bytes = 384;
constexpr usize symbol_name_room_bytes = 384;

} // namespace

usize capture_stack_trace(const MutableView<uptr> addresses) noexcept {
    if (!capability_supported(Capability::stack_trace)) return 0;
    const usize wanted = addresses.count() < maximum_stack_trace_addresses ? addresses.count()
                                                                           : maximum_stack_trace_addresses;
    if (wanted == 0) return 0;
    const usize result = host::capture_report_addresses(addresses.data(), wanted);
    return result;
}

void write_stack_trace(const View<uptr> addresses) noexcept {
    char image_room[image_name_room_bytes]{};
    char symbol_room[symbol_name_room_bytes]{};
    char line[stack_trace_line_capacity]{};
    // write(2) is a POSIX cancellation point. A cancellation acted on there
    // would unwind through this noexcept function and terminate, so it is
    // held off for the writes and restored after (fail_fast has already
    // turned it off for good before its reporter runs, fail_fast.cpp).
    const int cancellation = host::cancellation_off();
    for (usize index = 0; index < addresses.count(); ++index) {
        const uptr address = addresses[index];
        stack_line::Frame frame{};
        frame.index = index;
        frame.address = address;
        // A return address points after its call; the instruction before
        // it is the call, which belongs to the caller's function even when
        // the call is the function's last instruction.
        const uptr lookup = address == 0 ? 0 : address - 1;
        const Result<AddressDescription> described =
            describe_address(lookup, MutableView<char>(image_room, image_name_room_bytes),
                             MutableView<char>(symbol_room, symbol_name_room_bytes));
        if (described.succeeded()) {
            const AddressDescription& description = *described.value();
            frame.image = description.image_name;
            frame.image_known = true;
            frame.image_offset = address - description.image_base;
            if (description.symbol_address != 0 && !description.symbol_name.empty()) {
                frame.symbol = description.symbol_name;
                frame.symbol_offset = address - description.symbol_address;
            }
        }
        const usize count = stack_line::format(frame, MutableView<char>(line, stack_trace_line_capacity));
        host::write_standard_error(line, count);
    }
    host::restore_cancellation(cancellation);
}

} // namespace stxlib
