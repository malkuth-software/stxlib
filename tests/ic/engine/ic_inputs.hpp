#pragma once

// The inputs the block engine's generated blocks hand to STXlib's components
// (named in the declarations' [engine] tables, docs/ic_declarations.md in
// template_harness, section 8): scratch bytes and files, key sets, kept
// memory blocks with a pattern that shows an overlap, pool batches that
// count their runs, thread entries, a font built by tests/font_builder.hpp
// and FFT buffers. Every input is per slot or bounded, so a block's draw
// stays within what the inputs can realise (the blocks' `assume` lines say
// where).

#include "font_builder.hpp"
#include "stxlib/fft.hpp"
#include "stxlib/files.hpp"
#include "stxlib/font_file.hpp"
#include "stxlib/glyph_store.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/path.hpp"
#include "stxlib/stop_token.hpp"
#include "stxlib/text.hpp"
#include "stxlib/thread_pool.hpp"
#include "stxlib/threads.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace stxlib_ic::inputs {

using stxlib::Byte;
using stxlib::u32;
using stxlib::u64;
using stxlib::u8;
using stxlib::usize;

inline constexpr usize slots = 4;
inline constexpr usize scratch_capacity = 65536;

[[noreturn]] inline void unavailable(const char* const what) noexcept {
    std::fprintf(stderr, "ic engine: %s is unavailable\n", what);
    std::abort();
}

// ---- bytes -------------------------------------------------------------------------------------

inline Byte* scratch_storage() noexcept {
    static Byte storage[scratch_capacity]{};
    return storage;
}

// `count` bytes (at most scratch_capacity) to read into.
inline stxlib::MutableView<Byte> scratch_bytes(const usize count) noexcept {
    const usize kept = count < scratch_capacity ? count : scratch_capacity;
    const stxlib::MutableView<Byte> result(scratch_storage(), kept);
    return result;
}

// `count` bytes (at most scratch_capacity) to write, a counting pattern.
inline stxlib::View<Byte> scratch_view(const usize count) noexcept {
    const usize kept = count < scratch_capacity ? count : scratch_capacity;
    static Byte pattern[scratch_capacity]{};
    static bool made = false;
    if (!made) {
        for (usize index = 0; index < scratch_capacity; ++index) pattern[index] = static_cast<Byte>(index * 7U + 1U);
        made = true;
    }
    const stxlib::View<Byte> result(pattern, kept);
    return result;
}

// Every element is zero (a zeroed allocation).
template <typename Element>
inline bool all_zero(const stxlib::View<Element> elements) noexcept {
    for (usize index = 0; index < elements.count(); ++index) {
        if (elements[index] != Element{}) return false;
    }
    return true;
}

// ---- keys --------------------------------------------------------------------------------------

inline constexpr usize key_capacity = 256;

// `count` keys (at most key_capacity), strictly ascending or, when not, with
// one pair out of order (count >= 2).
inline stxlib::View<u64> keys(const usize count, const bool ascending) noexcept {
    static u64 storage[slots][key_capacity]{};
    static usize turn = 0;
    turn = (turn + 1) % slots;
    u64* const keys_of = storage[turn];
    const usize kept = count < key_capacity ? count : key_capacity;
    for (usize index = 0; index < kept; ++index) keys_of[index] = static_cast<u64>(index) * 3U + 1U;
    if (!ascending && kept >= 2) {
        const u64 first = keys_of[0];
        keys_of[0] = keys_of[1];
        keys_of[1] = first;
    }
    const stxlib::View<u64> result(keys_of, kept);
    return result;
}

inline stxlib::View<u64> values(const usize count) noexcept {
    static u64 storage[key_capacity]{};
    const usize kept = count < key_capacity ? count : key_capacity;
    for (usize index = 0; index < kept; ++index) storage[index] = static_cast<u64>(index) * 11U + 5U;
    const stxlib::View<u64> result(storage, kept);
    return result;
}

// Text keys: "k000" to "k255", ascending, or with the first two swapped.
inline stxlib::View<stxlib::TextView> text_keys(const usize count, const bool ascending) noexcept {
    static char bytes[key_capacity][4]{};
    static stxlib::TextView storage[key_capacity]{};
    static bool made = false;
    if (!made) {
        for (usize index = 0; index < key_capacity; ++index) {
            bytes[index][0] = 'k';
            bytes[index][1] = static_cast<char>('0' + index / 100);
            bytes[index][2] = static_cast<char>('0' + index / 10 % 10);
            bytes[index][3] = static_cast<char>('0' + index % 10);
        }
        made = true;
    }
    const usize kept = count < key_capacity ? count : key_capacity;
    for (usize index = 0; index < kept; ++index) {
        stxlib::OperationResult<stxlib::TextView> text = stxlib::validate_utf8(bytes[index], 4);
        if (text.failed()) unavailable("a text key");
        storage[index] = *text.value();
    }
    if (!ascending && kept >= 2) {
        const stxlib::TextView first = storage[0];
        storage[0] = storage[1];
        storage[1] = first;
    }
    const stxlib::View<stxlib::TextView> result(storage, kept);
    return result;
}

// One text key of the table text_keys draws from ("k000" to "k255"), or one
// it never holds ("k256" and on read as "kzzz").
inline stxlib::TextView text_key(const usize which) noexcept {
    static char bytes[4]{};
    const usize index = which % (key_capacity + 1);
    bytes[0] = 'k';
    if (index == key_capacity) {
        bytes[1] = 'z';
        bytes[2] = 'z';
        bytes[3] = 'z';
    } else {
        bytes[1] = static_cast<char>('0' + index / 100);
        bytes[2] = static_cast<char>('0' + index / 10 % 10);
        bytes[3] = static_cast<char>('0' + index % 10);
    }
    stxlib::OperationResult<stxlib::TextView> text = stxlib::validate_utf8(bytes, 4);
    if (text.failed()) unavailable("a text key");
    return *text.value();
}

// ---- files -------------------------------------------------------------------------------------

struct Scratch final {
    stxlib::Path directory{};
    stxlib::Path files[slots]{};
    bool made{false};
};

inline Scratch& scratch() noexcept {
    static Scratch state{};
    return state;
}

// A scratch file for each slot, in a directory of this process's own
// (TMPDIR/stxlib_ic_<number>; remove_scratch removes it).
inline const stxlib::Path& scratch_file(const usize slot) noexcept {
    Scratch& state = scratch();
    if (!state.made) {
        stxlib::OperationResult<stxlib::Path> base = stxlib::temporary_directory();
        if (base.failed()) unavailable("the temporary directory");
        char name[64]{};
        const int length = std::snprintf(name, sizeof(name), "stxlib_ic_%llx",
                                         static_cast<unsigned long long>(reinterpret_cast<stxlib::uptr>(&state)));
        stxlib::OperationResult<stxlib::TextView> text = stxlib::validate_utf8(name, static_cast<usize>(length));
        if (text.failed()) unavailable("the scratch name");
        stxlib::OperationResult<stxlib::Path> relative = stxlib::Path::parse(*text.value());
        if (relative.failed()) unavailable("the scratch name");
        stxlib::OperationResult<stxlib::Path> directory = (*base.value()).join(*relative.value());
        if (directory.failed()) unavailable("the scratch directory");
        state.directory = static_cast<stxlib::OperationResult<stxlib::Path>&&>(directory).take_value();
        static_cast<void>(stxlib::create_directory(state.directory));
        for (usize index = 0; index < slots; ++index) {
            char file_name[16]{};
            const int file_length = std::snprintf(file_name, sizeof(file_name), "file_%zu", index);
            stxlib::OperationResult<stxlib::TextView> file_text =
                stxlib::validate_utf8(file_name, static_cast<usize>(file_length));
            if (file_text.failed()) unavailable("a scratch file name");
            stxlib::OperationResult<stxlib::Path> file_relative = stxlib::Path::parse(*file_text.value());
            if (file_relative.failed()) unavailable("a scratch file name");
            stxlib::OperationResult<stxlib::Path> file = state.directory.join(*file_relative.value());
            if (file.failed()) unavailable("a scratch file");
            state.files[index] = static_cast<stxlib::OperationResult<stxlib::Path>&&>(file).take_value();
        }
        state.made = true;
    }
    return state.files[slot % slots];
}

// Removes the scratch directory, when one was made.
inline void remove_scratch() noexcept {
    Scratch& state = scratch();
    if (!state.made) return;
    static_cast<void>(stxlib::remove_tree(state.directory));
    state.made = false;
}

// ---- kept memory blocks ------------------------------------------------------------------------

inline constexpr usize block_capacity = 64;
inline constexpr usize pattern_limit = 1U << 16U;

// Blocks a component handed out, each filled with its own byte: an overlap
// or a write by the allocator into a held block breaks the pattern (intact).
// Released with Release at the end.
template <void (*Release)(void*) noexcept>
class Blocks final {
public:
    Blocks() noexcept = default;
    Blocks(const Blocks&) = delete;
    Blocks& operator=(const Blocks&) = delete;
    Blocks(Blocks&& other) noexcept { take(other); }
    Blocks& operator=(Blocks&& other) noexcept {
        if (this != &other) {
            clear();
            take(other);
        }
        return *this;
    }
    ~Blocks() { clear(); }

    // Keeps a block (when there is room; a block with no room is released)
    // and returns its address.
    void* keep(void* const address, const usize size) noexcept {
        if (address == nullptr) return nullptr;
        if (count_ == block_capacity) {
            Release(address);
            return address;
        }
        const usize filled = size < pattern_limit ? size : pattern_limit;
        const Byte tag = static_cast<Byte>(next_tag_);
        next_tag_ = next_tag_ * 5U + 3U;
        std::memset(address, tag, filled);
        addresses_[count_] = address;
        sizes_[count_] = filled;
        tags_[count_] = tag;
        count_ = count_ + 1;
        return address;
    }

    // Releases one kept block (which, modulo the count); true when there was none.
    bool release_at(const u32 which) noexcept {
        if (count_ == 0) return true;
        const usize index = which % count_;
        Release(addresses_[index]);
        count_ = count_ - 1;
        addresses_[index] = addresses_[count_];
        sizes_[index] = sizes_[count_];
        tags_[index] = tags_[count_];
        return true;
    }

    // An address inside a kept block, not its start (null when none is
    // longer than a byte).
    [[nodiscard]] void* interior(const u32 which) const noexcept {
        for (usize step = 0; step < count_; ++step) {
            const usize index = (which + step) % count_;
            if (sizes_[index] > 1) return static_cast<Byte*>(addresses_[index]) + 1;
        }
        return nullptr;
    }

    [[nodiscard]] usize count() const noexcept { return count_; }

    [[nodiscard]] bool intact() const noexcept {
        for (usize index = 0; index < count_; ++index) {
            const Byte* const bytes = static_cast<const Byte*>(addresses_[index]);
            for (usize offset = 0; offset < sizes_[index]; ++offset) {
                if (bytes[offset] != tags_[index]) return false;
            }
        }
        return true;
    }

private:
    void clear() noexcept {
        while (count_ != 0) static_cast<void>(release_at(0));
    }

    void take(Blocks& other) noexcept {
        count_ = other.count_;
        next_tag_ = other.next_tag_;
        for (usize index = 0; index < count_; ++index) {
            addresses_[index] = other.addresses_[index];
            sizes_[index] = other.sizes_[index];
            tags_[index] = other.tags_[index];
        }
        other.count_ = 0;
    }

    void* addresses_[block_capacity]{};
    usize sizes_[block_capacity]{};
    Byte tags_[block_capacity]{};
    usize count_{0};
    u32 next_tag_{0x5AU};
};

inline void release_memory(void* const address) noexcept { stxlib::memory::release(address); }

using MemoryBlocks = Blocks<&release_memory>;

// ---- pool batches ------------------------------------------------------------------------------

inline constexpr usize task_capacity = 256;

struct PoolSlot final {
    stxlib::PoolTask tasks[task_capacity]{};
    std::atomic<u64> ran{0};
    u64 expected{0};
};

inline PoolSlot& pool_slot(const usize slot) noexcept {
    static PoolSlot storage[slots]{};
    return storage[slot % slots];
}

inline void count_task(void* const argument) noexcept {
    std::atomic<u64>& ran = *static_cast<std::atomic<u64>*>(argument);
    ran.fetch_add(1, std::memory_order_relaxed);
}

// A batch of `count` tasks (at most task_capacity) that count their runs;
// with_null makes one task's function null. Only for a slot with no batch
// open (the engine's state says so). A call the pool will refuse (accepted
// false) gets a scratch batch, so the slot's last batch stays the one wait checks.
inline stxlib::View<stxlib::PoolTask> pool_tasks(const usize slot, const usize count, const bool with_null,
                                                 const bool accepted) noexcept {
    static PoolSlot scratch{};
    PoolSlot& owned = accepted ? pool_slot(slot) : scratch;
    const usize kept = count < task_capacity ? count : task_capacity;
    owned.ran.store(0, std::memory_order_relaxed);
    owned.expected = with_null ? 0 : kept;
    for (usize index = 0; index < kept; ++index) {
        owned.tasks[index].function = &count_task;
        owned.tasks[index].argument = &owned.ran;
    }
    if (with_null && kept != 0) owned.tasks[kept / 2].function = nullptr;
    const stxlib::View<stxlib::PoolTask> result(owned.tasks, kept);
    return result;
}

// Every task of the slot's last batch ran exactly once.
inline bool pool_finished(const usize slot) noexcept {
    const PoolSlot& owned = pool_slot(slot);
    const bool result = owned.ran.load(std::memory_order_acquire) == owned.expected;
    return result;
}

// ---- threads -----------------------------------------------------------------------------------

inline std::atomic<u64>& thread_counter(const usize slot) noexcept {
    static std::atomic<u64> storage[slots]{};
    return storage[slot % slots];
}

inline void thread_entry(const stxlib::StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    std::atomic<u64>& ran = *static_cast<std::atomic<u64>*>(context);
    ran.fetch_add(1, std::memory_order_release);
}

// A start the requirements refuse (accepted false) gets a scratch counter, so
// it cannot reset the counter of a thread already running in the slot.
inline void* thread_context(const usize slot, const bool accepted) noexcept {
    static std::atomic<u64> scratch{0};
    std::atomic<u64>& counter = accepted ? thread_counter(slot) : scratch;
    counter.store(0, std::memory_order_relaxed);
    return &counter;
}

inline bool thread_ran(const usize slot) noexcept {
    const bool result = thread_counter(slot).load(std::memory_order_acquire) != 0;
    return result;
}

// ---- locks -------------------------------------------------------------------------------------

// One Mutex per slot for UniqueLock's blocks. Only the slot's lock names
// it, so on the engine's one thread it is free whenever that lock is not
// locked.
inline stxlib::Mutex& slot_mutex(const usize slot) noexcept {
    static stxlib::Mutex storage[slots]{};
    return storage[slot % slots];
}

// UniqueLock::release as a caller must use it: the duty to unlock passes to
// the caller, which unlocks a mutex the lock held, so the slot's Mutex is
// free for the next lock.
inline stxlib::Mutex* release_unique_lock(stxlib::UniqueLock& lock) noexcept {
    const bool held = lock.owns();
    stxlib::Mutex* const result = lock.release();
    if (held) (*result).unlock();
    return result;
}

// ---- stop tokens -------------------------------------------------------------------------------

inline stxlib::StopSource made_stop_source(const bool stopped) noexcept {
    stxlib::OperationResult<stxlib::StopSource> made = stxlib::StopSource::create();
    if (made.failed()) unavailable("a stop source");
    stxlib::StopSource result = static_cast<stxlib::OperationResult<stxlib::StopSource>&&>(made).take_value();
    if (stopped) static_cast<void>(result.request_stop());
    return result;
}

// A token for StopCallback's blocks, by kind: 0 from a source never
// stopped (the callback is listed), 1 from a stopped source (it runs at
// once), 2 with no state (it is inert).
inline stxlib::StopToken stop_token(const u8 kind) noexcept {
    static stxlib::StopSource armed = made_stop_source(false);
    static stxlib::StopSource stopped = made_stop_source(true);
    if (kind == 0) return armed.token();
    if (kind == 1) return stopped.token();
    return stxlib::StopToken();
}

inline std::atomic<u64>& stop_counter(const usize slot) noexcept {
    static std::atomic<u64> storage[slots]{};
    return storage[slot % slots];
}

// A StopCallback's function: counts its runs in its slot's counter.
inline void stop_function(void* const context) noexcept {
    std::atomic<u64>& ran = *static_cast<std::atomic<u64>*>(context);
    ran.fetch_add(1, std::memory_order_relaxed);
}

inline void* stop_context(const usize slot) noexcept { return &stop_counter(slot); }

// ---- a font ------------------------------------------------------------------------------------

// Four glyphs: .notdef (empty), a square, a square with a hole, a bar;
// 'A' to 'C' map to glyphs 1 to 3.
inline const stxlib::glyphs::Font& font() noexcept {
    static font_builder::Bytes bytes{};
    static stxlib::glyphs::Font opened{};
    static bool made = false;
    if (!made) {
        std::vector<font_builder::Bytes> glyphs{};
        glyphs.push_back(font_builder::Bytes{});
        glyphs.push_back(
            font_builder::simple_glyph({{{50, 0, true}, {50, 500, true}, {450, 500, true}, {450, 0, true}}}));
        glyphs.push_back(font_builder::simple_glyph({{{50, 0, true}, {50, 500, true}, {450, 500, true}, {450, 0, true}},
                                                     {{150, 100, true}, {350, 100, true}, {350, 400, true},
                                                      {150, 400, true}}}));
        glyphs.push_back(
            font_builder::simple_glyph({{{200, 0, true}, {200, 700, true}, {300, 700, true}, {300, 0, true}}}));
        const std::vector<font_builder::Metric> metrics = {{500, 0}, {500, 50}, {500, 50}, {500, 200}};
        const font_builder::Bytes map = font_builder::map_format_4({{'A', 'C', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}});
        font_builder::Bytes glyf{};
        font_builder::Bytes loca{};
        font_builder::Bytes hmtx{};
        font_builder::glyph_tables(glyphs, metrics, metrics.size(), true, glyf, loca, hmtx);
        bytes = font_builder::font_of(0x00010000U, {{"OS/2", font_builder::os2_table(3, 0, 750, -200, 50, 950, 250)},
                                                     {"cmap", font_builder::cmap_table({{3, 1, map}})},
                                                     {"glyf", glyf},
                                                     {"head", font_builder::head_table(1000, true)},
                                                     {"hhea", font_builder::hhea_table(800, -220, 50, 4)},
                                                     {"hmtx", hmtx},
                                                     {"loca", loca},
                                                     {"maxp", font_builder::maxp_table(4)}});
        if (stxlib::glyphs::open_font(font_builder::view_of(bytes), 0, opened).failed()) unavailable("the test font");
        made = true;
    }
    return opened;
}

inline const stxlib::glyphs::FontLimits& font_limits() noexcept {
    static const stxlib::glyphs::FontLimits limits{};
    return limits;
}

inline stxlib::glyphs::StoredGlyph& stored_glyph() noexcept {
    static stxlib::glyphs::StoredGlyph stored{};
    return stored;
}

inline usize& kept_count() noexcept {
    static usize kept = 0;
    return kept;
}

// `count` characters (at most 16): 'A' to 'C' and others the font lacks.
inline stxlib::View<u32> characters(const usize count) noexcept {
    static const u32 storage[16] = {'A', 'B', 'C', 'D', 'A', 0x1F600U, 'C', 'B', 0, 'A', 0x10FFFFU, 'B', 'C', ' ',
                                    'A', 'Z'};
    const usize kept = count < 16 ? count : 16;
    const stxlib::View<u32> result(storage, kept);
    return result;
}

// ---- FFT buffers -------------------------------------------------------------------------------

inline constexpr usize fft_capacity = 8192;

template <typename Real>
inline Real* reals_storage() noexcept {
    static Real storage[fft_capacity]{};
    return storage;
}

template <typename Complex>
inline Complex* complex_storage() noexcept {
    static Complex storage[fft_capacity]{};
    return storage;
}

// `count` samples (at most fft_capacity): a sum of two tones.
template <typename Real>
inline stxlib::View<Real> samples(const usize count) noexcept {
    Real* const storage = reals_storage<Real>();
    const usize kept = count < fft_capacity ? count : fft_capacity;
    for (usize index = 0; index < kept; ++index) {
        const double at = static_cast<double>(index);
        storage[index] = static_cast<Real>(std::sin(at * 0.37) + 0.25 * std::cos(at * 1.9));
    }
    const stxlib::View<Real> result(storage, kept);
    return result;
}

template <typename Real>
inline stxlib::MutableView<Real> sample_output(const usize count) noexcept {
    const usize kept = count < fft_capacity ? count : fft_capacity;
    const stxlib::MutableView<Real> result(reals_storage<Real>(), kept);
    return result;
}

template <typename Complex>
inline stxlib::MutableView<Complex> spectrum_output(const usize count) noexcept {
    const usize kept = count < fft_capacity ? count : fft_capacity;
    const stxlib::MutableView<Complex> result(complex_storage<Complex>(), kept);
    return result;
}

// `count` bins (at most fft_capacity) of a small spectrum.
template <typename Complex>
inline stxlib::View<Complex> spectrum(const usize count) noexcept {
    Complex* const storage = complex_storage<Complex>();
    const usize kept = count < fft_capacity ? count : fft_capacity;
    for (usize index = 0; index < kept; ++index) {
        storage[index].re = static_cast<decltype(storage[index].re)>(1.0 / (1.0 + static_cast<double>(index)));
        storage[index].im = static_cast<decltype(storage[index].im)>(index % 3 == 0 ? 0.0 : 0.5);
    }
    const stxlib::View<Complex> result(storage, kept);
    return result;
}

// A plan's const transforms' workspace: `count` reals (at most
// work_capacity), apart from every buffer above.
inline constexpr usize work_capacity = 4 * fft_capacity;

template <typename Real>
inline stxlib::MutableView<Real> work(const usize count) noexcept {
    static Real storage[work_capacity]{};
    const usize kept = count < work_capacity ? count : work_capacity;
    const stxlib::MutableView<Real> result(storage, kept);
    return result;
}

// magnitude's output: `count` reals (at most fft_capacity), apart from the
// samples it reads.
template <typename Real>
inline Real* magnitude_storage() noexcept {
    static Real storage[fft_capacity]{};
    return storage;
}

template <typename Real>
inline stxlib::MutableView<Real> magnitude_output(const usize count) noexcept {
    const usize kept = count < fft_capacity ? count : fft_capacity;
    const stxlib::MutableView<Real> result(magnitude_storage<Real>(), kept);
    return result;
}

// The first `count` magnitudes are finite and not negative.
template <typename Real>
inline bool magnitudes_finite(const usize count) noexcept {
    const Real* const storage = magnitude_storage<Real>();
    for (usize index = 0; index < count && index < fft_capacity; ++index) {
        if (!std::isfinite(storage[index]) || storage[index] < Real{0}) return false;
    }
    return true;
}

// The first `count` outputs are finite.
template <typename Real>
inline bool samples_finite(const usize count) noexcept {
    const Real* const storage = reals_storage<Real>();
    for (usize index = 0; index < count && index < fft_capacity; ++index) {
        if (!std::isfinite(storage[index])) return false;
    }
    return true;
}

template <typename Complex>
inline bool spectrum_finite(const usize count) noexcept {
    const Complex* const storage = complex_storage<Complex>();
    for (usize index = 0; index < count && index < fft_capacity; ++index) {
        if (!std::isfinite(storage[index].re) || !std::isfinite(storage[index].im)) return false;
    }
    return true;
}

} // namespace stxlib_ic::inputs
