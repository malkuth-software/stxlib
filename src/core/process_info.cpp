// The process's identity, arguments and memory (include/stxlib/process.hpp)
// and the main-thread test (include/stxlib/threads.hpp), over the family's
// process module (hosted/process_backend.hpp). A1 platform-rest: GUI.Forms
// read these directly (theme_files.cpp, application.cpp,
// bundled_font_memory_report.cpp at 491156eac). Holds no state: each call
// asks the host.

#include "stxlib/capabilities.hpp"
#include "stxlib/process.hpp"
#include "stxlib/text.hpp"
#include "stxlib/threads.hpp"

#include "core/command_line.hpp"
#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "hosted/process_backend.hpp"

namespace stxlib {

namespace detail {

class ProcessArgumentsBuilder final {
public:
    [[nodiscard]] static ProcessArguments make(Buffer<char> text, Buffer<ArgumentRecord> records) noexcept {
        ProcessArguments result;
        result.text_ = static_cast<Buffer<char>&&>(text);
        result.records_ = static_cast<Buffer<ArgumentRecord>&&>(records);
        return result;
    }
};

} // namespace detail

namespace {

constexpr Operation arguments_operation = "stxlib.process.arguments";

OperationResult<ProcessArguments> unsupported_arguments() noexcept {
    const ErrorRecord record = host_failure::record(ErrorKind::unsupported, arguments_operation, HostCode::none);
    return OperationResult<ProcessArguments>::failure(record);
}

bool is_utf8(const char* const bytes, const usize count) noexcept {
    const OperationResult<TextView> checked = validate_utf8(bytes, count);
    const bool result = checked.succeeded();
    return result;
}

// The records of a NUL-separated block held in `text`: each argument ends
// at a NUL; bytes after the last NUL (a host that dropped the final NUL)
// form one more argument.
OperationResult<ProcessArguments> from_nul_separated(Buffer<char> text) noexcept {
    const usize count = text.count();
    usize arguments = 0;
    for (usize index = 0; index < count; ++index) {
        if (text[index] == '\0') arguments = arguments + 1;
    }
    if (count != 0 && text[count - 1] != '\0') arguments = arguments + 1;
    OperationResult<Buffer<detail::ArgumentRecord>> made = Buffer<detail::ArgumentRecord>::allocate_zeroed(arguments);
    if (made.failed()) return OperationResult<ProcessArguments>::failure(*made.error());
    Buffer<detail::ArgumentRecord> records = static_cast<OperationResult<Buffer<detail::ArgumentRecord>>&&>(made).take_value();
    usize start = 0;
    usize next = 0;
    for (usize index = 0; index <= count && next < arguments; ++index) {
        if (index < count && text[index] != '\0') continue;
        const usize length = index - start;
        records[next] = detail::ArgumentRecord{start, length, is_utf8(text.view().data() + start, length)};
        next = next + 1;
        start = index + 1;
    }
    ProcessArguments result = detail::ProcessArgumentsBuilder::make(
        static_cast<Buffer<char>&&>(text), static_cast<Buffer<detail::ArgumentRecord>&&>(records));
    return OperationResult<ProcessArguments>::success(static_cast<ProcessArguments&&>(result));
}

// The command line `line` split by the C runtime's rules
// (core/command_line.hpp): measured, then written into exact buffers.
OperationResult<ProcessArguments> from_command_line(const View<char> line) noexcept {
    const command_line::SplitCounts needed =
        command_line::split_windows_command_line(line, MutableView<char>(), MutableView<usize>());
    OperationResult<Buffer<char>> made_text = Buffer<char>::allocate_zeroed(needed.text_bytes);
    if (made_text.failed()) return OperationResult<ProcessArguments>::failure(*made_text.error());
    Buffer<char> text = static_cast<OperationResult<Buffer<char>>&&>(made_text).take_value();
    OperationResult<Buffer<usize>> made_starts = Buffer<usize>::allocate_zeroed(needed.argument_count);
    if (made_starts.failed()) return OperationResult<ProcessArguments>::failure(*made_starts.error());
    Buffer<usize> starts = static_cast<OperationResult<Buffer<usize>>&&>(made_starts).take_value();
    const command_line::SplitCounts written =
        command_line::split_windows_command_line(line, text.mutable_view(), starts.mutable_view());
    STXLIB_REQUIRE(written.text_bytes == needed.text_bytes && written.argument_count == needed.argument_count,
                   "split_windows_command_line: the two passes disagree");
    OperationResult<Buffer<detail::ArgumentRecord>> made = Buffer<detail::ArgumentRecord>::allocate_zeroed(needed.argument_count);
    if (made.failed()) return OperationResult<ProcessArguments>::failure(*made.error());
    Buffer<detail::ArgumentRecord> records = static_cast<OperationResult<Buffer<detail::ArgumentRecord>>&&>(made).take_value();
    for (usize index = 0; index < needed.argument_count; ++index) {
        const usize start = starts[index];
        // Each argument is followed by its NUL, before the next one's start.
        const usize end = index + 1 < needed.argument_count ? starts[index + 1] : needed.text_bytes;
        const usize length = end - start - 1;
        records[index] = detail::ArgumentRecord{start, length, is_utf8(text.view().data() + start, length)};
    }
    ProcessArguments result = detail::ProcessArgumentsBuilder::make(
        static_cast<Buffer<char>&&>(text), static_cast<Buffer<detail::ArgumentRecord>&&>(records));
    return OperationResult<ProcessArguments>::success(static_cast<ProcessArguments&&>(result));
}

} // namespace

TextView ArgumentText::unicode() const noexcept {
    STXLIB_REQUIRE(unicode_, "ArgumentText::unicode: the argument is not valid Unicode");
    const TextView result = detail::TextViewAccess::adopt_validated(bytes_, count_);
    return result;
}

ArgumentText ProcessArguments::argument(const usize index) const& noexcept {
    const detail::ArgumentRecord& record = records_[index];
    const char* const bytes = record.count == 0 ? text_.view().data() : text_.view().data() + record.offset;
    const ArgumentText result(bytes, record.count, record.unicode);
    return result;
}

u64 process_id() noexcept {
    const u64 result = process_backend::process_id();
    return result;
}

OperationResult<ProcessArguments> process_arguments(const usize limit) noexcept {
    if (!capability_supported(Capability::process_arguments)) return unsupported_arguments();
    Growable<char> raw;
    const OperationStatus read = process_backend::arguments(limit, raw);
    if (read.failed()) return OperationResult<ProcessArguments>::failure(*read.error());
    if (process_backend::argument_form == process_backend::ArgumentForm::windows_command_line) {
        const View<char> line(raw.data(), raw.count());
        OperationResult<ProcessArguments> result = from_command_line(line);
        return result;
    }
    OperationResult<Buffer<char>> made = Buffer<char>::allocate_zeroed(raw.count());
    if (made.failed()) return OperationResult<ProcessArguments>::failure(*made.error());
    Buffer<char> text = static_cast<OperationResult<Buffer<char>>&&>(made).take_value();
    for (usize index = 0; index < raw.count(); ++index) text[index] = raw[index];
    OperationResult<ProcessArguments> result = from_nul_separated(static_cast<Buffer<char>&&>(text));
    return result;
}

OperationResult<ProcessMemory> process_memory() noexcept {
    if (!capability_supported(Capability::memory_report)) {
        const ErrorRecord record =
            host_failure::record(ErrorKind::unsupported, "stxlib.process.memory", HostCode::none);
        return OperationResult<ProcessMemory>::failure(record);
    }
    ProcessMemory memory{};
    const OperationStatus read = process_backend::memory(memory);
    if (read.failed()) return OperationResult<ProcessMemory>::failure(*read.error());
    // The host's peak is sampled (process.hpp), so it is raised to the
    // current size, which it can never truly be below.
    if (memory.peak_resident_bytes < memory.resident_bytes) memory.peak_resident_bytes = memory.resident_bytes;
    return OperationResult<ProcessMemory>::success(memory);
}

OperationResult<bool> is_main_thread() noexcept {
    if (!capability_supported(Capability::main_thread)) {
        const ErrorRecord record =
            host_failure::record(ErrorKind::unsupported, "stxlib.threads.is_main_thread", HostCode::none);
        return OperationResult<bool>::failure(record);
    }
    bool initial = false;
    const OperationStatus asked = process_backend::is_main_thread(initial);
    if (asked.failed()) return OperationResult<bool>::failure(*asked.error());
    return OperationResult<bool>::success(initial);
}

} // namespace stxlib
