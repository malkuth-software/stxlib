// Anonymous pipes on Emscripten (include/stxlib/pipes.hpp): a page has no
// standard streams a client can hold and no process to start, so
// Capability::anonymous_pipes is false and every call fails with
// unsupported (core/capabilities.cpp). Nothing here calls the host.

#include "stxlib/pipes.hpp"

#include "core/host_failure.hpp"

namespace stxlib {

void PipeReader::close() noexcept { handle_ = closed; }

OperationResult<PipeRead> PipeReader::read(const MutableView<Byte> into) noexcept {
    STXLIB_REQUIRE(!into.empty(), "PipeReader::read: no room");
    return OperationResult<PipeRead>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.pipes.read", HostCode::none));
}

void PipeWriter::close() noexcept { handle_ = closed; }

OperationStatus PipeWriter::write_all(const View<Byte>) noexcept {
    return host_failure::failed(ErrorKind::unsupported, "stxlib.pipes.write", HostCode::none);
}

OperationResult<PipePair> create_pipe() noexcept {
    return OperationResult<PipePair>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.pipes.create", HostCode::none));
}

OperationResult<ProtocolStreams> claim_protocol_streams() noexcept {
    return OperationResult<ProtocolStreams>::failure(
        host_failure::record(ErrorKind::unsupported, "stxlib.pipes.claim_protocol_streams", HostCode::none));
}

} // namespace stxlib
