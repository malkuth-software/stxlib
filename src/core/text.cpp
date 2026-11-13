// validate_utf8 and Utf8String (include/stxlib/text.hpp).

#include "stxlib/text.hpp"

#include "stxlib/detail/utf8.hpp"
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

Result<TextView> validate_utf8(const char* const bytes, const usize count) noexcept {
    STXLIB_REQUIRE(bytes != nullptr || count == 0, "validate_utf8: a null pointer with a non-zero count");
    if (count == 0) return Result<TextView>::success(TextView());
    const usize prefix = detail::utf8_prefix(bytes, count, count);
    if (prefix != count) {
        const u32 offset = prefix > static_cast<usize>(u32_max) ? u32_max : static_cast<u32>(prefix);
        const ErrorRecord ill_formed =
            ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.text.validate_utf8",
                              ErrorDetail{.code = offset, .message = "the bytes are not well-formed UTF-8"});
        return Result<TextView>::failure(ill_formed);
    }
    const TextView validated = detail::TextViewAccess::adopt_validated(bytes, count);
    return Result<TextView>::success(validated);
}

Result<Utf8String> Utf8String::copy_of(const TextView text) noexcept {
    Result<Buffer<char>> allocated = Buffer<char>::allocate_zeroed(text.count());
    if (allocated.failed()) return Result<Utf8String>::failure(*allocated.error());
    Utf8String result;
    result.bytes_ = static_cast<Result<Buffer<char>>&&>(allocated).take_value();
    if (!text.empty()) std::memcpy(result.bytes_.mutable_view().data(), text.data(), text.count());
    return Result<Utf8String>::success(static_cast<Utf8String&&>(result));
}

TextView Utf8String::view() const& noexcept {
    const View<char> bytes = bytes_.view();
    const TextView result = detail::TextViewAccess::adopt_validated(bytes.data(), bytes.count());
    return result;
}

} // namespace stxlib
