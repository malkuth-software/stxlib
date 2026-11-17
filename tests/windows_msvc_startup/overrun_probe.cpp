// A deliberate stack buffer overrun under the stack protector: the cookie
// check fails at the function's return and the shim ends the process with
// __fastfail (STATUS_STACK_BUFFER_OVERRUN). Returns 0 only if the overrun
// went unnoticed.

#include <cstring>

namespace {

__attribute__((noinline)) void overrun(const char* const text, const unsigned long count) {
    char buffer[8];
    std::memcpy(buffer, text, count);
    volatile char sink = buffer[0];
    (void)sink;
}

}  // namespace

int main() {
    char text[64];
    std::memset(text, 'x', sizeof(text));
    volatile unsigned long count = sizeof(text);
    overrun(text, count);
    return 0;
}
