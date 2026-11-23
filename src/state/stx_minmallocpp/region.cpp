// stx_minmallocpp: regions, the free-run index and huge mappings
// (region.hpp states the structure and the bounds).
//
// Every function here that touches Structure runs with Structure::lock
// held by its caller, except allocate_huge and release_huge, which take it
// themselves. None calls anything but the host layer (os.hpp), which cannot
// unwind or be cancelled, so each leaves the tables whole.

#include "state/stx_minmallocpp/region.hpp"

namespace stxlib::stx_minmallocpp {

constinit u32 directory[directory_cells]{};
constinit Structure structure{};

namespace {

// ---- the run tables of one region --------------------------------------------------

[[nodiscard]] u16* run_lengths(const uptr base) noexcept {
    u16* const result = reinterpret_cast<u16*>(base + run_length_slice * slice_bytes);
    return result;
}

[[nodiscard]] u32* run_nexts(const uptr base) noexcept {
    u32* const result = reinterpret_cast<u32*>(base + run_next_slice * slice_bytes);
    return result;
}

[[nodiscard]] u32* run_previouses(const uptr base) noexcept {
    u32* const result = reinterpret_cast<u32*>(base + run_previous_slice * slice_bytes);
    return result;
}

// The region and the first slice a run's name stands for.
[[nodiscard]] uptr base_of_run(const u32 name) noexcept {
    const uptr result = structure.region_bases[name >> run_slice_bits];
    return result;
}

[[nodiscard]] usize slice_of_run(const u32 name) noexcept {
    const usize result = static_cast<usize>(name) & (region_slices - 1);
    return result;
}

// Sets the run that follows, or precedes, the free run `name` in its list.
void set_run_next(const u32 name, const u32 following) noexcept {
    u32* const nexts = run_nexts(base_of_run(name));
    nexts[slice_of_run(name)] = following;
}

void set_run_previous(const u32 name, const u32 preceding) noexcept {
    u32* const previouses = run_previouses(base_of_run(name));
    previouses[slice_of_run(name)] = preceding;
}

// ---- the index -------------------------------------------------------------------

// Files the free run [slice, slice + length) of the region at `base`:
// writes its two boundary tags and puts it first in its list.
void insert_run(const uptr base, const usize slice, const usize length) noexcept {
    STX_MINMALLOCPP_CHECK(slice >= first_data_slice && length >= 1 && slice + length <= region_slices);
    u16* const lengths = run_lengths(base);
    lengths[slice] = static_cast<u16>(length);
    lengths[slice + length - 1] = static_cast<u16>(length);
    const RunList list = run_list_of(length);
    const u32 name = (header_of(base).index << run_slice_bits) | static_cast<u32>(slice);
    const u32 following = structure.run_heads[list.first][list.second];
    set_run_next(name, following);
    set_run_previous(name, no_run);
    if (following != no_run) set_run_previous(following, name);
    structure.run_heads[list.first][list.second] = name;
    structure.run_second_level[list.first] =
        static_cast<u8>(structure.run_second_level[list.first] | (static_cast<u32>(1) << list.second));
    structure.run_first_level = structure.run_first_level | (static_cast<u32>(1) << list.first);
}

// Takes the free run [slice, slice + length) out of its list. Its tags are
// left for the caller to overwrite.
void remove_run(const uptr base, const usize slice, const usize length) noexcept {
    STX_MINMALLOCPP_CHECK(run_lengths(base)[slice] == length && run_lengths(base)[slice + length - 1] == length);
    const RunList list = run_list_of(length);
    const u32 before = run_previouses(base)[slice];
    const u32 after = run_nexts(base)[slice];
    if (before != no_run) {
        set_run_next(before, after);
    } else {
        structure.run_heads[list.first][list.second] = after;
    }
    if (after != no_run) set_run_previous(after, before);
    if (structure.run_heads[list.first][list.second] != no_run) return;
    structure.run_second_level[list.first] =
        static_cast<u8>(structure.run_second_level[list.first] & ~(static_cast<u32>(1) << list.second));
    if (structure.run_second_level[list.first] != 0) return;
    structure.run_first_level = structure.run_first_level & ~(static_cast<u32>(1) << list.first);
}

// The first non-empty list all of whose runs have at least `need` slices;
// first == run_first_levels when there is none. Requires
// 1 <= need < 2^14. No list is walked: two bitmap tests.
[[nodiscard]] RunList fitting_list(const usize need) noexcept {
    STX_MINMALLOCPP_CHECK(need >= 1 && need < region_slices);
    const RunList none{.first = run_first_levels, .second = 0};
    RunList list = run_list_of(need);
    if (run_list_floor(list) < need) {
        list.second = list.second + 1;
        if (list.second == run_second_levels) {
            list.first = list.first + 1;
            list.second = 0;
        }
    }
    if (list.first >= run_first_levels) return none;
    const u32 at_or_above = static_cast<u32>(0xf) << list.second;
    const u32 seconds = static_cast<u32>(structure.run_second_level[list.first]) & at_or_above;
    if (seconds != 0) {
        list.second = static_cast<usize>(std::countr_zero(seconds));
        return list;
    }
    const u32 through = (static_cast<u32>(2) << list.first) - 1;
    const u32 firsts = structure.run_first_level & ~through;
    if (firsts == 0) return none;
    list.first = static_cast<usize>(std::countr_zero(firsts));
    list.second = static_cast<usize>(std::countr_zero(static_cast<u32>(structure.run_second_level[list.first])));
    return list;
}

// Files the run [slice, slice + count), joined with the free runs that end
// just before it and start just after it (the boundary tags say so).
void coalesce_and_insert(const uptr base, const usize slice, const usize count) noexcept {
    u16* const lengths = run_lengths(base);
    usize first = slice;
    usize length = count;
    if (first > first_data_slice && lengths[first - 1] != 0) {
        const usize before = lengths[first - 1];
        remove_run(base, first - before, before);
        first = first - before;
        length = length + before;
    }
    const usize end = slice + count;
    if (end < region_slices && lengths[end] != 0) {
        const usize after = lengths[end];
        remove_run(base, end, after);
        length = length + after;
    }
    insert_run(base, first, length);
}

// ---- commit units ----------------------------------------------------------------

// `address` rounded down, and up, to the host's page size.
[[nodiscard]] uptr floor_to_page(const uptr address) noexcept {
    const uptr result = address & ~static_cast<uptr>(structure.os_page - 1);
    return result;
}

[[nodiscard]] uptr ceiling_to_page(const uptr address) noexcept {
    const uptr mask = static_cast<uptr>(structure.os_page - 1);
    const uptr result = (address + mask) & ~mask;
    return result;
}

// Commits the data of `count` consecutive units from `unit`, and the free
// and remote words of their slices: three host calls. The word ranges are
// rounded outward to host pages; they stay inside their tables, which
// start and end on slices. A range that is already committed is committed
// again, which changes nothing.
[[nodiscard]] bool commit_units(const uptr base, const usize unit, const usize count) noexcept {
    const usize words_bytes = unit_slices * page_words * sizeof(u64);
    const uptr free_first = base + free_words_slice * slice_bytes + unit * words_bytes;
    const uptr remote_first = base + remote_words_slice * slice_bytes + unit * words_bytes;
    const uptr free_start = floor_to_page(free_first);
    const uptr remote_start = floor_to_page(remote_first);
    const usize free_length = static_cast<usize>(ceiling_to_page(free_first + count * words_bytes) - free_start);
    const usize remote_length = static_cast<usize>(ceiling_to_page(remote_first + count * words_bytes) - remote_start);
    if (!os::commit(reinterpret_cast<void*>(free_start), free_length)) return false;
    if (!os::commit(reinterpret_cast<void*>(remote_start), remote_length)) return false;
    const bool data = os::commit(reinterpret_cast<void*>(base + unit * unit_bytes), count * unit_bytes);
    return data;
}

// Whether the unit's data is committed.
[[nodiscard]] bool unit_committed(const RegionHeader& header, const usize unit) noexcept {
    const u64 word = header.unit_committed[unit / word_bits];
    const bool result = ((word >> (unit % word_bits)) & 1) != 0;
    return result;
}

void set_unit_committed(RegionHeader& header, const usize unit, const bool committed) noexcept {
    const u64 bit = static_cast<u64>(1) << (unit % word_bits);
    const u64 word = header.unit_committed[unit / word_bits];
    header.unit_committed[unit / word_bits] = committed ? (word | bit) : (word & ~bit);
}

// The slices of [first, end) that lie in `unit`.
[[nodiscard]] usize slices_in_unit(const usize first, const usize end, const usize unit) noexcept {
    const usize unit_first = unit * unit_slices;
    const usize unit_end = unit_first + unit_slices;
    const usize from = first > unit_first ? first : unit_first;
    const usize to = end < unit_end ? end : unit_end;
    const usize result = to - from;
    return result;
}

// Commits what [first, first + count) needs and counts its slices as used.
// Each contiguous range of uncommitted units is committed in one step. On a
// refused commit nothing is counted; ranges committed before the refusal
// stay committed and idle.
[[nodiscard]] bool commit_run(const uptr base, const usize first, const usize count) noexcept {
    RegionHeader& header = header_of(base);
    const usize end = first + count;
    const usize first_unit = first / unit_slices;
    const usize last_unit = (end - 1) / unit_slices;
    usize unit = first_unit;
    while (unit <= last_unit) {
        if (unit_committed(header, unit)) {
            unit = unit + 1;
            continue;
        }
        usize range_end = unit + 1;
        while (range_end <= last_unit && !unit_committed(header, range_end)) range_end = range_end + 1;
        if (!commit_units(base, unit, range_end - unit)) return false;
        for (usize committed = unit; committed < range_end; ++committed) {
            set_unit_committed(header, committed, true);
            header.idle_units = header.idle_units + 1;
        }
        unit = range_end;
    }
    for (usize used_unit = first_unit; used_unit <= last_unit; ++used_unit) {
        if (header.unit_used[used_unit] == 0) header.idle_units = header.idle_units - 1;
        const usize used = static_cast<usize>(header.unit_used[used_unit]) + slices_in_unit(first, end, used_unit);
        STX_MINMALLOCPP_CHECK(used <= unit_slices);
        header.unit_used[used_unit] = static_cast<u8>(used);
    }
    return true;
}

// ---- regions ----------------------------------------------------------------------

// Reserves a region, commits its eager metadata, enters it in the
// directory and files its data as one free run. One reservation (several
// host calls inside os::reserve) and one commit.
[[nodiscard]] bool add_region() noexcept {
    if (structure.region_count == region_capacity) return false;
    void* const reserved = os::reserve(region_bytes, region_bytes);
    if (reserved == nullptr) return false;
    const uptr base = reinterpret_cast<uptr>(reserved);
    const bool below_limit = (base >> address_bits) == 0 && ((base + region_bytes - 1) >> address_bits) == 0;
    if (!below_limit || !os::commit(reserved, eager_meta_slices * slice_bytes)) {
        os::release(reserved, region_bytes);
        return false;
    }
    RegionHeader& header = header_of(base);
    header.index = static_cast<u32>(structure.region_count);
    structure.region_bases[structure.region_count] = base;
    structure.region_count = structure.region_count + 1;
    insert_run(base, first_data_slice, region_slices - first_data_slice);
    set_directory_cell(static_cast<usize>(base >> region_shift), cell_region);
    return true;
}

} // namespace

// ---- taking and giving runs ---------------------------------------------------------

RunPlace take_run(const usize count, const usize alignment_slices) noexcept {
    STX_MINMALLOCPP_CHECK(count >= 1 && std::has_single_bit(alignment_slices));
    const RunPlace none{};
    const usize need = count + alignment_slices - 1;
    RunList list = fitting_list(need);
    if (list.first == run_first_levels) {
        if (!add_region()) return none;
        list = fitting_list(need);
        if (list.first == run_first_levels) return none;
    }
    const u32 name = structure.run_heads[list.first][list.second];
    const uptr base = base_of_run(name);
    const usize start = slice_of_run(name);
    u16* const lengths = run_lengths(base);
    const usize length = lengths[start];
    remove_run(base, start, length);
    const usize first = (start + alignment_slices - 1) & ~(alignment_slices - 1);
    const usize taken_end = first + count;
    const usize end = start + length;
    STX_MINMALLOCPP_CHECK(taken_end <= end);
    // An allocated run's two boundary tags are 0 (region.hpp).
    lengths[first] = 0;
    lengths[taken_end - 1] = 0;
    if (first > start) insert_run(base, start, first - start);
    if (end > taken_end) insert_run(base, taken_end, end - taken_end);
    if (!commit_run(base, first, count)) {
        coalesce_and_insert(base, first, count);
        return none;
    }
    const RunPlace result{.base = base, .slice = first};
    return result;
}

void give_run(const uptr base, const usize slice, const usize count) noexcept {
    STX_MINMALLOCPP_CHECK(slice >= first_data_slice && count >= 1 && slice + count <= region_slices);
    RegionHeader& header = header_of(base);
    const usize end = slice + count;
    const usize first_unit = slice / unit_slices;
    const usize last_unit = (end - 1) / unit_slices;
    // Units that go idle past the limit are decommitted, each contiguous
    // range of them in one host call.
    usize range_first = 0;
    usize range_count = 0;
    for (usize unit = first_unit; unit <= last_unit; ++unit) {
        const usize in_unit = slices_in_unit(slice, end, unit);
        STX_MINMALLOCPP_CHECK(header.unit_used[unit] >= in_unit);
        const usize used = static_cast<usize>(header.unit_used[unit]) - in_unit;
        header.unit_used[unit] = static_cast<u8>(used);
        bool drop = false;
        if (used == 0) {
            if (header.idle_units < idle_unit_limit) {
                header.idle_units = header.idle_units + 1;
            } else {
                drop = true;
            }
        }
        if (drop) {
            if (range_count == 0) range_first = unit;
            range_count = range_count + 1;
            set_unit_committed(header, unit, false);
            continue;
        }
        if (range_count != 0) {
            os::decommit(reinterpret_cast<void*>(base + range_first * unit_bytes), range_count * unit_bytes);
            range_count = 0;
        }
    }
    if (range_count != 0) {
        os::decommit(reinterpret_cast<void*>(base + range_first * unit_bytes), range_count * unit_bytes);
    }
    coalesce_and_insert(base, slice, count);
}

void decommit_idle_units() noexcept {
    for (usize region = 0; region < structure.region_count; ++region) {
        const uptr base = structure.region_bases[region];
        RegionHeader& header = header_of(base);
        if (header.idle_units == 0) continue;
        for (usize unit = first_data_slice / unit_slices; unit < region_units; ++unit) {
            if (header.unit_used[unit] != 0 || !unit_committed(header, unit)) continue;
            os::decommit(reinterpret_cast<void*>(base + unit * unit_bytes), unit_bytes);
            set_unit_committed(header, unit, false);
        }
        header.idle_units = 0;
    }
}

// ---- huge mappings -------------------------------------------------------------------

namespace {

// The index of a free huge record, taken; huge_capacity when all are in
// use. Takes the lock.
[[nodiscard]] usize take_huge_record() noexcept {
    const LockScope scope(structure.lock);
    if (structure.free_huge_count != 0) {
        structure.free_huge_count = structure.free_huge_count - 1;
        const usize reused = structure.free_huge_records[structure.free_huge_count];
        return reused;
    }
    if (structure.huge_made == huge_capacity) return huge_capacity;
    const usize made = structure.huge_made;
    structure.huge_made = made + 1;
    return made;
}

void give_huge_record(const usize index) noexcept {
    const LockScope scope(structure.lock);
    structure.free_huge_records[structure.free_huge_count] = static_cast<u16>(index);
    structure.free_huge_count = structure.free_huge_count + 1;
}

// The bytes a huge mapping reserves for `committed` bytes (region.hpp,
// "huge mappings"): whole regions where addresses are wide, where address
// space costs nothing until committed; the committed bytes themselves on
// wasm32, where every reserved byte is linear memory.
[[nodiscard]] constexpr usize huge_reservation(const usize committed) noexcept {
    if constexpr (wide_addresses) return round_up(committed, region_bytes);
    return committed;
}

// The directory cells a huge mapping of `reserved` bytes touches; its last
// cell is partly its own on wasm32.
[[nodiscard]] constexpr usize huge_cells(const usize reserved) noexcept {
    if constexpr (wide_addresses) return reserved >> region_shift;
    return round_up(reserved, region_bytes) >> region_shift;
}

} // namespace

void* allocate_huge(const usize bytes) noexcept {
    // The record first, so that the host calls are made outside the lock
    // and only when the mapping can be recorded.
    const usize index = take_huge_record();
    if (index == huge_capacity) return nullptr;
    const usize committed = round_up(bytes, structure.os_page);
    const usize reserved = huge_reservation(committed);
    void* const block = os::reserve(reserved, region_bytes);
    if (block == nullptr) {
        give_huge_record(index);
        return nullptr;
    }
    const uptr address = reinterpret_cast<uptr>(block);
    const bool below_limit = (address >> address_bits) == 0 && ((address + reserved - 1) >> address_bits) == 0;
    if (!below_limit || !os::commit(block, committed)) {
        os::release(block, reserved);
        give_huge_record(index);
        return nullptr;
    }
    HugeRecord& record = structure.huge[index];
    record.reserved_bytes = reserved;
    record.block.store(address);
    const usize first_cell = static_cast<usize>(address >> region_shift);
    const usize cells = huge_cells(reserved);
    for (usize cell = 0; cell < cells; ++cell) {
        set_directory_cell(first_cell + cell, cell_huge_first + static_cast<u32>(index));
    }
    return block;
}

bool release_huge(const u32 cell, const uptr address) noexcept {
    const usize index = static_cast<usize>(cell - cell_huge_first);
    if (index >= huge_capacity) return false;
    HugeRecord& record = structure.huge[index];
    // Taking the address out of the record is the release: a second one, or
    // a release of any other address in the mapping, finds no match.
    uptr expected = address;
    if (address == 0 || !record.block.compare_exchange_strong(expected, 0)) return false;
    const usize reserved = record.reserved_bytes;
    const usize first_cell = static_cast<usize>(address >> region_shift);
    const usize cells = huge_cells(reserved);
    for (usize cell_index = 0; cell_index < cells; ++cell_index) {
        set_directory_cell(first_cell + cell_index, cell_none);
    }
    os::release(reinterpret_cast<void*>(address), reserved);
    give_huge_record(index);
    return true;
}

} // namespace stxlib::stx_minmallocpp
