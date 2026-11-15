// log_ring.cpp: the last bytes a target wrote (log_ring.h).

#include "log_ring.h"

namespace stxsupervisor {

LogRing::LogRing() noexcept : bytes_{}, next_(0), kept_(0), total_(0) {}

void LogRing::append(std::span<const char> bytes) noexcept {
    // Only the last log_ring_bytes of a long append can survive it.
    const std::span<const char> tail =
        bytes.size() > log_ring_bytes ? bytes.subspan(bytes.size() - log_ring_bytes) : bytes;
    for (std::size_t index = 0; index < tail.size(); index += 1) {
        bytes_[next_] = tail[index];
        next_ = next_ + 1u == log_ring_bytes ? 0u : next_ + 1u;
    }
    kept_ = kept_ + tail.size() > log_ring_bytes ? log_ring_bytes : kept_ + tail.size();
    const std::uint64_t added = static_cast<std::uint64_t>(bytes.size());
    total_ = total_ > UINT64_MAX - added ? UINT64_MAX : total_ + added;
}

LogRingEnd LogRing::copy_end(std::span<char> destination) const noexcept {
    const std::size_t count = kept_ < destination.size() ? kept_ : destination.size();
    // The oldest of the newest count bytes.
    std::size_t from = (next_ + log_ring_bytes - count) % log_ring_bytes;
    for (std::size_t index = 0; index < count; index += 1) {
        destination[index] = bytes_[from];
        from = from + 1u == log_ring_bytes ? 0u : from + 1u;
    }
    LogRingEnd end{};
    end.size = count;
    end.began_inside = total_ > static_cast<std::uint64_t>(count);
    return end;
}

std::uint64_t LogRing::total() const noexcept {
    return total_;
}

void LogRing::clear() noexcept {
    next_ = 0;
    kept_ = 0;
    total_ = 0;
}

}  // namespace stxsupervisor
