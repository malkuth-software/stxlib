// validate_utf8 and Utf8String (include/stxlib/text.hpp).

#include "stxlib/text.hpp"

#include "stxlib/detail/utf8.hpp"
#include "stxlib/detail/vocabulary_allocation.hpp"
#include "stxlib/detail/growth.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/require.hpp"

#include <cstring>

namespace stxlib {

namespace detail {

// Only ever called during constant evaluation, where the call itself is the
// compile error; defined so that no build can reach a missing symbol.
void literal_is_not_utf8() noexcept {
    requirement_failed("a TextView literal is not well-formed UTF-8", __FILE__, __LINE__);
}

} // namespace detail

OperationResult<TextView> validate_utf8(const char* const bytes, const usize count) noexcept {
    STXLIB_REQUIRE(bytes != nullptr || count == 0, "validate_utf8: a null pointer with a non-zero count");
    if (count == 0) return OperationResult<TextView>::success(TextView());
    const usize prefix = detail::utf8_prefix(bytes, count, count);
    if (prefix != count) {
        const u32 offset = prefix > static_cast<usize>(u32_max) ? u32_max : static_cast<u32>(prefix);
        const ErrorRecord ill_formed =
            ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.text.validate_utf8",
                              ErrorDetail{.code = offset, .message = "the bytes are not well-formed UTF-8"});
        return OperationResult<TextView>::failure(ill_formed);
    }
    const TextView validated = detail::TextViewAccess::adopt_validated(bytes, count);
    return OperationResult<TextView>::success(validated);
}

// ---- Utf8String ------------------------------------------------------------

namespace {

ErrorRecord text_too_long(const Operation operation) noexcept {
    const ErrorRecord result = ErrorRecord::make(ErrorKind::limit_exceeded, operation,
                                                 ErrorDetail{.message = "the text length is not representable"});
    return result;
}

ErrorRecord text_exhausted(const Operation operation) noexcept {
    const ErrorRecord result = ErrorRecord::make(ErrorKind::out_of_memory, operation);
    return result;
}

// A byte that continues a sequence (10xxxxxx). In well-formed text every
// other byte starts one.
bool continues_sequence(const char byte) noexcept {
    const u8 value = static_cast<u8>(byte);
    const bool result = (value & 0xc0u) == 0x80u;
    return result;
}

// True when `pointer` lies in [first, first + count). Compared as addresses,
// since the two may belong to different objects.
bool points_into(const char* const pointer, const char* const first, const usize count) noexcept {
    const uptr address = reinterpret_cast<uptr>(pointer);
    const uptr start = reinterpret_cast<uptr>(first);
    const bool result = address >= start && address - start < count;
    return result;
}

// Writes prefix, middle and suffix one after another into `target` and the
// NUL after them. The sources may lie anywhere except in target's range.
void join_into(char* const target, const TextView prefix, const TextView middle, const TextView suffix) noexcept {
    usize at = 0;
    if (!prefix.empty()) std::memcpy(target, prefix.data(), prefix.count());
    at = at + prefix.count();
    if (!middle.empty()) std::memcpy(target + at, middle.data(), middle.count());
    at = at + middle.count();
    if (!suffix.empty()) std::memcpy(target + at, suffix.data(), suffix.count());
    at = at + suffix.count();
    target[at] = '\0';
}

} // namespace

Utf8String::Utf8String(Utf8String&& other) noexcept : count_(other.count_), capacity_(other.capacity_), storage_() {
    if (other.on_heap()) {
        storage_.block = other.storage_.block;
    } else {
        std::memcpy(storage_.inline_bytes, other.storage_.inline_bytes, inline_capacity + 1);
    }
    other.count_ = 0;
    other.capacity_ = inline_capacity;
    std::memset(other.storage_.inline_bytes, 0, inline_capacity + 1);
}

Utf8String::Utf8String(const Utf8String& other) noexcept : Utf8String(take_or_fail_fast(other.copy())) {}

Utf8String& Utf8String::operator=(const Utf8String& other) noexcept {
    if (this == &other) return *this;
    Utf8String made = take_or_fail_fast(other.copy());
    *this = static_cast<Utf8String&&>(made);
    return *this;
}

Utf8String& Utf8String::operator=(Utf8String&& other) noexcept {
    if (this == &other) return *this;
    release();
    count_ = other.count_;
    capacity_ = other.capacity_;
    if (other.on_heap()) {
        storage_.block = other.storage_.block;
    } else {
        std::memcpy(storage_.inline_bytes, other.storage_.inline_bytes, inline_capacity + 1);
    }
    other.count_ = 0;
    other.capacity_ = inline_capacity;
    std::memset(other.storage_.inline_bytes, 0, inline_capacity + 1);
    return *this;
}

Utf8String::~Utf8String() { release(); }

char* Utf8String::bytes() noexcept {
    if (on_heap()) return storage_.block;
    return storage_.inline_bytes;
}

const char* Utf8String::bytes() const noexcept {
    if (on_heap()) return storage_.block;
    return storage_.inline_bytes;
}

OperationResult<Utf8String> Utf8String::with_capacity(const usize room) noexcept {
    Utf8String result;
    const OperationStatus reserved = result.reserve(room);
    if (reserved.failed()) return OperationResult<Utf8String>::failure(*reserved.error());
    return OperationResult<Utf8String>::success(static_cast<Utf8String&&>(result));
}

OperationResult<Utf8String> Utf8String::copy_of(const TextView text) noexcept {
    Utf8String result;
    const OperationStatus assigned = result.replace_checked(0, 0, text, "stxlib.text.copy_of");
    if (assigned.failed()) return OperationResult<Utf8String>::failure(*assigned.error());
    return OperationResult<Utf8String>::success(static_cast<Utf8String&&>(result));
}

OperationResult<Utf8String> Utf8String::copy() const noexcept {
    OperationResult<Utf8String> result = copy_of(view());
    return result;
}

TextView Utf8String::view() const& noexcept {
    const TextView result = detail::TextViewAccess::adopt_validated(bytes(), count_);
    return result;
}

const char* Utf8String::c_text() const& noexcept { return bytes(); }

bool Utf8String::is_boundary(const usize offset) const noexcept {
    if (offset > count_) return false;
    if (offset == count_) return true;
    const bool continuation = continues_sequence(bytes()[offset]);
    return !continuation;
}

bool Utf8String::equals(const TextView text) const noexcept {
    const bool result = view() == text;
    return result;
}

bool Utf8String::operator==(const Utf8String& other) const noexcept {
    const bool result = view() == other.view();
    return result;
}

bool Utf8String::operator!=(const Utf8String& other) const noexcept {
    const bool equal = *this == other;
    return !equal;
}

OperationStatus Utf8String::reserve(const usize wanted) noexcept {
    if (wanted <= capacity_) return OperationStatus::success();
    if (wanted == usize_max) return OperationStatus::failure(text_too_long("stxlib.text.reserve"));
    const usize next = detail::grown_capacity<usize>(capacity_, wanted, 1);
    void* const block = detail::vocabulary_allocate(next + 1, 1);
    if (block == nullptr) return OperationStatus::failure(text_exhausted("stxlib.text.reserve"));
    char* const target = static_cast<char*>(block);
    std::memcpy(target, bytes(), count_ + 1);
    if (on_heap()) detail::vocabulary_release(storage_.block);
    storage_.block = target;
    capacity_ = next;
    return OperationStatus::success();
}

OperationStatus Utf8String::assign(const TextView text) noexcept {
    const OperationStatus result = replace_checked(0, count_, text, "stxlib.text.assign");
    return result;
}

OperationStatus Utf8String::append(const TextView text) noexcept {
    const OperationStatus result = replace_checked(count_, 0, text, "stxlib.text.append");
    return result;
}

OperationStatus Utf8String::append_code_point(const u32 code_point) noexcept {
    const bool surrogate = code_point >= 0xd800u && code_point <= 0xdfffu;
    if (surrogate || code_point > 0x10ffffu) {
        const ErrorRecord refused =
            ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.text.append_code_point",
                              ErrorDetail{.code = code_point, .message = "not a Unicode scalar value"});
        return OperationStatus::failure(refused);
    }
    // Unicode section 3.9, Table 3-6 (UTF-8 bit distribution).
    char encoded[4] = {0, 0, 0, 0};
    usize length = 0;
    if (code_point <= 0x7fu) {
        encoded[0] = static_cast<char>(code_point);
        length = 1;
    } else if (code_point <= 0x7ffu) {
        encoded[0] = static_cast<char>(0xc0u | (code_point >> 6));
        encoded[1] = static_cast<char>(0x80u | (code_point & 0x3fu));
        length = 2;
    } else if (code_point <= 0xffffu) {
        encoded[0] = static_cast<char>(0xe0u | (code_point >> 12));
        encoded[1] = static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu));
        encoded[2] = static_cast<char>(0x80u | (code_point & 0x3fu));
        length = 3;
    } else {
        encoded[0] = static_cast<char>(0xf0u | (code_point >> 18));
        encoded[1] = static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu));
        encoded[2] = static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu));
        encoded[3] = static_cast<char>(0x80u | (code_point & 0x3fu));
        length = 4;
    }
    // A scalar value's encoding is well-formed by Table 3-6 (checked for
    // every scalar value by tests/owned_text_exhaustive.cpp).
    const TextView piece = detail::TextViewAccess::adopt_validated(encoded, length);
    const OperationStatus result = replace_checked(count_, 0, piece, "stxlib.text.append_code_point");
    return result;
}

OperationStatus Utf8String::insert(const usize offset, const TextView text) noexcept {
    const OperationStatus result = replace_checked(offset, 0, text, "stxlib.text.insert");
    return result;
}

OperationStatus Utf8String::erase(const usize offset, const usize length) noexcept {
    const OperationStatus result = replace_checked(offset, length, TextView(), "stxlib.text.erase");
    return result;
}

OperationStatus Utf8String::replace(const usize offset, const usize length, const TextView text) noexcept {
    const OperationStatus result = replace_checked(offset, length, text, "stxlib.text.replace");
    return result;
}

OperationStatus Utf8String::truncate(const usize length) noexcept {
    if (length > count_) {
        const ErrorRecord refused = ErrorRecord::make(ErrorKind::out_of_range, "stxlib.text.truncate");
        return OperationStatus::failure(refused);
    }
    const OperationStatus result = replace_checked(length, count_ - length, TextView(), "stxlib.text.truncate");
    return result;
}

void Utf8String::clear() noexcept {
    count_ = 0;
    bytes()[0] = '\0';
}

void Utf8String::release() noexcept {
    if (on_heap()) detail::vocabulary_release(storage_.block);
    count_ = 0;
    capacity_ = inline_capacity;
    std::memset(storage_.inline_bytes, 0, inline_capacity + 1);
}

// The one editing operation: every change of the text is a replacement of
// one boundary-delimited range by a well-formed text.
OperationStatus Utf8String::replace_checked(const usize offset, const usize length, const TextView text,
                                   const Operation operation) noexcept {
    // Validate before mutating (house style, section 2).
    if (offset > count_ || length > count_ - offset) {
        const ErrorRecord refused = ErrorRecord::make(ErrorKind::out_of_range, operation,
                                                      ErrorDetail{.message = "the range is outside the text"});
        return OperationStatus::failure(refused);
    }
    const usize end = offset + length;
    if (!is_boundary(offset) || !is_boundary(end)) {
        const ErrorRecord refused = ErrorRecord::make(ErrorKind::invalid_argument, operation,
                                                      ErrorDetail{.message = "an offset is inside a UTF-8 sequence"});
        return OperationStatus::failure(refused);
    }
    const usize kept = count_ - length;
    // The new count and its NUL must be representable.
    if (text.count() > usize_max - 1 - kept) return OperationStatus::failure(text_too_long(operation));
    const usize new_count = kept + text.count();

    char* const current = bytes();
    const TextView prefix = detail::TextViewAccess::adopt_validated(current, offset);
    const TextView suffix = detail::TextViewAccess::adopt_validated(current + end, count_ - end);
    const bool aliased = !text.empty() && points_into(text.data(), current, capacity_ + 1);

    if (new_count <= capacity_ && !aliased) {
        // In place: move the suffix to its new position (the ranges may
        // overlap, hence memmove), then write the text. The text lies
        // outside this string, so neither step disturbs it.
        if (!suffix.empty()) std::memmove(current + offset + text.count(), current + end, suffix.count());
        if (!text.empty()) std::memcpy(current + offset, text.data(), text.count());
        current[new_count] = '\0';
        count_ = new_count;
        return OperationStatus::success();
    }

    if (new_count <= capacity_) {
        // In place, but the text borrows from this string: build the result
        // in separate storage first (on the stack when it fits inline), then
        // copy it back; capacity is kept.
        char local[inline_capacity + 1] = {};
        char* scratch = local;
        void* scratch_block = nullptr;
        if (new_count > inline_capacity) {
            scratch_block = detail::vocabulary_allocate(new_count + 1, 1);
            if (scratch_block == nullptr) return OperationStatus::failure(text_exhausted(operation));
            scratch = static_cast<char*>(scratch_block);
        }
        join_into(scratch, prefix, text, suffix);
        std::memcpy(current, scratch, new_count + 1);
        detail::vocabulary_release(scratch_block);
        count_ = new_count;
        return OperationStatus::success();
    }

    // Growth: a new block, built while the old storage (and any text
    // borrowed from it) is still alive, then adopted.
    const usize next = detail::grown_capacity<usize>(capacity_, new_count, 1);
    void* const block = detail::vocabulary_allocate(next + 1, 1);
    if (block == nullptr) return OperationStatus::failure(text_exhausted(operation));
    char* const target = static_cast<char*>(block);
    join_into(target, prefix, text, suffix);
    if (on_heap()) detail::vocabulary_release(storage_.block);
    storage_.block = target;
    capacity_ = next;
    count_ = new_count;
    return OperationStatus::success();
}

i32 compare_text(const TextView left, const TextView right) noexcept {
    const usize shorter = left.count() < right.count() ? left.count() : right.count();
    if (shorter != 0) {
        const int order = std::memcmp(left.data(), right.data(), shorter);
        if (order < 0) return -1;
        if (order > 0) return 1;
    }
    if (left.count() < right.count()) return -1;
    if (left.count() > right.count()) return 1;
    return 0;
}

} // namespace stxlib
