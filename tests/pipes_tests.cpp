// Tests of anonymous pipes (include/stxlib/pipes.hpp): a round trip, the
// end of the stream when the writer closes, a write to a pipe whose reader
// closed failing as a result (not a signal), moves, and the closed ends'
// refusals. claim_protocol_streams is not run here: it rewires this
// process's standard output for good (the MCP tests drive a server over
// create_pipe pairs instead).

#include "stxlib/pipes.hpp"

#include "test_support.hpp"

#include <cstring>

namespace {

using stxlib::Byte;
using stxlib::MutableView;
using stxlib::OperationResult;
using stxlib::OperationStatus;
using stxlib::PipePair;
using stxlib::PipeRead;
using stxlib::usize;
using stxlib::View;

PipePair make_pair() {
    OperationResult<PipePair> made = stxlib::create_pipe();
    CHECK(made.succeeded());
    if (made.failed()) return PipePair{};
    return static_cast<OperationResult<PipePair>&&>(made).take_value();
}

View<Byte> bytes_of(const char* const text) {
    const View<Byte> result(reinterpret_cast<const Byte*>(text), std::strlen(text));
    return result;
}

void round_trip() {
    PipePair pair = make_pair();
    CHECK(pair.reader.is_open() && pair.writer.is_open());
    CHECK(pair.writer.write_all(bytes_of("hello\n")).succeeded());
    Byte buffer[64]{};
    OperationResult<PipeRead> got = pair.reader.read(MutableView<Byte>(buffer));
    CHECK(got.succeeded());
    if (got.succeeded()) {
        const PipeRead& read = *got.value();
        CHECK(!read.is_end());
        CHECK(read.byte_count() != nullptr && *read.byte_count() == 6);
        CHECK(std::memcmp(buffer, "hello\n", 6) == 0);
    }
}

void end_of_stream() {
    PipePair pair = make_pair();
    CHECK(pair.writer.write_all(bytes_of("last")).succeeded());
    pair.writer.close();
    CHECK(!pair.writer.is_open());
    Byte buffer[16]{};
    usize total = 0;
    bool ended = false;
    for (int attempt = 0; attempt < 8 && !ended; ++attempt) {
        OperationResult<PipeRead> got = pair.reader.read(MutableView<Byte>(buffer + total, sizeof buffer - total));
        CHECK(got.succeeded());
        if (got.failed()) break;
        const PipeRead& read = *got.value();
        if (read.is_end()) {
            ended = true;
        } else {
            total = total + *read.byte_count();
        }
    }
    CHECK(ended);
    CHECK(total == 4 && std::memcmp(buffer, "last", 4) == 0);
}

void write_after_reader_closed() {
    PipePair pair = make_pair();
    pair.reader.close();
    const OperationStatus written = pair.writer.write_all(bytes_of("nobody listens"));
    CHECK(written.failed());
}

void moves_and_closed_ends() {
    PipePair pair = make_pair();
    stxlib::PipeReader moved = static_cast<stxlib::PipeReader&&>(pair.reader);
    CHECK(moved.is_open());
    CHECK(!pair.reader.is_open());
    Byte buffer[4]{};
    OperationResult<PipeRead> refused = pair.reader.read(MutableView<Byte>(buffer));
    CHECK(refused.failed());
    stxlib::PipeWriter writer = static_cast<stxlib::PipeWriter&&>(pair.writer);
    CHECK(writer.write_all(bytes_of("x")).succeeded());
    CHECK(pair.writer.write_all(bytes_of("x")).failed());
    OperationResult<PipeRead> got = moved.read(MutableView<Byte>(buffer));
    CHECK(got.succeeded() && (*got.value()).byte_count() != nullptr && *(*got.value()).byte_count() == 1);
}

void large_write() {
    // More than a pipe's buffer (64 KiB on most hosts), read by this thread
    // only after the write completes would deadlock; so write less than
    // that, in many pieces, and read it all back.
    PipePair pair = make_pair();
    char piece[1000];
    for (usize index = 0; index < sizeof piece; ++index) piece[index] = static_cast<char>('a' + index % 26);
    for (int round = 0; round < 16; ++round) {
        CHECK(pair.writer.write_all(View<Byte>(reinterpret_cast<const Byte*>(piece), sizeof piece)).succeeded());
    }
    pair.writer.close();
    usize total = 0;
    bool same = true;
    Byte buffer[4096];
    for (;;) {
        OperationResult<PipeRead> got = pair.reader.read(MutableView<Byte>(buffer));
        if (got.failed() || (*got.value()).is_end()) break;
        const usize count = *(*got.value()).byte_count();
        for (usize index = 0; index < count; ++index) {
            if (buffer[index] != static_cast<Byte>('a' + (total + index) % 1000 % 26)) same = false;
        }
        total = total + count;
    }
    CHECK(total == 16000);
    CHECK(same);
}

} // namespace

int main() {
    round_trip();
    end_of_stream();
    write_after_reader_closed();
    moves_and_closed_ends();
    large_write();
    return stxlib_tests::finish("pipes");
}
