// Exception objects of awkward shapes: a large one by value, an
// over-aligned one, one owning heap memory (deep copy and destruction), and
// a class with virtual bases caught by value (its copy constructor takes
// the "most derived" flag).

#include "case_support.hpp"

#include <string>

namespace {

struct Large {
    int cells[512];
    explicit Large(const int seed) {
        for (int index = 0; index < 512; ++index) cells[index] = seed + index;
    }
};

struct alignas(32) Aligned {
    double values[4];
};

struct Owning {
    std::string text;
    explicit Owning(const char* const owning_text) : text(owning_text) {}
};

int virtual_copies = 0;

struct Core {
    int core;
    explicit Core(const int core_value) : core(core_value) {}
    Core(const Core& other) : core(other.core) {
        virtual_copies = virtual_copies + 1;
    }
    virtual ~Core() {}
};

struct Shell : virtual Core {
    int shell = 3;
    explicit Shell(const int shell_value) : Core(shell_value) {}
    Shell(const Shell& other) : Core(other), shell(other.shell) {}
};

int sum(const Large& large) {
    int total = 0;
    for (int index = 0; index < 512; ++index) total = total + large.cells[index];
    return total;
}

} // namespace

int main() {
    try {
        throw Large(1);
    } catch (const Large caught) {
        SAYF("large by value %d", sum(caught));
    }
    try {
        Aligned aligned{};
        aligned.values[0] = 1.5;
        aligned.values[3] = 2.5;
        throw aligned;
    } catch (const Aligned& caught) {
        const bool on_alignment = (reinterpret_cast<unsigned long long>(&caught) % 32) == 0;
        SAYF("aligned %d %.1f %.1f", on_alignment ? 1 : 0, caught.values[0], caught.values[3]);
    }
    try {
        throw Owning("a string long enough to live on the heap, not in the small buffer");
    } catch (const Owning caught) {
        SAYF("owning by value %s", caught.text.c_str());
    }
    try {
        throw Shell(7);
    } catch (const Shell caught) {
        SAYF("virtual bases by value %d %d copies %d", caught.core, caught.shell, virtual_copies);
    }
    virtual_copies = 0;
    try {
        throw Shell(8);
    } catch (const Core caught) {
        SAYF("virtual base sliced %d copies %d", caught.core, virtual_copies);
    }
    return 0;
}
