// Tests of the owned vocabulary's text, Utf8String (owner, D15 before 1.0).
//
// What is exhaustive here, and over which domain (proofs/REPORT.md):
// - append_code_point: every u32 from 0 to 0x10FFFF, plus the values just
//   above and the top of the range; each is accepted exactly when it is a
//   scalar value, and its bytes decode back to it with an independent
//   decoder and are well-formed;
// - replace (the one editing operation behind assign, append, insert, erase
//   and truncate): every text of up to three pieces from {a, é, €, 𝄞} (one
//   to four bytes each), from three starting capacities (inline, just past
//   inline, roomy), at every offset and length from 0 to count + 1, with
//   every such text and every boundary substring of the target itself as
//   the replacement; each case is also run with its allocation refused.
//   The result is compared with a std::string model; it must be well-formed
//   and NUL-terminated, and a refused or rejected call must change nothing;
// - compare_text: every pair of those texts, against code point order.

#include "stxlib/boundary.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using stxlib::u32;
using stxlib::usize;

// ---- an independent decoder -------------------------------------------------

// Decodes well-formed UTF-8 (the caller checks well-formedness separately)
// by the bit layout of Unicode Table 3-6.
std::vector<u32> decode(const char* const bytes, const usize count) {
    std::vector<u32> result;
    usize index = 0;
    while (index < count) {
        const u32 lead = static_cast<unsigned char>(bytes[index]);
        usize length = 1;
        u32 value = lead;
        if (lead >= 0xf0u) {
            length = 4;
            value = lead & 0x07u;
        } else if (lead >= 0xe0u) {
            length = 3;
            value = lead & 0x0fu;
        } else if (lead >= 0xc0u) {
            length = 2;
            value = lead & 0x1fu;
        }
        for (usize offset = 1; offset < length && index + offset < count; ++offset) {
            const u32 next = static_cast<unsigned char>(bytes[index + offset]);
            value = (value << 6) | (next & 0x3fu);
        }
        result.push_back(value);
        index = index + length;
    }
    return result;
}

void append_code_point_exhaustive() {
    long long mismatches = 0;
    long long accepted = 0;
    stxlib::Utf8String text;
    const u32 last = 0x110010u;
    for (u32 code_point = 0; code_point <= last; ++code_point) {
        text.clear();
        const stxlib::Status status = text.append_code_point(code_point);
        const bool scalar = code_point <= 0x10ffffu && !(code_point >= 0xd800u && code_point <= 0xdfffu);
        if (status.succeeded() != scalar) {
            mismatches = mismatches + 1;
            continue;
        }
        if (!scalar) {
            if ((*status.error()).kind() != stxlib::ErrorKind::invalid_argument || !text.empty()) {
                mismatches = mismatches + 1;
            }
            continue;
        }
        accepted = accepted + 1;
        usize expected_length = 4;
        if (code_point < 0x10000u) expected_length = 3;
        if (code_point < 0x800u) expected_length = 2;
        if (code_point < 0x80u) expected_length = 1;
        const stxlib::TextView view = text.view();
        const std::vector<u32> decoded = decode(view.data(), view.count());
        const bool good = view.count() == expected_length && decoded.size() == 1 && decoded[0] == code_point &&
                          stxlib::detail::utf8_well_formed(view.data(), view.count()) &&
                          text.c_text()[view.count()] == '\0';
        if (!good) mismatches = mismatches + 1;
    }
    const u32 top[2] = {0x7fffffffu, 0xffffffffu};
    for (const u32 code_point : top) {
        text.clear();
        if (text.append_code_point(code_point).succeeded()) mismatches = mismatches + 1;
    }
    CHECK(mismatches == 0);
    CHECK(accepted == 0x110000 - 0x800);
    std::printf("append_code_point: %lld scalar values accepted, %lld mismatches\n", accepted, mismatches);
}

// ---- replace: bounded exhaustive --------------------------------------------

const char* const pieces[4] = {"a", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9d\x84\x9e"};

struct Sample final {
    std::string bytes;
    std::vector<usize> boundaries; // every sequence start, and the count
};

std::vector<Sample> samples() {
    std::vector<Sample> result;
    const Sample empty{std::string(), std::vector<usize>{0}};
    result.push_back(empty);
    for (usize length = 1; length <= 3; ++length) {
        usize total = 1;
        for (usize place = 0; place < length; ++place) total = total * 4;
        for (usize code = 0; code < total; ++code) {
            Sample sample;
            usize rest = code;
            for (usize place = 0; place < length; ++place) {
                sample.boundaries.push_back(sample.bytes.size());
                sample.bytes.append(pieces[rest % 4]);
                rest = rest / 4;
            }
            sample.boundaries.push_back(sample.bytes.size());
            result.push_back(sample);
        }
    }
    return result;
}

bool is_listed_boundary(const Sample& sample, const usize offset) {
    for (const usize boundary : sample.boundaries) {
        if (boundary == offset) return true;
    }
    return false;
}

stxlib::TextView view_of(const std::string& bytes) {
    const stxlib::Result<stxlib::TextView> valid = stxlib::validate_utf8(bytes.data(), bytes.size());
    CHECK(valid.succeeded());
    const stxlib::TextView result = *valid.value();
    return result;
}

enum class Start : unsigned char { copied, just_past_inline, roomy };

stxlib::Utf8String start_from(const Sample& sample, const Start start) {
    usize capacity = 0;
    if (start == Start::just_past_inline) capacity = stxlib::Utf8String::inline_capacity + 1;
    if (start == Start::roomy) capacity = 40;
    stxlib::Result<stxlib::Utf8String> made = stxlib::Utf8String::with_capacity(capacity);
    CHECK(made.succeeded());
    stxlib::Utf8String result = static_cast<stxlib::Result<stxlib::Utf8String>&&>(made).take_value();
    CHECK(result.assign(view_of(sample.bytes)).succeeded());
    return result;
}

bool holds(const stxlib::Utf8String& text, const std::string& model) {
    const stxlib::TextView view = text.view();
    if (view.count() != model.size()) return false;
    if (model.size() != 0 && std::memcmp(view.data(), model.data(), model.size()) != 0) return false;
    if (text.c_text()[view.count()] != '\0') return false;
    if (text.capacity() < text.count()) return false;
    const bool result = stxlib::detail::utf8_well_formed(view.data(), view.count());
    return result;
}

struct TextTally final {
    long long cases{0};
    long long refusals_checked{0};
    long long mismatches{0};
};

// One replacement case: the expected outcome from the model, the actual
// outcome granted, then the same case refused.
void replace_case(const Sample& sample, const Start start, const usize offset, const usize length,
                  const std::string& replacement, const bool from_itself, TextTally& tally) {
    const usize count = sample.bytes.size();
    const bool in_range = offset <= count && length <= count - offset;
    const bool on_boundaries =
        in_range && is_listed_boundary(sample, offset) && is_listed_boundary(sample, offset + length);
    std::string expected = sample.bytes;
    if (on_boundaries) expected = sample.bytes.substr(0, offset) + replacement + sample.bytes.substr(offset + length);

    for (int refused = 0; refused < 2; ++refused) {
        stxlib::Utf8String text = start_from(sample, start);
        const usize capacity_before = text.capacity();
        // A replacement drawn from the target borrows from it (the aliasing
        // case); otherwise it borrows from the model string.
        stxlib::TextView argument = view_of(replacement);
        if (from_itself) {
            const usize at = sample.bytes.find(replacement);
            argument = stxlib::detail::TextViewAccess::adopt_validated(text.view().data() + at, replacement.size());
        }
        if (refused == 1) stxlib::detail::arm_vocabulary_refusals(0);
        const stxlib::Status status = text.replace(offset, length, argument);
        if (refused == 1) stxlib::detail::disarm_vocabulary_refusals();
        tally.cases = tally.cases + 1;
        if (!on_boundaries) {
            const stxlib::ErrorKind wanted =
                in_range ? stxlib::ErrorKind::invalid_argument : stxlib::ErrorKind::out_of_range;
            const bool good = status.failed() && (*status.error()).kind() == wanted && holds(text, sample.bytes) &&
                              text.capacity() == capacity_before;
            if (!good) tally.mismatches = tally.mismatches + 1;
            continue;
        }
        if (status.succeeded()) {
            if (!holds(text, expected)) tally.mismatches = tally.mismatches + 1;
            continue;
        }
        // Only a refused allocation may fail here, and it changes nothing.
        tally.refusals_checked = tally.refusals_checked + 1;
        const bool good = refused == 1 && (*status.error()).kind() == stxlib::ErrorKind::out_of_memory &&
                          holds(text, sample.bytes) && text.capacity() == capacity_before;
        if (!good) tally.mismatches = tally.mismatches + 1;
    }
}

void replace_exhaustive() {
    const std::vector<Sample> all = samples();
    const Start starts[3] = {Start::copied, Start::just_past_inline, Start::roomy};
    TextTally tally;
    for (const Sample& sample : all) {
        const usize count = sample.bytes.size();
        // The target's own boundary substrings, for the aliasing cases.
        std::vector<std::string> own;
        for (usize from = 0; from < sample.boundaries.size(); ++from) {
            for (usize to = from + 1; to < sample.boundaries.size(); ++to) {
                const usize first = sample.boundaries[from];
                own.push_back(sample.bytes.substr(first, sample.boundaries[to] - first));
            }
        }
        for (const Start start : starts) {
            for (usize offset = 0; offset <= count + 1; ++offset) {
                for (usize length = 0; length <= count + 1; ++length) {
                    for (const Sample& replacement : all) {
                        replace_case(sample, start, offset, length, replacement.bytes, false, tally);
                    }
                    for (const std::string& piece : own) replace_case(sample, start, offset, length, piece, true, tally);
                }
            }
        }
    }
    CHECK(tally.mismatches == 0);
    CHECK(tally.refusals_checked > 0);
    std::printf("replace: %lld cases, %lld refusals checked, %lld mismatches\n", tally.cases, tally.refusals_checked,
                tally.mismatches);
}

// ---- compare_text -----------------------------------------------------------

int code_point_order(const std::string& left, const std::string& right) {
    const std::vector<u32> a = decode(left.data(), left.size());
    const std::vector<u32> b = decode(right.data(), right.size());
    const usize shorter = a.size() < b.size() ? a.size() : b.size();
    for (usize index = 0; index < shorter; ++index) {
        if (a[index] < b[index]) return -1;
        if (a[index] > b[index]) return 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

void compare_exhaustive() {
    const std::vector<Sample> all = samples();
    long long mismatches = 0;
    for (const Sample& left : all) {
        for (const Sample& right : all) {
            const stxlib::i32 order = stxlib::compare_text(view_of(left.bytes), view_of(right.bytes));
            if (order != code_point_order(left.bytes, right.bytes)) mismatches = mismatches + 1;
        }
    }
    CHECK(mismatches == 0);
    std::printf("compare_text: %zu pairs, %lld mismatches\n", all.size() * all.size(), mismatches);
}

// ---- moves, wrappers and limits ---------------------------------------------

void moves_and_wrappers() {
    stxlib::Result<stxlib::Utf8String> short_made = stxlib::Utf8String::copy_of("short");
    CHECK(short_made.succeeded());
    stxlib::Utf8String short_text = static_cast<stxlib::Result<stxlib::Utf8String>&&>(short_made).take_value();
    CHECK(short_text.capacity() == stxlib::Utf8String::inline_capacity);
    const stxlib::Utf8String moved_short(static_cast<stxlib::Utf8String&&>(short_text));
    CHECK(moved_short.equals("short") && short_text.empty() && short_text.c_text()[0] == '\0');

    stxlib::Result<stxlib::Utf8String> long_made = stxlib::Utf8String::copy_of("a text longer than fifteen bytes");
    CHECK(long_made.succeeded());
    stxlib::Utf8String long_text = static_cast<stxlib::Result<stxlib::Utf8String>&&>(long_made).take_value();
    CHECK(long_text.capacity() > stxlib::Utf8String::inline_capacity);
    const char* const block = long_text.c_text();
    stxlib::Utf8String moved_long;
    moved_long = static_cast<stxlib::Utf8String&&>(long_text);
    CHECK(moved_long.c_text() == block && long_text.empty());
    CHECK(long_text.capacity() == stxlib::Utf8String::inline_capacity);

    stxlib::Utf8String built;
    CHECK(built.append("grü").succeeded());
    CHECK(built.append_code_point(0x1f600u).succeeded());
    CHECK(built.insert(0, "»").succeeded());
    CHECK(built.equals("»grü\xf0\x9f\x98\x80"));
    CHECK(!built.is_boundary(1) && built.is_boundary(2));
    const stxlib::Status inside = built.insert(1, "x");
    CHECK(inside.failed() && (*inside.error()).kind() == stxlib::ErrorKind::invalid_argument);
    CHECK(built.erase(0, 2).succeeded() && built.equals("grü\xf0\x9f\x98\x80"));
    const stxlib::Status beyond = built.truncate(100);
    CHECK(beyond.failed() && (*beyond.error()).kind() == stxlib::ErrorKind::out_of_range);
    CHECK(built.truncate(4).succeeded() && built.equals("grü"));
    stxlib::Result<stxlib::Utf8String> copied = built.copy();
    CHECK(copied.succeeded() && *copied.value() == built);
    built.clear();
    CHECK(built.empty() && *copied.value() != built);
    built.release();
    CHECK(built.capacity() == stxlib::Utf8String::inline_capacity);

    stxlib::Utf8String limited;
    const stxlib::Status huge = limited.reserve(stxlib::usize_max);
    CHECK(huge.failed() && (*huge.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    stxlib::detail::arm_vocabulary_refusals(0);
    stxlib::Result<stxlib::Utf8String> refused = stxlib::Utf8String::with_capacity(64);
    stxlib::Result<stxlib::Utf8String> inline_needs_none = stxlib::Utf8String::copy_of("fits inline");
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::out_of_memory);
    CHECK(inline_needs_none.succeeded());

    // Owned text inside the owned vector: elements relocate by move.
    stxlib::Vector<stxlib::Utf8String> names;
    for (int index = 0; index < 20; ++index) {
        stxlib::Result<stxlib::Utf8String> name = stxlib::Utf8String::copy_of("a name long enough for a block");
        CHECK(name.succeeded());
        CHECK(names.push(static_cast<stxlib::Result<stxlib::Utf8String>&&>(name).take_value()).succeeded());
    }
    CHECK(names.count() == 20 && names[19].equals("a name long enough for a block"));
}

} // namespace

int main() {
    append_code_point_exhaustive();
    replace_exhaustive();
    compare_exhaustive();
    moves_and_wrappers();
    return stxlib_tests::finish("owned_text");
}
