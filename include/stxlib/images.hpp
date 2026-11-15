#pragma once

// The loaded images (executables and shared libraries): which image and
// nearest symbol hold an address (Capability::address_lookup), and the
// named data regions images carry (Capability::image_regions). Replaces
// GUI.Forms' direct dladdr, GetModuleHandleExW and GetModuleFileNameW
// (core/failure/fail_fast.cpp, abi/drawing_skia_c_api.cpp) and its own
// image walks (core/text/localizable/localizable.cpp: getsectiondata over
// the dyld images, PE section headers over the process's modules, and
// PT_NOTE segments through dl_iterate_phdr), all at 491156eac (A0
// inventory, section 2). A1 platform-rest.
//
// Lifetime: everything an image lends (a region's bytes, a symbol's
// address) is valid while the image stays loaded. The executable and the
// libraries it was linked with stay loaded for the whole run; a library
// loaded later stays while a Library (library.hpp) holds it.

#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// ---- address lookup -------------------------------------------------------

// What the host knows about an address. The names are copied into the
// caller's rooms and borrowed from them: valid while the rooms are, and cut
// to their capacity (the *_truncated flags say so). They are the host's
// bytes: on POSIX hosts the image's file name as the loader recorded it and
// the symbol's linker name; on Windows WTF-8 of the module's file name and
// the export's ASCII name.
struct AddressDescription final {
    // The address at which the image is loaded (its header).
    uptr image_base{0};
    // The image's file name; may be relative on POSIX hosts when the
    // program was started by a relative name.
    View<char> image_name{};
    bool image_name_truncated{false};
    // The nearest symbol at or below the address that the host can name:
    // dladdr's dynamic symbols on POSIX hosts (a static function is not
    // among them), the module's exports on Windows (found by walking its
    // export directory; DbgHelp is not used: it allocates and is not
    // thread-safe). Empty, with symbol_address 0, when none is known.
    View<char> symbol_name{};
    bool symbol_name_truncated{false};
    uptr symbol_address{0};
};

// Describes `address`. Allocates nothing in STXlib's code, so a fault
// report can call it (fail_fast.hpp lists it among a reporter's calls).
// The host's loader takes its own locks, so it must not be called from a
// signal handler. What each loader was read to do (sb1, 2026-11-15):
// - glibc 2.41 elf/dl-addr.c: _dl_addr takes GL(dl_load_lock) with
//   __rtld_lock_lock_recursive and releases it, and allocates nothing; the
//   lock is recursive, so a thread failing inside the loader (a static
//   constructor run by dlopen) takes it again without deadlock.
// - macOS: dyld's APIs::dladdr (apple-oss-distributions/dyld,
//   dyld/DyldAPIs.cpp on its main branch, not a pinned tag) allocates
//   nothing and takes no lock for the shared cache and for images that
//   never unload; other images are found under the loaders' read lock
//   (findImageMappedAt, "slow path").
// - FreeBSD: rtld's dladdr takes rtld_bind_lock for reading
//   (libexec/rtld-elf/rtld.c:4374-4427 at the pin; whether a thread that
//   holds it for writing can take it again was not read).
// - musl's dladdr was not read at a pin.
// - Windows: GetModuleHandleExW (FROM_ADDRESS, UNCHANGED_REFCOUNT),
//   GetModuleFileNameW into a fixed 1,024-unit buffer on the stack,
//   K32GetModuleInformation, and a walk of the module's export directory
//   that reads only inside the module's image (core/pe_image.hpp). The
//   sources are closed and Microsoft's documentation does not say whether
//   these three calls allocate or which locks they take, so on Windows
//   neither is established; the A2 test (reporter_stack_unallocated)
//   refuses C++ allocation there, not the process heap.
// Failures: not_found
// with HostCode::no_image when no loaded image holds the address;
// unsupported where Capability::address_lookup is false.
[[nodiscard]] Result<AddressDescription> describe_address(uptr address, MutableView<char> image_name_room,
                                                          MutableView<char> symbol_name_room) noexcept;

// ---- image regions --------------------------------------------------------

// The region to find, spelled in each image format's terms. A program
// places its data in a region of its own (a section attribute in its
// source) and asks for it here by those names.
//   - Mach-O (macOS): the section `mach_o_section` in the segment
//     `mach_o_segment`, through getsectiondata (each at most 16 bytes, the
//     format's name fields).
//   - PE (Windows): the section `pe_section` (at most 8 bytes, the image
//     format's name field; COFF groups "name$suffix" sections into "name"
//     when linking, so a grouped name is matched as well).
//   - ELF (Linux, FreeBSD): section names are not available at run time
//     (section headers need not be loaded), so every PT_NOTE segment of each
//     image is visited, and the program finds its own notes in them by their
//     owner name. A section whose name begins ".note" is a note section,
//     which the linker places in a PT_NOTE segment.
struct ImageRegionQuery final {
    TextView mach_o_segment{};
    TextView mach_o_section{};
    TextView pe_section{};
};

enum class ImageRegionForm : u8 {
    named_section = 0, // Mach-O or PE: exactly the named section's bytes
    note_segment = 1,  // ELF: a PT_NOTE segment, which may hold other programs' notes too
};

struct ImageRegion final {
    // Borrowed from the loaded image (see Lifetime, above).
    View<Byte> bytes{};
    ImageRegionForm form{ImageRegionForm::named_section};
    // The alignment of the region's entries: 4 for named sections, the
    // segment's p_align for ELF notes (4 or 8).
    u32 alignment{4};
    uptr image_base{0};
};

// Called once per region found; return true to continue, false to stop.
// It runs while the host's image list is held (dl_iterate_phdr holds
// FreeBSD's rtld_phdr_lock, libexec/rtld-elf/rtld.c:4532-4541 at the pin;
// glibc holds its load lock): it must not load or unload a library, nor
// call describe_address or visit_image_regions.
using ImageRegionVisitor = bool (*)(const ImageRegion& region, void* context) noexcept;

// Visits the region in every loaded image, in the host's image order.
// Images loaded or unloaded during the visit may be missed. Failures:
// invalid_argument with HostCode::invalid_name for a name longer than its
// format allows (or empty, for the current format), or a null visitor;
// out_of_memory (the Windows module list); foreign_failure with the
// host's code; unsupported where Capability::image_regions is false.
[[nodiscard]] Status visit_image_regions(const ImageRegionQuery& query, ImageRegionVisitor visitor,
                                         void* context) noexcept;

// As visit_image_regions, for the one image holding `address` only;
// not_found with HostCode::no_image when none does.
[[nodiscard]] Status visit_image_regions_containing(uptr address, const ImageRegionQuery& query,
                                                    ImageRegionVisitor visitor, void* context) noexcept;

} // namespace stxlib
