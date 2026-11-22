# Block engine regressions

Each `.cpp` here is a failure the block engine found (tests/ic/ic_run.cpp,
`--emit tests/ic/regressions`), minimised and written as an ordinary test:
its transcript in the comment, its bytes replayed through the engine and
checked to find nothing. CMake builds one test per file. A file is kept
after its fix, so the fix stays tested.
