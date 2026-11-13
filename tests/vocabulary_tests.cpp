// Tests of the owned vocabulary: views, text, buffers, identities, handles,
// results and the error record. Tested, not proved (proofs/REPORT.md).

#include "stxlib/boundary.hpp"

#include "test_support.hpp"

#include <cstring>

namespace {

using stxlib::usize;

void views_borrow_and_bound() {
    int numbers[4] = {1, 2, 3, 4};
    const stxlib::View<int> all(numbers);
    CHECK(all.count() == 4);
    CHECK(all[2] == 3);
    const stxlib::View<int> tail = all.from(1);
    CHECK(tail.count() == 3 && tail[0] == 2);
    CHECK(all.from(4).empty());
    CHECK(all.first(0).empty());
    const stxlib::MutableView<int> writable(numbers);
    writable[0] = 10;
    CHECK(numbers[0] == 10);
    CHECK(writable.read_only()[0] == 10);
    const stxlib::View<int> none;
    CHECK(none.empty() && none.data() == nullptr);
}

void literals_and_validation() {
    const stxlib::TextView greeting = "grüße";
    CHECK(greeting.count() == 7);
    CHECK(greeting == stxlib::TextView("grüße"));
    CHECK(greeting != stxlib::TextView("gruesse"));
    static_assert(stxlib::detail::utf8_well_formed("\xe2\x82\xac", 3), "the euro sign");
    static_assert(!stxlib::detail::utf8_well_formed("\xc0\x80", 2), "an overlong NUL");
    static_assert(!stxlib::detail::utf8_well_formed("\xed\xa0\x80", 3), "a surrogate");
    static_assert(!stxlib::detail::utf8_well_formed("\xf4\x90\x80\x80", 4), "above U+10FFFF");

    const char valid[] = {'a', static_cast<char>(0xc3), static_cast<char>(0xa9)};
    const stxlib::Result<stxlib::TextView> accepted = stxlib::validate_utf8(valid, sizeof(valid));
    CHECK(accepted.succeeded());
    CHECK((*accepted.value()).count() == 3);

    const char invalid[] = {'a', 'b', static_cast<char>(0xff), 'c'};
    const stxlib::Result<stxlib::TextView> refused = stxlib::validate_utf8(invalid, sizeof(invalid));
    CHECK(refused.failed());
    CHECK((*refused.error()).kind() == stxlib::ErrorKind::invalid_argument);
    CHECK((*refused.error()).code() == 2);

    const char cut[] = {'a', static_cast<char>(0xe2), static_cast<char>(0x82)};
    const stxlib::Result<stxlib::TextView> cut_off = stxlib::validate_utf8(cut, sizeof(cut));
    CHECK(cut_off.failed() && (*cut_off.error()).code() == 1);

    CHECK(stxlib::validate_utf8(nullptr, 0).succeeded());
}

void strings_own_their_bytes() {
    char source[] = "copy me";
    const stxlib::Result<stxlib::TextView> view = stxlib::validate_utf8(source, 7);
    CHECK(view.succeeded());
    stxlib::Result<stxlib::Utf8String> copied = stxlib::Utf8String::copy_of(*view.value());
    CHECK(copied.succeeded());
    source[0] = 'X';
    const stxlib::Utf8String owned = static_cast<stxlib::Result<stxlib::Utf8String>&&>(copied).take_value();
    CHECK(owned.view() == stxlib::TextView("copy me"));
    stxlib::Result<stxlib::Utf8String> empty = stxlib::Utf8String::copy_of(stxlib::TextView());
    CHECK(empty.succeeded() && (*empty.value()).empty());
}

void buffers_zero_and_refuse() {
    stxlib::Result<stxlib::Buffer<stxlib::u32>> made = stxlib::Buffer<stxlib::u32>::allocate_zeroed(100);
    CHECK(made.succeeded());
    stxlib::Buffer<stxlib::u32> buffer = static_cast<stxlib::Result<stxlib::Buffer<stxlib::u32>>&&>(made).take_value();
    CHECK(buffer.count() == 100);
    bool all_zero = true;
    for (usize index = 0; index < buffer.count(); ++index) {
        if (buffer[index] != 0) all_zero = false;
    }
    CHECK(all_zero);
    buffer[99] = 7;
    stxlib::Buffer<stxlib::u32> moved(static_cast<stxlib::Buffer<stxlib::u32>&&>(buffer));
    CHECK(buffer.empty() && moved.count() == 100 && moved[99] == 7);
    moved.release();
    CHECK(moved.empty());

    stxlib::Result<stxlib::Buffer<stxlib::u32>> none = stxlib::Buffer<stxlib::u32>::allocate_zeroed(0);
    CHECK(none.succeeded() && (*none.value()).empty());

    stxlib::Result<stxlib::Buffer<stxlib::u64>> huge =
        stxlib::Buffer<stxlib::u64>::allocate_zeroed(stxlib::usize_max / 4);
    CHECK(huge.failed() && (*huge.error()).kind() == stxlib::ErrorKind::limit_exceeded);
}

struct WidgetTag final {};

void identities_never_wrap() {
    stxlib::IdentityIssuer<WidgetTag> issuer;
    stxlib::Result<stxlib::Identity<WidgetTag>> first = issuer.issue();
    stxlib::Result<stxlib::Identity<WidgetTag>> second = issuer.issue();
    CHECK(first.succeeded() && (*first.value()).value() == 1);
    CHECK(second.succeeded() && (*second.value()).value() == 2);
    CHECK(!stxlib::Identity<WidgetTag>().present());

    stxlib::IdentityIssuer<WidgetTag> nearly = stxlib::IdentityIssuer<WidgetTag>::resume(stxlib::u64_max - 1);
    stxlib::Result<stxlib::Identity<WidgetTag>> last = nearly.issue();
    CHECK(last.succeeded() && (*last.value()).value() == stxlib::u64_max);
    stxlib::Result<stxlib::Identity<WidgetTag>> refused = nearly.issue();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    CHECK(nearly.last_issued() == stxlib::u64_max);
}

int released_handles = 0;

struct TestHandleTraits final {
    static constexpr bool stxlib_handle_traits = true;
    using Value = int;
    static constexpr Value empty_value() noexcept { return -1; }
    static constexpr bool is_empty(const Value value) noexcept { return value < 0; }
    static void release(const Value) noexcept { released_handles = released_handles + 1; }
};

void handles_release_once() {
    released_handles = 0;
    {
        stxlib::UniqueHandle<TestHandleTraits> owner = stxlib::UniqueHandle<TestHandleTraits>::adopt(3);
        CHECK(owner.owns() && owner.get() == 3);
        stxlib::UniqueHandle<TestHandleTraits> second(static_cast<stxlib::UniqueHandle<TestHandleTraits>&&>(owner));
        CHECK(!owner.owns() && second.owns());
    }
    CHECK(released_handles == 1);
    stxlib::UniqueHandle<TestHandleTraits> given = stxlib::UniqueHandle<TestHandleTraits>::adopt(5);
    const int value = given.relinquish();
    CHECK(value == 5 && !given.owns());
    CHECK(released_handles == 1);
}

void records_cut_at_whole_sequences() {
    // 199 ASCII bytes then a two-byte sequence: the sequence does not fit
    // in 200 bytes and is dropped whole.
    char text[201]{};
    std::memset(text, 'a', 199);
    text[199] = static_cast<char>(0xc3);
    text[200] = static_cast<char>(0xa9);
    const stxlib::Result<stxlib::TextView> long_text = stxlib::validate_utf8(text, 201);
    CHECK(long_text.succeeded());
    const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(
        stxlib::ErrorKind::io_failure, "stxlib.tests.record",
        stxlib::ErrorDetail{.code = 9,
                            .foreign = stxlib::ForeignCode::of(stxlib::ForeignFamily::posix_errno, 5),
                            .message = *long_text.value()});
    CHECK(record.kind() == stxlib::ErrorKind::io_failure);
    CHECK(record.code() == 9);
    CHECK(record.foreign().present() && record.foreign().value() == 5);
    CHECK(record.message().count() == 199);
    CHECK(record.message_truncated());
    CHECK(record.operation().name() == stxlib::TextView("stxlib.tests.record"));
    CHECK(record.exception_type().empty());
    CHECK(!record.shadow().present());

    const stxlib::Result<stxlib::TextView> exact = stxlib::validate_utf8(text, 200 - 1);
    const stxlib::ErrorRecord fits = stxlib::ErrorRecord::make(stxlib::ErrorKind::io_failure, "stxlib.tests.record",
                                                               stxlib::ErrorDetail{.message = *exact.value()});
    CHECK(fits.message().count() == 199 && !fits.message_truncated());
    CHECK(fits.sequence() == record.sequence() + 1);
}

void kind_names_are_stable() {
    CHECK(stxlib::error_kind_name(stxlib::ErrorKind::invalid_argument) == stxlib::TextView("invalid_argument"));
    CHECK(stxlib::error_kind_name(stxlib::ErrorKind::internal_failure) == stxlib::TextView("internal_failure"));
    CHECK(stxlib::error_kind_name(static_cast<stxlib::ErrorKind>(999)) == stxlib::TextView("unknown"));
}

void results_hold_one_alternative() {
    const stxlib::Status good = stxlib::Status::success();
    CHECK(good.succeeded() && good.error() == nullptr);
    const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(stxlib::ErrorKind::not_found, "stxlib.tests.result");
    const stxlib::Status bad = stxlib::Status::failure(record);
    CHECK(bad.failed() && (*bad.error()).kind() == stxlib::ErrorKind::not_found);

    stxlib::Result<int> number = stxlib::Result<int>::success(42);
    CHECK(number.succeeded() && *number.value() == 42 && number.error() == nullptr);
    CHECK(number.status().succeeded());
    stxlib::Result<int> failed = stxlib::Result<int>::failure(record);
    CHECK(failed.value() == nullptr && failed.status().failed());
    stxlib::Result<int> moved(static_cast<stxlib::Result<int>&&>(failed));
    CHECK(moved.failed() && (*moved.error()).kind() == stxlib::ErrorKind::not_found);
}

} // namespace

int main() {
    views_borrow_and_bound();
    literals_and_validation();
    strings_own_their_bytes();
    buffers_zero_and_refuse();
    identities_never_wrap();
    handles_release_once();
    records_cut_at_whole_sequences();
    kind_names_are_stable();
    results_hold_one_alternative();
    const int status = stxlib_tests::finish("stxlib vocabulary");
    return status;
}
