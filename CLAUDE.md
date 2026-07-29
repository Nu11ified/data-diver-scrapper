# Data Diver: working rules

If you need a paragraph-long comment to justify why the workaround is OK, the code is wrong. Fix the code.

Do not narrate the code in comments. No file-header essays, no block comments above
functions restating what the signature already says, no commentary explaining why a
change was made. Names and types carry that. The only comment worth writing states a
constraint the code cannot show, such as an external format quirk or an ordering
requirement, and it fits on one line. When in doubt, delete it.

Reference the /docs/cpp-guide.md for how to write the CPP effectively

## What this project is

Data Diver is a C++ public-record ingestion engine. It classifies county source
documents, maps inconsistent schemas onto one canonical property-event schema,
resolves records to properties, and maintains an evidence-backed property
lifecycle. When a source changes shape, it detects the drift and attempts to
repair the extraction mapping automatically.

It is not a demo. Every path a user can reach from the CLI runs the real
pipeline against real bytes.

## Rules that follow from that

1. No stubbed returns, no fabricated numbers, no "TODO: implement". If a stage
   cannot do the job, it reports a failure that surfaces in the run record.
2. Every number shown in the UI comes from a measurement taken during a run.
   Timings come from a clock, byte counts from the fetcher, memory from the OS.
3. Confidence scores come from a model or a validator, never from a constant.
4. New dependencies need a reason. The engine builds with a C++20 compiler,
   libcurl, and zlib. Nothing else.
5. Tests cover the behaviour, not the implementation. `ctest` must pass before
   a commit.
6. Commit in small steps with messages that say what changed and why.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/datadiver
```
