// Exceptions the C++ library throws through this runtime: out_of_range,
// length_error, invalid_argument, bad_alloc from operator new, and the
// hierarchy they are caught through. The what() texts of the runtime's
// own classes differ between libraries (vcruntime's "bad allocation",
// libc++'s "std::bad_alloc"), so only the handler that matched is printed.

#include "case_support.hpp"

#include <new>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    std::vector<int> values(3, 1);
    try {
        const int value = values.at(5);
        SAYF("unreached %d", value);
    } catch (const std::out_of_range&) {
        say("at threw out_of_range");
    }
    try {
        const int value = values.at(7);
        SAYF("unreached %d", value);
    } catch (const std::logic_error&) {
        say("out_of_range caught as logic_error");
    }
    try {
        const int value = std::stoi("not a number");
        SAYF("unreached %d", value);
    } catch (const std::invalid_argument&) {
        say("stoi threw invalid_argument");
    }
    try {
        std::string text;
        text.reserve(text.max_size() + 1);
    } catch (const std::length_error&) {
        say("reserve threw length_error");
    } catch (const std::bad_alloc&) {
        say("reserve threw bad_alloc");
    }
    try {
        // volatile: a constant size this large is rejected at compile time.
        volatile std::size_t huge = static_cast<std::size_t>(1) << 62;
        char* const block = new char[huge];
        SAYF("unreached %d", block != nullptr ? 1 : 0);
        delete[] block;
    } catch (const std::bad_alloc&) {
        say("new threw bad_alloc");
    }
    try {
        throw std::runtime_error("runtime text");
    } catch (const std::exception& caught) {
        SAYF("runtime_error as exception: %s", caught.what());
    }
    return 0;
}
