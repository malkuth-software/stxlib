// Host entropy (include/stxlib/entropy.hpp) over the family's generator
// (host::fill_entropy, hosted/<family>/entropy_<family>.cpp).

#include "stxlib/entropy.hpp"

#include "core/host_failure.hpp"
#include "hosted/host.hpp"

namespace stxlib {

Status fill_entropy(const MutableView<Byte> output) noexcept {
    if (output.count() == 0) return Status::success();
    if (!host::fill_entropy(output.data(), output.count())) {
        return host_failure::failed(ErrorKind::foreign_failure, "stxlib.entropy.fill", HostCode::none);
    }
    return Status::success();
}

} // namespace stxlib
