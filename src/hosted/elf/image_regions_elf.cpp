// Image regions on ELF hosts (hosted/image_backend.hpp, visit_regions):
// every PT_NOTE segment of each loaded object, through dl_iterate_phdr,
// shared by Linux (glibc, musl) and FreeBSD. GUI.Forms' localizable.cpp
// reads its tables the same way (491156eac). A1 platform-rest.
//
// FreeBSD at the pin (fb9033d2876d): dl_iterate_phdr calls the callback
// once per object with rtld_phdr_lock held and rtld_bind_lock released
// (libexec/rtld-elf/rtld.c:4520-4558), and stops when it returns non-zero;
// struct dl_phdr_info gives dlpi_addr, dlpi_phdr and dlpi_phnum
// (sys/sys/link_elf.h:81-89); PT_LOAD and PT_NOTE are
// sys/sys/elf_common.h:551 and 554. FreeBSD has no ElfW macro, so the
// program header type is taken from dl_phdr_info itself (below). glibc and
// musl: the same interface (glibc holds its load lock during the
// callback; unconfirmed at a pin).
//
// A segment's address is dlpi_addr + p_vaddr and its size p_memsz; a note
// segment's entries are aligned to p_align (4, or 8 for some notes).

#include "hosted/image_backend.hpp"

#include "core/host_failure.hpp"

#include <cstddef>

#include <link.h>
#if defined(__linux__)
#include <elf.h>
#endif

namespace stxlib::image_backend {

namespace {

// The program header type, from the declaration of dl_phdr_info (house
// style, section 2: never retyped by hand).
template <typename Pointer>
struct PointeeOf;

template <typename Element>
struct PointeeOf<const Element*> {
    using Type = Element;
};

using ProgramHeader = PointeeOf<decltype(dl_phdr_info::dlpi_phdr)>::Type;

struct Search final {
    bool containing{false};
    uptr address{0};
    ImageRegionVisitor visitor{nullptr};
    void* context{nullptr};
    bool found{false};
};

// Whether a PT_LOAD segment of the object holds `address`.
bool holds(const dl_phdr_info& information, const uptr address) noexcept {
    for (usize index = 0; index < static_cast<usize>(information.dlpi_phnum); ++index) {
        const ProgramHeader& header = information.dlpi_phdr[index];
        if (header.p_type != PT_LOAD) continue;
        const uptr start = static_cast<uptr>(information.dlpi_addr) + static_cast<uptr>(header.p_vaddr);
        const uptr size = static_cast<uptr>(header.p_memsz);
        if (address >= start && address - start < size) return true;
    }
    return false;
}

// Visits the object's note segments; false when the visitor asks to stop.
bool visit_notes(const dl_phdr_info& information, const Search& search) noexcept {
    for (usize index = 0; index < static_cast<usize>(information.dlpi_phnum); ++index) {
        const ProgramHeader& header = information.dlpi_phdr[index];
        if (header.p_type != PT_NOTE || header.p_memsz == 0) continue;
        const uptr start = static_cast<uptr>(information.dlpi_addr) + static_cast<uptr>(header.p_vaddr);
        ImageRegion region{};
        region.bytes = View<Byte>(reinterpret_cast<const Byte*>(start), static_cast<usize>(header.p_memsz));
        region.form = ImageRegionForm::note_segment;
        region.alignment = header.p_align == 8 ? 8 : 4;
        region.image_base = static_cast<uptr>(information.dlpi_addr);
        if (!search.visitor(region, search.context)) return false;
    }
    return true;
}

// dl_iterate_phdr's callback: its type is checked against the declaration
// where it is passed. Non-zero stops the iteration.
int visit_object(dl_phdr_info* const information, const std::size_t, void* const data) {
    Search& search = *static_cast<Search*>(data);
    if (search.containing) {
        if (!holds(*information, search.address)) return 0;
        search.found = true;
        static_cast<void>(visit_notes(*information, search));
        return 1;
    }
    const bool more = visit_notes(*information, search);
    return more ? 0 : 1;
}

} // namespace

Status visit_regions(const ImageRegionQuery& query, const bool containing, const uptr address,
                     const ImageRegionVisitor visitor, void* const context) noexcept {
    static_cast<void>(query); // ELF notes are found by type (images.hpp)
    Search search{};
    search.containing = containing;
    search.address = address;
    search.visitor = visitor;
    search.context = context;
    static_cast<void>(::dl_iterate_phdr(visit_object, &search));
    if (containing && !search.found) {
        return host_failure::failed(ErrorKind::not_found, "stxlib.images.visit_regions_containing", HostCode::no_image);
    }
    return Status::success();
}

} // namespace stxlib::image_backend
