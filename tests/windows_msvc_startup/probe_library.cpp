// A DLL for the startup probe: a static object constructed by the DLL's
// initializers, a thread_local with a dynamic initializer, and no DllMain
// (the shim's default one is chosen).

#include <atomic>

namespace {

struct StaticValue {
    int value;
    StaticValue() : value(40) {}
};

StaticValue static_value;

std::atomic<int> thread_value_constructions{0};

int make_thread_value() {
    thread_value_constructions.fetch_add(1);
    return 2;
}

thread_local int thread_value = make_thread_value();

}  // namespace

extern "C" __declspec(dllexport) int probe_library_value(void) {
    const int value = static_value.value + thread_value;
    return value;
}

extern "C" __declspec(dllexport) int probe_library_thread_constructions(void) {
    const int count = thread_value_constructions.load();
    return count;
}
