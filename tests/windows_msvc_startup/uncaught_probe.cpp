// An exception no handler catches: the shim's unhandled-exception filter
// calls terminate(), whose default handler aborts (exit code 3). Returns 0
// only if the throw was somehow survived.

#include <stdexcept>

int main() {
    throw std::logic_error("uncaught");
}
