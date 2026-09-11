# Native host fuzzing for TSM

Two coverage-guided libFuzzer targets compile the production TSM sources:

- `fuzz_vtparse`: arbitrary binary input, callback bounds, normalized event
  traces, and parser state equivalence across whole-buffer, bytewise, and
  variable-sized feeds. Print spans are compared as individual codepoints.
- `fuzz_termstate`: the same chunk comparisons for screen cells, inactive
  screen, saved cursors, modes, dirty ranges, parser state, and response bytes.
  It checks cursor/ring bounds, reads visible scrollback rows, feeds more data
  while scrolled back, clears dirty tracking, and checks reset output.

Inputs are raw VT bytes with no harness header. Terminal dimensions are selected
deterministically from an input hash: 1x1, 1x4, 7x1, 7x3, 80x24, or 220x60,
with 0/3/6/9 scrollback rows. Changing the input can change the geometry too.
Each invocation creates and frees its state. Inputs above 64 KiB are skipped;
the runner enforces that same maximum. `CHECK` stays active in release builds.

## Build with the existing Windows compiler

The checked local simulator and test caches use MSVC 19.44 (VS 2022).
No ESP-IDF, WSL, SDL, SSH server, or device is needed. Use an **x64 Native Tools
Command Prompt** with CMake and Ninja on PATH. The Visual Studio installation
must include **C++ AddressSanitizer**, including its libFuzzer libraries.

From the repository root:

```bat
cmake -S tests/tsm -B tests/tsm/build/fuzz-msvc -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTSM_BUILD_FUZZERS=ON
cmake --build tests/tsm/build/fuzz-msvc
ctest --test-dir tests/tsm/build/fuzz-msvc --output-on-failure
```

A separate build directory avoids changing the existing simulator/test cache.
For this machine the developer environment can be initialized with
`call C:\dev\msvc.2021\VC\Auxiliary\Build\vcvars64.bat` in cmd.exe.
The old `tests/tsm/build` Visual Studio generator cache refers to an instance
no longer registered with the installer; the separate Ninja build works.

MSVC builds use `/fsanitize=fuzzer /fsanitize=address /Zi`. CMake disables
incompatible runtime checks for the fuzz targets and incremental linking, and
copies the compiler's ASan runtime DLL beside the executables. Normal Unity
targets retain their original flags. VS multi-configuration builds also work
with `--config RelWithDebInfo` on build and `-C RelWithDebInfo` on CTest.

Microsoft labels its [libFuzzer option experimental](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-building?view=msvc-170#fsanitizefuzzer-compiler-option-experimental).
MSVC ASan detects memory errors; it does **not** provide UBSan signed-overflow
checking. Explicit invariants catch some arithmetic corruption, not all of it.
Optional GNU-driver Clang builds enable ASan and UBSan as well; the native MSVC
build is the validated path here. Fuzzing does not prove VT compatibility or
ESP32-specific allocator/timing behavior, and throughput needs separate benchmarks.

## Run, replay, minimize

Python is used only for the convenience runner and corpus regeneration.
CTest replays the checked-in corpus for both targets without mutating it.

```bat
python tests/tsm/fuzz/run_fuzz.py --build tests/tsm/build/fuzz-msvc --seconds 60
python tests/tsm/fuzz/run_fuzz.py --build tests/tsm/build/fuzz-msvc --target termstate --seconds 3600 --seed 42
python tests/tsm/fuzz/run_fuzz.py --build tests/tsm/build/fuzz-msvc --seconds 300 --shards 2 --jobs 4 --memory-budget-mb 4096
python tests/tsm/fuzz/run_fuzz.py --build tests/tsm/build/fuzz-msvc --target termstate --replay PATH_TO_CRASH
python tests/tsm/fuzz/run_fuzz.py --build tests/tsm/build/fuzz-msvc --target termstate --minimize PATH_TO_CRASH --seconds 120
```

`--seconds` is per target and shard. By default the runner starts at most two
processes, uses one shard per target, limits each process to 1024 MiB RSS, and
uses a 2048 MiB aggregate memory budget. `--jobs` is the requested concurrency;
the effective concurrency is reduced to fit
`floor(memory-budget-mb / rss-limit-mb)`. A memory budget smaller than one RSS
limit is rejected.

Each fuzzing shard has separate logs, writable coverage corpus, and artifacts
under `build/fuzz-msvc/fuzz-results/<target>/shard-N/`. Subsequent runs reuse
the matching shard corpus. Logs are overwritten per shard; crash files are
retained. The expanded example above runs four campaigns concurrently and is
a suitable starting point for an 8-logical-core host with at least 4 GiB free.

Replay and minimization always run one process and do not create shard jobs,
even if `--jobs` or `--shards` is supplied. Their files remain directly under
`build/fuzz-msvc/fuzz-results/<target>/`. Minimization writes
`artifacts/minimized.vt`; preserve the original failure too. The runner returns
nonzero if any process fails and sets a 10-second per-input timeout.

The executables also accept standard [libFuzzer options](https://llvm.org/docs/LibFuzzer.html#options).
For example, `fuzz_vtparse.exe -runs=0 PATH_TO_CORPUS` replays a directory.
An input failure prints the check or ASan report and writes the exact bytes.
Avoid displaying arbitrary corpus bytes directly in a terminal; use a hex viewer.

Turn each confirmed failure into a Unity test before fixing production code.
Keep its raw seed in the generator or a separately documented regression file.
A deterministic seed makes runs easier to compare, but wall-clock run lengths
and toolchain changes affect the mutations performed.

## Corpus and provenance

`corpus/` contains 263 binary seeds (13,167 bytes at initial generation).
`corpus_manifest.json` records each filename, size, and origin. `.gitattributes`
disables newline conversion so Git preserves raw CR/LF bytes on Windows.

- **Harvested locally:** C string literals containing control/non-ASCII bytes
  from `test_vtparse.c` and `test_termstate.c`. Adjacent literals are joined and
  C escapes decoded; these include partial feeds and expected replies, not
  complete recordings of every unit test. C byte-array initializers are not
  harvested automatically.
- **Generated here:** print-buffer and OSC boundaries; CSI parameter limits and
  oversized numbers; malformed/truncated UTF-8; all 256 byte values; interrupted
  strings; scrollback wraparound; mode changes; charset/reset; and a synthetic
  full-screen TUI session. These are original project test data under the
  repository's MIT license, not third-party captures or private SSH recordings.
- `vt.dict` supplies VT introductions, terminators, mode sequences, separators,
  numeric boundaries, and UTF-8 fragments for guided mutation.

Regenerate after updating tests:

```bat
python tests/tsm/fuzz/generate_corpus.py
```

The generator uses only the Python standard library and local source files;
it requires no network. It deduplicates by content and removes obsolete files
listed in its previous manifest. Separately named manual seeds are preserved.
Review and commit the binary seeds, manifest, and dictionary together.

## Initial findings

Corpus replay caught signed parameter accumulation wrapping `2147483648` into
a negative value. Parameters now saturate at `INT32_MAX`. Follow-up Unity tests
also caught overflow when large values were added to cursor coordinates, an
origin-relative row, and erase counts; these now clamp before addition.
The tests were observed failing before the fixes. Their sequences are included
in the harvested corpus.

Validation on MSVC 19.44, RelWithDebInfo: all 173 Unity tests and both corpus
replays passed. Final 60-second campaigns (`--seed 42`, reusing the first
campaign's corpus) completed 354,858 parser and 36,669 terminal invocations
without further failures. These counts describe a short smoke campaign, not
exhaustive coverage. The original overflow artifact also replayed successfully
after the fix.
