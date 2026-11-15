#pragma once

// A byte buffer of tool-chosen length and contents for the C++ harnesses.
//
// The buffer is allocated at exactly its length, so a read one byte past the
// input is a memory error the tool reports; a fixed array of the capacity
// would hide it. The length is at most `capacity` (the row's bound, STX_N).
//
// In unbounded runs (STX_UNBOUNDED, k-induction) the contents are not
// written: they are whatever the tool's model gives fresh memory, which for
// ESBMC and CBMC is an arbitrary value per byte. That is an assumption of
// those runs and the runner records it.

#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

class SymbolicBytes final {
public:
    explicit SymbolicBytes(const std::size_t capacity) : length_(nondet_size()) {
        STX_ASSUME(length_ <= capacity);
        storage_.reset(new unsigned char[length_]);
#if !defined(STX_UNBOUNDED)
        for (std::size_t index = 0; index < length_; ++index) storage_[index] = nondet_uchar();
#endif
    }
    SymbolicBytes(const SymbolicBytes&) = delete;
    SymbolicBytes& operator=(const SymbolicBytes&) = delete;

    // Restricts every byte to one of `alphabet` (the bound's alphabet
    // abstraction for rows 07, 08 and 11).
    void restrict_to(const std::span<const unsigned char> alphabet) {
        for (std::size_t index = 0; index < length_; ++index) {
            const bool member = in_alphabet(storage_[index], alphabet);
            STX_ASSUME(member);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return length_; }
    [[nodiscard]] const unsigned char* bytes() const noexcept { return storage_.get(); }
    [[nodiscard]] std::string_view view() const noexcept {
        const std::string_view result(reinterpret_cast<const char*>(storage_.get()), length_);
        return result;
    }

private:
    [[nodiscard]] static bool in_alphabet(const unsigned char byte,
                                          const std::span<const unsigned char> alphabet) noexcept {
        for (std::size_t index = 0; index < alphabet.size(); ++index) {
            if (alphabet[index] == byte) return true;
        }
        return false;
    }

    std::size_t length_{0};
    std::unique_ptr<unsigned char[]> storage_{};
};
