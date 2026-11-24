// The allocator at a module's unload (the GUI.Forms ABI unload test under
// LSan, 2026-11-24): a library linking STXlib is loaded, allocates and
// releases a block, and is unloaded. Under LeakSanitizer a registry table
// left in the unloaded module's statics is reported at exit and fails the
// test (src/state/allocator.cpp, RegistryEnd). Linux only.

#include <cstdio>

#include <dlfcn.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: stxlib_allocator_unload_tests <library>\n");
        return 2;
    }
    for (int round = 0; round < 3; ++round) {
        void* const library = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            std::fprintf(stderr, "dlopen: %s\n", ::dlerror());
            return 1;
        }
        using Round = int (*)() noexcept;
        const auto run = reinterpret_cast<Round>(::dlsym(library, "stxlib_unload_round"));
        if (run == nullptr || run() != 0) {
            std::fprintf(stderr, "round %d failed\n", round);
            return 1;
        }
        if (::dlclose(library) != 0) {
            std::fprintf(stderr, "dlclose: %s\n", ::dlerror());
            return 1;
        }
        void* const still = ::dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
        std::printf("round %d: library %s\n", round, still == nullptr ? "unloaded" : "still loaded");
        if (still != nullptr) ::dlclose(still);
    }
    return 0;
}
