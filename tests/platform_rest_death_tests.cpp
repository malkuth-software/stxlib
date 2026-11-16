// Must-end tests of sealed pages (pages.hpp), A1 platform-rest. Each case
// runs as its own process (tests/death_check.cmake) and must end abnormally.
// A case that survives prints "survived" and returns 0, which the check
// rejects; where Capability::page_protection is false a case prints
// "skipped".
//
//   sealed_writable  asking a sealed block for its writable bytes is a failed
//                    requirement, with STXlib's report
//   sealed_store     a write through a pointer kept from before sealing
//                    faults (the host's protection; any report)

#include "stxlib/capabilities.hpp"
#include "stxlib/pages.hpp"

#include <cstdio>
#include <cstring>

namespace {

bool supported() {
    const bool result = stxlib::capability_supported(stxlib::Capability::page_protection);
    if (!result) std::printf("skipped: page_protection unsupported\n");
    return result;
}

bool sealed_block(stxlib::PageBlock& block, stxlib::Byte*& kept) {
    stxlib::OperationResult<stxlib::PageBlock> mapped = stxlib::PageBlock::map(64);
    if (mapped.failed()) {
        std::printf("skipped: the host refused the pages\n");
        return false;
    }
    block = static_cast<stxlib::OperationResult<stxlib::PageBlock>&&>(mapped).take_value();
    const stxlib::MutableView<stxlib::Byte> writable = block.writable();
    writable[0] = static_cast<stxlib::Byte>(1);
    kept = writable.data();
    if (block.seal().failed()) {
        std::printf("skipped: the host refused to seal\n");
        return false;
    }
    return true;
}

int sealed_writable() {
    if (!supported()) return 0;
    stxlib::PageBlock block;
    stxlib::Byte* kept = nullptr;
    if (!sealed_block(block, kept)) return 0;
    const stxlib::MutableView<stxlib::Byte> writable = block.writable();
    std::printf("survived %zu\n", static_cast<std::size_t>(writable.count()));
    return 0;
}

int sealed_store() {
    if (!supported()) return 0;
    stxlib::PageBlock block;
    stxlib::Byte* kept = nullptr;
    if (!sealed_block(block, kept)) return 0;
    // volatile, so the store is made and not folded away.
    volatile stxlib::Byte* const target = kept;
    target[1] = static_cast<stxlib::Byte>(2);
    std::printf("survived %d\n", static_cast<int>(target[1]));
    return 0;
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: stxlib_platform_rest_death_tests <case>\n");
        return 2;
    }
    const char* const name = argv[1];
    if (std::strcmp(name, "sealed_writable") == 0) return sealed_writable();
    if (std::strcmp(name, "sealed_store") == 0) return sealed_store();
    std::fprintf(stderr, "unknown case %s\n", name);
    return 2;
}
