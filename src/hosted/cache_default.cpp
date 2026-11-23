// The cache module of the families that report no cache sizes here
// (hosted/cache_backend.hpp): FreeBSD, Windows and Emscripten. The caller's
// defaults stand (a first-level cache of 32 KiB, 8-way, 64-byte lines, and a
// second level of 256 KiB). The walk's layout then fits that machine, which
// changes speed only, never a result.

#include "hosted/cache_backend.hpp"

namespace stxlib::cache_backend {

void read_data_caches(DataCaches& caches) noexcept { static_cast<void>(caches); }

} // namespace stxlib::cache_backend
