// An exception no handler takes: std::terminate ([except.handle]p9),
// reached through the startup's unhandled-exception filter.

#include "case_support.hpp"

namespace {

void deep(const int depth) {
    if (depth == 0) throw depth;
    deep(depth - 1);
}

} // namespace

int main() {
    std::set_terminate(case_terminate_handler);
    say("throwing with no handler");
    deep(5);
    say("returned, wrong");
    return 0;
}
