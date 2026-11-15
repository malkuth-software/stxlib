// turn_file_system.cpp: turn files on FreeBSD (turn_file_system.h).

#include "turn_file_system.h"

#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace stxstore {

namespace {

constexpr int interrupted_retries_max = 16;

class DescriptorOwner {
public:
    explicit DescriptorOwner(int descriptor) noexcept : descriptor_(descriptor) {}
    DescriptorOwner(const DescriptorOwner&) = delete;
    DescriptorOwner& operator=(const DescriptorOwner&) = delete;
    ~DescriptorOwner() {
        if (descriptor_ >= 0) {
            (void)close(descriptor_);
        }
    }
    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_;
    }
    // Closes now and reports close's errno (0 when it succeeded).
    [[nodiscard]] int close_now() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        if (descriptor < 0 || close(descriptor) == 0) {
            return 0;
        }
        return errno;
    }

private:
    int descriptor_;
};

// Reads the whole file into buffer. The size read, or nothing with errno
// in error_number (EFBIG when the file does not fit).
[[nodiscard]] std::optional<std::size_t> read_whole(const char* path, std::span<char> buffer,
                                                    int& error_number) noexcept {
    error_number = 0;
    DescriptorOwner file(::open(path, O_RDONLY | O_CLOEXEC));
    if (file.descriptor() < 0) {
        error_number = errno;
        return std::nullopt;
    }
    struct stat status{};
    if (fstat(file.descriptor(), &status) != 0) {
        error_number = errno;
        return std::nullopt;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        error_number = EFTYPE;
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(status.st_size) > buffer.size()) {
        error_number = EFBIG;
        return std::nullopt;
    }
    std::size_t total = 0;
    int interrupted = 0;
    // Each successful read adds a byte or ends at end of file; one byte of
    // room past the expected size shows a file that grew meanwhile.
    while (total < buffer.size()) {
        const ssize_t got = read(file.descriptor(), buffer.data() + total, buffer.size() - total);
        if (got < 0) {
            if (errno == EINTR && interrupted < interrupted_retries_max) {
                interrupted += 1;
                continue;
            }
            error_number = errno;
            return std::nullopt;
        }
        if (got == 0) {
            break;
        }
        total += static_cast<std::size_t>(got);
    }
    const std::optional<std::size_t> size = total;
    return size;
}

[[nodiscard]] bool write_all(int descriptor, std::span<const char> bytes, int& error_number) noexcept {
    std::size_t total = 0;
    int interrupted = 0;
    // Each successful write adds at least one byte.
    while (total < bytes.size()) {
        const ssize_t put = write(descriptor, bytes.data() + total, bytes.size() - total);
        if (put < 0) {
            if (errno == EINTR && interrupted < interrupted_retries_max) {
                interrupted += 1;
                continue;
            }
            error_number = errno;
            return false;
        }
        if (put == 0) {
            error_number = EIO;
            return false;
        }
        total += static_cast<std::size_t>(put);
    }
    return true;
}

}  // namespace

TurnState read_turns(const TurnPaths& paths, std::size_t body_max, std::span<char> buffer_0,
                     std::span<char> buffer_1) noexcept {
    TurnState state{};
    std::span<char> buffers[2] = {buffer_0, buffer_1};
    for (std::size_t index = 0; index < 2u; index += 1) {
        state.turns[index] = std::nullopt;
        state.errors[index] = 0;
        // One byte more than a valid file can hold, so a longer one reads
        // as too long rather than as a cut valid file.
        const std::size_t limit = turn_header_bytes_max + body_max + 1u;
        const std::span<char> buffer =
            buffers[index].size() < limit ? buffers[index] : buffers[index].subspan(0, limit);
        int error_number = 0;
        const std::optional<std::size_t> size = read_whole(paths.files[index], buffer, error_number);
        if (!size.has_value()) {
            state.errors[index] = error_number == ENOENT ? 0 : error_number;
            continue;
        }
        state.turns[index] = parse_turn_file(std::span<const char>(buffer.data(), *size), body_max);
    }
    state.choice = choose_turns(state.turns[0], state.turns[1]);
    return state;
}

TurnWriteOutcome::TurnWriteOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

TurnWriteOutcome TurnWriteOutcome::written(std::size_t file_index, std::uint32_t sequence) noexcept {
    TurnWritten state{};
    state.file_index = file_index;
    state.sequence = sequence;
    const TurnWriteOutcome outcome(Alternatives(std::in_place_type<TurnWritten>, state));
    return outcome;
}

TurnWriteOutcome TurnWriteOutcome::full() noexcept {
    const TurnWriteOutcome outcome(Alternatives(std::in_place_type<TurnStoreFull>));
    return outcome;
}

TurnWriteOutcome TurnWriteOutcome::failed(int error_number) noexcept {
    TurnWriteFailed state{};
    state.error_number = error_number;
    const TurnWriteOutcome outcome(Alternatives(std::in_place_type<TurnWriteFailed>, state));
    return outcome;
}

const TurnWritten* TurnWriteOutcome::as_written() const noexcept {
    return std::get_if<TurnWritten>(&alternatives_);
}

bool TurnWriteOutcome::is_full() const noexcept {
    return std::holds_alternative<TurnStoreFull>(alternatives_);
}

const TurnWriteFailed* TurnWriteOutcome::as_failed() const noexcept {
    return std::get_if<TurnWriteFailed>(&alternatives_);
}

TurnWriteOutcome write_turn(const TurnPaths& paths, const TurnChoice& choice, std::span<const char> body,
                            unsigned int mode) noexcept {
    if (!choice.next_sequence.has_value()) {
        return TurnWriteOutcome::full();
    }
    if (choice.write_index > 1u || body.size() > turn_body_bytes_max) {
        return TurnWriteOutcome::failed(EINVAL);
    }
    if (mkdir(paths.directory, 0700) != 0 && errno != EEXIST) {
        return TurnWriteOutcome::failed(errno);
    }
    char header[turn_header_bytes_max];
    const std::size_t header_size = format_turn_header(*choice.next_sequence, body, header);
    DescriptorOwner file(::open(paths.files[choice.write_index], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                static_cast<mode_t>(mode)));
    if (file.descriptor() < 0) {
        return TurnWriteOutcome::failed(errno);
    }
    int error_number = 0;
    if (!write_all(file.descriptor(), std::span<const char>(header, header_size), error_number) ||
        !write_all(file.descriptor(), body, error_number)) {
        return TurnWriteOutcome::failed(error_number);
    }
    if (fsync(file.descriptor()) != 0) {
        return TurnWriteOutcome::failed(errno);
    }
    const int closed = file.close_now();
    if (closed != 0) {
        return TurnWriteOutcome::failed(closed);
    }
    // The directory, too; its failure does not undo a synced file.
    DescriptorOwner directory(::open(paths.directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.descriptor() >= 0) {
        (void)fsync(directory.descriptor());
    }
    return TurnWriteOutcome::written(choice.write_index, *choice.next_sequence);
}

}  // namespace stxstore
