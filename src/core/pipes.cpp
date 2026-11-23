// The parts of anonymous pipes (include/stxlib/pipes.hpp) that do not call
// the host: the read outcome, moves and adoption. The host's calls are in
// src/hosted/<family>/pipes_<family>.cpp.

#include "stxlib/pipes.hpp"

namespace stxlib {

PipeRead PipeRead::bytes(const usize count) noexcept {
    STXLIB_REQUIRE(count > 0, "PipeRead::bytes: a read of no bytes is the end of the stream");
    const PipeRead result(Kind::bytes, count);
    return result;
}

PipeReader& PipeReader::operator=(PipeReader&& other) noexcept {
    if (this == &other) return *this;
    close();
    handle_ = other.handle_;
    other.handle_ = closed;
    return *this;
}

PipeReader::~PipeReader() { close(); }

PipeReader PipeReader::adopt(const i64 handle) noexcept {
    STXLIB_REQUIRE(handle != closed, "PipeReader::adopt: no handle");
    PipeReader result(handle);
    return result;
}

PipeWriter& PipeWriter::operator=(PipeWriter&& other) noexcept {
    if (this == &other) return *this;
    close();
    handle_ = other.handle_;
    other.handle_ = closed;
    return *this;
}

PipeWriter::~PipeWriter() { close(); }

PipeWriter PipeWriter::adopt(const i64 handle) noexcept {
    STXLIB_REQUIRE(handle != closed, "PipeWriter::adopt: no handle");
    PipeWriter result(handle);
    return result;
}

} // namespace stxlib
