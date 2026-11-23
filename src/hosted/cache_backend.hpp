#pragma once

// The machine's data caches, as each family's cache module reports them
// (macos/cache_macos.cpp, linux/cache_linux.cpp; cache_default.cpp for the
// families that report nothing); exactly one is linked. stx_fft's walk lays
// out its work buffers by them (src/algorithms/fft/walk_plan.cpp): the block
// size of its depth-first passes and the gaps that keep runs a power of two
// apart out of the same cache sets. The layout changes the order of
// independent work and the places of values in the workspace, never a
// computed value, so a host that reports nothing, or reports differently,
// changes speed only.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/types.hpp"

namespace stxlib::cache_backend {

// Sizes in bytes. A field the host does not report keeps the value it had
// when the call was made; the caller starts from defaults.
struct DataCaches final {
    usize level1{32 * 1024};     // the first-level data cache of one core
    usize level2{256 * 1024};    // the second-level cache one core sees
    usize line{64};              // the first-level line
    usize ways{8};               // the first-level associativity
};

// Fills what the host reports: on hosts with two kinds of core, the
// performance cores' caches. Never fails; reads host facts only (no lock is
// held, nothing is changed by the call).
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void read_data_caches(DataCaches& caches) noexcept;

} // namespace stxlib::cache_backend
